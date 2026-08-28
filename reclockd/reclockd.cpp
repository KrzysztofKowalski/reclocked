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
//
// v4.1 (2026-08-25):
//   A. BUG-fix detekcji Hyprlanda: detect() jednorazowe przy starcie demona
//      (systemd → przed sesją) zostawiało demona na default cap=07 na zawsze.
//      Teraz cykliczny re-detect co poll_ms gdy !hypr_alive.
//   B. GR-idle gate: DOWN transycje (reclok pamięci w locie) odraczane gdy
//      chwilowy busy > gr-idle-promille (default 300‰). vblank chroni scanout,
//      nie GR mid-render — live DOWN pod renderem = wedge kanału (crash pulpitu).
//      UP-LOAD/BOOST-UP NIE gate'owane (rosnący zegar bezpieczniejszy).
//   C. Detekcja Discord/YouTube po tytule okna ([preferred-titles], case-
//      insensitive) — są kartami w przeglądarce, nie mają własnej klasy.
//   D. [low-power] (terminale): focus terminala → wymusza default cap=07 z
//      priorytetem nad preferred (YouTube w tle nie podbija zegara).
//   E. JSON escape fix w json_str/json_str_all (\" \\ \n \t).
//
// v4.2 (2026-08-25):
//   F. KONTROLA WENTYLATORÓW applesmc: klasa Fan — liniowa krzywa temp→RPM dla
//      obu wentylatorów (fan1=Lewa, fan2=Prawa). Zakresy RPM (fanN_min/max) czytane
//      DYNAMICZNIE z sysfs przy starcie (nie hardkodowane — "wg 2ch wartosci" min/
//      max na wentylator). x = clamp((temp - temp_min) / (temp_max - temp_min), 0,1);
//      rpm = fanN_min + round(x * (fanN_max - fanN_min)). Aktualizacja co poll_ms
//      (1 s). Sekcja [fan]: enable/temp-min(51)/temp-max(91).
//   G. FAN OVERRIDE: flag-file `/run/reclockd/fan-override` zamraża auto wentyl.
//      (użytkownik steruje ręcznie: cusfan.sh / fullfan.sh). reclockctl fan-off/on.
//   H. FAIL-SAFE wentylatorów: restore_auto() przy wyjściu zdejmuje fanN_manual=0
//      → SMC przejmuje auto (nigdy nie zostawiaj wentyl. zablokowanych na manualu).
//      init() fail (brak applesmc / absurdalne zakresy) → fan wyłączony bez błędu.
//
// v4.3 (2026-08-25):
//   I. TITLE-MATCH = NAJWYŻSZY priorytet sygnału preferred. Dopasowanie tytułu
//      okna (Discord/YouTube, sekcja [preferred-titles]) ustawia flagę title_pref,
//      która omija regułę busy>busy_up w UP-LOAD: title-match AND temp<temp_up
//      przez temp_dwell AND poniżej ceiling → UP o 1 poziom BEZ busy-gate (reason
//      UP-TITLE). Powód: Discord/YouTube są memory-bound (GR busy 16-36%, nigdy
//      >80%), więc v4.1/v4.2 utykały na 0a — user nie czuł różnicy. Dodatkowo
//      title_pref SUPPRESSuje IDLE downshift (reason IDLE pominięty gdy title_pref)
//      — apka z focusem trzyma ceiling 0e. TERMAL DOWN pozostaje aktywne (ochrona
//      termiczna > priorytet tytułu). CEILING DOWN pozostaje (low-power terminal
//      z focusem > title-match — patrz low-power gate). UP-TITLE NIE jest gate'owane
//      GR-idle (rosnący zegar bezpieczniejszy).
//
// v4.4 (2026-08-26):
//   J. [caps] — per-klasowa polityka. Składnia: "klasa = floor=0a, max=0e, busy-up=50".
//      floor = stan spoczynkowy (IDLE nie schodzi niżej), max = ceiling (własny),
//      busy-up = własny próg UP-LOAD (%, 0 = globalny busy-up 80). Klasa
//      z wpisem jest preferred (gdy focused) i NIE łapie title-priority.
//      Desktop Discord (Electron "discord" + PWA "chrome-discord.com__channels_@me-Default"):
//      baza 0a, busy>50% → 0e, idle → 0a. TERMAL nadal nadrzędny (zejście
//      nawet poniżej floor). Karta Discord w przeglądarce (chromium/firefox)
//      nie jest w [caps] → title-priority force-0e bez zmian.
//
// v4.5 (2026-08-26):
//   K. SAMOUZDRAWIANIE po S3: po suspend/resume (deep) GPU traci zasilanie i
//      konfigurację liczników busy PMU w BAR0 (R_IDLE_CTRL/R_IDLE_MASK) —
//      liczniki nie zliczają, sample() zwraca stale 1000‰, IDLE downshift i
//      GR-idle gate są zablokowane, daemon zostaje w 0e (raport 63). W pętli
//      głównej readback R_IDLE_CTRL co cykl; gdy (ctrl & CTRL_VALUE_MASK) !=
//      CTRL_VALUE_ALWAYS (konfiguracja stracona — typowo po resume), robi
//      gpu.init_counters() + reset_after_transition() i loguje do journala.
//      Readback jest tani (mmap, co interval_ms) i bez fałszywych alarmów
//      (nie zależy od busy/temp). Współpracuje z hookiem system-sleep
//      (restart na post resume) jako siatka bezpieczeństwa.
//
// v4.6 (2026-08-26):
//   L. KOMPILATOR→FAN 100%: gdy wykryty uruchomiony kompilator
//      (clang/gcc/g++/cc/c++/cc1/cc1plus/make/cmake/ninja/cargo/rustc/... —
//      skan /proc/*/comm z fallbackiem na cmdline), wentylatory na fan-max%
//      (default 100 = pełne wiatraki). Sekcja [compiler]: enable/fan-max/names
//      (names = dodatkowe nazwy przecinkami). Fan.set_boost(pct) pisze
//      manual=1 + output dla obu wentylatorów (liniowa interpolacja między
//      fanN_min a fanN_max). Boost działa TYLKO w auto-mode — fan-override
//      flag-file (/run/reclockd/fan-override) ma priorytet (nie nadpisuje
//      ręcznego sterowania). Gdy kompilator zniknie → powrót do krzywej temp
//      (normalna ścieżka). Detekcja co poll_cycles (1 s), próg — zatrzymaj
//      scan gdy znaleziono.
//
// v5.0 (2026-08-26):
//   M. SWITCHD — moduł dGPU power-state + render routing (Etap 1: monitor w DIS).
//      Topologia (vgaswitcheroo DIS/IGD), power dGPU (manual/runpm), polityka
//      (twarda/miękka promocja, demote, min-residence, cooldown, gate termiczny),
//      wykonawca (power-on/off gated topologią), status /run/reclockd/status +
//      /run/switchd/dgpu, NVRAM read gpu-power-prefs (efivarfs → raw flash),
//      igpu_freq_mhz read-only. W DIS = monitor (zero zmian power). Sekcje
//      [switch]/[dpower]/[dgpu-hard]/[dgpu-soft]/[igpu]/[dgpu-procs].
//   N. DGPU OVERRIDE: flag-file /run/reclockd/dgpu-override (on|off) — wzorzec
//      fan-override (G). reclockctl dgpu-on/dgpu-off/dgpu-auto. Daemon w tick()
//      wymusza target PRZED polityką (decide pominięty). Override "off" nadal
//      przechodzi przez bramki apply(): wait_idle (tylko węzły dGPU — lekcja
//      G1 live) + set_off + wait_off. Status: pole "override" (""|"on"|"off").
//
// v5.1 (2026-08-27):
//   O. FAN CURVE IGPU-ONLY: osobna krzywa wentylatorów gdy włączone jest tylko
//      iGPU (dGPU OFF — w IGD hwmon nouveau znika, temp=CPU/coretemp). Sekcja
//      [fan] zyskuje klucze temp-min-igd/temp-max-igd (domyślnie 41/91°C) —
//      cichsza: wiatraki wchodzą na max dopiero przy 91°C zamiast 67°C. Gdy dGPU
//      ON → krzywa standardowa temp-min/temp-max (51/91). Wybór krzywej wg
//      sw.dgpu_off() (stan power dGPU), niezależny od topologii.
//
// v5.3 (2026-08-28):
//   P. NVRAM CACHE: cache gpu-power-prefs do /run/reclockd/nvram-prefs. Skan
//      raw flash (/dev/mtd0ro) przy każdym starcie gasi kbd backlight (odczyt
//      MTD → ledtrig_mtd_activity → trigger "nand-disk" na smc::kbd_backlight
//      → oneshot blink → LKSB=0; raport 80). /run to tmpfs — cache resetuje
//      się przy reboot (poprawne: prefs zmienia się tylko przez firmware przy
//      to-igd/to-dis + reboot).
//
// v5.4 (2026-08-28):
//   R. [dgpu-active] — TRÓJSTOPNIOWA POLITYKA PSTATE dla CAŁEGO dGPU-ON:
//      baseline (0a, „efficient power save") / deep idle (07) / heavy (0e).
//      Detekcja aktywności usera przez evdev (/dev/input/event*, osobny wątek
//      z poll(); scroll+klawiatura+mysz). 0e WYŁĄCZNIE busy-driven
//      (busy > busy-enter 80% + temp < temp-up) — tytuł/klasa video
//      ([preferred-titles]/[video-classes]) tylko INFORMACYJNY (status/log).
//      07 = brak inputu przez activity-dwell-ms ORAZ busy < deep-idle-busy.
//      [caps] floor = twarde minimum (max(cap_floor, floor_dynamic)),
//      low-power ceiling (klasy [low-power]), termalne per-profil (lub wspólne
//      przez temp-per-profile=false). title_pref (force-0e po tytule) ZNIKA.
//      Sekcja [dgpu-active]; escape hatch enable=false → stara logika
//      (profil/ladder) bez zmian. Status: dgpu_state / input_active / video.
//
// v5.6 (2026-08-28):
//   S. SWITCHD TUNE: przeglądarki USUNIĘTE z [dgpu-hard] — karty
//      Discord/YouTube promują po tytule okna ([preferred-titles], icontains)
//      zamiast po klasie; reszta kart jest neutralna (iGPU). Promocja tytułowa
//      NIE jest zapinką: busy < title-idle-busy (default 33%, [switch]) przez
//      dwell-out → demote do iGPU (power-off). Focus na nie-Discord/YT → demote
//      po 1 s (niezależnie od busy — downclock do 07 robi [dgpu-active]).
//      Timery switch skrócone: min-residence/dwell-out/cooldown/min-switch-gap
//      = 1000 ms. [dgpu-active] bez zmian.

#include <algorithm>
#include <atomic>
#include <cctype>
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
#include <functional>
#include <getopt.h>
#include <glob.h>
#include <linux/input.h>
#include <map>
#include <mutex>
#include <poll.h>
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
static const char* FAN_OVERRIDE_FILE = "/run/reclockd/fan-override";
static const char* DEFAULT_CONFIG = "/etc/reclockd.conf";

// v5.0: switchd — ścieżki power/topologia/status.
static const char* VGA_SWITCHEROO = "/sys/kernel/debug/vgaswitcheroo/switch";
static const char* RUNTIME_STATUS = "/sys/bus/pci/devices/0000:01:00.0/power/runtime_status";
static const char* AUTOSUSPEND_DELAY = "/sys/bus/pci/devices/0000:01:00.0/power/autosuspend_delay_ms";
static const char* IGPU_RPS_CUR = "/sys/class/drm/card1/gt/gt0/rps_cur_freq_mhz";
static const char* SWITCH_STATUS_FILE = "/run/reclockd/status";
static const char* SWITCH_DGPU_FILE = "/run/switchd/dgpu";
static const char* DGPU_OVERRIDE_FILE = "/run/reclockd/dgpu-override";

// v5.0: BDF dGPU (GK107) i jego audio (HDA DIS-A 0000:01:00.1). Węzły DRM i
// sound rozwiązywane przez BDF (DGPU_PCI / DGPU_AUDIO_PCI) — numeracja cardN/
// renderDN/controlCN zmienia się między bootami (na tej maszynie akurat
// card0/renderD129/controlC2; controlC1 to PCH, NIE audio dGPU).
static const char* DGPU_PCI        = "0000:01:00.0";
static const char* DGPU_AUDIO_PCI  = "0000:01:00.1";

// v4.2: SMC applesmc — sterowanie wentylatorami MacBooka. fan1=Lewa,
// fan2=Prawa. fanN_manual (1=manual, 0=auto SMC) + fanN_output (cel RPM).
// fanN_min/fanN_max czytane dynamicznie przy starcie (zależne od HW).
static const char* FAN_BASE = "/sys/devices/platform/applesmc.768";

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
    // v4.1: tytuły okien "preferred" (substring case-insensitive; np. "YouTube",
    // "Discord"). Discord/YouTube są kartami w przeglądarce — nie mają własnej
    // klasy okna, więc detekcja po tytule jest dodatkową ścieżką obok klasy.
    std::set<std::string> preferred_titles;
    // v4.1: klasy okien "low-power" (terminale). Gdy okno z focusem pasuje →
    // wymuś profil default (cap=07) z priorytetem nad preferred (nawet jeśli
    // Discord/YouTube generuje load w tle, terminal z focusem trzyma 07).
    std::set<std::string> low_power_classes;
    // v4.4: per-klasa polityka [caps] — floor (stan spoczynkowy, IDLE nie schodzi
    // poniżej), max (ceiling), busy-up (własny próg UP-LOAD, %; 0 = globalny).
    // Składnia: "klasa = floor=0a, max=0e, busy-up=50" (każdy klucz opcjonalny).
    struct ClassCap {
        int floor   = -1;   // idx w LADDER; -1 = brak floor (IDLE do 07)
        int max     = -1;   // idx w LADDER; -1 = ceiling profilu
        int busy_up = 0;    // % busy do UP-LOAD; 0 = globalny busy-up (80)
    };
    std::map<std::string, ClassCap> class_caps;

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
    // v4.1: próg chwilowego busy (‰) poniżej którego DOWN transycje są
    // dozwolone (GR-idle gate). reclok pamięci w locie pod renderowaniem GR
    // wedge'uje silnik (udokumentowane: live 0a→07 → 631 trapów PROP). vblank
    // chroni scanout, NIE chroni GR mid-render — ten gate odracza DOWN aż
    // busy dolinie. UP-LOAD/BOOST-UP NIE są gate'owane (rosnący zegar pamięci
    // bezpieczniejszy; UP wymaga busy>80% więc gate na 30% blokowałby UP).
    int  gr_idle_promille = 300;
    // v4.2: kontrola wentylatorów applesmc. Krzywa temp→RPM: liniowa interpolacja
    // między fanN_min (temp_min) a fanN_max (temp_max), aktualizowana co poll_ms
    // (1 s). fanN_min/fanN_max czytane dynamicznie z sysfs przy starcie (nie
    // hardkodowane — każde HW ma inny zakres). Override flag-file zatrzymuje auto.
    bool fan_enable     = true;
    int  fan_temp_min   = 51;     // °C → min RPM (najcicho)
    int  fan_temp_max   = 91;     // °C → max RPM (najgłośniej)
    // v5.1: osobna krzywa gdy tylko iGPU (dGPU OFF) — cichsza. Gdy dGPU OFF hwmon
    // nouveau znika, temp = CPU (coretemp); CPU może się grzać wyżej zanim wiatraki
    // wejdą na max. Wybór w pętli: sw.dgpu_off() → krzywa igd, inaczej standardowa.
    int  fan_temp_min_igd = 41;   // °C → min RPM (tryb tylko-iGPU)
    int  fan_temp_max_igd = 91;   // °C → max RPM (tryb tylko-iGPU)
    // v4.6: sekcja [compiler] — boost wentylatorów gdy wykryty kompilator
    // (skan /proc/*/comm z fallbackiem na cmdline). fan_max = % maksymalnych
    // RPM (100 = pełne wiatraki). names = dodatkowe nazwy do wykrycia (przecinkami).
    bool compiler_enable   = true;
    int  compiler_fan_max  = 100;
    std::set<std::string> compiler_names;
    // v5.0: moduł switchd — dGPU power-state + render routing.
    struct SwitchCfg {
        bool enable = true;              // [switch] enable — moduł aktywny (w DIS = monitor)
        int  tick_ms = 1000;             // decision tick (== poll_ms)
        int  dwell_in_ms = 3000;         // entry dwell (miękka promocja)
        int  dwell_out_ms = 5000;        // exit dwell
        int  min_residence_ms = 20000;   // min-residence na dGPU (anti-flapping)
        int  cooldown_ms = 45000;        // cooldown po demote
        int  wait_ready_ms = 2000;       // wait-for-ready po power-on
        int  min_switch_gap_ms = 10000;  // min odstęp power-toggle
        int  temp_gate = 82;             // °C — nie promuj gdy dGPU gorętszy
        int  busy_enter = 80;            // % busy enter (miękka promocja)
        int  busy_exit = 40;             // % busy exit
        std::string backend = "manual";  // [dpower] manual | runpm
        int  autosuspend_ms = 5000;      // runpm
        int  wait_idle_timeout_ms = 5000; // /proc fd scan przed OFF
        int  wait_ready_timeout_ms = 10000; // nouveau reinit po ON
        int  pstate_settle_ms = 10000;   // nie pisz pstate po power-on (clock settle)
        int  pstate_write_timeout_ms = 2000; // timeout zapisu pstate (kernel hang safety)
        // v5.6: % busy poniżej którego karta Discord/YouTube (promocja tytułowa)
        // jest "idle" → demote do iGPU (power-off) po dwell-out. Konfigurowalne
        // w [switch] (title-idle-busy), reload przez SIGHUP.
        int  title_idle_busy = 33;
        // v5.6: po demote klasy tytułowej (Discord/YouTube) trzymaj dGPU OFF przez
        // ten czas przed ponowną promocją — zapobiega churnowi (grający YT na iGPU
        // nie re-promuje co 3-4 s). Konfigurowalne w [switch] (title-idle-hold-ms).
        int  title_idle_hold_ms = 30000;
        // v5.6: % busy poniżej którego klasa [dgpu-idle] (np. mpv) jest "idle"
        // → demote do iGPU (power-off) po dwell-out. Konfigurowalne w [switch]
        // (class-idle-busy), reload przez SIGHUP.
        int  class_idle_busy = 33;
        // v5.6: tytuły Discord/YouTube (kopiowane z [preferred-titles] w parserze)
        // — promocja tytułowa karty w przeglądarce, NIE zapinka (idle-release).
        std::set<std::string> preferred_titles;
        std::set<std::string> dgpu_hard;  // [dgpu-hard] klasy — twarda promocja
        std::set<std::string> dgpu_soft;  // [dgpu-soft] klasy — miękka (busy-gated)
        std::set<std::string> dgpu_idle;  // [dgpu-idle] klasy — twarda promocja + idle-demote
        std::set<std::string> igpu;       // [igpu] klasy — zawsze iGPU (democja)
        std::set<std::string> dgpu_procs; // [dgpu-procs] procesy — CUDA/DRI_PRIME
    } sw;
    // v5.4: sekcja [dgpu-active] — trójstopniowa polityka pstate dla całego
    // dGPU-ON (raport 79, decyzje usera 2026-08-28). enable=false (domyślnie) =
    // escape hatch: daemon działa jak dotychczas (profile/ladder). enable=true →
    // nowa logika. 0e WYŁĄCZNIE busy-driven (busy > busy-enter) + margines
    // termalny — tytuł video NIE daje 0e (kwalifikator tylko informacyjny).
    struct DgpuActiveCfg {
        bool enable = false;            // escape hatch — domyślnie WYŁĄCZONE
        int  baseline = 0x0a;           // stan spoczynkowy dGPU pracującego
        int  max      = 0x0e;           // ceiling dla sustained load
        int  low_power_ceiling = 0x0a;  // ceiling gdy focused ∈ [low-power]
        std::string activity_source = "evdev";  // evdev | none | cursorpos (→ none)
        int  activity_dwell_ms = 8000;  // brak inputu przez X → floor do 07
        int  deep_idle_busy = 20;       // % busy < (i brak inputu) → deep idle 07
        int  busy_enter = 80;           // % busy > sustained → 0e (jedyna droga do 0e)
        int  busy_exit  = 40;           // % busy ≤ → IDLE downshift (histereza)
        bool temp_per_profile = true;   // true = temp-down/up z profilu focusa
        int  temp_down = 82;            // wspólne termalne (gdy temp-per-profile=false)
        int  temp_up   = 75;
        // [video-classes] — klasy odtwarzaczy (mpv/vlc) kwalifikujące video
        // (informacyjnie — status/log; tytuł NIE decyduje o stanie pstate).
        std::set<std::string> video_classes;
    } dgpu;
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

