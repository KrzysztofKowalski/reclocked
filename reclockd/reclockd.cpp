// reclockd v4 — auto-reclocking GT 750M (GK107, Kepler) pod nouveau.
//
// Buduje na v3 (polityka temperaturowo-obciążeniowa, dwell-countery, hwmon temp,
// mmap BAR0 PMU idle counters, vblank sync). NOWOŚCI v4:
//
//   1. PROFILE app-aware: `default` (cap 07, throttle) vs `preferred` (cap 0e
//      z eskalacją do 0f przy sustained high busy). Profil wybierany dynamicznie
//      na podstawie aktywnej apki z Hyprlanda (hyprctl activewindow + clients).
//   2. CONFIG /etc/reclockd.conf: lista [preferred] class-ów + progi per profil.
//      Własny parser INI-line, bez zewnętrznych deps.
//   3. MANUAL OVERRIDE: flag-file `/run/reclockd/override` — daemon zamraża auto
//      gdy istnieje. `pstate.sh set` tworzy flag-file, `pstate.sh auto` usuwa.
//   4. KONTROLA DAEMONA: SIGHUP → re-read configu bez restartu. systemd unit
//      + wrapper `reclockctl`.
//
// BEZPIECZNA drabinka 3-stanowa [07, 0a, 0e] (indeksy 0,1,2). Ceiling z profilu
// caps UP. DOWN zawsze dostępny (TERMAL/IDLE). Nigdy skok 07→0e (UP o 1 poziom).
//
// 0f (max, 967/2500 MHz) JEST WYKLUCZONY z LADDER (drabinki auto). Zamiast tego
// 0f działa jako BOOST TIER nad drabinką — wchodzi TYLKO gdy:
//   * aktualny pstate == szczyt drabinki (0e), tzn. g_cur_idx == ceiling,
//   * AND busy > busy-boost (85%) przez boost-dwell (5 s) — sustained ciężki load,
//   * AND temp < temp-up profilu preferred (75°C) przez temp-dwell (5 s).
// Straż termiczna 0f (po progach preferred): temp > temp-up (75°C) LUB temp >=
// temp-down (82°C) → NATYCHMIAST drop z 0f do 0e (szczyt drabinki). 0f to
// najgorętszy pstate = pierwszy do ścięcia. Wyjście loadowe: busy < busy-up
// (80%) → drop z 0f (histereza enter@85 / exit@80). 0f NIE jest w LADDER, więc
// nigdy nie jest krokiem przejściowym drabinki — tylko osobnym tierem boost.
// Opt-in ręczny nadal dostępny: `pstate.sh set 0f` tworzy override (daemon stoi).
//
// THERMAL GATING PER-PROFIL (NIE globalny): default (non-browser) temp-down=65,
// temp-up=58 — ostrożny throttle dla terminala/edytora. preferred (browser)
// temp-down=82, temp-up=75 — pełna wydajność, throttle dopiero przy 82°C.
// Straż termiczna (TERMAL DOWN) jest priorytetowa nad loadem w OBU profilach
// (dla default cap=07 na idx 0 nie ma niżej, ale logika jest obecna). Histereza
// load 80/40 (pasmo 40 pp).
//
// UP 07→0a→0e (UP-LOAD): g_cur_idx < ceiling AND temp < temp_up przez temp_dwell
//                        AND busy > busy_up. Jeden poziom na krok.
// DOWN 0e→0a→07 (TERMAL|IDLE|CEILING): temp > temp_down przez temp_dwell
//                        OR busy ≤ busy_down przez idle_dwell OR g_cur_idx > ceiling.
// BOOST 0e→0f (BOOST-UP): g_cur_idx==ceiling AND busy>busy_boost przez boost_dwell
//                        AND temp<temp_up przez temp_dwell. Tier nad drabinką.
// BOOST-DOWN 0f→0e: temp>temp_up LUB temp>=temp_down (NATYCHMIAST, priorytet)
//                        OR busy<busy_up (histereza loadowa).
//
// Sygnał preferred: hyprctl -j activewindow (focus) + hyprctl -j clients
// (running). Preferred aktywne gdy: focused.class ∈ lista LUB (running.class ∈
// lista AND busy > busy_up). Profile dwell (domyślnie 2 s) rate-limituje zmiany
// profilu (alt-tab nie migocze ceiling).
//
// Wymaga roota. USTAWIENIA RUNTIME-ONLY: reboot resetuje zegary.
// Bezpieczeństwo: walidacja pstate ∈ {07,0a,0e,0f}, nigdy zapis gdy ten sam stan,
// SIGTERM→restore --exit-state, fail-safe hwmon (brak temp → pominięte warunki
// termalne, nigdy awaryjny UP). Brak Hyprlanda/hyprctl → fallback do default.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <drm/drm.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ------------------------------------------------------------------ ścieżki

static const char* PCI_RESOURCE = "/sys/bus/pci/devices/0000:01:00.0/resource0";
static const char* PSTATE_FILE  = "/sys/kernel/debug/dri/0000:01:00.0/pstate";
static const char* POWER_CTRL   = "/sys/bus/pci/devices/0000:01:00.0/power/control";
static const char* HWMON_DIR    = "/sys/class/hwmon";
static const char* DRM_CARD     = "/dev/dri/card0"; // dGPU (nouveau) — eDP scanout
static const char* OVERRIDE_FILE = "/run/reclockd/override";
static const char* DEFAULT_CONFIG = "/etc/reclockd.conf";

// ------------------------------------- rejestry PMU (BAR0) wg gk20a_devfreq.c

static constexpr uint32_t R_IDLE_CTRL      = 0x10A50C;
static constexpr uint32_t R_IDLE_MASK      = 0x10A504;
static constexpr uint32_t R_IDLE_COUNT     = 0x10A508;
static constexpr uint32_t R_IDLE_THRESHOLD = 0x10A8A0;
static constexpr uint32_t R_IDLE_INTR_EN   = 0x10A9E8;
static constexpr uint32_t R_IDLE_INTR_ST   = 0x10A9EC;

static constexpr uint32_t C_TOTAL = 0, C_BUSY = 4;

static constexpr uint32_t CTRL_VALUE_MASK   = 0x3;
static constexpr uint32_t CTRL_VALUE_BUSY   = 0x2;
static constexpr uint32_t CTRL_VALUE_ALWAYS = 0x3;
static constexpr uint32_t CTRL_FILTER_MASK  = 0x4;
static constexpr uint32_t MASK_GR  = 0x1;
static constexpr uint32_t MASK_CE2 = 0x200000;
static constexpr uint32_t COUNT_MASK  = 0x7FFFFFFF;
static constexpr uint32_t COUNT_RESET = 0x80000000;

// --------------------------------------------------------------- stany / drabinka

// 3 stałe poziomy bezpiecznej drabinki. Indeksy 0,1,2 = 07, 0a, 0e.
// 0f NIE jest w drabince (opt-in ręczny przez override flag — patrz header).
static const uint32_t LADDER[3] = { 0x07, 0x0a, 0x0e };
static const int LADDER_N = 3;

