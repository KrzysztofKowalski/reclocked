# /etc/reclockd.conf — konfiguracja daemona reclockd v4.2 (polityka Etap 3 + fan)
# Kopiuj: sudo cp reclockd.conf /etc/reclockd.conf
#
# Lista class-ów aplikacji "preferred" (hyprctl activewindow/clients .class).
# Preferred = apka z tej listy FOCUSED LUB (RUNNING + busy > busy-up) LUB
# (focused.title zawiera wzorzec z [preferred-titles]).
# Tu: przeglądarki (szeroki scope — każda z focusem pod obciążeniem → 0e,
# idle DOWNshiftuje). Discord/YouTube są kartami w przeglądarce — detekcja
# po tytule w [preferred-titles], nie po klasie (nie mają własnej klasy okna).

[preferred]
chromium
firefox
google-chrome
brave

# v4.1: tytuły okien "preferred" (substring case-insensitive). Discord/YouTube
# są kartami w przeglądarce (klasa okna = przeglądarka) — treść widać w tytule.
# Discord web: tytuł zawiera "Discord". YouTube: "YouTube" / " - YouTube".
# v4.4: dotyczy TYLKO kart w przeglądarce (klasa chromium/firefox itd.) —
# desktop Discord (Electron lub PWA Chrome) jest obsługiwany przez [caps].
[preferred-titles]
Discord
YouTube
 - YouTube

# v4.4: per-klasowa polityka. Klucz = klasa okna; wartość = floor=…, max=…, busy-up=…
#   floor    — stan spoczynkowy (IDLE nie spada poniżej), hex pstate (np. 0a)
#   max      — ceiling (nie wyżej niż to), hex pstate (np. 0e)
#   busy-up  — własny próg UP-LOAD (%), zamiast globalnego busy-up (80)
# Klasa z wpisem = preferred (gdy focused) i NIE łapie title-priority.
# Desktop Discord — dwie możliwe klasy (nie wiadomo której user używa):
#   - Electron:  "discord"
#   - PWA Chrome: "chrome-discord.com__channels_@me-Default"
# Polityka: baza 0a (idle trzyma 0a), busy > 50% → 0e, busy spada → znowu 0a.
# TERMAL nadal nadrzędny (może zejść poniżej floor). Karta Discord w przeglądarce
# (chromium/firefox itd.) NIE jest w [caps] → force-0e po tytule bez zmian.
[caps]
discord = floor=0a, max=0e, busy-up=50
chrome-discord.com__channels_@me-Default = floor=0a, max=0e, busy-up=50

# v4.1: klasy okien "low-power" (terminale). Focus okna z tej listy → wymusza
# profil default (cap=07) z priorytetem nad preferred — nawet jeśli Discord/
# YouTube generuje load w tle, terminal z focusem trzyma 07 (oszczędność).
[low-power]
foot
footclient
Alacritty
alacritty
kitty
Kitty
ghostty
Ghostty
wezterm
org.wezcel.Weasel

[profile default]
# Apka non-preferred (terminal, edytor): cap 07, twardy throttle.
# Thermal PER-PROFIL — konserwatywny: powyżej 65°C zwalnia, odzysk poniżej 58°C.
max-pstate = 07
temp-down = 65
temp-up = 58

[profile preferred]
# Apka preferred (przeglądarki): bezpieczna drabinka 07↔0a↔0e (cap 0e).
# v4.1: 0f WYŁĄCZONY (boost-pstate = -1) — 0e i 0a oba stabilne, 0f bez widocznej
# różnicy wydajności vs 0e. Jak gorąco → drabinka DOWN 0e→0a (TERMAL, jeden krok).
# Thermal preferred: powyżej 82°C zwalnia (drabinka), odzysk poniżej 75°C.
max-pstate = 0e
temp-down = 82
temp-up = 75
boost-pstate = -1
busy-boost = 85
boost-dwell-ms = 5000

# Parametry ogólne (nadpisują wbudowane defaulty). Histereza load 80/40 (40 pp).
[global]
interval-ms = 200
poll-ms = 1000
busy-up = 80
busy-down = 40
temp-dwell-ms = 5000
idle-dwell-ms = 5000
profile-dwell-ms = 2000
win-ms = 1000
exit-state = 0a
# v4.1: próg chwilowego busy (‰) poniżej którego DOWN transycje są dozwolone
# (GR-idle gate). 300 = 30%. reclok pamięci w locie pod renderem GR wedge'uje
# silnik — gate odracza DOWN aż busy dolinie. UP-LOAD/BOOST-UP nie gate'owane.
gr-idle-promille = 300