// v5.4: nazwa stanu [dgpu-active] dla statusu/logów. Mapowanie po indeksie
// drabinki względem configu: max (0e) = "heavy", baseline (0a) = "active",
// 07 = "deep_idle"; idx<0 = "off". Gdy baseline==max (degeneracja) → "heavy".
static const char* dgpu_state_name(int idx, const Config::DgpuActiveCfg& d)
{
    if (idx < 0) return "off";
    if (idx == 0) return "deep_idle";
    if (idx >= state_to_idx(d.max)) return "heavy";
    return "active";
}

// v5.1: stan fan dla statusu (/run/reclockd/status) — aktualna krzywa + obroty.
// Wypełniane w fan block pętli głównej, czytane przez write_status() (pstate.sh,
// pasek Omarchy). off|override|compiler|igd|dga; tmin/tmax = aktywny zakres.
static std::string g_fan_curve = "off";
static int  g_fan_tmin = 51, g_fan_tmax = 91;
static int  g_fan_rpm1 = 0, g_fan_rpm2 = 0;

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
    // v5.0: O_CREAT|O_TRUNC — pliki statusu (/run/reclockd/status, /run/switchd/dgpu)
    // nie istnieją przy pierwszym zapisie; bez O_CREAT open() zwraca ENOENT i zapis
    // cicho ginie. Dla sysfs (fan1_manual, fan1_output, pstate itd.) to no-op —
    // pliki zawsze istnieją, sysfs ignoruje O_TRUNC.
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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

// v4.1: case-insensitive substring. Pusty needle → false (nie traktuj pustego
// wzorca jako match — zapobiega fałszywym sygnałom pref przy pustych wpisach).
static bool icontains(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return false;
    if (needle.size() > hay.size()) return false;
    auto to_lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); j++) {
            if (to_lower((unsigned char)hay[i + j]) != to_lower((unsigned char)needle[j])) {
                match = false; break;
            }
        }
        if (match) return true;
    }
    return false;
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
        if (section == "preferred-titles") {
            cfg.preferred_titles.insert(t);
            // v5.6: kopiuj do SwitchCfg — switchd promocja tytułowa Discord/YT.
            cfg.sw.preferred_titles.insert(t);
            continue;
        }
        if (section == "low-power") {
            cfg.low_power_classes.insert(t);
            continue;
        }
        // v4.4: sekcja "caps" — per-klasowa polityka: "klasa = floor=0a, max=0e, busy-up=50".
        if (section == "caps") {
            size_t eqc = t.find('=');
            if (eqc == std::string::npos) continue;
            std::string kc = trim(t.substr(0, eqc));
            std::string vc = trim(t.substr(eqc + 1));
            Config::ClassCap cc;
            size_t p0 = 0;
            while (p0 <= vc.size()) {
                size_t csep = vc.find(',', p0);
                std::string tok = trim(vc.substr(p0, csep == std::string::npos
                    ? std::string::npos : csep - p0));
                if (!tok.empty()) {
                    size_t eq = tok.find('=');
                    std::string k, v;
                    if (eq == std::string::npos) { k = tok; v = ""; }
                    else { k = trim(tok.substr(0, eq)); v = trim(tok.substr(eq + 1)); }
                    if      (k == "floor")     cc.floor   = parse_state(v);
                    else if (k == "max")       cc.max     = parse_state(v);
                    else if (k == "busy-up")   cc.busy_up = parse_int(v);
                }
                if (csep == std::string::npos) break;
                p0 = csep + 1;
            }
            cfg.class_caps[kc] = cc;
            continue;
        }
        // v4.2: sekcja [fan] — klucze klucz=wartość (enable/temp-min/temp-max).
        // v5.1: temp-min-igd/temp-max-igd — krzywa gdy tylko iGPU (dGPU OFF).
        if (section == "fan") {
            size_t eqf = t.find('=');
            if (eqf == std::string::npos) continue;
            std::string keyf = trim(t.substr(0, eqf));
            std::string valf = trim(t.substr(eqf + 1));
            if      (keyf == "enable")      cfg.fan_enable     = (valf == "1" || valf == "true" || valf == "yes");
            else if (keyf == "temp-min")    cfg.fan_temp_min   = parse_int(valf);
            else if (keyf == "temp-max")    cfg.fan_temp_max   = parse_int(valf);
            else if (keyf == "temp-min-igd") cfg.fan_temp_min_igd = parse_int(valf);
            else if (keyf == "temp-max-igd") cfg.fan_temp_max_igd = parse_int(valf);
            continue;
        }
        // v4.6: sekcja [compiler] — boost wentylatorów gdy wykryty kompilator.
        // Klucze: enable (1/0), fan-max (% maksymalnych RPM), names (przecinkami).
        if (section == "compiler") {
            size_t eqg = t.find('=');
            if (eqg == std::string::npos) continue;
            std::string keyg = trim(t.substr(0, eqg));
            std::string valg = trim(t.substr(eqg + 1));
            if      (keyg == "enable")  cfg.compiler_enable  = (valg == "1" || valg == "true" || valg == "yes");
            else if (keyg == "fan-max") cfg.compiler_fan_max = parse_int(valg);
            else if (keyg == "names") {
                // Lista nazw przecinkami: "ccache, sccache, ..." (opcjonalne).
                cfg.compiler_names.clear();
                size_t p0 = 0;
                while (p0 <= valg.size()) {
                    size_t csep = valg.find(',', p0);
                    std::string tok = trim(valg.substr(p0, csep == std::string::npos
                        ? std::string::npos : csep - p0));
                    if (!tok.empty()) cfg.compiler_names.insert(tok);
                    if (csep == std::string::npos) break;
                    p0 = csep + 1;
                }
            }
            continue;
        }
        // v5.0: sekcja [switch] — moduł switchd (dGPU power-state + render routing).
        if (section == "switch") {
            size_t eqs = t.find('=');
            if (eqs == std::string::npos) continue;
            std::string keys = trim(t.substr(0, eqs));
            std::string vals = trim(t.substr(eqs + 1));
            if      (keys == "enable")            cfg.sw.enable            = (vals == "1" || vals == "true" || vals == "yes");
            else if (keys == "tick-ms")           cfg.sw.tick_ms           = parse_int(vals);
            else if (keys == "dwell-in-ms")       cfg.sw.dwell_in_ms       = parse_int(vals);
            else if (keys == "dwell-out-ms")      cfg.sw.dwell_out_ms      = parse_int(vals);
            else if (keys == "min-residence-ms")  cfg.sw.min_residence_ms  = parse_int(vals);
            else if (keys == "cooldown-ms")       cfg.sw.cooldown_ms       = parse_int(vals);
            else if (keys == "wait-ready-ms")     cfg.sw.wait_ready_ms     = parse_int(vals);
            else if (keys == "min-switch-gap-ms") cfg.sw.min_switch_gap_ms = parse_int(vals);
            else if (keys == "temp-gate")         cfg.sw.temp_gate         = parse_int(vals);
            else if (keys == "busy-enter")        cfg.sw.busy_enter        = parse_int(vals);
            else if (keys == "busy-exit")         cfg.sw.busy_exit         = parse_int(vals);
            else if (keys == "pstate-settle-ms")  cfg.sw.pstate_settle_ms  = parse_int(vals);
            else if (keys == "pstate-write-timeout-ms") cfg.sw.pstate_write_timeout_ms = parse_int(vals);
            else if (keys == "title-idle-busy")   cfg.sw.title_idle_busy   = parse_int(vals);
            else if (keys == "title-idle-hold-ms") cfg.sw.title_idle_hold_ms = parse_int(vals);
            else if (keys == "class-idle-busy")  cfg.sw.class_idle_busy   = parse_int(vals);
            continue;
        }
        // v5.0: sekcja [dpower] — backend power dGPU.
        if (section == "dpower") {
            size_t eqd = t.find('=');
            if (eqd == std::string::npos) continue;
            std::string keyd = trim(t.substr(0, eqd));
            std::string vald = trim(t.substr(eqd + 1));
            if      (keyd == "backend")               cfg.sw.backend               = vald;
            else if (keyd == "autosuspend-ms")        cfg.sw.autosuspend_ms        = parse_int(vald);
            else if (keyd == "wait-idle-timeout-ms")  cfg.sw.wait_idle_timeout_ms  = parse_int(vald);
            else if (keyd == "wait-ready-timeout-ms") cfg.sw.wait_ready_timeout_ms = parse_int(vald);
            continue;
        }
        // v5.0: sekcje listowe switchd — bare keys (klasy/procesy).
        if (section == "dgpu-hard")  { cfg.sw.dgpu_hard.insert(t);  continue; }
        if (section == "dgpu-soft")  { cfg.sw.dgpu_soft.insert(t);  continue; }
        if (section == "dgpu-idle")  { cfg.sw.dgpu_idle.insert(t);  continue; }
        if (section == "igpu")       { cfg.sw.igpu.insert(t);       continue; }
        if (section == "dgpu-procs") { cfg.sw.dgpu_procs.insert(t); continue; }
        // v5.4: [video-classes] — klasy odtwarzaczy kwalifikujące video (mpv/vlc).
        if (section == "video-classes") { cfg.dgpu.video_classes.insert(t); continue; }
        // v5.4: sekcja [dgpu-active] — trójstopniowa polityka pstate dla dGPU-ON.
        // deep-idle-busy (nowy klucz, decyzja usera 2026-08-28); activity-wake-busy
        // akceptowany jako alias wstecz. video-* klucze z wersji roboczej raportu 79
        // ZNIKNĘŁY (0e nie jest już video-driven) — nieznane klucze są ignorowane.
        if (section == "dgpu-active") {
            size_t eqa = t.find('=');
            if (eqa == std::string::npos) continue;
            std::string keya = trim(t.substr(0, eqa));
            std::string vala = trim(t.substr(eqa + 1));
            if      (keya == "enable")               cfg.dgpu.enable            = (vala == "1" || vala == "true" || vala == "yes");
            else if (keya == "baseline")             cfg.dgpu.baseline          = parse_state(vala);
            else if (keya == "max")                  cfg.dgpu.max               = parse_state(vala);
            else if (keya == "low-power-ceiling")    cfg.dgpu.low_power_ceiling = parse_state(vala);
            else if (keya == "activity-source")      cfg.dgpu.activity_source   = vala;
            else if (keya == "activity-dwell-ms")    cfg.dgpu.activity_dwell_ms = parse_int(vala);
            else if (keya == "deep-idle-busy")       cfg.dgpu.deep_idle_busy    = parse_int(vala);
            else if (keya == "activity-wake-busy")   cfg.dgpu.deep_idle_busy    = parse_int(vala); // alias
            else if (keya == "busy-enter")           cfg.dgpu.busy_enter        = parse_int(vala);
            else if (keya == "busy-exit")            cfg.dgpu.busy_exit         = parse_int(vala);
            else if (keya == "temp-per-profile")     cfg.dgpu.temp_per_profile  = (vala == "1" || vala == "true" || vala == "yes");
            else if (keya == "temp-down")            cfg.dgpu.temp_down         = parse_int(vala);
            else if (keya == "temp-up")              cfg.dgpu.temp_up           = parse_int(vala);
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
        else if (key == "gr-idle-promille")  cfg.gr_idle_promille  = parse_int(val);
    }
    if (cfg.gr_idle_promille < 0) cfg.gr_idle_promille = 0;
    if (cfg.gr_idle_promille > 1000) cfg.gr_idle_promille = 1000;
    // v4.2: sanity fan — temp_max musi być > temp_min (inaczej krzywa zdegenerowana).
    if (cfg.fan_temp_max <= cfg.fan_temp_min) {
        logf(0, "config: fan temp-max <= temp-min (%d <= %d) — koryguję na 51/91",
             cfg.fan_temp_max, cfg.fan_temp_min);
        cfg.fan_temp_min = 51; cfg.fan_temp_max = 91;
    }
    // v5.1: sanity krzywa igd (tylko-iGPU) — analogicznie, korekta na 41/91.
    if (cfg.fan_temp_max_igd <= cfg.fan_temp_min_igd) {
        logf(0, "config: fan temp-max-igd <= temp-min-igd (%d <= %d) — koryguję na 41/91",
             cfg.fan_temp_max_igd, cfg.fan_temp_min_igd);
        cfg.fan_temp_min_igd = 41; cfg.fan_temp_max_igd = 91;
    }
    // v4.6: sanity compiler — fan-max clamp do [0,100] (100 = pełne wiatraki).
    if (cfg.compiler_fan_max < 0) cfg.compiler_fan_max = 0;
    if (cfg.compiler_fan_max > 100) cfg.compiler_fan_max = 100;
    // v5.0: sanity switchd.
    if (cfg.sw.tick_ms <= 0) cfg.sw.tick_ms = 1000;
    if (cfg.sw.dwell_in_ms < 0) cfg.sw.dwell_in_ms = 0;
    if (cfg.sw.dwell_out_ms < 0) cfg.sw.dwell_out_ms = 0;
    if (cfg.sw.min_residence_ms < 0) cfg.sw.min_residence_ms = 0;
    if (cfg.sw.cooldown_ms < 0) cfg.sw.cooldown_ms = 0;
    if (cfg.sw.wait_ready_ms < 0) cfg.sw.wait_ready_ms = 0;
    if (cfg.sw.min_switch_gap_ms < 0) cfg.sw.min_switch_gap_ms = 0;
    if (cfg.sw.temp_gate < 0) cfg.sw.temp_gate = 0;
    if (cfg.sw.busy_enter < 0) cfg.sw.busy_enter = 0;
    if (cfg.sw.busy_enter > 100) cfg.sw.busy_enter = 100;
    if (cfg.sw.busy_exit < 0) cfg.sw.busy_exit = 0;
    if (cfg.sw.busy_exit > 100) cfg.sw.busy_exit = 100;
    if (cfg.sw.backend != "manual" && cfg.sw.backend != "runpm") cfg.sw.backend = "manual";
    if (cfg.sw.autosuspend_ms < 0) cfg.sw.autosuspend_ms = 0;
    if (cfg.sw.wait_idle_timeout_ms < 0) cfg.sw.wait_idle_timeout_ms = 0;
    if (cfg.sw.wait_ready_timeout_ms < 0) cfg.sw.wait_ready_timeout_ms = 0;
    if (cfg.sw.pstate_settle_ms < 0) cfg.sw.pstate_settle_ms = 0;
    if (cfg.sw.pstate_write_timeout_ms < 100) cfg.sw.pstate_write_timeout_ms = 100;
    // v5.6: sanity title-idle-busy — % busy, clamp [0,100]; błędna wartość
    // (ujemna/absurdalna) → fallback 33.
    if (cfg.sw.title_idle_busy < 0 || cfg.sw.title_idle_busy > 100) cfg.sw.title_idle_busy = 33;
    // v5.6: sanity title-idle-hold-ms — min 1000 ms (1 s), błędna wartość → 30000.
    if (cfg.sw.title_idle_hold_ms < 1000) cfg.sw.title_idle_hold_ms = 30000;
    // v5.6: sanity class-idle-busy — % busy, clamp [0,100]; błędna wartość → 33.
    if (cfg.sw.class_idle_busy < 0 || cfg.sw.class_idle_busy > 100) cfg.sw.class_idle_busy = 33;
    // v5.4: sanity [dgpu-active]. Stany muszą być znane (07/0a/0e/0f) i
    // baseline <= max. Progi busy clamp do [0,100]. activity-source: tylko
    // "evdev" daje sygnał inputu; "none"/"cursorpos" → brak (idle po dwell).
    if (cfg.dgpu.activity_source != "evdev" && cfg.dgpu.activity_source != "none" &&
        cfg.dgpu.activity_source != "cursorpos")
        cfg.dgpu.activity_source = "evdev";
    if (cfg.dgpu.activity_dwell_ms < 0) cfg.dgpu.activity_dwell_ms = 0;
    if (cfg.dgpu.deep_idle_busy < 0) cfg.dgpu.deep_idle_busy = 0;
    if (cfg.dgpu.deep_idle_busy > 100) cfg.dgpu.deep_idle_busy = 100;
    if (cfg.dgpu.busy_enter < 0) cfg.dgpu.busy_enter = 0;
    if (cfg.dgpu.busy_enter > 100) cfg.dgpu.busy_enter = 100;
    if (cfg.dgpu.busy_exit < 0) cfg.dgpu.busy_exit = 0;
    if (cfg.dgpu.busy_exit > 100) cfg.dgpu.busy_exit = 100;
    // v5.4: busy-exit MUSI być < gr-idle-promille (30%) — inaczej zejście z 0e
    // (busy ≤ exit) będzie wiecznie deferowane przez GR-idle gate (raport 79
    // ryzyko 7). Default 40% jest nad progiem — to świadoma histereza; przy
    // gr-idle-promille < 400 zejście IDLE z 0e jest deferowane pod renderem.
    if (!known_state(cfg.dgpu.baseline)) cfg.dgpu.baseline = 0x0a;
    if (!known_state(cfg.dgpu.max)) cfg.dgpu.max = 0x0e;
    if (!known_state(cfg.dgpu.low_power_ceiling)) cfg.dgpu.low_power_ceiling = 0x0a;
    if (state_to_idx(cfg.dgpu.baseline) > state_to_idx(cfg.dgpu.max)) {
        logf(0, "config: [dgpu-active] baseline > max — koryguję max=baseline");
        cfg.dgpu.max = cfg.dgpu.baseline;
    }
    if (cfg.dgpu.temp_down <= cfg.dgpu.temp_up) {
        logf(0, "config: [dgpu-active] temp-down <= temp-up (%d <= %d) — koryguję temp-up=temp-down-1",
             cfg.dgpu.temp_down, cfg.dgpu.temp_up);
        cfg.dgpu.temp_up = cfg.dgpu.temp_down - 1;
        if (cfg.dgpu.temp_up < 0) cfg.dgpu.temp_up = 0;
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

    // v5.0: munmap + mmap BAR0 (gdyby mapping był stale po power-cycle).
    bool reopen_mmio()
    {
        if (map_ != MAP_FAILED) { munmap(map_, MAP_SIZE); map_ = MAP_FAILED; }
        return open_mmio();
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
        bool nv = scan_nouveau();
        // v5.0 G1: w IGD dGPU OFF → hwmon nouveau zniknął → read_temp()=-1.
        // Krzywa fan musi znać też CPU (coretemp) — inaczej wentylatory na
        // min przy gorącym procesorze. coretemp NIE znika przy power-cycle.
        scan_coretemp();
        return nv;
    }
    ~Hwmon() { if (fd_ >= 0) close(fd_); if (cpu_fd_ >= 0) close(cpu_fd_); }

    // v5.0: re-scan hwmon nouveau po power-cycle (node zniknął przy power-off;
    // stary fd byłby -1 na zawsze → read_temp() = -1 = krzywa min).
    void reinit()
    {
        if (fd_ >= 0) { close(fd_); fd_ = -1; }
        path_.clear();
        // v5.0 G1: cpu_fd_ (coretemp) celowo NIE ruszany — coretemp nie znika
        // przy power-cycle dGPU, a re-open bez close = leak fd. Tylko re-scan
        // nouveau jak dotychczas.
        scan_nouveau();
    }

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

    // v5.0 G1: temp CPU (coretemp, "Package id 0") — °C, -1 gdy brak. Wzorzec
    // jak read_temp() ale na cpu_fd_ (osobne źródło, niezależne od power-cycle).
    int read_cpu_temp() const
    {
        if (cpu_fd_ < 0) return -1;
        if (lseek(cpu_fd_, 0, SEEK_SET) < 0) return -1;
        char buf[32];
        ssize_t n = read(cpu_fd_, buf, sizeof buf - 1);
        if (n <= 0) return -1;
        buf[n] = 0;
        return std::atoi(buf) / 1000;
    }
    const std::string& path() const { return path_; }
    bool ok() const { return fd_ >= 0; }

private:
    bool scan_nouveau()
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
    void scan_coretemp()
    {
        for (int i = 0; i < 64; i++) {
            std::string p = std::string(HWMON_DIR) + "/hwmon" + std::to_string(i);
            std::string name;
            if (read_file((p + "/name").c_str(), name) != 0) continue;
            if (name.find("coretemp") == std::string::npos) continue;
            cpu_path_ = p + "/temp1_input";
            cpu_fd_ = open(cpu_path_.c_str(), O_RDONLY);
            if (cpu_fd_ < 0) {
                std::perror(("open " + cpu_path_).c_str());
                cpu_fd_ = -1;   // fallback nieaktywny
            }
            return;
        }
    }

    std::string path_;
    int fd_ = -1;
    std::string cpu_path_;
    int cpu_fd_ = -1;
};