static int state_to_idx(uint32_t st)
{
    for (int i = 0; i < LADDER_N; i++) if (LADDER[i] == st) return i;
    return -1;
}
static bool known_state(int s)
{
    return s == 0x07 || s == 0x0a || s == 0x0e || s == 0x0f;
}
static const char* state_hex(int st)
{
    // Rotating buforów — state_hex wywoływane wielokrotnie w jednym logf
    // (np. "cap=%s boost=%s"), więc pojedynczy static bufor powodowałby
    // aliasing (bug znany z v3 state_name). 4 bufory = max 4 wywołania na logf.
    static char bufs[4][8];
    static int idx = 0;
    char* buf = bufs[idx & 3];
    idx++;
    std::snprintf(buf, 8, "%02x", (unsigned int)st & 0xff);
    return buf;
}

// ------------------------------------------------------------------ profile

struct Profile {
    int  max_pstate   = 0x07;  // ceiling (idx w drabince)
    int  boost_pstate = -1;    // -1 = brak boost; 0f = off-ladder boost tier (honorowane)
    int  busy_boost   = 85;    // % busy avg > → BOOST UP (0e→0f) przez boost_dwell
    int  boost_dwell_ms = 5000;
    int  boost_hyst   = 10;    // pp rezerwa histerezy (obecnie boost-exit po busy_up)
    int  temp_down    = 65;    // °C TERMAL DOWN (per-profil; default 65, preferred 82)
    int  temp_up      = 58;    // °C: temp < temp_up → dozwolony UP (default 58, preferred 75)
};

struct Config {
    //Lista class-ów aplikacji "preferred" (dopasowywane do hyprctl class).
    std::set<std::string> preferred_classes;

    Profile def;       // profil default (apka nie-preferred)
    Profile preferred; // profil preferred (apka z listy)

    // Wspólne parametry polityki (histereza load: up 80% / down 40%, pasmo 40 pp):
    int  interval_ms    = 200;
    int  busy_up        = 80;
    int  busy_down      = 40;
    int  temp_dwell_ms  = 5000;
    int  idle_dwell_ms  = 5000;
    int  win_ms         = 1000;
    int  profile_dwell_ms = 2000; // rate-limit zmiany profilu
    int  poll_ms        = 1000;   // okres pollingu hyprctl (multiplik interval)
    int  exit_state     = 0x07;
    bool vblank_sync    = true;
    bool probe = false;
    bool dry   = false;
    int  verbosity = 1;

    // Defaulty profilu preferred różnią się od default — ustaw w konstruktorze.
    // Thermal jest PER-PROFIL: default 65/58 (apki non-preferred — ostrożnie),
    // preferred 82/75 (browser — pełna wydajność, throttle dopiero przy 82).
    Config() {
        preferred.max_pstate   = 0x0e;
        preferred.boost_pstate = -1;   // domyślnie bez boost; config odkomentowuje 0f
        preferred.temp_down    = 82;
        preferred.temp_up      = 75;
    }
};

static Config g_cfg;
static std::string g_config_path; // --config lub DEFAULT_CONFIG

// ------------------------------------------------------------------ pomocnicze

static void logf(int level, const char* fmt, ...)
{
    if (level > g_cfg.verbosity) return;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char ts[32];
    std::strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    std::printf("[%s.%03ld] ", ts, ms.count());
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

static volatile std::sig_atomic_t g_stop = 0;
static volatile std::sig_atomic_t g_reload = 0;
static void on_term(int) { g_stop = 1; }
static void on_hup(int)  { g_reload = 1; }

static int write_file(const char* path, const std::string& data)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, data.data(), data.size());
    close(fd);
    return n == (ssize_t)data.size() ? 0 : -1;
}

static int read_file(const char* path, std::string& out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[4096];
    ssize_t n;
    out.clear();
    while ((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, n);
    close(fd);
    return n == 0 ? 0 : -1;
}

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ------------------------------------------------------------------ config parser
// Format:
//   [preferred]
//   chromium
//   firefox
//
//   [profile default]
//   max-pstate = 07
//   temp-down = 67
//
//   [profile preferred]
//   max-pstate = 0e
//   boost-pstate = 0f
//   busy-boost = 80
//   boost-dwell-ms = 5000
//   temp-down = 80
//   temp-up = 70
//
// Sekcje nie-Profile mogą zawierać też klucze ogólne (interval-ms, busy-up, ...).

static int parse_state(const std::string& v)
{
    return (int)std::strtol(v.c_str(), nullptr, 16);
}
static int parse_int(const std::string& v)
{
    return std::atoi(v.c_str());
}

static bool load_config(const std::string& path, Config& cfg)
{
    std::string content;
    if (read_file(path.c_str(), content) != 0) return false;

    std::string section;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line = content.substr(pos, (eol == std::string::npos
            ? content.size() - pos : eol - pos));
        pos = (eol == std::string::npos) ? content.size() : eol + 1;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '[' && t.back() == ']') {
            section = t.substr(1, t.size() - 2);
            continue;
        }
        if (section == "preferred") {
            cfg.preferred_classes.insert(t);
            continue;
        }
        // Sekcje [profile default] / [profile preferred] oraz ew. [global].
        size_t eq = t.find('=');
        std::string key, val;
        if (eq == std::string::npos) { key = t; val = ""; }
        else { key = trim(t.substr(0, eq)); val = trim(t.substr(eq + 1)); }

        Profile* prof = nullptr;
        if (section == "profile default")   prof = &cfg.def;
        else if (section == "profile preferred") prof = &cfg.preferred;
        else if (section == "global" || section.empty()) {
            // klucze ogólne — obsłuż poniżej
        } else {
            continue; // nieznana sekcja — ignoruj
        }

        if (prof) {
            if      (key == "max-pstate")     prof->max_pstate   = parse_state(val);
            else if (key == "boost-pstate")   prof->boost_pstate = parse_state(val);
            else if (key == "busy-boost")     prof->busy_boost   = parse_int(val);
            else if (key == "boost-dwell-ms") prof->boost_dwell_ms = parse_int(val);
            else if (key == "boost-hyst")     prof->boost_hyst   = parse_int(val);
            else if (key == "temp-down")      prof->temp_down    = parse_int(val);
            else if (key == "temp-up")        prof->temp_up      = parse_int(val);
            continue;
        }
        // globalne klucze
        if      (key == "interval-ms")       cfg.interval_ms       = parse_int(val);
        else if (key == "busy-up")           cfg.busy_up           = parse_int(val);
        else if (key == "busy-down")         cfg.busy_down         = parse_int(val);
        else if (key == "temp-dwell-ms")     cfg.temp_dwell_ms     = parse_int(val);
        else if (key == "idle-dwell-ms")     cfg.idle_dwell_ms     = parse_int(val);
        else if (key == "win-ms")            cfg.win_ms            = parse_int(val);
        else if (key == "profile-dwell-ms")  cfg.profile_dwell_ms  = parse_int(val);
        else if (key == "poll-ms")           cfg.poll_ms           = parse_int(val);
        else if (key == "exit-state")        cfg.exit_state        = parse_state(val);
        else if (key == "vblank-sync")       cfg.vblank_sync       = (val == "1" || val == "true" || val == "yes");
    }
    return true;
}

// ---------------------------------------------------------------- HW dostęp (GPU)

class Gpu {
public:
    Gpu() : map_(MAP_FAILED) {}
    ~Gpu() { if (map_ != MAP_FAILED) munmap(map_, MAP_SIZE); }

