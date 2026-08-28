# /etc/reclockd.conf — konfiguracja daemona reclockd v5.6 (polityka [dgpu-active] + fan + switchd)
# Kopiuj: sudo cp reclockd.conf /etc/reclockd.conf
#
# Lista class-ów aplikacji "preferred" (hyprctl activewindow/clients .class).
# Preferred = apka z tej listy FOCUSED LUB (RUNNING + busy > busy-up) LUB
# (focused.title zawiera wzorzec z [preferred-titles]).
# Tu: przeglądarki (szeroki scope — każda z focusem pod obciążeniem → 0e,
# idle DOWNshiftuje). Discord/YouTube są kartami w przeglądarce — detekcja
# po tytule w [preferred-titles], nie po klasie (nie mają własnej klasy okna).
# Gdy [dgpu-active] enable=true: profile dostarczają TYLKO progów termalnych
# (max-pstate ignorowane) — stan pstate wybiera maszyna stanów [dgpu-active].

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
# v5.6: przy [dgpu-active] tytuł jest INFORMACYJNY (status/log "video: ...") —
# NIE wymusza 0e. 0e wchodzi wyłącznie przez busy > busy-enter (80%) + temp.
[preferred-titles]
Discord
YouTube
 - YouTube

# v5.6: klasy odtwarzaczy kwalifikujące "video" INFORMACYJNIE (status/log), gdy
# tytuł nie zawiera znanego serwisu (mpv/vlc mają ścieżkę pliku w tytule).
# Tytuł/klasa video NIE decyduje o stanie pstate (0e = wyłącznie busy-driven).
[video-classes]
# mpv
# vlc

# v4.4: per-klasowa polityka. Klucz = klasa okna; wartość = floor=…, max=…, busy-up=…
#   floor    — stan spoczynkowy (IDLE nie spada poniżej), hex pstate (np. 0a)
#   max      — ceiling (nie wyżej niż to), hex pstate (np. 0e)
#   busy-up  — własny próg UP-LOAD (%), zamiast globalnego busy-up (80)
# Klasa z wpisem = preferred (gdy focused) i NIE łapie title-priority.
# v5.6: desktop Discord ZOSTAŁ USUNIĘTY (decyzja usera 2026-08-28) — Discord
# traktowany jak każda inna klasa na dGPU: dostaje pełny eco z [dgpu-active]
# (0a baseline / 07 deep idle / 0e tylko przy realnym busy > 80%).
# Sekcja zostaje dla klas, które chcą TWARDE floor (np. floor=0a = nigdy 07).
# TERMAL nadal nadrzędny (może zejść poniżej floor). Karta Discord w przeglądarce
# (chromium/firefox itd.) nie jest w [caps] → obsługiwana przez [dgpu-active].
[caps]
# discord = floor=0a, max=0e, busy-up=50
# chrome-discord.com__channels_@me-Default = floor=0a, max=0e, busy-up=50

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

# v5.6: [dgpu-active] — TRÓJSTOPNIOWA polityka pstate dla CAŁEGO dGPU, gdy ON.
# (raport 79 + decyzje usera 2026-08-28). Zastępuje wybór stanu przez profil:
#   0a (baseline) — „efficient power save" — domyślny stan dGPU pracującego
#   07 (deep idle) — dGPU ON, nic się nie renderuje, brak inputu przez dwell
#   0e (heavy)     — WYŁĄCZNIE busy-driven: busy > busy-enter (80%) sustained
#                    ORAZ temp < temp-up (margines termalny). Tytuł video NIE daje 0e.
# Detekcja aktywności usera: evdev (/dev/input/event*, osobny wątek — scroll+
# klawiatura+mysz). Bez evdev (activity-source != evdev lub brak urządzeń):
# brak sygnału inputu — idle po dwell, wake tylko przez busy/zmianę tytułu.
# enable = false → daemon działa jak dotychczas (profile/ladder, stara logika).
[dgpu-active]
enable = true
baseline = 0a
max = 0e
# ceiling gdy focused ∈ [low-power] (terminal): tło nie dostaje 0e, ale
# baseline 0a zostaje (oszczędność przy zachowaniu responsywności).
low-power-ceiling = 0a
activity-source = evdev
# Brak inputu przez X i busy < deep-idle-busy → floor spada do 07 (deep idle).
activity-dwell-ms = 8000
# Busy % poniżej którego dGPU może wejść w deep idle (07). YouTube ~25-36% busy
# → trzyma 0a; 480p (busy < 20%) → może zejść do 07.
deep-idle-busy = 20
# Wejście/zejście 0e dla loadu ogólnego (jedyna droga do 0e). busy-enter
# nadpisuje globalny busy-up (80) w kontekście dGPU-active.
busy-enter = 80
busy-exit = 40
# Termalne: true = per-profil (def 65/58, preferred 82/75 wg focusa); false =
# wspólne temp-down/temp-up poniżej. Wejście 0e wymaga temp < temp-up.
temp-per-profile = true
# temp-down = 82
# temp-up = 75

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
#   title-idle-busy   — % busy: karta Discord/YT (promocja tytułowa) z busy poniżej
#                       progu jest 'idle' → demote do iGPU (power-off) po dwell-out
# v5.6: przeglądarki NIE są już w [dgpu-hard] — karty Discord/YouTube promują po
# tytule ([preferred-titles]); reszta kart neutralna (iGPU). Promocja tytułowa
# nie jest zapinką (busy < title-idle-busy → demote). Timery skrócone do 1 s.
[switch]
enable = true
tick-ms = 1000
dwell-in-ms = 3000
dwell-out-ms = 1000
min-residence-ms = 1000
cooldown-ms = 1000
wait-ready-ms = 2000
min-switch-gap-ms = 1000
temp-gate = 82
busy-enter = 80
busy-exit = 40
# v5.6: busy % poniżej którego karta Discord/YouTube (promocja tytułowa) jest 'idle'
# → demote do iGPU (power-off) po dwell-out. Łatwa zmiana + reload (SIGHUP).
# Grający YT ~25-36% busy zostaje na dGPU; prawdziwy idle zdejmuje dGPU.
title-idle-busy = 33
# v5.6: busy % poniżej którego klasa [dgpu-idle] (np. mpv) jest 'idle' → demote
# do iGPU (power-off) po dwell-out. Łatwa zmiana + reload (SIGHUP).
class-idle-busy = 33
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
# v5.6: przeglądarki USUNIĘTE — karty Discord/YouTube promują po tytule
# ([preferred-titles]), reszta kart jest neutralna (iGPU). Twarda promocja
# tylko dla ciężkich apki. Tytułowa promocja nie jest zapinką:
# busy < title-idle-busy → demote do iGPU.
[dgpu-hard]
game
blender
steam

# Klasy okien z miękką promocją (busy-gated) — na start puste
[dgpu-soft]

# Klasy okien z idle-demote: promują do dGPU jak twarde, ale busy < class-idle-busy
# → demote do iGPU (np. mpv — wideo na dGPU, pauza/idle na iGPU)
[dgpu-idle]
mpv

# Klasy okien zawsze na iGPU (democja)
[igpu]
foot
kitty
alacritty

# Procesy wymagające dGPU (skan /proc/*/comm) — CUDA, DRI_PRIME
[dgpu-procs]
cuda
blender