// ----------------------------------------------------------- wentylatory (applesmc)
// v4.2: sterowanie wentylatorami MacBooka przez SMC applesmc. Dwa wentylatory:
// fan1 (Lewa), fan2 (Prawa). Zakresy RPM (fanN_min/fanN_max) czytane dynamicznie
// z sysfs przy starcie — NIE hardkodowane (każde HW ma inny zakres; user chciał
// "zakresy generowane dynamicznie wg 2ch wartosci" = min/max z sysfs na wentylator).
//
// Krzywa temp→RPM: liniowa interpolacja. x = clamp((temp - tmin) / (tmax - tmin))
// w [0,1]; rpm = fanN_min + round(x * (fanN_max - fanN_min)). temp ≤ tmin → min
// RPM (najcicho), temp ≥ tmax → max RPM (najgłośniej). Aktualizacja co poll_ms (1 s)
// — zbieżne z cyklem ankiety hyprctl.
//
// Fail-safe: init() zwraca false gdy brak plików applesmc → fan wyłączony bez
// błędu (demon działa bez sterowania wentylatorami). restore_auto() przy wyjściu
// zdejmuje manual (fanN_manual=0) → SMC przejmuje auto-kontrolę (reclockd nigdy
// nie zostawia wentylatorów zablokowanych na manualu po zatrzymaniu).

class Fan {
public:
    bool init()
    {
        std::string s;
        std::string base = std::string(FAN_BASE) + "/";
        if (read_file((base + "fan1_min").c_str(), s) != 0) return false;
        fan1_min_ = std::atoi(s.c_str());
        if (read_file((base + "fan1_max").c_str(), s) != 0) return false;
        fan1_max_ = std::atoi(s.c_str());
        if (read_file((base + "fan2_min").c_str(), s) != 0) return false;
        fan2_min_ = std::atoi(s.c_str());
        if (read_file((base + "fan2_max").c_str(), s) != 0) return false;
        fan2_max_ = std::atoi(s.c_str());
        // Sanity: min ≤ max i niezerowe; inaczej fail-safe (nie pisz manual=1).
        if (fan1_min_ <= 0 || fan1_max_ <= fan1_min_ ||
            fan2_min_ <= 0 || fan2_max_ <= fan2_min_) {
            logf(0, "fan: absurdalne zakresy sysfs (f1=%d-%d f2=%d-%d) — wyłączam",
                 fan1_min_, fan1_max_, fan2_min_, fan2_max_);
            return false;
        }
        ok_ = true;
        return true;
    }

    // Ustaw wentylatory wg temp. temp<0 (brak hwmon) → pomiń (zostaw poprzedni stan).
    // tmin>=tmax (zły config) → clamp do max RPM (bezpieczniej chłodzić).
    void set(int temp, int tmin, int tmax)
    {
        if (!ok_) return;
        // v5.0: temp<0 (hwmon nouveau zniknął — dGPU OFF) → krzywa min (x=0).
        // Wcześniej fail-safe "nie ruszaj" zostawiał wentylatory na ostatniej
        // wartości; po power-off dGPU temp jest stale -1, więc krzywa musi zejść
        // do min (SMC ma ochronę termiczną nadrzędną nad manual).
        if (temp < 0) temp = tmin;
        double x;
        if (tmax <= tmin) x = 1.0;               // zły zakres → max (chłodź)
        else if (temp <= tmin) x = 0.0;
        else if (temp >= tmax) x = 1.0;
        else x = (double)(temp - tmin) / (double)(tmax - tmin);
        int rpm1 = fan1_min_ + (int)(x * (fan1_max_ - fan1_min_) + 0.5);
        int rpm2 = fan2_min_ + (int)(x * (fan2_max_ - fan2_min_) + 0.5);
        write_file((std::string(FAN_BASE) + "/fan1_manual").c_str(), "1");
        write_file((std::string(FAN_BASE) + "/fan2_manual").c_str(), "1");
        write_file((std::string(FAN_BASE) + "/fan1_output").c_str(), std::to_string(rpm1));
        write_file((std::string(FAN_BASE) + "/fan2_output").c_str(), std::to_string(rpm2));
        last_temp_ = temp; last_rpm1_ = rpm1; last_rpm2_ = rpm2;
    }

    // v4.6: BOOST — wymuś pct% maksymalnych RPM (100 = pełne wiatraki). Zamiast
    // krzywej temp→RPM, stały procent zakresu: rpm = fanN_min + pct/100*(fanN_max
    // - fanN_min). Manual=1 + zapis output dla obu wentylatorów (jak set()).
    // pct clamp do [0,100]. Używane gdy wykryty kompilator (sekcja [compiler]).
    void set_boost(int pct)
    {
        if (!ok_) return;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        double x = pct / 100.0;
        int rpm1 = fan1_min_ + (int)(x * (fan1_max_ - fan1_min_) + 0.5);
        int rpm2 = fan2_min_ + (int)(x * (fan2_max_ - fan2_min_) + 0.5);
        write_file((std::string(FAN_BASE) + "/fan1_manual").c_str(), "1");
        write_file((std::string(FAN_BASE) + "/fan2_manual").c_str(), "1");
        write_file((std::string(FAN_BASE) + "/fan1_output").c_str(), std::to_string(rpm1));
        write_file((std::string(FAN_BASE) + "/fan2_output").c_str(), std::to_string(rpm2));
        last_temp_ = -1; last_rpm1_ = rpm1; last_rpm2_ = rpm2;
    }

    // Fail-safe: zwróć kontrolę do SMC. Wołane przy wyjściu demona (restore).
    void restore_auto()
    {
        if (!ok_) return;
        write_file((std::string(FAN_BASE) + "/fan1_manual").c_str(), "0");
        write_file((std::string(FAN_BASE) + "/fan2_manual").c_str(), "0");
        logf(1, "fan: restore auto (fanN_manual=0) — SMC przejmuje");
    }

    bool ok() const { return ok_; }
    int fan1_min() const { return fan1_min_; }
    int fan1_max() const { return fan1_max_; }
    int fan2_min() const { return fan2_min_; }
    int fan2_max() const { return fan2_max_; }
    int last_rpm1() const { return last_rpm1_; }
    int last_rpm2() const { return last_rpm2_; }
    int last_temp() const { return last_temp_; }

private:
    bool ok_ = false;
    int fan1_min_ = 0, fan1_max_ = 0;
    int fan2_min_ = 0, fan2_max_ = 0;
    int last_temp_ = -1, last_rpm1_ = 0, last_rpm2_ = 0;
};

static bool fan_override_active()
{
    struct stat st;
    return stat(FAN_OVERRIDE_FILE, &st) == 0;
}

// ----------------------------------------------------------- kompilatory (/proc)
// v4.6: detekcja uruchomionych kompilatorów. Skan /proc/<pid>/comm (basename
// procesu) z fallbackiem na cmdline (argv[0] basename) — łapie np. procesy,
// których comm nie jest oczywisty. Dopasowanie case-sensitive; oprócz listy
// bazowej też prefiksy wersji (gcc-*, g++-*, clang-*) i rozszerzenie z configu
// ([compiler] names). Scan co poll_cycles (1 s), zatrzymaj gdy znaleziono.

static const std::set<std::string>& default_compiler_names()
{
    static const std::set<std::string> names = {
        "clang", "clang++", "clang-14", "clang-15", "clang-16", "clang-17", "clang-18",
        "gcc", "g++", "cc", "c++", "cc1", "cc1plus", "cc1obj", "cc1objplus",
        "make", "cmake", "ninja", "ninja-build", "cargo", "rustc", "meson",
        "go", "javac", "ld", "ld.lld", "lld", "as", "sccache", "ccache",
    };
    return names;
}

// Zwraca nazwę jeśli pasuje do listy kompilatorów, inaczej "".
static std::string compiler_match(const std::string& name)
{
    if (name.empty()) return "";
    const std::set<std::string>& base = default_compiler_names();
    if (base.count(name)) return name;
    if (g_cfg.compiler_names.count(name)) return name;
    // Wersjonowane binaria: gcc-12, g++-12, clang-19, ... (prefix, case-sensitive).
    if (name.rfind("gcc-", 0) == 0 || name.rfind("g++-", 0) == 0 ||
        name.rfind("clang-", 0) == 0) return name;
    return "";
}

// Skanuje /proc i zwraca nazwę wykrytego kompilatora lub "" gdy brak.
// Wywoływana tylko w bloku wentylatorów (co poll_cycles = 1 s).
static std::string compiler_running()
{
    DIR* d = opendir("/proc");
    if (!d) return "";
    struct dirent* e;
    std::string found;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue; // tylko PIDs
        std::string bp = std::string("/proc/") + e->d_name;
        std::string comm;
        if (read_file((bp + "/comm").c_str(), comm) == 0) {
            comm = trim(comm);
            found = compiler_match(comm);
            if (!found.empty()) break;
        }
        // Fallback: cmdline (argv[0] basename). Pomiń wątki jądra (comm "[...]" —
        // cmdline i tak puste), oszczędza otwieranie plików dla ~100 wątków.
        if (comm.empty() || comm[0] != '[') {
            std::string cmdline;
            if (read_file((bp + "/cmdline").c_str(), cmdline) == 0 && !cmdline.empty()) {
                size_t nul = cmdline.find('\0');
                std::string a0 = cmdline.substr(0, nul == std::string::npos
                    ? cmdline.size() : nul);
                size_t slash = a0.rfind('/');
                std::string base = slash == std::string::npos ? a0 : a0.substr(slash + 1);
                found = compiler_match(base);
                if (!found.empty()) break;
            }
        }
    }
    closedir(d);
    return found;
}

// v5.0: pstate write z timeoutem. Kernel może zawisnąć w nvkm_pstate_calc
// (wait_event na workqueue) po power-cycle — zapis debugfs blokuje w D state
// na zawsze. Zapis w osobnym wątku; main czeka max pstate_write_timeout_ms.
// Po timeout: wątek zostaje (kernel go trzyma), daemon żyje dalej i NIE pisze
// pstate aż do resetu (g_pstate_stuck — czyszczone przez recover_after_power_on
// po kolejnym power-cycle).
static std::atomic<bool> g_pstate_done{false};
static std::atomic<int>  g_pstate_result{-1};
static std::atomic<bool> g_pstate_busy{false};
static std::atomic<bool> g_pstate_stuck{false};

static void pstate_write_worker(uint32_t st)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02x", st);
    int r = write_file(PSTATE_FILE, buf);
    g_pstate_result = r;
    g_pstate_done = true;
}

static int set_pstate(uint32_t st)
{
    if (g_pstate_stuck) {
        logf(0, "pstate: pomijam %02x — poprzedni zapis utknął w kernelu "
                "(nvkm_pstate_calc). Power-cycle dGPU (reclockctl dgpu-off/on) "
                "zresetuje stan.", st);
        return -1;
    }
    // Serializacja: czekaj na poprzedni zapis (jeśli wciąż trwa).
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(g_cfg.sw.pstate_write_timeout_ms);
    while (g_pstate_busy && !g_pstate_done &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_pstate_busy && !g_pstate_done) {
        g_pstate_stuck = true;
        logf(0, "pstate: BŁĄD — poprzedni zapis utknął w kernelu (nvkm_pstate_calc). "
                "Wątek odrzucony, daemon żyje. Dalsze zapisy pstate wstrzymane.");
        return -1;
    }
    g_pstate_done = false;
    g_pstate_result = -1;
    g_pstate_busy = true;
    std::thread(pstate_write_worker, st).detach();
    deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(g_cfg.sw.pstate_write_timeout_ms);
    while (!g_pstate_done && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!g_pstate_done) {
        g_pstate_stuck = true;
        logf(0, "pstate: BŁĄD — zapis %02x nie skończył się w %dms (kernel hang w "
                "nvkm_pstate_calc). Wątek odrzucony, daemon żyje. Dalsze zapisy pstate "
                "wstrzymane do power-cycle dGPU.",
             st, g_cfg.sw.pstate_write_timeout_ms);
        return -1;
    }
    g_pstate_busy = false;
    return g_pstate_result;
}

// v4.1: GR-idle gate — czy chwilowy busy (‰ nad ostatni interwal) jest poniżej
// progu? Używane przed DOWN transycjami (reclok pamięci w locie pod renderem
// GR wedge'uje silnik). b pochodzi z gpu.sample() (PMU BAR0, reset co sample).
static bool gr_idle_ok(uint32_t busy_promille)
{
    return (int)busy_promille <= g_cfg.gr_idle_promille;
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
// v5.0: ostatnia próbka busy (‰) — współdzielona między pętlą główną a switchd tick.
static volatile uint32_t g_last_busy = 0;

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

// Numer karty PANELU (konektor eDP/LVDS connected) — kartę panelu musi śledzić
// vblank sync. W DIS = card0 (dGPU prowadzi panel), po G1/IGD = card1 (iGPU).
// Hardcoded DRM_CARD (card0) w IGD jest błędny: card0 to dGPU bez aktywnego
// CRTC → ioctl WAIT_VBLANK pada EBUSY, a własny fd daemona na card0 blokuje
// fd_busy() (daemon sam siebie — power-off odroczony). Zwraca "" gdy panel nie
// znaleziony (fallback na DRM_CARD).
static std::string panel_drm_card()
{
    DIR* d = opendir("/sys/class/drm");
    if (!d) return "";
    struct dirent* e;
    std::string card;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name.rfind("card", 0) != 0) continue;
        size_t dash = name.find('-');
        if (dash == std::string::npos || dash <= 4) continue;   // "card<N>-<connector>"
        bool digits = true;
        for (size_t i = 4; i < dash; i++)
            if (!std::isdigit((unsigned char)name[i])) { digits = false; break; }
        if (!digits) continue;
        std::string conn = name.substr(dash + 1);
        if (conn.rfind("eDP", 0) != 0 && conn.rfind("LVDS", 0) != 0) continue;
        std::string st;
        if (read_file(("/sys/class/drm/" + name + "/status").c_str(), st) != 0) continue;
        if (trim(st) == "connected") {
            card = name.substr(4, dash - 4);
            break;
        }
    }
    closedir(d);
    return card;
}