    bool open_mmio()
    {
        int fd = open(PCI_RESOURCE, O_RDWR | O_SYNC);
        if (fd < 0) { std::perror("open resource0"); return false; }
        map_ = mmap(nullptr, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MAP_BASE);
        close(fd);
        if (map_ == MAP_FAILED) { std::perror("mmap BAR0"); return false; }
        return true;
    }

    uint32_t rd(uint32_t reg) const
    {
        return *reinterpret_cast<volatile uint32_t*>(
            static_cast<char*>(map_) + (reg - MAP_BASE));
    }
    void wr(uint32_t reg, uint32_t val)
    {
        *reinterpret_cast<volatile uint32_t*>(
            static_cast<char*>(map_) + (reg - MAP_BASE)) = val;
    }

    void init_counters()
    {
        wr(R_IDLE_INTR_EN, 0);
        wr(R_IDLE_THRESHOLD + C_TOTAL * 4, 0x7FFFFFFF);

        uint32_t v = rd(R_IDLE_CTRL + C_TOTAL * 16);
        v &= ~(CTRL_VALUE_MASK | CTRL_FILTER_MASK);
        v |= CTRL_VALUE_ALWAYS;
        wr(R_IDLE_CTRL + C_TOTAL * 16, v);

        wr(R_IDLE_MASK + C_BUSY * 16, MASK_GR | MASK_CE2);

        v = rd(R_IDLE_CTRL + C_BUSY * 16);
        v &= ~(CTRL_VALUE_MASK | CTRL_FILTER_MASK);
        v |= CTRL_VALUE_BUSY;
        wr(R_IDLE_CTRL + C_BUSY * 16, v);
    }

    uint32_t sample() // 0..1000 ‰
    {
        uint32_t busy  = rd(R_IDLE_COUNT + C_BUSY * 16) & COUNT_MASK;
        uint32_t total = rd(R_IDLE_COUNT + C_TOTAL * 16) & COUNT_MASK;
        uint32_t st    = rd(R_IDLE_INTR_ST) & 1;

        wr(R_IDLE_COUNT + C_BUSY * 16, COUNT_RESET);
        wr(R_IDLE_COUNT + C_TOTAL * 16, COUNT_RESET);

        if (st) { wr(R_IDLE_INTR_ST, 1); return 1000; }
        if (total == 0 || busy > total) return 1000;
        return static_cast<uint32_t>(
            static_cast<uint64_t>(busy) * 1000 / total);
    }

    void dump_raw()
    {
        std::printf("raw: busy=%08x total=%08x ctrl_busy=%08x ctrl_total=%08x mask_busy=%08x\n",
                    rd(R_IDLE_COUNT + C_BUSY * 16),
                    rd(R_IDLE_COUNT + C_TOTAL * 16),
                    rd(R_IDLE_CTRL + C_BUSY * 16),
                    rd(R_IDLE_CTRL + C_TOTAL * 16),
                    rd(R_IDLE_MASK + C_BUSY * 16));
    }

private:
    static constexpr uint32_t MAP_BASE = 0x10A000;
    static constexpr size_t   MAP_SIZE = 0x2000;
    void* map_;
};

// ----------------------------------------------------------- hwmon (temp)

class Hwmon {
public:
    bool init()
    {
        for (int i = 0; i < 64; i++) {
            std::string p = std::string(HWMON_DIR) + "/hwmon" + std::to_string(i);
            std::string name;
            if (read_file((p + "/name").c_str(), name) != 0) continue;
            if (name.find("nouveau") == std::string::npos) continue;
            path_ = p + "/temp1_input";
            fd_ = open(path_.c_str(), O_RDONLY);
            if (fd_ < 0) { std::perror(("open " + path_).c_str()); return false; }
            return true;
        }
        return false;
    }
    ~Hwmon() { if (fd_ >= 0) close(fd_); }

    int read_temp() const // °C, -1 gdy brak
    {
        if (fd_ < 0) return -1;
        if (lseek(fd_, 0, SEEK_SET) < 0) return -1;
        char buf[32];
        ssize_t n = read(fd_, buf, sizeof buf - 1);
        if (n <= 0) return -1;
        buf[n] = 0;
        return std::atoi(buf) / 1000;
    }
    const std::string& path() const { return path_; }
    bool ok() const { return fd_ >= 0; }

private:
    std::string path_;
    int fd_ = -1;
};

static int set_pstate(uint32_t st)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02x", st);
    return write_file(PSTATE_FILE, buf);
}

static std::string current_state()
{
    std::string content;
    if (read_file(PSTATE_FILE, content) != 0) return "";
    size_t star = content.find('*');
    if (star == std::string::npos) return "boot";
    size_t sol = content.rfind('\n', star);
    size_t colon = content.find(':', sol + 1);
    return content.substr(sol + 1, colon - sol - 1);
}

static volatile int g_cur_idx = -1;

// ----------------------------------------------------------- sliding window

struct Ring {
    std::vector<uint32_t> buf;
    size_t cap = 0, head = 0, count = 0;
    uint64_t sum = 0;
    void resize(size_t c) { cap = c ? c : 1; buf.assign(cap, 0); head = count = 0; sum = 0; }
    void clear() { head = count = 0; sum = 0; }
    void push(uint32_t v) {
        if (count < cap) { buf[head] = v; sum += v; head = (head + 1) % cap; count++; }
        else { sum -= buf[head]; sum += v; buf[head] = v; head = (head + 1) % cap; }
    }
    uint32_t avg() const { return count ? (uint32_t)(sum / count) : 0; }
};

// ----------------------------------------------------------- vblank sync

static int g_drm_fd = -1;
static bool drm_open()
{
    g_drm_fd = open(DRM_CARD, O_RDWR | O_CLOEXEC);
    if (g_drm_fd < 0) return false;
    // Zrzuć KMS master natychmiast po open(). Pierwszy opener card0 zostaje
    // domyślnym DRM masterem; bez tego reclockd blokuje Hyprlandowi przejęcie
    // card0 przez libseat/logind (EBUSY -> "Found no gpus" -> crash -> black
    // screen). Vblank (_DRM_VBLANK_RELATIVE) działa bez mastera — dowód:
    // reclockd współistniał z Hyprlandem gdy fbcon był masterem. Błąd DROP_MASTER
    // (EINVAL/ENODEV) gdy nie jesteśmy masterem jest oczekiwany i ignorowany.
    if (ioctl(g_drm_fd, DRM_IOCTL_DROP_MASTER, 0) < 0 && errno != EINVAL && errno != ENODEV)
        logf(0, "drm: DROP_MASTER błąd (%s)", std::strerror(errno));
    return true;
}
static void drm_vblank_wait()
{
    if (g_drm_fd < 0) return;
    union drm_wait_vblank v{};
    v.request.type     = _DRM_VBLANK_RELATIVE;
    v.request.sequence = 1;
    if (ioctl(g_drm_fd, DRM_IOCTL_WAIT_VBLANK, &v) < 0)
        logf(0, "vblank: ioctl WAIT_VBLANK błąd (%s)", std::strerror(errno));
}

// ----------------------------------------------------------- hyprctl (JSON)