# v4.2: sterowanie wentylatorami applesmc (SMC MacBooka). Krzywa temp→RPM:
# liniowa interpolacja między fanN_min (przy temp-min) a fanN_max (przy temp-max).
# Zakresy RPM (fanN_min/max) czytane dynamicznie z sysfs przy starcie — NIE
# hardkodowane (fan1≈2160-6156, fan2≈2000-5700 RPM na GT 750M). Aktualizacja
# co poll-ms (1 s). temp ≤ temp-min → min RPM (cicho), temp ≥ temp-max → max RPM.
# fan-override (/run/reclockd/fan-override, reclockctl fan-off) zamraża auto.
# restore_auto przy wyjściu → fanN_manual=0 (SMC przejmuje, fail-safe).
# v5.1: temp-min-igd/temp-max-igd — osobna krzywa gdy włączone TYLKO iGPU
# (dGPU OFF; temp = CPU/coretemp, bo hwmon nouveau znika). Gdy dGPU ON →
# krzywa temp-min/temp-max (51/91); obie krzywe wchodzą na max przy 91°C.
# Cichsze: start wiatraków przy 51°C (dga) / 41°C (igd). Wybór wg sw.dgpu_off().
[fan]
enable = true
temp-min = 51
temp-max = 91
temp-min-igd = 41
temp-max-igd = 91

# v4.6: boost wentylatorów gdy wykryty uruchomiony kompilator. Skan /proc/*/comm
# (fallback cmdline) co poll-ms (1 s) za: clang, clang++, gcc, g++, cc, c++,
# cc1, cc1plus, make, cmake, ninja, cargo, rustc, meson, go, javac, ld, as,
# sccache, ccache + prefiksy wersji (gcc-*, g++-*, clang-*).
#   enable   — 1/0 (default 1): włącza całość
#   fan-max  — % maksymalnych RPM (default 100 = pełne wiatraki; 50 = pół zakresu)
#   names    — dodatkowe nazwy procesów do wykrycia (przecinkami; opcjonalne)
# Boost działa TYLKO w auto-mode — fan-override (/run/reclockd/fan-override,
# reclockctl fan-off) ma priorytet (ręczne sterowanie nie jest nadpisywane).
# Gdy kompilator zniknie → powrót do krzywej temp (normalna ścieżka).
[compiler]
enable = true
fan-max = 100
# names = mycc, mybuild

# v5.0: switchd module — dGPU power-state + render routing.
# W topologii DIS (dGPU prowadzi panel) moduł działa w trybie MONITOR — zero
# zmian power (power-off dGPU zablokowany gate scanout). Power executor aktywuje
# się po G1 (boot-time switch na IGD przez gpu-power-prefs + reboot).
#   enable            — 1/0 (default 1): moduł aktywny
#   tick-ms           — decision tick (default 1000, == poll-ms)
#   dwell-in-ms       — entry dwell miękkiej promocji (busy-gated)
#   dwell-out-ms      — exit dwell demote
#   min-residence-ms  — min czas na dGPU po promocji (anti-flapping)
#   cooldown-ms       — cooldown po demote przed ponowną promocją
#   wait-ready-ms     — timeout wait_off po power-off
#   min-switch-gap-ms — min odstęp między power-toggle
#   temp-gate         — °C: nie promuj gdy dGPU gorętszy
#   busy-enter        — % busy do miękkiej promocji
#   busy-exit         — % busy do demote
#   pstate-settle-ms  — nie pisz pstate po power-on (GPU clock settle po D3hot→D0;
#                       pierwsza zmiana clocka może zawiesić workqueue nouveau)
#   pstate-write-timeout-ms — timeout zapisu pstate (kernel hang safety: zapis w
#                       osobnym wątku, po timeout daemon żyje dalej i wstrzymuje pstate)
[switch]
enable = true
tick-ms = 1000
dwell-in-ms = 3000
dwell-out-ms = 5000
min-residence-ms = 20000
cooldown-ms = 45000
wait-ready-ms = 2000
min-switch-gap-ms = 10000
temp-gate = 82
busy-enter = 80
busy-exit = 40
pstate-settle-ms = 10000
pstate-write-timeout-ms = 2000

# Backend power dGPU.
#   backend               — manual (echo ON/OFF do vgaswitcheroo) | runpm (power/control)
#   autosuspend-ms        — autosuspend_delay_ms dla runpm
#   wait-idle-timeout-ms  — /proc fd scan przed power-off (otwarte /dev/dri)
#   wait-ready-timeout-ms — timeout nouveau reinit po power-on (runtime_status=active)
[dpower]
backend = manual
autosuspend-ms = 5000
wait-idle-timeout-ms = 5000
wait-ready-timeout-ms = 10000

# Klasy okien wymagające dGPU (twarda promocja, bez busy-gate)
# v5.2: przeglądarki — focus przeglądarki → dGPU ON (power + pstate 0a/0e
# z profilu [preferred]). Demote po przejściu focusu na [low-power]/[igpu].
[dgpu-hard]
game
blender
steam
chromium
firefox
google-chrome
brave

# Klasy okien z miękką promocją (busy-gated) — na start puste
[dgpu-soft]

# Klasy okien zawsze na iGPU (democja)
[igpu]
foot
kitty
alacritty

# Procesy wymagające dGPU (skan /proc/*/comm) — CUDA, DRI_PRIME
[dgpu-procs]
cuda
blender