static int g_drm_fd = -1;
static bool drm_open()
{
    // vblank sync musi śledzić kartę PANELU — w DIS = card0 (dGPU), po G1/IGD =
    // card1 (iGPU). Hardcoded DRM_CARD w IGD to fd na dGPU bez CRTC (ioctl vblank
    // EBUSY) + własny fd blokuje fd_busy(). panel_drm_card() rozwiązuje kartę po
    // connected eDP/LVDS; fallback na DRM_CARD.
    std::string panel = panel_drm_card();
    std::string dev = panel.empty() ? std::string(DRM_CARD) : "/dev/dri/card" + panel;
    g_drm_fd = open(dev.c_str(), O_RDWR | O_CLOEXEC);
    if (g_drm_fd < 0) {
        logf(0, "drm: open %s nieudany (%s)", dev.c_str(), std::strerror(errno));
        return false;
    }
    logf(1, "drm: vblank sync na %s", dev.c_str());
    // Zrzuć KMS master natychmiast po open(). Pierwszy opener danej karty zostaje
    // domyślnym DRM masterem; bez tego reclockd blokuje Hyprlandowi przejęcie
    // karty przez libseat/logind (EBUSY -> "Found no gpus" -> crash -> black
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
//
// v4.1: poprawna obsługa escape sekwencji JSON (\" \\ \n \t). Wcześniej json_str
// szukał końcowego `"` przez find('"', k+1) — ucinał łańcuch przy pierwszym
// `\"` (np. tytuł YouTube z cudzysłowem w nazwie wideo). Teraz skan znak-po-
// -znaku z unescape. Wspólny helper json_extract_string eliminuje duplikację.

// Skanuje łańcuch JSON zaczynając od otwierającego `"` pod pozycją `q`.
// Zwraca pozycję zamykającego `"` (lub npos gdy brak) i wypełnia `out`
// unescaped'ną treścią. Obsługuje \", \\, \n, \t (inne: dosłownie backslash+znak).
static size_t json_extract_string(const std::string& json, size_t q, std::string& out)
{
    out.clear();
    size_t i = q + 1;
    while (i < json.size()) {
        char c = json[i];
        if (c == '"') return i;            // koniec łańcucha
        if (c == '\\' && i + 1 < json.size()) {
            char e = json[i + 1];
            switch (e) {
                case '"':  out.push_back('"');  i += 2; continue;
                case '\\': out.push_back('\\'); i += 2; continue;
                case 'n':  out.push_back('\n'); i += 2; continue;
                case 't':  out.push_back('\t'); i += 2; continue;
                default:   out.push_back('\\'); out.push_back(e); i += 2; continue;
            }
        }
        out.push_back(c);
        i++;
    }
    return std::string::npos;              // brak zamykającego `"` — zwróć co zebrano
}

static bool json_str(const std::string& json, const std::string& key, std::string& out)
{
    std::string pat = "\"" + key + "\"";
    size_t k = json.find(pat);
    if (k == std::string::npos) return false;
    k = json.find(':', k + pat.size());
    if (k == std::string::npos) return false;
    k = json.find('"', k + 1);
    if (k == std::string::npos) return false;
    size_t end = json_extract_string(json, k, out);
    return end != std::string::npos;
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
        std::string val;
        size_t end = json_extract_string(json, k, val);
        if (end == std::string::npos) break;
        out.push_back(val);
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
    // v4.1: zwraca też title (do detekcji Discord/YouTube po tytule — są kartami
    // w przeglądarce, nie mają własnej klasy okna).
    bool activewindow(std::string& cls, std::string& title)
    {
        std::string json;
        if (!hyprctl_json("activewindow", json)) return false;
        if (json.find("\"class\"") == std::string::npos) return false; // puste
        bool ok = json_str(json, "class", cls);
        json_str(json, "title", title); // opcjonalne — brak title nie psuje class
        return ok;
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

// ----------------------------------------------------------- switchd: topologia
// v5.0: moduł switchd — dGPU power-state + render routing (Etap 1: monitor w DIS).
// Topologia: vgaswitcheroo (root) — "2:DIS:+:Pwr:0000:01:00.0" → DIS (dGPU
// prowadzi panel), "0:IGD:+" → IGD. Fallback: /sys/class/drm/card0-*/status
// (eDP-1 connected na card0 = DIS).

enum Topo { DIS, IGD, UNKNOWN };

class Topology {
public:
    Topo detect()
    {
        std::string content;
        if (read_file(VGA_SWITCHEROO, content) == 0) {
            if (content.find(":DIS:+") != std::string::npos) { topo_ = DIS; return topo_; }
            if (content.find(":IGD:+") != std::string::npos) { topo_ = IGD; return topo_; }
        }
        // Fallback: jakikolwiek konektor card0 connected (eDP-1 = panel) → DIS.
        if (card0_has_connected()) { topo_ = DIS; return topo_; }
        topo_ = UNKNOWN;
        return topo_;
    }
    Topo topo() const { return topo_; }
    const char* name() const
    {
        switch (topo_) {
            case DIS: return "dis";
            case IGD: return "igd";
            default:  return "unknown";
        }
    }
private:
    static bool card0_has_connected()
    {
        DIR* d = opendir("/sys/class/drm");
        if (!d) return false;
        struct dirent* e;
        bool found = false;
        while ((e = readdir(d))) {
            std::string name = e->d_name;
            if (name.rfind("card0-", 0) != 0) continue;
            std::string status;
            if (read_file(("/sys/class/drm/" + name + "/status").c_str(), status) == 0) {
                if (trim(status) == "connected") { found = true; break; }
            }
        }
        closedir(d);
        return found;
    }
    Topo topo_ = UNKNOWN;
};

// ----------------------------------------------------------- switchd: węzły dGPU (BDF)
// Węzły DRM i audio dGPU rozwiązywane przez PCI BDF — NIE przez numerację cardN/
// renderDN/controlCN (numeracja zmienia się między bootami). Kontekst G1 live:
// w IGD kompozytor (Hyprland) zawsze trzyma fd na iGPU (card1 + renderD128) —
// gate "każde /dev/dri/*" byłby wiecznie busy i power-off nigdy by nie przeszedł
// (lekcja z live-testu G1: apply() → "dGPU zajęty (otwarte fd /dev/dri) —
// power-off odroczony"). Dlatego fd_busy() blokuje TYLKO węzły dGPU.

// readlink + basename celu (np. /sys/class/drm/card0/device → "0000:01:00.0").
// Bez libgen.h — własny basename, bez mutowania bufora.
static std::string readlink_basename(const std::string& path)
{
    char link[256];
    ssize_t n = readlink(path.c_str(), link, sizeof link - 1);
    if (n <= 0) return "";
    link[n] = 0;
    std::string target(link);
    size_t slash = target.find_last_of('/');
    return slash == std::string::npos ? target : target.substr(slash + 1);
}

// Węzły DRM dGPU: /dev/dri/card<N> + /dev/dri/renderD<N> dla PCI == DGPU_PCI.
static std::set<std::string> dgpu_drm_nodes()
{
    std::set<std::string> nodes;
    DIR* d = opendir("/sys/class/drm");
    if (!d) return nodes;
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        // Tylko węzły card<N> / renderD<N> — konektory (cardN-DP-1) mają
        // device → cardN, nie pasują do DGPU_PCI (filtr i tak je odsiewa).
        auto is_digits = [&](size_t off) {
            if (name.size() <= off) return false;
            for (size_t i = off; i < name.size(); i++)
                if (!std::isdigit((unsigned char)name[i])) return false;
            return true;
        };
        bool is_card = name.rfind("card", 0) == 0 && is_digits(4);
        bool is_rend = name.rfind("renderD", 0) == 0 && is_digits(7);
        if (!is_card && !is_rend) continue;
        if (readlink_basename("/sys/class/drm/" + name + "/device") == DGPU_PCI)
            nodes.insert("/dev/dri/" + name);
    }
    closedir(d);
    return nodes;
}

// Numer karty audio dGPU (np. "2") — karta na PCI == DGPU_AUDIO_PCI.
// (Na tej maszynie: card2/controlC2 = HDA NVidia 0000:01:00.1; controlC1 to
// PCH 0000:00:1b.0 — NIE dGPU. Matcher v5.0 na controlC1 był błędny.)
// LEKCJA G1 (live): wireplumber (PipeWire session manager) trzyma controlC<N>
// PERMANENTNIE — otwiera control każdej karty i nigdy nie zamyka. Przez to gate
// na controlC blokowałby power-off na zawsze mimo wolnego dGPU. Audio gate musi
// dotyczyć TYLKO AKTYWNEGO PCM PLAYBACK (fd na /dev/snd/pcmC<N>D...p), nigdy
// control. Zwraca "" gdy karta nie znaleziona.
static std::string dgpu_snd_playback_card()
{
    DIR* d = opendir("/sys/class/sound");
    if (!d) return "";
    struct dirent* e;
    std::string card;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name.rfind("card", 0) != 0 || name.size() <= 4) continue;
        bool digits = true;
        for (size_t i = 4; i < name.size(); i++)
            if (!std::isdigit((unsigned char)name[i])) { digits = false; break; }
        if (!digits) continue;
        // /sys/class/sound/card<N>/device → PCI. (controlC<N>/device prowadzi
        // do card<N>, więc skanujemy karty, nie controlC.)
        if (readlink_basename("/sys/class/sound/" + name + "/device") == DGPU_AUDIO_PCI) {
            card = name.substr(4);
            break;
        }
    }
    closedir(d);
    return card;
}

// v5.0: dgpu-override flag-file (/run/reclockd/dgpu-override). reclockctl
// dgpu-on/dgpu-off/dgpu-auto. Wzorzec fan-override (G): "" = brak pliku (auto),
// "on" = dGPU wymuszony ON, "off" = dGPU wymuszony OFF.
static std::string dgpu_override()
{
    struct stat st;
    if (stat(DGPU_OVERRIDE_FILE, &st) != 0) return "";
    std::string c;
    read_file(DGPU_OVERRIDE_FILE, c);
    return trim(c);
}

// ----------------------------------------------------------- switchd: power dGPU

class DgpuPower {
public:
    struct State {
        std::string vgasw;    // "Pwr" | "Off" | "Dyn" | "DynOff" | ""
        std::string runtime;  // "active" | "suspended" | "unsupported" | ""
        // vgaswitcheroo jest AUTORYTATYWNY: po odcięciu gmux (DISCRETE_POWER=OFF,
        // PCI D3hot) nouveau zgłasza stale runtime_status="active", mimo że karta
        // jest martwa — sam runtime kłamałby (raport 63: busy=1000‰ na martwej
        // karcie, boost pstate). "Off"/"DynOff" = OFF. runtime tylko jako fallback
        // (backend runpm bez vgaswitcheroo).
        bool on() const {
            if (!vgasw.empty()) return vgasw == "Pwr" || vgasw == "Dyn";
            return runtime == "active";
        }
    };

    DgpuPower(const std::string& backend, int autosuspend_ms)
        : backend_(backend), autosuspend_ms_(autosuspend_ms) {}

    void set_recover_cb(std::function<bool()> cb) { recover_cb_ = std::move(cb); }

    State read() const
    {
        State st;
        std::string content;
        if (read_file(VGA_SWITCHEROO, content) == 0) {
            size_t pos = 0;
            while (pos < content.size()) {
                size_t eol = content.find('\n', pos);
                std::string line = content.substr(pos, eol == std::string::npos
                    ? std::string::npos : eol - pos);
                if (line.find("0000:01:00.0") != std::string::npos) {
                    // Format: "2:DIS:+:Pwr:0000:01:00.0" — pole 3 = power state.
                    size_t p0 = 0;
                    int field = 0;
                    while (p0 <= line.size()) {
                        size_t colon = line.find(':', p0);
                        std::string tok = line.substr(p0, colon == std::string::npos
                            ? std::string::npos : colon - p0);
                        if (field == 3) { st.vgasw = tok; break; }
                        field++;
                        if (colon == std::string::npos) break;
                        p0 = colon + 1;
                    }
                    break;
                }
                if (eol == std::string::npos) break;
                pos = eol + 1;
            }
        }
        std::string rs;
        if (read_file(RUNTIME_STATUS, rs) == 0) st.runtime = trim(rs);
        return st;
    }

    bool set_on()
    {
        if (backend_ == "runpm") {
            write_file(AUTOSUSPEND_DELAY, std::to_string(autosuspend_ms_));
            return write_file(POWER_CTRL, "on") == 0;
        }
        return write_file(VGA_SWITCHEROO, "ON") == 0;
    }

    bool set_off()
    {
        if (backend_ == "runpm") {
            write_file(AUTOSUSPEND_DELAY, std::to_string(autosuspend_ms_));
            return write_file(POWER_CTRL, "auto") == 0;
        }
        return write_file(VGA_SWITCHEROO, "OFF") == 0;
    }

    // wait_ready po power-on → pełne recover_after_power_on() (callback ustawiony
    // przez Switchd — recovery wymaga hw/gpu/reset_cb, poza zakresem DgpuPower).
    bool wait_ready()
    {
        if (!recover_cb_) {
            logf(0, "switch: wait_ready bez callbacku recovery");
            return false;
        }
        return recover_cb_();
    }

    // Skan /proc/*/fd za otwarte fd na węzłach dGPU (DRM exact + PCM playback
    // audio dGPU). WAŻNE: TYLKO węzły dGPU (rozwiązane przez BDF). W IGD
    // kompozytor (Hyprland) zawsze trzyma fd na iGPU (card1 + renderD128) —
    // matcher "/dev/dri/*" z v5.0 pierwotnego = wiecznie busy (lekcja G1 live).
    // Audio: NIE controlC<N> — wireplumber (manager dźwięku) trzyma control
    // permanentnie, gate by nigdy nie puścił. Blokuje TYLKO aktywny PCM playback
    // (fd na /dev/snd/pcmC<N>D*p); capture (sufiks "c") NIE blokuje.
    // Wzorzec skanu /proc kompilatorów (compiler_running) — bez lsof.
    static bool fd_busy()
    {
        std::set<std::string> nodes = dgpu_drm_nodes();
        std::string snd_card = dgpu_snd_playback_card();
        std::string pcm_prefix = snd_card.empty() ? "" : "/dev/snd/pcmC" + snd_card;

        DIR* d = opendir("/proc");
        if (!d) return false;
        struct dirent* e;
        bool busy = false;
        while ((e = readdir(d))) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            // Daemon nie może blokować sam siebie (własny fd vblank na card<N>).
            if (std::strtol(e->d_name, nullptr, 10) == (long)getpid()) continue;
            std::string fddir = std::string("/proc/") + e->d_name + "/fd";
            DIR* fd = opendir(fddir.c_str());
            if (!fd) continue;
            struct dirent* fe;
            while ((fe = readdir(fd))) {
                if (fe->d_name[0] == '.') continue;
                char link[256];
                std::string lp = fddir + "/" + fe->d_name;
                ssize_t n = readlink(lp.c_str(), link, sizeof link - 1);
                if (n <= 0) continue;
                link[n] = 0;
                std::string target(link);
                if (nodes.count(target)) {
                    busy = true;
                    closedir(fd);
                    closedir(d);
                    return true;
                }
                // Audio dGPU: tylko aktywny PCM playback (pcmC<N>D*p).
                if (!pcm_prefix.empty() && target.rfind(pcm_prefix, 0) == 0 &&
                    !target.empty() && target.back() == 'p') {
                    busy = true;
                    closedir(fd);
                    closedir(d);
                    return true;
                }
            }
            closedir(fd);
        }
        closedir(d);
        return busy;
    }

    // Czekaj aż dGPU nie będzie zajęty (brak otwartych fd). Timeout, retry 500 ms.
    bool wait_idle(int timeout_ms)
    {
        int waited = 0;
        while (waited < timeout_ms) {
            if (!fd_busy()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            waited += 500;
        }
        return !fd_busy();
    }

private:
    std::string backend_;
    int autosuspend_ms_;
    std::function<bool()> recover_cb_;
};

// ----------------------------------------------------------- switchd: konsumenci (/proc)
// v5.0: skan /proc za konsumentów dGPU. Dwa sygnały:
//   - /proc/*/environ zawiera DRI_PRIME=... (render-offload na dGPU)
//   - /proc/*/comm ∈ [dgpu-procs] (CUDA, blender — procesy wymagające dGPU)
// Zwraca listę nazw procesów (comm) — do polityki (dgpu_procs) i statusu.

static std::vector<std::string> dgpu_consumers()
{
    std::vector<std::string> out;
    DIR* d = opendir("/proc");
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        std::string bp = std::string("/proc/") + e->d_name;
        std::string comm;
        bool is_consumer = false;
        if (read_file((bp + "/comm").c_str(), comm) == 0) {
            comm = trim(comm);
            if (!comm.empty() && g_cfg.sw.dgpu_procs.count(comm)) is_consumer = true;
        }
        if (!is_consumer) {
            std::string env;
            if (read_file((bp + "/environ").c_str(), env) == 0) {
                size_t p0 = 0;
                while (p0 < env.size()) {
                    size_t nul = env.find('\0', p0);
                    std::string var = env.substr(p0, nul == std::string::npos
                        ? std::string::npos : nul - p0);
                    if (var.rfind("DRI_PRIME=", 0) == 0) { is_consumer = true; break; }
                    if (nul == std::string::npos) break;
                    p0 = nul + 1;
                }
            }
        }
        if (is_consumer && !comm.empty()) out.push_back(comm);
    }
    closedir(d);
    return out;
}

// Czy na dGPU (card0) jest podłączony zewnętrzny wyświetlacz (poza eDP-1)?
static bool external_display_on_dgpu()
{
    DIR* d = opendir("/sys/class/drm");
    if (!d) return false;
    struct dirent* e;
    bool found = false;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name.rfind("card0-", 0) != 0) continue;
        if (name == "card0-eDP-1") continue;
        std::string status;
        if (read_file(("/sys/class/drm/" + name + "/status").c_str(), status) == 0) {
            if (trim(status) == "connected") { found = true; break; }
        }
    }
    closedir(d);
    return found;
}