// Prosty ekstraktor pól tekstowych z JSON (bez pełnego parsera; hyprctl daje
// dobrze uformowany JSON). Znajduje pierwsze wystąpienie "key" i zwraca wartość
// string po nim.
static bool json_str(const std::string& json, const std::string& key, std::string& out)
{
    std::string pat = "\"" + key + "\"";
    size_t k = json.find(pat);
    if (k == std::string::npos) return false;
    k = json.find(':', k + pat.size());
    if (k == std::string::npos) return false;
    k = json.find('"', k + 1);
    if (k == std::string::npos) return false;
    size_t end = json.find('"', k + 1);
    if (end == std::string::npos) return false;
    out = json.substr(k + 1, end - k - 1);
    return true;
}

static void json_str_all(const std::string& json, const std::string& key,
                         std::vector<std::string>& out)
{
    std::string pat = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pat, pos)) != std::string::npos) {
        size_t k = json.find(':', pos + pat.size());
        if (k == std::string::npos) break;
        k = json.find('"', k + 1);
        if (k == std::string::npos) break;
        size_t end = json.find('"', k + 1);
        if (end == std::string::npos) break;
        out.push_back(json.substr(k + 1, end - k - 1));
        pos = end + 1;
    }
}

// Uruchom hyprctl -j <cmd> i zwróć stdout. Zwraca false gdy niedostępne.
static bool hyprctl_json(const std::string& cmd, std::string& out)
{
    std::string full = std::string("hyprctl -j ") + cmd + " 2>/dev/null";
    FILE* p = popen(full.c_str(), "r");
    if (!p) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    int st = pclose(p);
    if (st != 0 || out.empty()) return false;
    return true;
}

// ----------------------------------------------------------- HyprCtl: detekcja

class HyprCtl {
public:
    // Znajdź instancję Hyprlanda: skan /run/user/<uid>/hypr/<his>/hyprland.lock.
    // Ustaw env XDG_RUNTIME_DIR + HYPRLAND_INSTANCE_SIGNATURE żeby hyprctl
    // (uruchamiany jako root przez popen) połączył się z socketem usera.
    // Socket .socket.sock ma mode srwxr-xr-x — root może się połączyć.
    bool detect()
    {
        DIR* run = opendir("/run/user");
        if (!run) return false;
        struct dirent* e;
        while ((e = readdir(run))) {
            if (e->d_name[0] == '.') continue;
            std::string base = std::string("/run/user/") + e->d_name + "/hypr";
            DIR* hd = opendir(base.c_str());
            if (!hd) continue;
            struct dirent* he;
            while ((he = readdir(hd))) {
                if (he->d_name[0] == '.') continue;
                std::string lock = base + "/" + he->d_name + "/hyprland.lock";
                struct stat st;
                if (stat(lock.c_str(), &st) == 0) {
                    uid_ = std::atoi(e->d_name);
                    his_ = he->d_name;
                    closedir(hd);
                    closedir(run);
                    apply_env();
                    return true;
                }
            }
            closedir(hd);
        }
        closedir(run);
        return false;
    }

    void apply_env()
    {
        std::string xdg = "/run/user/" + std::to_string(uid_);
        setenv("XDG_RUNTIME_DIR", xdg.c_str(), 1);
        setenv("HYPRLAND_INSTANCE_SIGNATURE", his_.c_str(), 1);
    }

    // Aktywne okno (focus). Zwraca false gdy brak (tty / hyprctl niedostępne).
    bool activewindow(std::string& cls)
    {
        std::string json;
        if (!hyprctl_json("activewindow", json)) return false;
        if (json.find("\"class\"") == std::string::npos) return false; // puste
        return json_str(json, "class", cls);
    }

    // Wszystkie uruchomione clienci (class). Zwraca false gdy niedostępne.
    bool clients(std::vector<std::string>& classes)
    {
        std::string json;
        if (!hyprctl_json("clients", json)) return false;
        json_str_all(json, "class", classes);
        return true;
    }

    int uid() const { return uid_; }
    const std::string& his() const { return his_; }

private:
    int uid_ = -1;
    std::string his_;
};

// ----------------------------------------------------------- override flag-file

static bool override_active()
{
    struct stat st;
    return stat(OVERRIDE_FILE, &st) == 0;
}

static std::string override_content()
{
    std::string c;
    read_file(OVERRIDE_FILE, c);
    return trim(c);
}

// ----------------------------------------------------------- użycie

static void usage(const char* argv0)
{
    std::printf(
        "reclockd v4 — polityka profilowa (app-aware) + override + reload\n"
        "Użycie: %s [opcje]\n"
        "  Profile: default (cap 07) ↔ preferred (cap 0e).\n"
        "  Bezpieczna drabinka AUTO: 07 ↔ 0a ↔ 0e. 0f = BOOST TIER nad drabinką\n"
        "  (wchodzi z 0e przy sustained busy>busy-boost AND temp<temp-up).\n"
        "  Straż termiczna PER-PROFIL: default 65/58, preferred 82/75.\n"
        "  Profil z hyprctl (activewindow + clients), lista class z configu.\n"
        "  UP 07→0a→0e (UP-LOAD): g_cur_idx<ceiling AND temp<temp_up przez\n"
        "                        temp_dwell AND busy>busy_up. Jeden poziom/krok.\n"
        "  BOOST 0e→0f: g_cur_idx==ceiling AND busy>busy_boost przez boost_dwell\n"
        "              AND temp<temp_up przez temp_dwell.\n"
        "  DOWN 0f→0e: temp>temp_up LUB temp>=temp_down (NATYCHMIAST) OR busy<busy_up.\n"
        "  DOWN 0e→0a→07: TERMAL (temp>temp_down przez temp_dwell) OR IDLE\n"
        "                 (busy≤busy_down przez idle_dwell) OR CEILING.\n"
        "  --config PATH      plik konfigu (domyślnie /etc/reclockd.conf)\n"
        "  --interval MS      okres próbkowania (200)\n"
        "  --poll-ms MS       okres pollingu hyprctl (1000)\n"
        "  --busy-up P        %% busy > → UP o 1 poziom (80)\n"
        "  --busy-down P      %% busy ≤ → idle dwell (40)\n"
        "  --busy-boost P     %% busy > → BOOST UP 0e→0f (85)\n"
        "  --boost-dwell-ms MS dwell sustained busy>busy-boost do wejścia 0f (5000)\n"
        "  --boost-hyst P     pp rezerwa histerezy boost (10)\n"
        "  --temp-up C        °C UP/BOOST dozwolony (default 58, preferred 75)\n"
        "  --temp-down C      °C DOWN TERMAL throttle (default 65, preferred 82)\n"
        "  --temp-dwell-ms MS dwell temperatury (5000)\n"
        "  --idle-dwell-ms MS dwell bezczynności (5000)\n"
        "  --profile-dwell-ms MS rate-limit zmiany profilu (2000)\n"
        "  --win-ms MS        okno wygładzania busy (1000)\n"
        "  --exit-state S     pstate przy wyjściu (07)\n"
        "  --vblank-sync / --no-vblank-sync  (domyślnie on)\n"
        "  --probe            20 próbek busy, bez zmian\n"
        "  --dry-run          decyzje bez zapisu pstate\n"
        "  -v                 więcej logów\n",
        argv0);
}