// ----------------------------------------------------------- switchd: polityka
// v5.6: promocja tytułowa — karta Discord/YouTube w przeglądarce (tytuł okna
// zawiera wzorzec z [preferred-titles], icontains) → dGPU, ALE NIE zapinka:
// busy < title-idle-busy ([switch], default 33%) przez dwell-out → demote
// do iGPU. Focus na nie-Discord/YT → demote po 1 s (niezależnie od busy).
// Twarda promocja ([dgpu-hard]/external/[dgpu-procs]) = latch.

class SwitchPolicy {
public:
    enum Target { DGPU, IGPU };

    struct Input {
        std::string focused_class;
        // v5.6: tytuł okna — promocja tytułowa Discord/YouTube ([preferred-titles]).
        std::string focused_title;
        std::vector<std::string> consumers;   // dgpu_consumers() — comm nazwy
        uint32_t busy = 0;                    // ‰ (g_last_busy)
        int temp = -1;                        // °C
        bool external_display = false;
    };

    // Decyzja co tick. Zwraca docelowy stan power dGPU.
    Target decide(const Input& in, const Config::SwitchCfg& cfg, int tick_ms)
    {
        auto now = std::chrono::steady_clock::now();
        auto since = [&](std::chrono::steady_clock::time_point tp) {
            return (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - tp).count();
        };

        const bool thermal_ok = (in.temp < 0 || in.temp < cfg.temp_gate);

        // Twarda promocja: focused class ∈ dgpu_hard LUB external LUB dgpu_procs
        // (bez tytułu — v5.6 przeglądarki usunięte z [dgpu-hard]).
        bool hard = false;
        if (!in.focused_class.empty() && cfg.dgpu_hard.count(in.focused_class)) hard = true;
        if (in.external_display) hard = true;
        if (!hard) {
            for (auto& c : in.consumers)
                if (cfg.dgpu_procs.count(c)) { hard = true; break; }
        }

        // v5.6: [dgpu-idle] — klasy z idle-demote (np. mpv): promują do dGPU jak
        // twarde, ALE busy < class_idle_busy → demote do iGPU (symetrycznie z
        // promocją tytułową Discord/YouTube).
        bool idle_class = !in.focused_class.empty() && cfg.dgpu_idle.count(in.focused_class);

        // v5.6: promocja tytułowa — karta Discord/YouTube w przeglądarce → dGPU,
        // ALE NIE jest zapinką: busy < title_idle_busy → demote do iGPU.
        bool title_promo = false;
        if (!in.focused_title.empty()) {
            for (auto& t : cfg.preferred_titles)
                if (icontains(in.focused_title, t)) { title_promo = true; break; }
        }
        // KOREKTA busy_known: busy z PMU jest miarodajny TYLKO gdy dGPU jest ON.
        // Gdy target_==IGPU (dGPU OFF), pętla główna wymusza busy=0 (PMU martwej
        // karty) — to NIE sygnał idle, tylko brak pomiaru. Bez tego zabezpieczenia
        // promocja tytułowa nigdy by nie wystartowała z OFF (idle_title=zawsze).
        const bool busy_known = (target_ == DGPU);
        const bool idle_title = busy_known && (int)in.busy < cfg.title_idle_busy * 10;  // ‰

        // Miękka promocja: focused class ∈ dgpu_soft AND busy > busy_enter.
        bool soft = !in.focused_class.empty() && cfg.dgpu_soft.count(in.focused_class) &&
                    (int)in.busy > cfg.busy_enter * 10;

        // v5.6 demote_sig (uproszczone wg usera): focus na NIE-Discord/YT →
        // demote po 1 s ZAWSZE (niezależnie od busy) — downclock do 07 robi
        // [dgpu-active], power-off robi switchd; busy < busy_exit jest pokryte
        // przez !title_promo. Focus na Discord/YT → demote tylko gdy idle_title
        // (busy_known && busy < title-idle-busy). igpu-class zostaje dla [igpu].
        bool demote_sig = !title_promo || idle_title ||
                          (!in.focused_class.empty() && cfg.igpu.count(in.focused_class));

        // Dwell countery (miękka promocja — busy-gated).
        dwell_in_ = (soft && thermal_ok) ? dwell_in_ + tick_ms : 0;

        // Twarda promocja — latch (jak dotychczas, early return). Sygnał twardy
        // trzyma dGPU NAWET gdy temp >= temp_gate (gate termiczny blokuje promocję,
        // ale nie demotuj pod aktywnym wymaganiem — inaczej flapping).
        if (hard) {
            dwell_out_ = 0;   // promocja kasuje sygnał demote
            // v5.6: [dgpu-idle] — idle-demote (np. mpv): gdy busy_known i busy <
            // class_idle_busy → NIE trzymaj dGPU (fall through do wspólnej ścieżki
            // demote). Gdy dGPU OFF (busy_known=false) → promuj jak twarda
            // (busy nieznane ≠ idle — to samo zabezpieczenie co title_promo).
            bool idle_now = idle_class && busy_known && (int)in.busy < cfg.class_idle_busy * 10;
            if (!idle_now) {
                if (thermal_ok) {
                    if (target_ != DGPU && since(last_demote_) < cfg.cooldown_ms) {
                        // cooldown — czekaj
                    } else {
                        if (target_ != DGPU) last_promote_ = now;
                        target_ = DGPU;
                    }
                }
                // !thermal_ok: nie promuj; trzymaj dGPU jeśli już włączony.
                return target_;
            }
            // idle_now: fall through — demote przez wspólną ścieżkę poniżej.
        }

        // v5.6: promocja tytułowa — aktywna karta Discord/YT → dGPU (cooldown-
        // gated), BEZ early-return (idle może zwolnić). Gdy dGPU OFF (busy nie-
        // znane) promuj na sam tytuł; gdy ON, nie promuj/trzymaj gdy idle
        // (anti-flapping). dwell_out_ zerowany póki aktywna → demote nie tyka.
        if (title_promo && !idle_title) {
            dwell_out_ = 0;
            // v5.6: hold-off po demote — YT na iGPU bez churnu
            if (target_ != DGPU && thermal_ok &&
                since(last_demote_) >= cfg.title_idle_hold_ms) {
                last_promote_ = now;
                target_ = DGPU;
            }
        }

        // Miękka promocja — busy-gated przez dwell_in.
        if (soft && thermal_ok && dwell_in_ >= cfg.dwell_in_ms) {
            dwell_out_ = 0;   // promocja kasuje sygnał demote
            if (target_ != DGPU && since(last_demote_) < cfg.cooldown_ms) {
                // cooldown — czekaj
            } else {
                if (target_ != DGPU) last_promote_ = now;
                target_ = DGPU;
            }
            return target_;
        }

        // Democja — po min-residence, przez dwell_out.
        dwell_out_ = (target_ == DGPU && demote_sig) ? dwell_out_ + tick_ms : 0;
        if (target_ == DGPU && demote_sig &&
            since(last_promote_) >= cfg.min_residence_ms &&
            dwell_out_ >= cfg.dwell_out_ms) {
            target_ = IGPU;
            last_demote_ = now;
            dwell_out_ = 0;
        }

        return target_;
    }

    Target target() const { return target_; }

private:
    Target target_ = IGPU;
    int dwell_in_ = 0, dwell_out_ = 0;
    std::chrono::steady_clock::time_point last_promote_{}, last_demote_{};
};

// ----------------------------------------------------------- switchd: status
// v5.0: NVRAM read gpu-power-prefs (read-only — monitor/status). GUID
// fa4ce28d-b62f-4c99-9cc3-6815686e30f9, dane 4 B: 01 00 00 00 = IGD,
// 00 00 00 00 = DIS. Firmware ukrywa zmienną przed enumeracją efivarfs →
// fallback raw flash (store $VSS 0x610050/0x620050, raporty 51/70).

static const char* NVRAM_EFIVAR =
    "/sys/firmware/efi/efivars/gpu-power-prefs-fa4ce28d-b62f-4c99-9cc3-6815686e30f9";
static const char* NVRAM_FLASH_DUMP =
    "";  // raw flash fallback — machine-specific dump path, see nvram_prefs_flash()

// GUID w formacie binarnym EFI (mixed-endian): fa4ce28d-b62f-4c99-9cc3-6815686e30f9.
static const uint8_t GPREFS_GUID_BIN[16] = {
    0x8d, 0xe2, 0x4c, 0xfa, 0x2f, 0xb6, 0x99, 0x4c,
    0x9c, 0xc3, 0x68, 0x15, 0x68, 0x6e, 0x30, 0xf9
};
// "gpu-power-prefs" w UTF-16LE (16 znaków z null = 32 B).
static const uint8_t GPREFS_NAME_UTF16[32] = {
    0x67,0x00, 0x70,0x00, 0x75,0x00, 0x2d,0x00, 0x70,0x00, 0x6f,0x00,
    0x77,0x00, 0x65,0x00, 0x72,0x00, 0x2d,0x00, 0x70,0x00, 0x72,0x00,
    0x65,0x00, 0x66,0x00, 0x73,0x00, 0x00,0x00
};

static uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Skanuje store VSS (offset, 64 KiB) za aktualną generacją gpu-power-prefs
// (State 0x7F). Zwraca "dis"/"igd"/"".
static std::string nvram_prefs_scan_store(int fd, off_t store_off)
{
    const size_t STORE_SIZE = 0x10000;
    off_t pos = store_off + 16;   // po nagłówku $VSS (16 B)
    off_t end = store_off + (off_t)STORE_SIZE;
    while (pos + 36 <= end) {
        uint8_t hdr[36];
        if (pread(fd, hdr, sizeof hdr, pos) != (ssize_t)sizeof hdr) break;
        // StartId 0xAA55 w bajtach flash = [0xAA, 0x55] (nie LE u16).
        if (hdr[0] != 0xAA || hdr[1] != 0x55) break;   // koniec zmiennych (padding)
        uint8_t state = hdr[2];
        uint32_t name_size = le32(hdr + 8);
        uint32_t data_size = le32(hdr + 12);
        if (memcmp(hdr + 16, GPREFS_GUID_BIN, 16) == 0 &&
            name_size == sizeof GPREFS_NAME_UTF16) {
            uint8_t name[32];
            if (pread(fd, name, sizeof name, pos + 36) == (ssize_t)sizeof name &&
                memcmp(name, GPREFS_NAME_UTF16, sizeof name) == 0) {
                if (state == 0x7F && data_size >= 1) {
                    uint8_t d;
                    if (pread(fd, &d, 1, pos + 36 + name_size) == 1)
                        return d == 1 ? "igd" : "dis";
                }
            }
        }
        pos += 36 + name_size + data_size;
    }
    return "";
}

// Odczyt gpu-power-prefs z raw flash (/dev/mtd0ro lub dump). Zwraca "" gdy brak.
static std::string nvram_prefs_flash()
{
    const char* paths[] = { "/dev/mtd0ro", NVRAM_FLASH_DUMP };
    for (auto* p : paths) {
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        std::string r = nvram_prefs_scan_store(fd, 0x610050);
        if (r.empty()) r = nvram_prefs_scan_store(fd, 0x620050);
        close(fd);
        if (!r.empty()) return r;
    }
    return "";
}

// Odczyt gpu-power-prefs z efivarfs. Zwraca "" gdy niedostępne.
static std::string nvram_prefs_efivarfs()
{
    std::string data;
    if (read_file(NVRAM_EFIVAR, data) != 0) return "";
    if (data.size() < 5) return "";
    uint8_t v = (uint8_t)data[4];   // pierwszy bajt danych (po 4 B atrybutów)
    if (v == 1) return "igd";
    if (v == 0) return "dis";
    return "";
}

// Pełny odczyt: efivarfs → raw flash. Wynik "dis"|"igd"|"unknown".
//
// v5.3: cache do /run/reclockd/nvram-prefs. Skan raw flash (/dev/mtd0ro) przy
// każdym starcie gasi kbd backlight: każdy odczyt MTD → ledtrig_mtd_activity()
// → trigger "nand-disk" na smc::kbd_backlight (applesmc.c:1071) → oneshot blink
// zostawia LED na 0 → LKSB=0 (raport 80). /run to tmpfs — cache resetuje się
// przy reboot, co jest poprawne (gpu-power-prefs zmienia się tylko przez
// firmware przy to-igd/to-dis + reboot). Flash czytany tylko gdy cache brakuje.
static const char* NVRAM_PREFS_CACHE = "/run/reclockd/nvram-prefs";

static std::string nvram_prefs_read()
{
    std::string cached;
    if (read_file(NVRAM_PREFS_CACHE, cached) == 0) {
        cached = trim(cached);
        logf(1, "nvram: cache %s (%s)", NVRAM_PREFS_CACHE, cached.c_str());
        return cached.empty() ? "unknown" : cached;
    }
    logf(1, "nvram: brak cache %s — skan efivarfs → raw flash", NVRAM_PREFS_CACHE);
    std::string r = nvram_prefs_efivarfs();
    if (r.empty()) r = nvram_prefs_flash();
    if (r.empty()) r = "unknown";
    write_file(NVRAM_PREFS_CACHE, r);   // best-effort — cache to tylko optymalizacja
    return r;
}

// iGPU (Intel Iris Pro 5200) — read-only monitoring (Opcja A, zero kontroli).
static int read_igpu_freq_mhz()
{
    std::string s;
    if (read_file(IGPU_RPS_CUR, s) != 0) return -1;
    return std::atoi(s.c_str());
}

static std::string json_escape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

// ----------------------------------------------------------- switchd: wykonawca + weryfikacja

class Switchd {
public:
    Switchd(Gpu& gpu, Hwmon& hw, const Config::SwitchCfg& cfg)
        : gpu_(gpu), hw_(hw), cfg_(cfg), power_(cfg.backend, cfg.autosuspend_ms)
    {
        power_.set_recover_cb([this]() { return recover_after_power_on(); });
    }

    void set_reset_cb(std::function<void()> cb) { reset_cb_ = std::move(cb); }

    bool enabled() const { return cfg_.enable; }

    void init()
    {
        topo_.detect();
        power_state_ = power_.read();
        // v5.3: mkdir przed nvram_prefs_read — katalog /run/reclockd musi istnieć
        // zanim cache nvram-prefs zapisze do niego plik.
        mkdir("/run/reclockd", 0755);
        nvram_prefs_ = nvram_prefs_read();
        mkdir("/run/switchd", 0755);
        logf(1, "switchd: topologia=%s, dGPU=%s, nvram_prefs=%s, backend=%s, "
                "target=%s (w DIS = monitor — zero zmian power)",
             topo_.name(), power_state_.on() ? "ON" : "OFF",
             nvram_prefs_.c_str(), cfg_.backend.c_str(), target_name());
    }

    void tick(HyprCtl& hypr, int temp, uint32_t last_busy)
    {
        // Własne hyprctl (activewindow) — niezależne od pollingu pstate.
        // v5.6: tytuł okna przekazywany do polityki — promocja tytułowa
        // Discord/YouTube ([preferred-titles]) w decide().
        std::string fcs, ftitle;
        bool a1 = hypr.activewindow(fcs, ftitle);

        // v5.0: dgpu-override — flag-file /run/reclockd/dgpu-override (on|off),
        // wzorzec fan-override (G). Plik istnieje → wymusza target, policy.decide()
        // nie rusza. Override "off" nadal przechodzi przez bramki apply():
        // wait_idle (po Bug1 puszcza — tylko węzły dGPU) + set_off + wait_off.
        // NIE bypassuj wait_idle — bramka bezpieczeństwa. "on" — power-on z
        // wait_ready jak normalnie. Status: pole "override" (""|"on"|"off").
        std::string ovr = dgpu_override();
        if (ovr == "on" || ovr == "off") {
            target_ = (ovr == "on") ? SwitchPolicy::DGPU : SwitchPolicy::IGPU;
            override_ = ovr;
            if (prev_override_ != ovr) {
                logf(1, "switch: override — dGPU %s wymuszony (flag-file %s)",
                     ovr == "on" ? "ON" : "OFF", DGPU_OVERRIDE_FILE);
                prev_override_ = ovr;
            }
        } else {
            override_ = "";
            if (!prev_override_.empty()) {
                logf(1, "switch: override ZDJĘTY — wznawiam politykę switchd");
                prev_override_.clear();
            }
            SwitchPolicy::Input in;
            in.focused_class = a1 ? fcs : "";
            in.focused_title = a1 ? ftitle : "";
            in.consumers = dgpu_consumers();
            in.busy = last_busy;
            in.temp = temp;
            in.external_display = external_display_on_dgpu();

            target_ = policy_.decide(in, cfg_, cfg_.tick_ms);
        }
        apply();
        write_status();
    }

    bool dgpu_off() const { return !power_state_.on(); }

    // v5.0: settle po power-on — nie pisz pstate przez pstate_settle_ms po
    // power-cycle (kernel nvkm_pstate_calc może wisieć po D3hot→D0). Ustawiane
    // w recover_after_power_on(); pętla główna pomija decyzję pstate w tym oknie.
    bool pstate_settle_active() const {
        return std::chrono::steady_clock::now() < pstate_settle_until_;
    }
    int pstate_settle_remaining_ms() const {
        auto d = pstate_settle_until_ - std::chrono::steady_clock::now();
        return d.count() > 0
            ? (int)std::chrono::duration_cast<std::chrono::milliseconds>(d).count() : 0;
    }

    const char* topo_name() const { return topo_.name(); }
    const char* target_name() const {
        return target_ == SwitchPolicy::DGPU ? "dgpu" : "igpu";
    }
    const char* mode_name() const {
        if (!cfg_.enable) return "off";
        return topo_.topo() == IGD ? "active (IGD)" : "monitor (DIS)";
    }
    const std::string& nvram_prefs() const { return nvram_prefs_; }

    // v5.4: status [dgpu-active] — wartości ustawiane przez pętlę główną co cykl.
    void set_dgpu_active_status(const char* state, int input_active, int video)
    {
        dgpu_state_ = state ? state : "off";
        dgpu_input_active_ = input_active;
        dgpu_video_ = video;
    }

    // Pełne recovery po power-cycle (kroki 2-10 wg planu). Wywoływane przez
    // wait_ready (po power-on) i przez S3 self-heal w pętli głównej.
    bool recover_after_power_on()
    {
        // Krok 2: wait runtime_status=active (timeout wait_ready_timeout_ms).
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(cfg_.wait_ready_timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            auto st = power_.read();
            if (st.runtime == "active") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        auto st = power_.read();
        if (st.runtime != "active") {
            last_error_ = "runtime_status != active po power-on";
            logf(0, "switch: BŁĄD recovery — runtime_status=%s (oczekiwano active)",
                 st.runtime.c_str());
            rollback_off();
            return false;
        }
        // Krok 3: verify vgaswitcheroo = Pwr.
        if (st.vgasw != "Pwr") {
            last_error_ = "vgaswitcheroo != Pwr po power-on";
            logf(0, "switch: BŁĄD recovery — vgaswitcheroo=%s (oczekiwano Pwr)",
                 st.vgasw.c_str());
            rollback_off();
            return false;
        }
        // Krok 4: verify pstate debugfs istnieje (VBIOS wrócił).
        if (current_state().empty()) {
            last_error_ = "pstate debugfs niedostępny po power-on";
            logf(0, "switch: BŁĄD recovery — pstate debugfs niedostępny");
            rollback_off();
            return false;
        }
        // Krok 5: hw.reinit() — re-scan hwmon nouveau (node zniknął przy power-off).
        hw_.reinit();
        // Krok 6: gpu.reopen_mmio() — świeży mapping BAR0 po power-cycle.
        if (!gpu_.reopen_mmio()) {
            last_error_ = "reopen_mmio nieudany po power-on";
            logf(0, "switch: BŁĄD recovery — reopen_mmio");
            rollback_off();
            return false;
        }
        // Krok 7: gpu.init_counters() — świeży power-cycle = liczniki BAR0 stracone.
        gpu_.init_counters();
        // Krok 8: re-sync g_cur_idx (pstate może być inny niż daemon myśli).
        std::string cur = current_state();
        if (!cur.empty() && cur != "boot") {
            uint32_t cs = (uint32_t)std::strtol(cur.c_str(), nullptr, 16);
            int idx = state_to_idx(cs);
            if (idx >= 0) g_cur_idx = idx;
        }
        // Krok 9: re-open DRM (gdy vblank_sync) — fd może być stale po power-cycle.
        if (g_cfg.vblank_sync) {
            if (g_drm_fd >= 0) { close(g_drm_fd); g_drm_fd = -1; }
            drm_open();
        }
        // Krok 10: reset_after_transition() — świeże okno busy.
        if (reset_cb_) reset_cb_();
        // v5.0: świeży power-cycle = świeży clock subsystem → odblokuj pstate
        // (g_pstate_stuck mógł zostać po hang'u nvkm_pstate_calc) i wstrzymaj
        // zapisy pstate na pstate_settle_ms (GPU musi się ustabilizować po
        // D3hot→D0 — pierwsza zmiana clocka może zawiesić workqueue).
        g_pstate_stuck = false;
        pstate_settle_until_ = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(cfg_.pstate_settle_ms);
        logf(1, "switch: recovery po power-on OK (hwmon=%s, idx=%d, pstate-settle=%dms)",
             hw_.ok() ? hw_.path().c_str() : "BRAK", g_cur_idx, cfg_.pstate_settle_ms);
        return true;
    }

private:
    void apply()
    {
        auto st = power_.read();
        bool cur_on = st.on();
        bool want_on = (target_ == SwitchPolicy::DGPU);

        if (want_on != cur_on) {
            auto now = std::chrono::steady_clock::now();
            auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_switch_).count();
            if (since_last < cfg_.min_switch_gap_ms) {
                logf(2, "switch: min-switch-gap — odraczam toggle (ostatni %dms temu)",
                     (int)since_last);
            } else if (want_on) {
                logf(1, "switch: power-on dGPU (target=DGPU, topologia=%s)", topo_.name());
                if (!power_.set_on()) {
                    last_error_ = "set_on nieudany";
                    logf(0, "switch: BŁĄD set_on");
                } else {
                    last_switch_ = now;
                    if (power_.wait_ready()) {
                        last_action_ = "power-on";
                        last_error_.clear();
                    }
                    // wait_ready fail → last_error_ ustawiony w recovery
                }
            } else {
                if (topo_.topo() != IGD) {
                    logf(1, "switch: monitor — power-off zablokowany (topologia %s)",
                         topo_.name());
                    last_action_ = "blocked-off";
                } else {
                    logf(1, "switch: power-off dGPU (target=IGPU, topologia=IGD)");
                    if (!power_.wait_idle(cfg_.wait_idle_timeout_ms)) {
                        last_error_ = "dGPU zajęty (otwarte fd /dev/dri) — power-off odroczony";
                        logf(0, "switch: %s", last_error_.c_str());
                    } else if (!power_.set_off()) {
                        last_error_ = "set_off nieudany";
                        logf(0, "switch: BŁĄD set_off");
                    } else {
                        last_switch_ = now;
                        if (wait_off()) {
                            last_action_ = "power-off";
                            last_error_.clear();
                        }
                    }
                }
            }
        } else {
            last_action_ = "none";
        }

        power_state_ = power_.read();
    }

    bool wait_off()
    {
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(cfg_.wait_ready_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            auto st = power_.read();
            if (st.runtime == "suspended" || st.vgasw == "Off" || st.vgasw == "DynOff")
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        auto st = power_.read();
        last_error_ = "dGPU nie przeszedł w off po power-off (runtime=" + st.runtime +
                      ", vgasw=" + st.vgasw + ")";
        logf(0, "switch: BŁĄD wait_off — %s", last_error_.c_str());
        return false;
    }

    void rollback_off()
    {
        if (topo_.topo() == IGD) {
            if (power_.set_off())
                logf(0, "switch: rollback — dGPU OFF");
            else
                logf(0, "switch: BŁĄD rollback — set_off nieudany");
        } else {
            logf(0, "switch: rollback pominięty (topologia %s — power-off zablokowany)",
                 topo_.name());
        }
    }

    void write_status()
    {
        auto st = power_.read();
        int igpu_freq = read_igpu_freq_mhz();
        char buf[768];
        // v5.4: "dgpu_state" niesie stan pstate [dgpu-active] (active|deep_idle|
        // heavy|off), gdy sekcja wyłączona → historyczne on/off (power). Power
        // on/off zostaje w "dgpu_power". Nowe pola: input_active (1/0), video (1/0).
        const char* dgpu_state_val = dgpu_state_.c_str();
        if (!g_cfg.dgpu.enable)
            dgpu_state_val = st.on() ? "on" : "off";
        std::snprintf(buf, sizeof buf,
            "{ \"topology\": \"%s\", \"dgpu_power\": \"%s\", \"dgpu_state\": \"%s\", "
            "\"input_active\": %d, \"video\": %d, "
            "\"target\": \"%s\", \"override\": \"%s\", \"last_action\": \"%s\", "
            "\"last_error\": \"%s\", \"nvram_prefs\": \"%s\", \"igpu_freq_mhz\": %d, "
            "\"fan_curve\": \"%s\", \"fan_tmin\": %d, \"fan_tmax\": %d, "
            "\"fan_rpm1\": %d, \"fan_rpm2\": %d, \"ts\": %lld }\n",
            topo_.name(), st.on() ? "on" : "off", dgpu_state_val,
            dgpu_input_active_, dgpu_video_,
            target_name(), json_escape(override_).c_str(),
            json_escape(last_action_).c_str(),
            json_escape(last_error_).c_str(), nvram_prefs_.c_str(), igpu_freq,
            g_fan_curve.c_str(), g_fan_tmin, g_fan_tmax, g_fan_rpm1, g_fan_rpm2,
            (long long)std::time(nullptr));
        write_file(SWITCH_STATUS_FILE, buf);
        write_file(SWITCH_DGPU_FILE, st.on() ? "on" : "off");
    }

    Gpu& gpu_;
    Hwmon& hw_;
    const Config::SwitchCfg& cfg_;
    DgpuPower power_;
    Topology topo_;
    SwitchPolicy policy_;
    std::function<void()> reset_cb_;

    SwitchPolicy::Target target_ = SwitchPolicy::IGPU;
    DgpuPower::State power_state_;
    std::string nvram_prefs_ = "unknown";
    std::string last_action_ = "none";
    std::string last_error_;
    std::chrono::steady_clock::time_point last_switch_{};
    std::chrono::steady_clock::time_point pstate_settle_until_{}; // v5.0: okno settle po power-on
    std::string override_ = "";       // aktywny dgpu-override (""|"on"|"off")
    std::string prev_override_ = "";  // poprzedni — log zmian (wzorzec fan-override)
    // v5.4: [dgpu-active] status — ustawiane przez pętlę główną.
    std::string dgpu_state_ = "off";
    int dgpu_input_active_ = 0;
    int dgpu_video_ = 0;
};

// ----------------------------------------------------------- input (evdev)
// v5.4: detekcja aktywności usera przez /dev/input/event* (evdev). Osobny wątek
// z poll() (timeout 500 ms) — nie kradnie zdarzeń Hyprlandowi: evdev jest
// multicast, każdy czytelnik ma własną kolejkę (raport 79 §2.1). Każdy event
// EV_KEY/EV_REL/EV_ABS ustawia atomowy timestamp last_activity_ms (steady_clock).
// Brak urządzeń / activity-source != evdev → last_activity_ms() = 0 (idle po
// dwell; wake tylko przez busy/title-change). Re-scan przy usuniętym fd.

class InputReader {
public:
    InputReader() = default;
    ~InputReader() { stop(); }

    // Uruchom wątek tylko gdy source == "evdev". Idempotentne: stop() → start()
    // pozwala zrestartować po SIGHUP (zmiana activity-source). Rescan jest
    // synchroniczny — device_count() jest dokładny natychmiast po start().
    void start(const std::string& source)
    {
        stop();
        if (source != "evdev") return;
        rescan();
        running_ = true;
        thread_ = std::thread([this]() { loop(); });
    }

    void stop()
    {
        running_ = false;
        if (thread_.joinable()) thread_.join();   // poll ma timeout 500 ms — join szybki
        std::lock_guard<std::mutex> lk(mu_);
        for (int fd : fds_) close(fd);
        fds_.clear();
    }

    // steady_clock ms ostatniego eventu; 0 = nigdy (brak urządzeń / source != evdev).
    uint64_t last_activity_ms() const
    {
        return last_activity_.load(std::memory_order_relaxed);
    }

    // Liczba otwartych urządzeń evdev (0 = brak sygnału inputu).
    int device_count() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return (int)fds_.size();
    }

private:
    void loop()
    {
        while (running_) {
            std::vector<int> fds;
            {
                std::lock_guard<std::mutex> lk(mu_);
                fds = fds_;
            }
            std::vector<struct pollfd> pfds;
            pfds.reserve(fds.size());
            for (int fd : fds) pfds.push_back({fd, POLLIN, 0});
            int r = poll(pfds.data(), pfds.size(), 500);
            if (r < 0) {
                if (errno == EINTR) continue;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                rescan();
                continue;
            }
            if (r == 0) continue;   // timeout — brak nowych eventów
            bool dropped = false;
            for (size_t i = 0; i < pfds.size(); i++) {
                if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { dropped = true; continue; }
                if (pfds[i].revents & POLLIN) drain(fds[i]);
            }
            if (dropped) rescan();   // urządzenie usunięte — prosty re-open
        }
    }

    void drain(int fd)
    {
        struct input_event ev;
        while (running_) {
            ssize_t n = read(fd, &ev, sizeof ev);
            if (n == (ssize_t)sizeof ev) {
                if (ev.type == EV_KEY || ev.type == EV_REL || ev.type == EV_ABS) {
                    auto now = std::chrono::steady_clock::now();
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();
                    last_activity_.store((uint64_t)ms, std::memory_order_relaxed);
                }
                // EV_SYN / EV_MSC — ignoruj (nie są aktywnością usera)
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;   // kolejka pusta (O_NONBLOCK)
            } else {
                break;   // błąd / usunięte urządzenie
            }
        }
    }

    void rescan()
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int fd : fds_) close(fd);
        fds_.clear();
        glob_t g;
        if (glob("/dev/input/event*", GLOB_NOSORT, nullptr, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) {
                // Pomijaj urządzenia które nie dają się otworzyć (root — większość
                // otworzy się; brak praw / chwilowa niedostępność = po prostu skip).
                int fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd >= 0) fds_.push_back(fd);
            }
            globfree(&g);
        }
    }

    std::atomic<uint64_t> last_activity_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mu_;       // chroni fds_
    std::vector<int> fds_;        // pod mu_
};

// ----------------------------------------------------------- użycie