// ------------------------------------------------------------------- główna

int main(int argc, char** argv)
{
    enum { OPT_CONFIG = 1000, OPT_INTERVAL, OPT_POLL, OPT_BUSY_UP, OPT_BUSY_DOWN,
           OPT_BUSY_BOOST, OPT_BOOST_DWELL, OPT_BOOST_HYST,
           OPT_TEMP_UP, OPT_TEMP_DOWN, OPT_TEMP_DWELL, OPT_IDLE_DWELL,
           OPT_PROFILE_DWELL, OPT_WIN, OPT_EXIT, OPT_PROBE, OPT_DRY };
    static const option long_opts[] = {
        {"config",          required_argument, nullptr, OPT_CONFIG},
        {"interval",        required_argument, nullptr, OPT_INTERVAL},
        {"poll-ms",         required_argument, nullptr, OPT_POLL},
        {"busy-up",         required_argument, nullptr, OPT_BUSY_UP},
        {"busy-down",       required_argument, nullptr, OPT_BUSY_DOWN},
        {"busy-boost",      required_argument, nullptr, OPT_BUSY_BOOST},
        {"boost-dwell-ms",  required_argument, nullptr, OPT_BOOST_DWELL},
        {"boost-hyst",      required_argument, nullptr, OPT_BOOST_HYST},
        {"temp-up",         required_argument, nullptr, OPT_TEMP_UP},
        {"temp-down",       required_argument, nullptr, OPT_TEMP_DOWN},
        {"temp-dwell-ms",   required_argument, nullptr, OPT_TEMP_DWELL},
        {"idle-dwell-ms",   required_argument, nullptr, OPT_IDLE_DWELL},
        {"profile-dwell-ms",required_argument, nullptr, OPT_PROFILE_DWELL},
        {"win-ms",          required_argument, nullptr, OPT_WIN},
        {"exit-state",      required_argument, nullptr, OPT_EXIT},
        {"probe",           no_argument,       nullptr, OPT_PROBE},
        {"dry-run",         no_argument,       nullptr, OPT_DRY},
        {"vblank-sync",     no_argument,       nullptr, 'V'},
        {"no-vblank-sync",  no_argument,       nullptr, 'N'},
        {"verbose",         no_argument,       nullptr, 'v'},
        {"help",            no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    // Domyślne wartości v3 w g_cfg już ustawione (def struct).
    // Najpierw parsuj CLI żeby dostać --config, potem load_config, potem
    // re-aplikuj CLI override. Trzymaj CLI wartości w osobnych polach.
    struct CliVal {
        bool set=false; int v=0;
    } cli_interval, cli_poll, cli_busy_up, cli_busy_down, cli_busy_boost,
      cli_boost_dwell, cli_boost_hyst, cli_temp_up, cli_temp_down,
      cli_temp_dwell, cli_idle_dwell, cli_profile_dwell, cli_win, cli_exit;
    bool cli_vblank = false, cli_vblank_val = true;

    int c;
    while ((c = getopt_long(argc, argv, "vh", long_opts, nullptr)) != -1) {
        switch (c) {
        case OPT_CONFIG:       g_config_path = optarg; break;
        case OPT_INTERVAL:     cli_interval.set=true;       cli_interval.v=std::atoi(optarg); break;
        case OPT_POLL:         cli_poll.set=true;           cli_poll.v=std::atoi(optarg); break;
        case OPT_BUSY_UP:      cli_busy_up.set=true;        cli_busy_up.v=std::atoi(optarg); break;
        case OPT_BUSY_DOWN:    cli_busy_down.set=true;      cli_busy_down.v=std::atoi(optarg); break;
        case OPT_BUSY_BOOST:   cli_busy_boost.set=true;     cli_busy_boost.v=std::atoi(optarg); break;
        case OPT_BOOST_DWELL:  cli_boost_dwell.set=true;    cli_boost_dwell.v=std::atoi(optarg); break;
        case OPT_BOOST_HYST:   cli_boost_hyst.set=true;     cli_boost_hyst.v=std::atoi(optarg); break;
        case OPT_TEMP_UP:      cli_temp_up.set=true;        cli_temp_up.v=std::atoi(optarg); break;
        case OPT_TEMP_DOWN:    cli_temp_down.set=true;      cli_temp_down.v=std::atoi(optarg); break;
        case OPT_TEMP_DWELL:   cli_temp_dwell.set=true;     cli_temp_dwell.v=std::atoi(optarg); break;
        case OPT_IDLE_DWELL:   cli_idle_dwell.set=true;     cli_idle_dwell.v=std::atoi(optarg); break;
        case OPT_PROFILE_DWELL:cli_profile_dwell.set=true;  cli_profile_dwell.v=std::atoi(optarg); break;
        case OPT_WIN:          cli_win.set=true;            cli_win.v=std::atoi(optarg); break;
        case OPT_EXIT:         cli_exit.set=true;           cli_exit.v=(int)std::strtol(optarg,nullptr,16); break;
        case OPT_PROBE:        g_cfg.probe = true; break;
        case OPT_DRY:          g_cfg.dry = true; break;
        case 'V':              cli_vblank=true; cli_vblank_val=true;  break;
        case 'N':              cli_vblank=true; cli_vblank_val=false; break;
        case 'v':              g_cfg.verbosity++; break;
        case 'h':              usage(argv[0]); return 0;
        default:               usage(argv[0]); return 2;
        }
    }

    if (geteuid() != 0) {
        std::fprintf(stderr, "reclockd: wymaga roota (mmap BAR0 + zapis debugfs)\n");
        return 1;
    }

    // Załaduj config (jeśli istnieje). Domyślne wartości już w g_cfg.
    if (g_config_path.empty()) g_config_path = DEFAULT_CONFIG;
    bool cfg_loaded = load_config(g_config_path, g_cfg);
    if (cfg_loaded)
        logf(1, "config załadowany: %s", g_config_path.c_str());
    else
        logf(1, "config %s niedostępny — używam wbudowanych defaultów",
             g_config_path.c_str());

    // Aplikuj CLI overrides (nadpisują config).
    if (cli_interval.set)       g_cfg.interval_ms      = cli_interval.v;
    if (cli_poll.set)           g_cfg.poll_ms          = cli_poll.v;
    if (cli_busy_up.set)        g_cfg.busy_up          = cli_busy_up.v;
    if (cli_busy_down.set)      g_cfg.busy_down        = cli_busy_down.v;
    if (cli_busy_boost.set)     { g_cfg.def.busy_boost = g_cfg.preferred.busy_boost = cli_busy_boost.v; }
    if (cli_boost_dwell.set)    { g_cfg.def.boost_dwell_ms = g_cfg.preferred.boost_dwell_ms = cli_boost_dwell.v; }
    if (cli_boost_hyst.set)     { g_cfg.def.boost_hyst = g_cfg.preferred.boost_hyst = cli_boost_hyst.v; }
    if (cli_temp_up.set)        { g_cfg.def.temp_up = g_cfg.preferred.temp_up = cli_temp_up.v; }
    if (cli_temp_down.set)      { g_cfg.def.temp_down = g_cfg.preferred.temp_down = cli_temp_down.v; }
    if (cli_temp_dwell.set)     g_cfg.temp_dwell_ms    = cli_temp_dwell.v;
    if (cli_idle_dwell.set)     g_cfg.idle_dwell_ms    = cli_idle_dwell.v;
    if (cli_profile_dwell.set)  g_cfg.profile_dwell_ms = cli_profile_dwell.v;
    if (cli_win.set)            g_cfg.win_ms           = cli_win.v;
    if (cli_exit.set)           g_cfg.exit_state       = cli_exit.v;
    if (cli_vblank)             g_cfg.vblank_sync      = cli_vblank_val;

    // Walidacja stanów w profilach.
    auto prof_ok = [](const Profile& p) {
        if (!known_state(p.max_pstate)) return false;
        if (p.boost_pstate >= 0 && !known_state(p.boost_pstate)) return false;
        // boost musi być POZA drabinką (off-ladder, state_to_idx == -1) i wyższy
        // niż max_pstate (np. 0f gdy max=0e). On-ladder boost <= max = błąd.
        if (p.boost_pstate >= 0) {
            int bi = state_to_idx(p.boost_pstate);
            if (bi >= 0 && bi <= state_to_idx(p.max_pstate)) return false;
        }
        return true;
    };
    if (!prof_ok(g_cfg.def) || !prof_ok(g_cfg.preferred)) {
        std::fprintf(stderr, "reclockd: niepoprawne stany w profilach configu\n");
        return 2;
    }
    if (!known_state(g_cfg.exit_state)) {
        std::fprintf(stderr, "reclockd: exit-state musi być 07|0a|0e|0f\n");
        return 2;
    }
    if (g_cfg.interval_ms <= 0) g_cfg.interval_ms = 200;
    if (g_cfg.poll_ms < g_cfg.interval_ms) g_cfg.poll_ms = g_cfg.interval_ms;

    Gpu gpu;
    if (!gpu.open_mmio()) return 1;

    if (g_cfg.probe) {
        gpu.dump_raw();
        gpu.init_counters();
        std::printf("probe: 20 próbek co %d ms (busy w %%) + temp\n", g_cfg.interval_ms);
        Hwmon hw; bool hok = hw.init();
        for (int i = 0; i < 20; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(g_cfg.interval_ms));
            uint32_t b = gpu.sample();
            std::printf("  próbka %2d: busy=%5.1f%% temp=%d°C\n", i + 1,
                        b / 10.0, hok ? hw.read_temp() : -1);
            std::fflush(stdout);
        }
        return 0;
    }

    Hwmon hw;
    bool hw_ok = hw.init();
    if (!hw_ok)
        logf(0, "UWAGA: hwmon nouveau niedostępny — warunki termalne pominięte (fail-safe)");

    std::string old_pm;
    bool pm_saved = read_file(POWER_CTRL, old_pm) == 0;
    if (pm_saved) write_file(POWER_CTRL, "on");

    auto restore = [&]() {
        if (!g_cfg.dry) {
            if (set_pstate((uint32_t)g_cfg.exit_state) == 0)
                logf(1, "wyjście: pstate -> %s", state_hex(g_cfg.exit_state));
        }
        if (pm_saved) write_file(POWER_CTRL, old_pm);
    };
    std::signal(SIGINT, on_term);
    std::signal(SIGTERM, on_term);
    std::signal(SIGHUP, on_hup);

    gpu.init_counters();
    if (g_cfg.vblank_sync) drm_open();

    // Wykryj Hyprlanda (socket usera). Brak → fallback do default.
    HyprCtl hypr;
    bool hypr_ok = hypr.detect();
    if (hypr_ok)
        logf(1, "hyprctl: uid=%d HIS=%s", hypr.uid(), hypr.his().c_str());
    else
        logf(0, "UWAGA: Hyprland niewykryty (/run/user/*/hypr/*/hyprland.lock) — "
                "fallback do profilu default (cap 07)");

    // Inicjalizuj bieżący indeks drabinki.
    g_cur_idx = -1;
    std::string cur = current_state();
    if (!cur.empty() && cur != "boot") {
        uint32_t cs = (uint32_t)std::strtol(cur.c_str(), nullptr, 16);
        g_cur_idx = state_to_idx(cs);
    }

    Ring ring;
    ring.resize((size_t)(g_cfg.win_ms / std::max(g_cfg.interval_ms, 1)));

    // Dwell-countery.
    int temp_low_dwell  = 0;
    int temp_high_dwell = 0;
    int idle_dwell      = 0;
    int boost_up_dwell  = 0; // busy > busy_boost (BOOST UP 0e→0f)

    // Boost tier: gdy true, GPU jest na 0f (off-ladder), g_cur_idx zostaje na
    // ceiling (0e). Każda zmiana boost resetuje dwell-countery.
    bool g_boost_active = false;

    auto reset_after_transition = [&]() {
        ring.clear();
        temp_low_dwell = temp_high_dwell = idle_dwell = 0;
        boost_up_dwell = 0;
    };

    // Stan profilu + rate-limit.
    bool pref_active = false;
    int pref_dwell = 0, def_dwell = 0;

    // Override stan (loguj wejście/wyjście).
    bool prev_override = false;

    // Polling hyprctl: co poll_ms (licznik cykli).
    int poll_cycles = std::max(1, g_cfg.poll_ms / g_cfg.interval_ms);
    int cycle = 0;
    std::string focused_class;
    std::vector<std::string> running_classes;
    bool hypr_alive = hypr_ok;

    logf(1, "start v4: interval=%dms poll=%dms, "
            "default[cap=%s temp-up=%d temp-down=%d], "
            "preferred[cap=%s boost=%s busy-boost=%d%% boost-dwell=%dms "
            "temp-up=%d temp-down=%d], "
            "busy-up=%d%% busy-down=%d%%, temp-dwell=%dms idle-dwell=%dms, "
            "profile-dwell=%dms, hwmon=%s, hypr=%s, vblank=%d, stan=%s",
         g_cfg.interval_ms, g_cfg.poll_ms,
         state_hex(g_cfg.def.max_pstate), g_cfg.def.temp_up, g_cfg.def.temp_down,
         state_hex(g_cfg.preferred.max_pstate),
         g_cfg.preferred.boost_pstate >= 0 ? state_hex(g_cfg.preferred.boost_pstate) : "-",
         g_cfg.preferred.busy_boost, g_cfg.preferred.boost_dwell_ms,
         g_cfg.preferred.temp_up, g_cfg.preferred.temp_down,
         g_cfg.busy_up, g_cfg.busy_down, g_cfg.temp_dwell_ms, g_cfg.idle_dwell_ms,
         g_cfg.profile_dwell_ms,
         hw_ok ? hw.path().c_str() : "BRAK",
         hypr_ok ? "tak" : "nie", g_cfg.vblank_sync ? 1 : 0,
         cur.c_str());

    const int busy_up_pp   = g_cfg.busy_up   * 10;
    const int busy_down_pp = g_cfg.busy_down * 10;

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(g_cfg.interval_ms));
        if (g_reload) {
            g_reload = 0;
            Config nc;
            // Zachowaj CLI-only flagi (probe/dry/verbosity) i config_path.
            nc.probe = g_cfg.probe; nc.dry = g_cfg.dry; nc.verbosity = g_cfg.verbosity;
            if (load_config(g_config_path, nc)) {
                // Re-aplikuj CLI overrides.
                if (cli_busy_boost.set)  { nc.def.busy_boost = nc.preferred.busy_boost = cli_busy_boost.v; }
                if (cli_boost_dwell.set) { nc.def.boost_dwell_ms = nc.preferred.boost_dwell_ms = cli_boost_dwell.v; }
                if (cli_boost_hyst.set)  { nc.def.boost_hyst = nc.preferred.boost_hyst = cli_boost_hyst.v; }
                if (cli_temp_up.set)     { nc.def.temp_up = nc.preferred.temp_up = cli_temp_up.v; }
                if (cli_temp_down.set)   { nc.def.temp_down = nc.preferred.temp_down = cli_temp_down.v; }
                if (cli_busy_up.set)     nc.busy_up = cli_busy_up.v;
                if (cli_busy_down.set)   nc.busy_down = cli_busy_down.v;
                if (cli_temp_dwell.set)  nc.temp_dwell_ms = cli_temp_dwell.v;
                if (cli_idle_dwell.set)  nc.idle_dwell_ms = cli_idle_dwell.v;
                if (cli_profile_dwell.set) nc.profile_dwell_ms = cli_profile_dwell.v;
                if (cli_interval.set)    nc.interval_ms = cli_interval.v;
                if (cli_poll.set)        nc.poll_ms = cli_poll.v;
                if (cli_win.set)         nc.win_ms = cli_win.v;
                if (cli_exit.set)        nc.exit_state = cli_exit.v;
                if (cli_vblank)          nc.vblank_sync = cli_vblank_val;
                g_cfg = nc;
                logf(1, "SIGHUP: config przeładowany (%s)", g_config_path.c_str());
            } else {
                logf(0, "SIGHUP: błąd reloadu configu — zostaję przy starym");
            }
        }

        uint32_t b = gpu.sample();
        ring.push(b);
        uint32_t busy_avg = ring.avg(); // ‰
        int temp = hw.read_temp();

        // Override flag-file — zamraża auto.
        bool ov = override_active();
        if (ov && !prev_override) {
            std::string oc = override_content();
            logf(1, "override AKTYWNY: hold (flag=%s)", oc.empty() ? "?" : oc.c_str());
        }
        if (!ov && prev_override) {
            logf(1, "override ZDJĘTY — wznawiam auto");
            // Re-synchronizuj bieżący stan z debugfs.
            std::string cs = current_state();
            if (!cs.empty() && cs != "boot") {
                uint32_t v = (uint32_t)std::strtol(cs.c_str(), nullptr, 16);
                int idx = state_to_idx(v);
                if (idx >= 0) g_cur_idx = idx;
            }
            reset_after_transition();
        }
        prev_override = ov;
        if (ov) {
            if (g_cfg.verbosity >= 2)
                logf(2, "override hold, busy=%.0f%%, temp=%d°C", busy_avg/10.0f, temp);
            continue;
        }

        // Polling hyprctl co poll_cycles.
        if (hypr_alive && (cycle % poll_cycles) == 0) {
            std::string fcs;
            std::vector<std::string> rcs;
            bool a1 = hypr.activewindow(fcs);
            bool a2 = hypr.clients(rcs);
            if (a1) focused_class = fcs; else focused_class.clear();
            if (a2) running_classes = rcs; else running_classes.clear();
            // Jeśli oba zawiodły — może Hyprland zrestartowany. Re-detect.
            if (!a1 && !a2) {
                logf(0, "hyprctl niedostępny — re-detect instancji");
                if (hypr.detect()) {
                    hypr.apply_env();
                    logf(1, "hyprctl re-detected: uid=%d HIS=%s",
                         hypr.uid(), hypr.his().c_str());
                } else {
                    hypr_alive = false;
                    logf(0, "Hyprland zniknął — fallback do default");
                }
            }
        }
        cycle++;

        // Sygnał preferred.
        bool pref_sig = false;
        if (hypr_alive) {
            // focused ∈ lista?
            if (!focused_class.empty() &&
                g_cfg.preferred_classes.count(focused_class))
                pref_sig = true;
            // running ∈ lista AND busy > busy_up?
            if (!pref_sig && (int)busy_avg > busy_up_pp) {
                for (auto& r : running_classes) {
                    if (g_cfg.preferred_classes.count(r)) { pref_sig = true; break; }
                }
            }
        }
        if (pref_sig) { pref_dwell += g_cfg.interval_ms; def_dwell = 0; }
        else          { def_dwell += g_cfg.interval_ms; pref_dwell = 0; }

        if (!pref_active && pref_dwell >= g_cfg.profile_dwell_ms) {
            pref_active = true; pref_dwell = 0;
            logf(1, "profil -> PREFERRED (focused=%s, busy=%.0f%%)",
                 focused_class.c_str(), busy_avg/10.0f);
        } else if (pref_active && def_dwell >= g_cfg.profile_dwell_ms) {
            pref_active = false; def_dwell = 0;
            logf(1, "profil -> DEFAULT (def_dwell=%dms)", g_cfg.profile_dwell_ms);
        }

        const Profile& prof = pref_active ? g_cfg.preferred : g_cfg.def;
        int ceiling = state_to_idx(prof.max_pstate);
        int busy_boost_pp = prof.busy_boost * 10;

        // boost aktywny gdy: zdefiniowany, poprawny, OFF-LADDER (state_to_idx<0,
        // czyli 0f) — on-ladder boost traktujemy jako wyłączony (to nie ma sensu
        // jako tier nad drabinką). Prof_ok już to walidował na starcie.
        const bool boost_enabled = (prof.boost_pstate >= 0 &&
                                    known_state(prof.boost_pstate) &&
                                    state_to_idx(prof.boost_pstate) < 0);

        // Dwell-countery.
        if (temp >= 0) {
            temp_low_dwell  = (temp <  prof.temp_up)   ? temp_low_dwell  + g_cfg.interval_ms : 0;
            temp_high_dwell = (temp >  prof.temp_down) ? temp_high_dwell + g_cfg.interval_ms : 0;
        } else {
            temp_low_dwell = temp_high_dwell = 0;
        }
        idle_dwell = ((int)busy_avg <= busy_down_pp)
                     ? idle_dwell + g_cfg.interval_ms : 0;
        boost_up_dwell = ((int)busy_avg >  busy_boost_pp)
                         ? boost_up_dwell + g_cfg.interval_ms : 0;

        // Jeśli bieżący stan poza drabinką (np. 0a, boot) → wymuś przejście
        // na najniższy (07) w następnym kroku decyzji.
        if (g_cur_idx < 0) {
            if (!g_cfg.dry) {
                if (g_cfg.vblank_sync) drm_vblank_wait();
                if (set_pstate(LADDER[0]) == 0) {
                    logf(1, "init: stan spoza drabinki -> %s", state_hex(LADDER[0]));
                    g_cur_idx = 0;
                    g_boost_active = false;
                    reset_after_transition();
                }
            } else {
                g_cur_idx = 0;
            }
            continue;
        }

        int target = g_cur_idx;
        const char* reason = nullptr;
        bool next_boost = g_boost_active;

        // ---- BOOST TIER (0f) — nad drabinką, straż termiczna priorytetowa ----
        if (g_boost_active) {
            // EXIT boost — TERMAL (NATYCHMIAST, priorytet nad loadem):
            //   temp > temp_up (58°C) LUB temp >= temp_down (65°C) → drop z 0f.
            //   0f = najgorętszy pstate = pierwszy do ścięcia.
            if (temp >= 0 && (temp > prof.temp_up || temp >= prof.temp_down)) {
                next_boost = false;
                target = std::min(ceiling, LADDER_N - 1); // powrót na 0e (lub niżej)
                reason = "BOOST-TERMAL";
            }
            // EXIT boost — CEILING: profil obniżył cap poniżej szczytu drabinki
            // (np. preferred→default). Zejdź z 0f na nowy ceiling.
            else if (g_cur_idx > ceiling) {
                next_boost = false;
                target = std::min(ceiling, LADDER_N - 1);
                reason = "BOOST-CEILING";
            }
            // EXIT boost — LOAD histereza: busy < busy_up (80%) → drop z 0f.
            // Histereza enter@busy_boost(85) / exit@busy_up(80) = pasmo 5 pp.
            else if ((int)busy_avg < busy_up_pp) {
                next_boost = false;
                target = g_cur_idx; // == ceiling, powrót na 0e
                reason = "BOOST-IDLE";
            }
            // else: boost hold — zostajemy na 0f.
        }
        // ENTER boost — tylko gdy: boost_enabled AND na szczycie drabinki (0e)
        // AND temp<temp_up przez temp_dwell AND busy>busy_boost przez boost_dwell
        // AND busy nadal >busy_boost (refresh). Jeden warunek wejścia, brak skip.
        else if (boost_enabled &&
                 g_cur_idx == ceiling &&
                 temp >= 0 && temp_low_dwell >= g_cfg.temp_dwell_ms &&
                 boost_up_dwell >= prof.boost_dwell_ms &&
                 (int)busy_avg > busy_boost_pp) {
            next_boost = true;
            target = g_cur_idx; // g_cur_idx zostaje na ceiling; pstate=0f niżej
            reason = "BOOST-UP";
        }

        // ---- LADDER (drabinka 07↔0a↔0e) — tylko gdy boost niezmieniony ----
        if (!g_boost_active && !next_boost) {
            // DOWN (priorytet: TERMAL > IDLE > CEILING). Jeden poziom na krok.
            // TERMAL działa PER-PROFIL (default 65/58, preferred 82/75) —
            // straż termiczna nie jest skipowana dla non-preferred.
            if (temp >= 0 && temp_high_dwell >= g_cfg.temp_dwell_ms && g_cur_idx > 0) {
                target = g_cur_idx - 1;
                reason = "TERMAL";
            } else if (g_cur_idx > 0 && idle_dwell >= g_cfg.idle_dwell_ms) {
                target = g_cur_idx - 1;
                reason = "IDLE";
            } else if (g_cur_idx > ceiling) {
                // Profil obniżył ceiling poniżej bieżącego — zjedź krok w dół
                // (dwell idle/termal załatwi resztę; skrót gdy ceiling twardo
                // blokuje obecny poziom, np. default cap=07 a jesteśmy na 0a/0e).
                target = g_cur_idx - 1;
                reason = "CEILING";
            }
            // UP (o 1 poziom, nigdy skok 07→0e). Wspólne progi load/temp dla obu
            // kroków 07→0a i 0a→0e.
            else if (g_cur_idx < ceiling &&
                     temp >= 0 && temp_low_dwell >= g_cfg.temp_dwell_ms &&
                     (int)busy_avg > busy_up_pp) {
                target = g_cur_idx + 1;
                reason = "UP-LOAD";
            }
        }

        // ---- WYKONANIE ----
        const bool boost_changed = (next_boost != g_boost_active);
        // Stan źródłowy do logu: 0f jeśli boost aktywny, inaczej LADDER[g_cur_idx].
        auto cur_hex = [&]() -> const char* {
            return g_boost_active ? state_hex((uint32_t)prof.boost_pstate)
                                  : state_hex(LADDER[g_cur_idx]);
        };

        if (boost_changed) {
            uint32_t write_state = next_boost
                ? (uint32_t)prof.boost_pstate            // wejście: 0f
                : LADDER[std::max(0, std::min(target, LADDER_N - 1))]; // wyjście: 0e/lżej
            float busy_pct = busy_avg / 10.0f;
            logf(1, "%s BOOST zmiana pstate: %s -> %s (busy=%.0f%%, temp=%d°C, %s, profil=%s)",
                 g_cfg.dry ? "dry-run:" : ">>",
                 cur_hex(), state_hex(write_state),
                 busy_pct, temp, reason ? reason : "?",
                 pref_active ? "preferred" : "default");
            if (!g_cfg.dry) {
                if (g_cfg.vblank_sync) drm_vblank_wait();
                if (set_pstate(write_state) == 0) {
                    g_boost_active = next_boost;
                    if (!next_boost) g_cur_idx = std::max(0, std::min(target, LADDER_N - 1));
                    // wejście boost: g_cur_idx zostaje na ceiling (0e)
                    reset_after_transition();
                } else {
                    logf(0, "BŁĄD: zapis pstate (boost) nieudany");
                }
            } else {
                g_boost_active = next_boost;
                if (!next_boost) g_cur_idx = std::max(0, std::min(target, LADDER_N - 1));
                reset_after_transition();
            }
        } else if (!next_boost && target != g_cur_idx &&
                   target >= 0 && target < LADDER_N) {
            float busy_pct = busy_avg / 10.0f;
            logf(1, "%s zmiana pstate: %s -> %s (busy=%.0f%%, temp=%d°C, %s, profil=%s)",
                 g_cfg.dry ? "dry-run:" : ">>",
                 state_hex(LADDER[g_cur_idx]), state_hex(LADDER[target]),
                 busy_pct, temp, reason ? reason : "?",
                 pref_active ? "preferred" : "default");
            if (!g_cfg.dry) {
                if (g_cfg.vblank_sync) drm_vblank_wait();
                if (set_pstate(LADDER[target]) == 0) {
                    g_cur_idx = target;
                    reset_after_transition();
                } else {
                    logf(0, "BŁĄD: zapis pstate nieudany");
                }
            } else {
                g_cur_idx = target;
                reset_after_transition();
            }
        } else if (g_cfg.verbosity >= 2) {
            float busy_pct = busy_avg / 10.0f;
            logf(2, "stan %s, busy=%.0f%%, temp=%d°C, profil=%s, ceiling=%d, "
                    "boost=%s, dwell[low=%d hi=%d idle=%d bu=%d] pref=%d",
                 g_boost_active ? state_hex((uint32_t)prof.boost_pstate)
                                : state_hex(LADDER[g_cur_idx]),
                 busy_pct, temp,
                 pref_active ? "pref" : "def", ceiling,
                 g_boost_active ? "ON" : (boost_enabled ? "off" : "-"),
                 temp_low_dwell, temp_high_dwell, idle_dwell,
                 boost_up_dwell, pref_dwell);
        }
    }

    restore();
    logf(1, "koniec.");
    return 0;
}