static void usage(const char* argv0)
{
    std::printf(
        "reclockd v5.6 — polityka profilowa (app-aware) + wentylatory + switchd + [dgpu-active] + switchd tune + override + reload + nvram cache\n"
        "Użycie: %s [opcje]\n"
        "  switchd (v5.0): dGPU power-state + render routing. W DIS = monitor\n"
        "    (zero zmian power). Sekcje [switch]/[dpower]/[dgpu-hard]/[dgpu-soft]\n"
        "    /[igpu]/[dgpu-procs]. Status: /run/reclockd/status.\n"
        "  switchd tune (v5.6): przeglądarki usunięte z [dgpu-hard] — karty\n"
        "    Discord/YouTube promują po tytule ([preferred-titles]); promocja\n"
        "    tytułowa NIE jest zapinką (busy < title-idle-busy → demote).\n"
        "  [dgpu-active] (v5.4): trójstopniowa polityka dla CAŁEGO dGPU-ON:\n"
        "    baseline (0a) / deep idle (07) / heavy (0e). Aktywność usera przez\n"
        "    evdev (/dev/input/event*). 0e WYŁĄCZNIE busy-driven (busy>busy-enter\n"
        "    + temp<temp-up); tytuł/klasa video tylko informacyjnie. [caps] floor\n"
        "    = twarde minimum, low-power ceiling, termalne per-profil. enable=false\n"
        "    → stara logika. Status: dgpu_state/input_active/video.\n"
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
    if (g_cfg.gr_idle_promille < 0) g_cfg.gr_idle_promille = 0;
    if (g_cfg.gr_idle_promille > 1000) g_cfg.gr_idle_promille = 1000;

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

    // v4.2: wentylatory applesmc. init czyta fanN_min/max dynamicznie z sysfs.
    Fan fan;
    bool fan_ok = false;
    if (g_cfg.fan_enable) {
        fan_ok = fan.init();
        if (fan_ok)
            logf(1, "fan: applesmc OK — fan1=%d-%d RPM, fan2=%d-%d RPM, "
                    "krzywa %d-%d°C (dGPU ON) / %d-%d°C (tylko-iGPU)",
                 fan.fan1_min(), fan.fan1_max(), fan.fan2_min(), fan.fan2_max(),
                 g_cfg.fan_temp_min, g_cfg.fan_temp_max,
                 g_cfg.fan_temp_min_igd, g_cfg.fan_temp_max_igd);
        else
            logf(0, "UWAGA: applesmc niedostępny — sterowanie wentylatorami wyłączone (fail-safe)");
    } else {
        logf(1, "fan: wyłączony w configu (enable=false)");
    }

    // v4.6: detekcja kompilatorów (boost wentylatorów). Status przy starcie.
    if (g_cfg.compiler_enable)
        logf(1, "compiler: detekcja /proc AKTYWNA (boost fan-max=%d%%)",
             g_cfg.compiler_fan_max);
    else
        logf(1, "compiler: wyłączony w configu (enable=false)");

    std::string old_pm;
    bool pm_saved = read_file(POWER_CTRL, old_pm) == 0;
    if (pm_saved) write_file(POWER_CTRL, "on");

    auto restore = [&]() {
        if (!g_cfg.dry) {
            if (set_pstate((uint32_t)g_cfg.exit_state) == 0)
                logf(1, "wyjście: pstate -> %s", state_hex(g_cfg.exit_state));
        }
        // v4.2: zwróć wentylatory do auto SMC (fail-safe — nie zostawiaj manual).
        if (fan_ok) fan.restore_auto();
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

    // v5.4: [dgpu-active] — stan maszyny (status + sygnały).
    // last_activity_ts = steady_clock ms ostatniej aktywności (evdev input /
    // zmiana tytułu/klasy / busy >= deep-idle-busy); 0 = brak aktywności nigdy.
    uint64_t last_activity_ts = 0;
    std::string prev_focused_class;
    std::string prev_focused_title;
    std::string dgpu_state_str = "off";   // "off"|"active"|"deep_idle"|"heavy"
    int dgpu_input_active = 0;
    int dgpu_video = 0;
    bool prev_title_video = false;   // log informacyjny zmiany statusu video

    auto reset_after_transition = [&]() {
        ring.clear();
        temp_low_dwell = temp_high_dwell = idle_dwell = 0;
        boost_up_dwell = 0;
    };

    // v5.0: moduł switchd — dGPU power-state + render routing.
    Switchd sw(gpu, hw, g_cfg.sw);
    sw.set_reset_cb(reset_after_transition);
    sw.init();

    // v5.4: detekcja aktywności usera przez evdev (osobny wątek).
    InputReader input;
    std::string current_input_source = g_cfg.dgpu.activity_source;
    input.start(current_input_source);

    // Stan profilu + rate-limit.
    bool pref_active = false;
    int pref_dwell = 0, def_dwell = 0;

    // Override stan (loguj wejście/wyjście).
    bool prev_override = false;

    // Polling hyprctl: co poll_ms (licznik cykli).
    int poll_cycles = std::max(1, g_cfg.poll_ms / g_cfg.interval_ms);
    int cycle = 0;
    std::string focused_class;
    std::string focused_title;   // v4.1: do detekcji Discord/YouTube po tytule
    std::vector<std::string> running_classes;
    bool hypr_alive = hypr_ok;
    // v4.2: poprzednie RPM do logowania zmian (poziom 1) vs co-1s log (poziom 2).
    int prev_fan_rpm1 = -1, prev_fan_rpm2 = -1;
    bool prev_fan_override = false;

    // v5.4: [dgpu-active] — status w start logu.
    std::string dgpu_active_log = "off";
    if (g_cfg.dgpu.enable) {
        dgpu_active_log = std::string("tak baseline=") + state_hex(g_cfg.dgpu.baseline) +
                          " max=" + state_hex(g_cfg.dgpu.max) +
                          " evdev=" + std::to_string(input.device_count()) + " urz.";
    }

    logf(1, "start v5.6: interval=%dms poll=%dms, "
            "default[cap=%s temp-up=%d temp-down=%d], "
            "preferred[cap=%s boost=%s busy-boost=%d%% boost-dwell=%dms "
            "temp-up=%d temp-down=%d], "
            "busy-up=%d%% busy-down=%d%%, temp-dwell=%dms idle-dwell=%dms, "
            "profile-dwell=%dms, hwmon=%s, hypr=%s, vblank=%d, "
            "gr-idle=%d‰, preferred-titles=%zu, low-power=%zu, "
            "fan=%s temp[%d-%d] igd[%d-%d] fan1[%d-%d] fan2[%d-%d], stan=%s, switch=%s, "
            "dgpu-active=%s",
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
         g_cfg.gr_idle_promille,
         g_cfg.preferred_titles.size(), g_cfg.low_power_classes.size(),
         fan_ok ? "tak" : (g_cfg.fan_enable ? "BRAK" : "off"),
         g_cfg.fan_temp_min, g_cfg.fan_temp_max,
         g_cfg.fan_temp_min_igd, g_cfg.fan_temp_max_igd,
         fan.fan1_min(), fan.fan1_max(), fan.fan2_min(), fan.fan2_max(),
         cur.c_str(), sw.mode_name(), dgpu_active_log.c_str());

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

        int temp = hw.read_temp();   // v5.0: przeniesione wyżej (współdzielone)

        // v5.4: restart InputReadera gdy SIGHUP zmienił activity-source.
        if (current_input_source != g_cfg.dgpu.activity_source) {
            logf(1, "input: activity-source %s -> %s (restart czytnika)",
                 current_input_source.c_str(), g_cfg.dgpu.activity_source.c_str());
            current_input_source = g_cfg.dgpu.activity_source;
            input.start(current_input_source);
        }

        // v5.0: switchd tick (co poll_cycles) — ZAWSZE, niezależnie od stanu dGPU.
        // Własne hyprctl (activewindow) + g_last_busy + temp.
        if (sw.enabled() && (cycle % poll_cycles) == 0)
            sw.tick(hypr, temp, g_last_busy);

        // v5.0: fan block (co poll_cycles) — PRZENIESIONY przed gate (działa też
        // gdy dGPU OFF; temp=-1 gdy hwmon nouveau zniknął → krzywa min, compiler
        // boost działa).
        if (fan_ok && g_cfg.fan_enable && (cycle % poll_cycles) == 0) {
            bool fov = fan_override_active();
            if (fov && !prev_fan_override)
                logf(1, "fan-override AKTYWNY: hold wentylatorów (flag=/run/reclockd/fan-override)");
            if (!fov && prev_fan_override)
                logf(1, "fan-override ZDJĘTY — wznawiam auto wentylatorów");
            prev_fan_override = fov;
            if (!fov) {
                // v4.6: kompilator → boost. Gdy wykryty uruchomiony kompilator
                // (clang/gcc/make/cmake/... — skan /proc, compiler_running()),
                // wentylatory na fan-max% (default 100 = pełne wiatraki). Boost
                // TYLKO w auto-mode — fan-override flag ma priorytet (nie
                // nadpisuj ręcznego sterowania). Gdy kompilator zniknie →
                // normalna ścieżka: krzywa temp poniżej.
                bool boost_on = false;
                std::string cname;
                if (g_cfg.compiler_enable) {
                    cname = compiler_running();
                    boost_on = !cname.empty();
                }
                // v5.0 G1: reaguj na najgorętsze źródło — dGPU gdy ON, CPU (coretemp) gdy OFF
                // (lekcja: temp=-1 przy dGPU OFF → krzywa min, CPU 58°C się grzeje).
                int ft = temp;
                int ctemp = hw.read_cpu_temp();
                if (ctemp > ft) ft = ctemp;
                // v5.1: aktywna krzywa — standardowa (dGPU ON) albo igd (tylko-iGPU,
                // dGPU OFF → temp=CPU). Wybór wg stanu power dGPU (nie topologii).
                int tmin = g_cfg.fan_temp_min;
                int tmax = g_cfg.fan_temp_max;
                bool igd_curve = sw.enabled() && sw.dgpu_off();
                if (igd_curve) { tmin = g_cfg.fan_temp_min_igd; tmax = g_cfg.fan_temp_max_igd; }
                // v5.1: zapamiętaj stan dla statusu (aktualna krzywa + obroty).
                if (boost_on) {
                    fan.set_boost(g_cfg.compiler_fan_max);
                    g_fan_curve = "compiler";
                } else {
                    fan.set(ft, tmin, tmax);
                    g_fan_curve = igd_curve ? "igd" : "dga";
                    g_fan_tmin = tmin; g_fan_tmax = tmax;
                }
                int r1 = fan.last_rpm1(), r2 = fan.last_rpm2();
                g_fan_rpm1 = r1; g_fan_rpm2 = r2;
                if (r1 != prev_fan_rpm1 || r2 != prev_fan_rpm2) {
                    if (boost_on)
                        logf(1, "fan: KOMPILATOR wykryty (%s) -> fan1=%d fan2=%d RPM (boost %d%%)",
                             cname.c_str(), r1, r2, g_cfg.compiler_fan_max);
                    else
                        logf(1, "fan: temp=%d°C -> fan1=%d fan2=%d RPM (krzywa %d-%d°C%s)",
                             ft, r1, r2, tmin, tmax, igd_curve ? " igd" : "");
                    prev_fan_rpm1 = r1; prev_fan_rpm2 = r2;
                } else if (g_cfg.verbosity >= 2) {
                    if (boost_on)
                        logf(2, "fan: KOMPILATOR wykryty (%s) fan1=%d fan2=%d RPM (bez zmian, boost %d%%)",
                             cname.c_str(), r1, r2, g_cfg.compiler_fan_max);
                    else
                        logf(2, "fan: temp=%d°C fan1=%d fan2=%d RPM (bez zmian)",
                             ft, r1, r2);
                }
            } else {
                // v5.1: override aktywny — daemon zamrożony, stan dla statusu.
                g_fan_curve = "override";
            }
        }

        // v5.0: gate pstate — gdy dGPU OFF, pomiń sample() (BAR0 po power-cut =
        // śmieci = 1000‰ = boost do 0e na martwej karcie; lekcja raportu 63).
        if (sw.enabled() && sw.dgpu_off()) {
            g_last_busy = 0;   // zero phantom busy — inaczej g_last_busy zamrożone
                               // na ostatniej wartości (np. 1000‰) i switchd tick
                               // (miękka promocja busy-gated) widzi busy z martwej
                               // karty. Po fixie on() (= vgasw Off) to realny stan.
            // v5.4: status — dGPU OFF, [dgpu-active] nieaktywny.
            if (g_cfg.dgpu.enable) {
                dgpu_state_str = "off"; dgpu_input_active = 0; dgpu_video = 0;
                sw.set_dgpu_active_status(dgpu_state_str.c_str(), dgpu_input_active, dgpu_video);
            }
            cycle++; continue;
        }

        // v5.0: settle po power-on — nie pisz pstate przez pstate_settle_ms po
        // power-cycle (kernel nvkm_pstate_calc może wisieć po D3hot→D0; pierwsza
        // zmiana clocka może zawiesić workqueue → następny zapis wisi w D state).
        if (sw.enabled() && sw.pstate_settle_active()) {
            if (g_cfg.verbosity >= 2)
                logf(2, "pstate: settle po power-on (%dms) — pomijam decyzję",
                     sw.pstate_settle_remaining_ms());
            // v5.4: status — nowa logika wstrzymana do końca settle.
            if (g_cfg.dgpu.enable) {
                dgpu_state_str = (g_cur_idx >= 0 && g_cur_idx < LADDER_N)
                                 ? dgpu_state_name(g_cur_idx, g_cfg.dgpu) : "settle";
                sw.set_dgpu_active_status(dgpu_state_str.c_str(), dgpu_input_active, dgpu_video);
            }
            cycle++; continue;
        }

        uint32_t b = gpu.sample();
        g_last_busy = b;

        // v4.5: samouzdrawianie po S3. Po suspend/resume (deep) GPU traci
        // konfigurację liczników busy PMU w BAR0 — total nie zlicza, sample()
        // zwraca stale 1000‰, co blokuje IDLE downshift i wiecznie odracza DOWN
        // przez GR-idle gate (daemon zostaje w 0e; raport 63). Readback
        // R_IDLE_CTRL jest tani (mmap, co interval_ms) i bez fałszywych alarmów:
        // CTRL_VALUE_ALWAYS to stan normalny, każda inna wartość = konfiguracja
        // stracona (typowy stan po resume). Po re-init liczniki zliczają ponownie
        // i daemon sam schodzi do poprawnego idle.
        uint32_t ctrl_v = gpu.rd(R_IDLE_CTRL + C_TOTAL * 16);
        if ((ctrl_v & CTRL_VALUE_MASK) != CTRL_VALUE_ALWAYS) {
            logf(0, "PMU busy counters config lost (post-resume?) — full recovery (ctrl=%08x)", ctrl_v);
            // v5.0: pełne recovery (kroki 2-10) — liczniki to dopiero początek;
            // power-cycle wymaga też hw.reinit, re-sync g_cur_idx, re-open DRM.
            if (sw.enabled())
                sw.recover_after_power_on();
            else {
                gpu.init_counters();
                reset_after_transition();   // ring.clear() + dwell=0 — świeże okno busy
            }
        }

        ring.push(b);
        uint32_t busy_avg = ring.avg(); // ‰

        // v4.3: title-match (Discord/YouTube po tytule okna) = najwyższy priorytet
        // sygnału preferred. Ustawiane w sekcji pref_sig poniżej; reset co cykl.
        // Gdy true: UP do ceiling bez busy-gate + suppress IDLE downshift (TERMAL
        // zostaje — termalne > title). Patrz UP-LOAD i IDLE DOWN.
        bool title_pref = false;

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
        // v4.1 Bug-fix: gdy !hypr_alive (daemon startował przed sesją Hyprlanda
        // — typowe dla usługi systemd), cyklicznie re-detect co poll_ms. Wcześniej
        // blok był zgated `if (hypr_alive && ...)` — gdy start zawiodło, re-detect
        // nigdy nie odpalał i demon zostawał na default cap=07 na całą sesję.
        if ((cycle % poll_cycles) == 0) {
            if (hypr_alive) {
                std::string fcs, ftitle;
                std::vector<std::string> rcs;
                bool a1 = hypr.activewindow(fcs, ftitle);
                bool a2 = hypr.clients(rcs);
                if (a1) { focused_class = fcs; focused_title = ftitle; }
                else    { focused_class.clear(); focused_title.clear(); }
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
            } else {
                // !hypr_alive: cykliczny re-detect (sesja może wstać po starcie demona).
                if (hypr.detect()) {
                    hypr.apply_env();
                    hypr_alive = true;
                    logf(1, "hyprctl detected: uid=%d HIS=%s (sesja gotowa)",
                         hypr.uid(), hypr.his().c_str());
                    // Natychmiastowy poll tego cyklu — pref_sig nie czeka kolejny poll_ms.
                    std::string fcs, ftitle;
                    std::vector<std::string> rcs;
                    if (hypr.activewindow(fcs, ftitle)) { focused_class = fcs; focused_title = ftitle; }
                    if (hypr.clients(rcs)) running_classes = rcs;
                }
                // else: zostań !hypr_alive, retry next poll_ms.
            }
        }
        cycle++;

        // Sygnał preferred.
        // v4.1: low-power gate — gdy okno z focusem to terminal (klasa ∈
        // low_power_classes), wymuś default (cap=07) z priorytetem nad preferred.
        // Nawet jeśli Discord/YouTube generuje busy w tle (running-busy), terminal
        // z focusem trzyma 07.
        bool low_power_focused = hypr_alive && !focused_class.empty() &&
                                 g_cfg.low_power_classes.count(focused_class) > 0;
        // v4.4: klasa z wpisem [caps] — per-klasowa polityka (floor/max/busy-up).
        // Stosowana gdy klasa jest w FOCUS; karta przeglądarki (chromium itd.)
        // nie ma wpisu → title-priority force-0e bez zmian.
        const Config::ClassCap* ccap = nullptr;
        if (hypr_alive && !focused_class.empty()) {
            auto cit = g_cfg.class_caps.find(focused_class);
            if (cit != g_cfg.class_caps.end()) ccap = &cit->second;
        }
        bool cap_class_focused = (ccap != nullptr);

        bool pref_sig = false;
        if (hypr_alive && !low_power_focused) {
            // focused ∈ lista ([preferred] lub [caps])?
            if (!focused_class.empty() &&
                (g_cfg.preferred_classes.count(focused_class) ||
                 cap_class_focused))
                pref_sig = true;
            // running ∈ lista ([preferred]) AND busy > busy_up?  [caps] tylko gdy focused.
            if (!pref_sig && (int)busy_avg > busy_up_pp) {
                for (auto& r : running_classes) {
                    if (g_cfg.preferred_classes.count(r)) { pref_sig = true; break; }
                }
            }
            // v4.1: focused.title zawiera wzorzec z preferred_titles? (Discord/YouTube
            // są kartami w przeglądarce — detekcja po tytule okna, case-insensitive.)
            // v4.3: title-match ma NAJWYŻSZY priorytet — ustawia title_pref=true, co
            // wymusza UP do ceiling bez busy-gate i suppress IDLE (patrz UP-LOAD/IDLE).
            // v4.4: pomiń title-match dla klas z [caps] — desktop Discord jest
            // obsługiwany przez [caps] (floor/busy-gate), nie force-0e po tytule.
            // Karta Discord w przeglądarce (chromium itd., NIE w [caps]) zachowuje
            // title-priority force-0e.
            // v5.4: przy [dgpu-active] tytuł NIE ustawia title_pref (force-0e znika) —
            // zostaje kwalifikatorem video (obliczanym niżej); pref_sig (profil dla
            // progów termalnych) nadal dostaje tytuł jako fallback dla przeglądarek
            // spoza [preferred].
            if (!focused_title.empty() && !cap_class_focused) {
                for (auto& t : g_cfg.preferred_titles) {
                    if (icontains(focused_title, t)) {
                        pref_sig = true;
                        if (!g_cfg.dgpu.enable) title_pref = true;
                        break;
                    }
                }
            }
        }
        if (pref_sig) { pref_dwell += g_cfg.interval_ms; def_dwell = 0; }
        else          { def_dwell += g_cfg.interval_ms; pref_dwell = 0; }

        if (!pref_active && pref_dwell >= g_cfg.profile_dwell_ms) {
            pref_active = true; pref_dwell = 0;
            logf(1, "profil -> PREFERRED (focused=%s, title=%s, busy=%.0f%%, title-pref=%s)",
                 focused_class.c_str(), focused_title.c_str(), busy_avg/10.0f,
                 title_pref ? "TAK" : "nie");
        } else if (pref_active && def_dwell >= g_cfg.profile_dwell_ms) {
            pref_active = false; def_dwell = 0;
            logf(1, "profil -> DEFAULT (def_dwell=%dms)", g_cfg.profile_dwell_ms);
        }

        const Profile& prof = pref_active ? g_cfg.preferred : g_cfg.def;
        int ceiling = state_to_idx(prof.max_pstate);
        // v4.4: [caps] — ceiling (max), floor (stan spoczynkowy) i własny busy-up.
        int cap_floor = -1;                       // idx w LADDER; -1 = brak
        // v5.4: przy [dgpu-active] generalny próg UP-LOAD = busy-enter z sekcji
        // (default 80), nie globalny busy-up. [caps] busy-up nadal per-klasowo.
        int class_busy_up_pp = g_cfg.dgpu.enable ? g_cfg.dgpu.busy_enter * 10
                                                 : busy_up_pp; // ‰ busy do UP-LOAD
        if (ccap) {
            if (ccap->max >= 0) {
                int max_idx = state_to_idx(ccap->max);
                if (max_idx >= 0 && max_idx < ceiling) ceiling = max_idx;
            }
            if (ccap->floor >= 0) {
                int fl = state_to_idx(ccap->floor);
                if (fl >= 0 && fl <= ceiling) cap_floor = fl;
            }
            if (ccap->busy_up > 0) class_busy_up_pp = ccap->busy_up * 10;
        }
        int busy_boost_pp = prof.busy_boost * 10;

        // boost aktywny gdy: zdefiniowany, poprawny, OFF-LADDER (state_to_idx<0,
        // czyli 0f) — on-ladder boost traktujemy jako wyłączony (to nie ma sensu
        // jako tier nad drabinką). Prof_ok już to walidował na starcie.
        const bool boost_enabled = (prof.boost_pstate >= 0 &&
                                    known_state(prof.boost_pstate) &&
                                    state_to_idx(prof.boost_pstate) < 0);

        // v5.4: [dgpu-active] — sygnały + aktywacja. title_video jest INFORMACYJNY
        // (status/log „video: ...", decyzja usera #4) — NIE decyduje o stanie
        // pstate: 0e wchodzi wyłącznie przez busy > busy-enter (+ termalne).
        bool title_video = false;
        if (g_cfg.dgpu.enable && hypr_alive && !cap_class_focused) {
            if (!focused_title.empty()) {
                for (auto& t : g_cfg.preferred_titles)
                    if (icontains(focused_title, t)) { title_video = true; break; }
            }
            if (!title_video && !focused_class.empty() &&
                g_cfg.dgpu.video_classes.count(focused_class))
                title_video = true;
        }

        // v5.4: tytuł video = INFORMACYJNY — log przy zmianie (decyzja usera #4).
        if (g_cfg.dgpu.enable && title_video != prev_title_video) {
            logf(1, "dgpu-active: video=%s (tytuł: \"%s\", klasa: %s) — informacyjnie, "
                    "0e wyłącznie przez busy",
                 title_video ? "TAK" : "NIE",
                 focused_title.c_str(), focused_class.c_str());
            prev_title_video = title_video;
        }

        // v5.4: last_activity_ts — źródła aktywności: evdev input, zmiana
        // tytułu/klasy okna, busy >= deep-idle-busy (GPU renderuje — animowana
        // strona bez scrolla trzyma floor 0a). no_input_ms to czas od
        // najpóźniejszego z nich; floor spada do 07 gdy ≥ dwell.
        uint64_t now_ms = 0;
        uint64_t no_input_ms = 0;   // 0 gdy aktywność świeża; UINT64_MAX gdy nigdy
        if (g_cfg.dgpu.enable) {
            auto now = std::chrono::steady_clock::now();
            now_ms = (uint64_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(now.time_since_epoch()).count();
            uint64_t ia = input.last_activity_ms();
            if (ia > last_activity_ts) last_activity_ts = ia;
            if (focused_class != prev_focused_class || focused_title != prev_focused_title) {
                last_activity_ts = now_ms;
                prev_focused_class = focused_class;
                prev_focused_title = focused_title;
            }
            if ((int)busy_avg >= g_cfg.dgpu.deep_idle_busy * 10)
                last_activity_ts = now_ms;
            no_input_ms = (last_activity_ts == 0)
                          ? UINT64_MAX : (now_ms > last_activity_ts ? now_ms - last_activity_ts : 0);
        }

        // v5.4: termalne [dgpu-active] — per-profil (wg focusa) lub wspólne.
        // Gdy sekcja wyłączona: td/tu = wartości profilu (temp-per-profile=true
        // default) — identycznie jak dotychczas.
        const int td = g_cfg.dgpu.temp_per_profile ? prof.temp_down : g_cfg.dgpu.temp_down;
        const int tu = g_cfg.dgpu.temp_per_profile ? prof.temp_up   : g_cfg.dgpu.temp_up;

        // Dwell-countery.
        if (temp >= 0) {
            temp_low_dwell  = (temp <  tu) ? temp_low_dwell  + g_cfg.interval_ms : 0;
            temp_high_dwell = (temp >  td) ? temp_high_dwell + g_cfg.interval_ms : 0;
        } else {
            temp_low_dwell = temp_high_dwell = 0;
        }
        // v5.4: próg IDLE — przy [dgpu-active] busy-exit z sekcji (default 40),
        // inaczej globalny busy-down (40). Histereza 80/40 w obu przypadkach.
        const int idle_down_pp = g_cfg.dgpu.enable ? g_cfg.dgpu.busy_exit * 10
                                                   : busy_down_pp;
        idle_dwell = ((int)busy_avg <= idle_down_pp)
                     ? idle_dwell + g_cfg.interval_ms : 0;
        boost_up_dwell = ((int)busy_avg >  busy_boost_pp)
                         ? boost_up_dwell + g_cfg.interval_ms : 0;

        // Jeśli bieżący stan poza drabinką (np. 0a, boot) → wymuś przejście
        // na najniższy (07) w następnym kroku decyzji.
        if (g_cur_idx < 0) {
            // v4.1: init to DOWN do 07 — gate'owane GR-idle (init mid-render ryzykowne).
            if (!g_cfg.dry && !gr_idle_ok(b)) {
                logf(2, "GR-idle gate: defer init -> 07 (busy=%d%% > %d%%)",
                     b / 10, g_cfg.gr_idle_promille / 10);
                continue;
            }
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
            // v5.4: status — stan po init (deep_idle 07).
            if (g_cfg.dgpu.enable) {
                dgpu_state_str = dgpu_state_name(g_cur_idx, g_cfg.dgpu);
                sw.set_dgpu_active_status(dgpu_state_str.c_str(), dgpu_input_active, dgpu_video);
            }
            continue;
        }

        int target = g_cur_idx;
        const char* reason = nullptr;
        bool next_boost = g_boost_active;
        // v4.1: czy ta transycja wymaga GR-idle przed zapisem? DOWN (spadek zegara
        // pamięci) = true (ryzyko wedge'a GR mid-render); UP-LOAD/BOOST-UP = false
        // (rosnący zegar bezpieczniejszy; UP wymaga busy>80% więc gate blokowałby).
        bool needs_gr_idle = false;
        // v5.4: [dgpu-active] — ceiling/floor dynamiczne (do logu verbose).
        int dgpu_ceiling = -1, dgpu_floor = -1;

        // ---- v5.4: [dgpu-active] — trójstopniowa maszyna stanów dla dGPU-ON ----
        // Gdy enable: ceiling/floor DYNAMICZNE dla wszystkich aplikacji (raport 79
        // §3.6 + decyzje usera 2026-08-28). Boost (0f) wyłączony; title_pref nie
        // istnieje (tytuł = INFORMACYJNY — nie decyduje o 0e). 0e wyłącznie przez
        // busy > busy-enter + margines termalny. Kolejność gałęzi: WAKE → TERMAL →
        // IDLE → CEILING → UP-FLOOR → UP-LOAD (TERMAL > DOWN > UP, jak stara
        // logika). Stany: DEEP_IDLE(07) / ACTIVE(baseline 0a) / HEAVY(max 0e).
        // Przejścia zawsze o 1 poziom (nigdy skok 07→0e). GR-idle gate chroni
        // wszystkie DOWN.
        if (g_cfg.dgpu.enable) {
            const int baseline_idx = state_to_idx(g_cfg.dgpu.baseline);
            const int max_idx      = state_to_idx(g_cfg.dgpu.max);

            // --- ceiling (dynamiczny, uogólniony) ---
            //   low-power focus (terminal) → low-power-ceiling (0a) — tło nie
            //     dostaje 0e (decyzja usera #7)
            //   busy > busy-enter ([caps] busy-up per-klasowo) sustained → max (0e)
            //   inaczej → baseline (0a) — 0e WYŁĄCZNIE busy-driven (decyzja usera #2)
            int ceiling_dg;
            if (low_power_focused)
                ceiling_dg = state_to_idx(g_cfg.dgpu.low_power_ceiling);
            else if ((int)busy_avg > class_busy_up_pp)
                ceiling_dg = max_idx;
            else
                ceiling_dg = baseline_idx;
            if (ccap && ccap->max >= 0) {   // [caps] własny ceiling — respektuj
                int m = state_to_idx(ccap->max);
                if (m >= 0 && m < ceiling_dg) ceiling_dg = m;
            }
            if (ceiling_dg < 0) ceiling_dg = 0;

            // --- floor (dynamiczny, wspólny dla wszystkich apk) ---
            //   aktywność (input evdev / świeży tytuł / busy >= deep-idle-busy) →
            //     baseline (0a) — YouTube (~25-36% busy) trzyma 0a
            //   brak aktywności ≥ activity-dwell-ms ORAZ busy < deep-idle-busy →
            //     07 (deep idle) — 480p (busy < 20%) może zejść do 07 (decyzja #3)
            //   [caps] floor = TWARDE minimum: floor = max(cap_floor, dynamic)
            bool floor_active = (last_activity_ts != 0) &&
                                no_input_ms < (uint64_t)g_cfg.dgpu.activity_dwell_ms;
            bool busy_above_wake = (int)busy_avg >= g_cfg.dgpu.deep_idle_busy * 10;
            int floor_dynamic;
            if (floor_active || busy_above_wake)
                floor_dynamic = baseline_idx;
            else if (no_input_ms >= (uint64_t)g_cfg.dgpu.activity_dwell_ms)
                floor_dynamic = 0;              // 07 — deep idle
            else
                floor_dynamic = baseline_idx;   // dwell nie minął — trzymaj baseline
            int floor = std::max(cap_floor, floor_dynamic);
            dgpu_ceiling = ceiling_dg;
            dgpu_floor = floor;

            // --- WAKE (W1): szybka ścieżka 07→0a (≤1 s) po aktywności. Nie czeka
            // na temp_low_dwell (responsywność — decyzja usera #4); gate: temp <
            // temp_down (jedna próbka) — nie budź się w trakcie throttle termicznego.
            if (g_cur_idx < floor && floor >= baseline_idx &&
                temp >= 0 && temp < td) {
                target = g_cur_idx + 1;
                reason = "WAKE-ACTIVITY";
                needs_gr_idle = false;
            }
            // --- DOWN ---
            else if (temp >= 0 && temp_high_dwell >= g_cfg.temp_dwell_ms && g_cur_idx > 0) {
                target = g_cur_idx - 1;
                reason = "TERMAL";               // T1/T2 — nadrzędne (może zejść poniżej floor)
                needs_gr_idle = true;
            }
            // D2/D3 (IDLE): g_cur_idx > floor — zejście na floor. 0e→0a gdy
            // busy spadł (ceiling wrócił do baseline); 0a→07 gdy floor spadł
            // po bezczynności (no-input ≥ activity-dwell ORAZ busy < deep-idle-busy).
            else if (g_cur_idx > 0 && idle_dwell >= g_cfg.idle_dwell_ms &&
                     g_cur_idx > floor) {
                target = g_cur_idx - 1;
                reason = "IDLE";
                needs_gr_idle = true;
            }
            else if (g_cur_idx > ceiling_dg) {
                target = g_cur_idx - 1;
                reason = "CEILING";
                needs_gr_idle = true;
            }
            // --- UP ---
            // UP-FLOOR: powrót do floor (np. [caps] 0a po TERMAL; lub 07→0a gdy
            // WAKE nie przeszedł przez temp). Wymaga temp_low_dwell (bezpieczne).
            else if (floor >= 0 && g_cur_idx < ceiling_dg && g_cur_idx < floor &&
                     temp >= 0 && temp_low_dwell >= g_cfg.temp_dwell_ms) {
                target = g_cur_idx + 1;
                reason = "UP-FLOOR";
            }
            // W3 (UP-LOAD): busy > busy-enter sustained (80%; [caps] busy-up
            // per-klasowo) ORAZ temp < temp-up (margines termalny). Jedyna droga
            // do 0e. UP NIE gate'owane GR-idle.
            else if (g_cur_idx < ceiling_dg && temp >= 0 &&
                     temp_low_dwell >= g_cfg.temp_dwell_ms &&
                     (int)busy_avg > class_busy_up_pp) {
                target = g_cur_idx + 1;
                reason = "UP-LOAD";
            }

            // dgpu-active nie używa boost — gdyby daemon był na 0f (przejście z
            // trybu bez dgpu-active po SIGHUP reload), zejdź na drabinkę.
            if (g_boost_active) {
                next_boost = false;
                target = std::min(ceiling_dg, LADDER_N - 1);
                reason = "DGPU-ACTIVE-EXIT-BOOST";
                needs_gr_idle = true;
            }

            // Status (pisany co poll_cycles przez sw.tick → write_status).
            // video = INFORMACYJNE (z tytułu, decyzja usera #4) — nie decyduje
            // o stanie; dgpu_state wybiera busy. input_active = CZYSTY input
            // evdev (diagnostyczny) — floor używa złożonego floor_active
            // (input + zmiana tytułu + busy ≥ deep-idle-busy).
            uint64_t ev_last = input.last_activity_ms();
            bool ev_active = (ev_last != 0) && now_ms > ev_last &&
                             (now_ms - ev_last) < (uint64_t)g_cfg.dgpu.activity_dwell_ms;
            dgpu_input_active = ev_active ? 1 : 0;
            dgpu_video = title_video ? 1 : 0;
            dgpu_state_str = (g_cur_idx >= 0 && g_cur_idx < LADDER_N)
                             ? dgpu_state_name(g_cur_idx, g_cfg.dgpu) : "off";
            sw.set_dgpu_active_status(dgpu_state_str.c_str(), dgpu_input_active, dgpu_video);
        } else {
        // ---- BOOST TIER (0f) — nad drabinką, straż termiczna priorytetowa ----
        if (g_boost_active) {
            // EXIT boost — TERMAL (NATYCHMIAST, priorytet nad loadem):
            //   temp > temp_up (58°C) LUB temp >= temp_down (65°C) → drop z 0f.
            //   0f = najgorętszy pstate = pierwszy do ścięcia.
            if (temp >= 0 && (temp > prof.temp_up || temp >= prof.temp_down)) {
                next_boost = false;
                target = std::min(ceiling, LADDER_N - 1); // powrót na 0e (lub niżej)
                reason = "BOOST-TERMAL";
                needs_gr_idle = true;            // DOWN z 0f
            }
            // EXIT boost — CEILING: profil obniżył cap poniżej szczytu drabinki
            // (np. preferred→default). Zejdź z 0f na nowy ceiling.
            else if (g_cur_idx > ceiling) {
                next_boost = false;
                target = std::min(ceiling, LADDER_N - 1);
                reason = "BOOST-CEILING";
                needs_gr_idle = true;            // DOWN z 0f
            }
            // EXIT boost — LOAD histereza: busy < busy_up (80%) → drop z 0f.
            // Histereza enter@busy_boost(85) / exit@busy_up(80) = pasmo 5 pp.
            else if ((int)busy_avg < busy_up_pp) {
                next_boost = false;
                target = g_cur_idx; // == ceiling, powrót na 0e
                reason = "BOOST-IDLE";
                needs_gr_idle = true;            // DOWN z 0f
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
            // UP do 0f — NIE gate'owane (wymaga busy>busy_boost; rosnący zegar).
        }

        // ---- LADDER (drabinka 07↔0a↔0e) — tylko gdy boost niezmieniony ----
        if (!g_boost_active && !next_boost) {
            // DOWN (priorytet: TERMAL > IDLE > CEILING). Jeden poziom na krok.
            // TERMAL działa PER-PROFIL (default 65/58, preferred 82/75) —
            // straż termiczna nie jest skipowana dla non-preferred.
            if (temp >= 0 && temp_high_dwell >= g_cfg.temp_dwell_ms && g_cur_idx > 0) {
                target = g_cur_idx - 1;
                reason = "TERMAL";
                needs_gr_idle = true;            // DOWN — TERMAL może zejść PONIŻEJ floor [caps]
            } else if (g_cur_idx > 0 && idle_dwell >= g_cfg.idle_dwell_ms &&
                       !title_pref &&
                       (cap_floor < 0 || g_cur_idx > cap_floor)) {
                // v4.3: !title_pref — suppress IDLE downshift gdy title-match.
                // v4.4: [caps] — IDLE zatrzymuje się na floor (np. 0a); klasa bez
                // floor (cap_floor<0) zachowuje stare zachowanie (IDLE do 07).
                target = g_cur_idx - 1;
                reason = "IDLE";
                needs_gr_idle = true;            // DOWN
            } else if (g_cur_idx > ceiling) {
                // Profil obniżył ceiling poniżej bieżącego — zjedź krok w dół
                // (dwell idle/termal załatwi resztę; skrót gdy ceiling twardo
                // blokuje obecny poziom, np. default cap=07 a jesteśmy na 0a/0e).
                target = g_cur_idx - 1;
                reason = "CEILING";
                needs_gr_idle = true;            // DOWN
            }
            // UP (o 1 poziom, nigdy skok 07→0e).
            // v4.4: UP-FLOOR — klasa [caps] wraca do stanu spoczynkowego floor
            // (np. 0a) bez busy-gate (wymaga tylko temp_low; tu nie ma skoku >1).
            else if (cap_floor >= 0 && g_cur_idx < ceiling && g_cur_idx < cap_floor &&
                     temp >= 0 && temp_low_dwell >= g_cfg.temp_dwell_ms) {
                target = g_cur_idx + 1;
                reason = "UP-FLOOR";
            }
            // v4.3: title_pref omija busy_up (Discord/YouTube w przeglądarce).
            // v4.4: [caps] ma własny busy-up (np. 50%) zamiast globalnego (80%).
            // UP pozostaje NIE gate'owane GR-idle (rosnący zegar bezpieczniejszy).
            else if (g_cur_idx < ceiling &&
                     temp >= 0 && temp_low_dwell >= g_cfg.temp_dwell_ms &&
                     ((int)busy_avg > class_busy_up_pp || title_pref)) {
                target = g_cur_idx + 1;
                reason = title_pref ? "UP-TITLE" : "UP-LOAD";
            }
        }
        }

        // ---- WYKONANIE ----
        const bool boost_changed = (next_boost != g_boost_active);
        // Stan źródłowy do logu: 0f jeśli boost aktywny, inaczej LADDER[g_cur_idx].
        auto cur_hex = [&]() -> const char* {
            return g_boost_active ? state_hex((uint32_t)prof.boost_pstate)
                                  : state_hex(LADDER[g_cur_idx]);
        };

        // v4.1: GR-idle gate dla DOWN transycji. Jeśli silnik GR zajęty
        // (busy > gr_idle_promille) — odroczy zapis o jeden cykl. NIE resetuj
        // dwell-counters / g_cur_idx / g_boost_active (defer ≠ wykonana transycja;
        // dwell rośnie dalej, transycja wykonana gdy busy dolinie w dolinie między
        // klatkami). UP-LOAD/BOOST-UP mają needs_gr_idle=false → nie deferowane.
        const bool defer = needs_gr_idle && !gr_idle_ok(b);
        if (defer) {
            const char* tgt = next_boost ? state_hex((uint32_t)prof.boost_pstate)
                             : (target >= 0 && target < LADDER_N
                                ? state_hex(LADDER[target]) : "?");
            logf(2, "GR-idle gate: defer %s %s->%s (busy=%d%% > %d%%)",
                 reason ? reason : "?", cur_hex(), tgt,
                 b / 10, g_cfg.gr_idle_promille / 10);
            continue;
        }

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
            if (g_cfg.dgpu.enable)
                logf(1, "%s dgpu-active: %s -> %s (busy=%.0f%%, temp=%d°C, %s, "
                        "profil=%s, input=%d, video=%d)",
                     g_cfg.dry ? "dry-run:" : ">>",
                     dgpu_state_name(g_cur_idx, g_cfg.dgpu),
                     dgpu_state_name(target, g_cfg.dgpu),
                     busy_pct, temp, reason ? reason : "?",
                     pref_active ? "preferred" : "default",
                     dgpu_input_active, dgpu_video);
            else
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
            if (g_cfg.dgpu.enable) {
                logf(2, "dgpu-active stan %s, busy=%.0f%%, temp=%d°C, ceiling=%d, "
                        "floor=%d, input=%d, video=%d, noinput=%llu ms, "
                        "dwell[low=%d hi=%d idle=%d]",
                     dgpu_state_name(g_cur_idx, g_cfg.dgpu),
                     busy_pct, temp, dgpu_ceiling, dgpu_floor,
                     dgpu_input_active, dgpu_video,
                     (unsigned long long)no_input_ms,
                     temp_low_dwell, temp_high_dwell, idle_dwell);
            } else {
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
    }

    restore();
    logf(1, "koniec.");
    return 0;
}