# reclocked

> Demon governor pstate w przestrzeni użytkownika (`reclockd`) plus łatki
> jądra i Mesa, które utrzymują starą kartę NVIDIA GT 750M (Kepler, GK107) na
> użytecznych taktowaniach pod otwartym sterownikiem **nouveau** — zamiast
> tkwić na taktowaniach rozruchowych i tracić 2-4x wydajności.

> English: [README.md](README.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-6c757d.svg)](#instalacja)
[![Language: C++17](https://img.shields.io/badge/C%2B%2B-17-00599c.svg)](#wymagania)

`reclocked` to mały, samodzielny demon w C++17 plus łatki jądra i Mesa, które
dostarczają automatyczny reclocking dla NVIDIA Kepler (GK107) pod nouveau —
bez zastrzeżonego sterownika, bez governor w jądrze i bez wsparcia reclocku
ze strony upstream.

---

## 🖥 O projekcie

Upstreamowy sterownik nouveau udostępnia ręczny reclocking pstate przez
debugfs, ale **nie ma automatycznego governor**: po rozruchu karta tkwi na
najniższym pstate (`07` = 270-405 MHz rdzeń / 838 MHz pamięć), dopóki ktoś nie
zapisze wyższego pstate ręcznie. Na GT 750M oznacza to stratę 2-4x wydajności
we wszystkim — od kompozycji w przeglądarce po gry shader-bound, bez
jakiegokolwiek skalowania zależnego od temperatury czy obciążenia.

`reclockd` wypełnia tę lukę. To demon w przestrzeni użytkownika, który odpytuje
obciążenie GPU (busy%) przez liczniki idle PMU na BAR0 oraz temperaturę przez
hwmon, i zapisuje przejścia pstate przez interfejs debugfs nouveau — governor
uwzględniający obciążenie i temperaturę, którego upstream nouveau nigdy nie
dostarczył dla Keplera. Działa jako usługa systemd, współistnieje z
kompozytorem Wayland (patrz [Problem DRM master](#problem-drm-master-i-dlaczego-reclockd-go-zrzuca)),
i obsługuje profile zależne od aplikacji (np. cap niskie dla terminala, pozwól
na boost dla przeglądarki).

To **nie** jest zamiennik brakującego governor w jądrze nouveau — to praktyczne
obejście działające tylko w czasie działania (runtime-only), dla sprzętu, który
nouveau de facto porzucił. Restart przywraca wszystko do taktowań rozruchowych;
demon ponownie stosuje politykę przy następnym starcie.

### ♻️ Po co ten projekt istnieje: z elektrośmieci do używalności

Projekt testowany na **MacBook Pro 15" Late 2013 z NVIDIA GeForce GT 750M**
(GK107, Kepler, NVE7, sm_30). Ta maszyna jest legendarnie problematyczna na
otwartych sterownikach graficznych:

- Kepler dGPU na nouveau nie ma auto-reclocku, więc tkwi na taktowaniach
  rozruchowych (2-4x wolniej niż potrafi).
- Układ dual-GPU (Intel iGPU + NVIDIA dGPU połączony przez multiplekser Apple
  `gmux`) sprawia, że przełączanie GPU na żywo jest niestabilne.
- Maksymalny pstate (`0f`) znany jest z zawieszania układów klasy NVE0 przy
  undervolcie.

W kombinacji te MacBooki Pro z tej generacji są wycofywane jako
**elektrośmieci**, bo „nie da się użyć dGPU porządnie pod Linuksem". `reclocked`
to próba zmiany tego: działający stos auto-reclocku (demon + łatka jądra +
łatka Mesa), który utrzymuje laptopa z 2013 z Keplerem jako używalną maszynę
codzienną w 2026. Z elektrośmieci do używalności.

---

## 🐧 Sprzęt i założenia

**Testowane na:** MacBook Pro 15" Late 2013, NVIDIA GeForce GT 750M (GK107 /
Kepler / NVE7 / sm_30), Arch Linux + nouveau. Środowisko to
[**Omarchy**](https://omarchy.dev) (Arch-based, Hyprland/Wayland) —
**zalecane** dla tego sprzętu, i konfiguracja, na której problem DRM-master /
czarnego ekranu był debugowany, a fix `DROP_MASTER` zweryfikowany przeciwko
kompozytorowi Hyprland.

**Założenia:**

- dGPU NVIDIA Kepler (GK107, NVE7) sterowany przez moduł jądra **nouveau**.
  Inne układy Keplera (GK104/GK106/GK208) lub karty nie-Kepler będą wymagały
  adaptacji wartości LADDER pstate, mapy rejestrów PMU BAR0 i lookupu hwmon.
- Moduł nouveau jest załadowany, a karta jest **aktywna w czasie działania**
  (nie runtime-suspended). `reclockd` ustawia `power/control=on` na urządzeniu
  PCI na czas swojego działania i przywraca poprzednią wartość przy wyjściu.
- **Dostęp do pstate w debugfs**: jądro musi wystawiać
  `/sys/kernel/debug/dri/<bdf>/pstate` (nouveau zbudowane z
  `CONFIG_DRM_NOUVEAU_DEBUG=on`, co jest domyślne dla kerneli dystrybucyjnych).
  Demon zapisuje pstate przez ten plik, co wymaga **roota**.
- Laptop dual-GPU (Intel i915 primary + NVIDIA nouveau dGPU), gdzie `card0` to
  dGPU nouveau napędzające panel eDP (np. przez multiplekser `gmux`). Demon
  otwiera `/dev/dri/card0` tylko do synchronizacji vblank, a następnie
  natychmiast zrzuca DRM master (patrz niżej). Desktopy single-GPU nouveau też
  działają, o ile `card0` jest urządzeniem nouveau poddawanym reclockowi.
- **systemd** (dla dostarczonej jednostki `.service`) lub ręczny start.
  `hyprctl` jest opcjonalny — gdy jest dostępny, profile zależne od aplikacji
  są włączone; gdy go brak, demon cofa się do profilu `default`.
- BDF dGPU w `reclockd.cpp` ma domyślnie `0000:01:00.0`. Dostosuj
  `PCI_RESOURCE`, `PSTATE_FILE`, `POWER_CTRL` i `DRM_CARD` w źródle, jeśli
  twoja karta siedzi pod innym adresem.

**Nie dla:** kart nie-Kepler bez adaptacji kodu, i nikogo, kto nie czuje się
komfortowo z uruchamianiem demona root, który przepisuje taktowania GPU co
200 ms. Najpierw przeczytaj [Bezpieczeństwo i ryzyka](#bezpieczenstwo-i-ryzyka).

---

## ✨ Funkcje

- 🎛 **Bezpieczna drabina LADDER 3-stopniowa** (`07 ↔ 0a ↔ 0e`): jeden krok
  pstate na decyzję, nigdy skok `07 → 0e`. Przejścia są warunkowane obciążeniem
  i temperaturą z licznikami dwell.
- 🪟 **Profile zależne od aplikacji**: `default` (cap `07`, termika
  konserwatywna — dla terminala/edytora) vs `preferred` (cap `0e` — dla
  przeglądarki). Profil jest wybierany z klasy aktywnego/uruchomionego okna
  raportowanej przez `hyprctl`, **lub** z tytułu aktywnego okna (patrz
  title-priority niżej). `profile-dwell` ogranicza częstotliwość przełączeń,
  żeby alt-tab nie przeskakiwał po sufitach.
- 🔁 **Re-detect Hyprlanda**: demon startuje jako usługa systemowa *zanim*
  powstanie sesja użytkownika, więc socket `hyprctl` nie jest jeszcze gotowy
  przy starcie. Zamiast jednorazowego checku, który zostawiałby demona na
  profilu `default` przez całą sesję, `reclockd` re-wykrywa Hyprlanda co cykl
  poll, dopóki sesja się nie pojawi — profilowanie zależne od aplikacji
  aktywuje się w ~1 s po loginie.
- 🛑 **GR-idle gate**: live zjazd pamięci w trakcie renderowania silnika GR
  może zawiesić kompozytora (Kepler przepisuje timingi framebuffera
  mid-frame → trapy PROP render-targetu). Synchronizacja vblank chroni tylko
  scanout, nie silnik GR. Przejścia DOWN są więc **odraczane**, dopóki
  chwilowe busy nie spadnie poniżej `gr-idle-promille` (domyślnie 300 ‰ = 30 %).
  Przejścia UP nie są gate'owane (rosnący zegar jest bezpieczniejszy
  mid-render).
- 🏷 **Title-priority (Discord / YouTube)**: karty w przeglądarce typu Discord
  i YouTube nie mają własnej klasy okna — ich tożsamość widać w *tytule* okna.
  `[preferred-titles]` dopasowuje tytuł aktywnego okna case-insensitive i przy
  matchu **wymusza `0e` z pominięciem reguły `busy > 80 %`** (te obciążenia są
  memory-bound, GR busy często 16-36 %, więc normalna ścieżka UP-LOAD nigdy
  nie zaskakuje). IDLE downshift jest suppressowany, dopóki tytuł pasuje, więc
  apka trzyma `0e`. Focus terminala nadal wygrywa `07` (patrz low-power
  niżej) — oba patrzą na to samo aktywne okno, więc się nie gryzą. Kolejność
  priorytetów: TERMAL > low-power terminal > title-match > class /
  running-busy > IDLE.
- 💻 **Terminale low-power**: `[low-power]` listuje klasy okien (np. `foot`,
  `alacritty`, `kitty`, `ghostty`, `wezterm`). Gdy terminal ma focus, demon
  wymusza profil `default` (cap `07`) z priorytetem nad `preferred` — nawet
  jeśli przeglądarka generuje obciążenie w tle.
- 🚀 **Tier boost (`0f`, domyślnie wyłączony)**: `0f` jest celowo **poza
  drabiną** (off-ladder) i jest teraz **domyślnie wyłączony**
  (`boost-pstate = -1`). Na tej GT 750M `0e` i `0a` są oba stabilne, a `0f`
  nie daje widocznego zysku vs `0e`, więc boost jest opt-in. Włącz go
  ustawiając `boost-pstate = 0f` w `reclockd.conf`: wchodzi tylko przy suficie
  drabiny (`0e`) pod utrzymanym busy > `busy-boost` przez `boost-dwell` z
  temp < `temp-up`, a zabezpieczenie termiczne zrzuca go natychmiast.
  Zawieszenia NVE0 przy max pstate są znane.
- 🎯 **Per-klasowa polityka `[caps]` (v4.4)**: per-klasowe `floor` / `max` /
  `busy-up` (`klasa = floor=0a, max=0e, busy-up=50`) — `floor` = stan
  spoczynkowy (IDLE nie schodzi niżej), `max` = własny sufit, `busy-up` =
  własny próg UP-LOAD (0 = globalny 80 %). Klasa z wpisem `[caps]` jest
  *preferred, gdy ma focus*, ale **nie** łapie title-priority — karta Discord
  w przeglądarce (chromium/firefox, bez wpisu w `[caps]`) nadal dostaje
  force-`0e` po tytule. Przykład: desktop Discord — baza `0a`, busy > 50 % →
  `0e`, idle → z powrotem `0a`. TERMAL pozostaje nadrzędny (może zejść poniżej
  `floor`).
- 🛡 **Samouzdrawianie po suspend/resume (v4.5)**: głęboki sleep (S3) odcina
  zasilanie GPU i gubi konfigurację liczników busy PMU w BAR0 — próbkowanie
  busy zwraca wtedy stale 1000‰, co blokuje zejście IDLE i GR-idle gate,
  zostawiając daemon w `0e`. v4.5 odczytuje `R_IDLE_CTRL` co cykl i, gdy
  konfiguracja ginie, ponownie inicjuje liczniki i resetuje okno busy (log do
  journala). Uzupełnia hook `system-sleep` (restart daemona po resume) jako
  siatka bezpieczeństwa. Bez zmian w konfiguracji.
- 🌬 **Kompilator → wiatraki 100% (v4.6)**: gdy działa kompilator lub tool
  budowania (`clang*`, `gcc*`, `g++*`, `cc`, `c++`, `cc1`, `cc1plus`, `make`,
  `cmake`, `ninja`, `cargo`, `rustc`, `meson`, `go`, `javac`, `ld`, `as`,
  `sccache`, `ccache`, ...), wentylatory lecą na max RPM — build nie
  throttle'uje termicznie. Detekcja skanuje `/proc/*/comm` (fallback cmdline)
  co cykl polla i łapie też prefiksy wersji (`gcc-14`, `clang-18`, ...). Boost
  działa TYLKO w auto-mode — ręczny override (reclockctl fan-off) ma
  priorytet. Gdy build się skończy, wiatraki wracają do krzywej temp. Konfig:
  sekcja `[compiler]` (`enable` / `fan-max` / `names`).
- 🔀 **switchd — power-state dGPU + routing renderu (v5.0)**: opcjonalny
  moduł power-state dla maszyn dual-GPU Apple (iGPU Intel + dGPU NVIDIA na
  muxie `gmux`). W topologii DIS (dGPU prowadzi panel) działa w trybie
  MONITOR — zero zmian power (power-off blokuje gate scanout). Power executor
  aktywuje się po **boot-time** przełączeniu na IGD (`gpu-power-prefs` przez
  `reclockctl to-igd` + reboot; live przełączanie muxa poza zakresem). Miękka
  promocja jest busy-gated, twarda promocja z klas okien `[dgpu-hard]` /
  procesów `[dgpu-procs]`, democja z `[igpu]`. `pstate-settle-ms` czeka na
  ustabilizowanie zegara GPU po power-on (pierwsza zmiana clocka po
  D3hot→D0 potrafi zawiesić workqueue nouveau), `pstate-write-timeout-ms`
  robi timeout zapisu pstate. CLI:
  `reclockctl switch-status | dgpu-on | dgpu-off | dgpu-auto | to-igd | to-dis`.
- 🌬 **Osobna krzywa fan tylko-iGPU (v5.1)**: gdy aktywne jest tylko iGPU
  (dGPU OFF, topologia IGD), hwmon nouveau znika i wentylatorami steruje temp
  CPU (`coretemp`) — a CPU może się grzać bezpiecznie wyżej niż dGPU. Wchodzi
  wtedy osobna, cichsza krzywa: `temp-min-igd` / `temp-max-igd` (domyślnie
  41/91 °C) — wiatraki lecą na max dopiero przy 91 °C zamiast 67 °C. Wybór
  krzywej wg **stanu power** dGPU (`sw.dgpu_off()`), nie topologii: OFF →
  krzywa igd, ON → standardowa 51/91 °C. `pstate.sh status` ma dodatkową
  sekcję `=== wentylatory ===` — aktywna krzywa + zakres + obroty z
  `/run/reclockd/status` (fallback do sysfs applesmc, gdy daemon nie działa).
- 🔀 **Przeglądarki wymuszają dGPU (v5.2)**: klasy przeglądarek (`chromium`,
  `firefox`, `google-chrome`, `brave`) dodane do `[dgpu-hard]` — focus
  przeglądarki promuje dGPU (power-on) z profilem `[preferred]` (pstate
  `0a/0e`); przejście focusu na terminal demote'uje z powrotem
  (`[igpu]`/`[low-power]`). Tylko konfiguracja — daemon widzi zmianę na żywo
  przez `reclockctl reload` (SIGHUP), bez rebuildu.
- 🌡 **Per-profil zabezpieczenie termiczne**: downclock termiczny jest
  **per-profil**, a nie globalny. `default` obniża taktowanie przy 65°C /
  odzyskuje poniżej 58°C; `preferred` obniża przy 82°C / odzyskuje poniżej
  75°C. Obniżenie termiczne ma priorytet nad obciążeniem (i nad
  title-priority) w obu profilach.
- 🌀 **Kontrola wentylatorów (applesmc)**: na laptopach Apple `reclockd`
  steruje też wentylatorami SMC przez `/sys/devices/platform/applesmc.768/`.
  Krzywa temp→RPM jest interpolowana liniowo między `fanN_min` i `fanN_max`,
  które są **czytane dynamicznie z sysfs przy starcie** (nie hardkodowane).
  Domyślny pas 51-91 °C, aktualizacja co cykl poll, niezależnie od pstate.
  `reclockctl fan-off` zamraża auto (steruj wentylatorami ręcznie); `fan-on`
  wznawia. Fail-safe przywraca auto SMC (`manual=0`) przy wyjściu.
- 🖥 **Synchronizacja vblank**: zapisy pstate są wyrównywane do vblank przez
  `DRM_IOCTL_WAIT_VBLANK`, żeby uniknąć zakłóceń w trakcie scanout.
- 🔓 **DROP_MASTER**: demon zrzuca DRM master natychmiast po otwarciu `card0`,
  więc nigdy nie blokuje kompozytora Wayland w przejęciu KMS. Patrz
  [Problem DRM master](#problem-drm-master-i-dlaczego-reclockd-go-zrzuca).
- ✋ **Ręczny override**: `pstate.sh set <pstate>` tworzy plik-flagę w
  `/run/reclockd/override`, który zamraża tryb auto; `pstate.sh auto` go
  czyści. Przydatne do benchmarków lub przypinania pstate.
- 🔔 **Live reload przez SIGHUP**: wyślij `SIGHUP` (lub `reclockctl reload`),
  żeby ponownie wczytać `/etc/reclockd.conf` bez restartu demona.
- 🛡 **Fail-safe**: brak hwmon → warunki termiczne pominięte (nigdy emergency
  UP); brak applesmc → kontrola wentylatorów wyłączona cicho; SIGTERM →
  przywróć `--exit-state` i auto wentylatorów SMC; CLI override wygrywa z
  configiem.
- 📦 **Bez zależności linkowania od libdrm**: używa tylko surowych ioctl z
  `<drm/drm.h>`.

---

## 🔓 Problem DRM master (i dlaczego reclockd go zrzuca)

Ta sekcja dokumentuje problem projektowy, który dotyczy **każdego** narzędzia
DRM w przestrzeni użytkownika, które musi współistnieć z kompozytorem Wayland
(Hyprland, Sway, KDE itp.). Opisany jest tu jako ogólny wzorzec, a nie jako
quirk jednej maszyny.

### ⚠️ Problem

Gdy proces otwiera węzeł urządzenia DRM-primary (`/dev/dri/card0`) `O_RDWR`,
jądro ustawia **pierwszego otwierającego** jako niejawnego DRM master. Jeśli
demon reclockujący otworzy `card0` pierwszy i utrzyma master, a potem serwer
wyświetlania (Hyprland / SDDM / logind / libseat) wystartuje i spróbuje
przejąć to samo urządzenie przez `TakeDevice` — urządzenie jest zajęte
(`EBUSY`), bo demon wciąż trzyma master → libseat nie może otworzyć KMS →
kompozytor nie znajduje GPU → kończy się awarią → użytkownik dostaje **czarny
ekran** przy starcie sesji.

Objaw to czarny ekran zaraz po loginie, podczas gdy demon reclockujący działa
sobie w tle trzymając master, którego nigdy nie używa do renderingu.

### ✅ Fix: DROP_MASTER

W `drm_open()`, `reclockd` otwiera `card0` z `O_RDWR | O_CLOEXEC` i
**natychmiast** wywołuje:

```c
ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
```

To zrzuca uprawnienia master (niezależnie od tego, czy demon był domyślnym
master, czy nie), pozostawiając go jako zwykły fd non-master. Reclocking nadal
działa bez master: czekanie na vblank używa
`_DRM_VBLANK_RELATIVE` + `DRM_IOCTL_WAIT_VBLANK`, co nie wymaga master.
`EINVAL` / `ENODEV` (zwracane, gdy demon nie jest master, np. bo kompozytor
już jest) są oczekiwane i ignorowane. Wywołania ioctl pochodzą bezpośrednio z
`<drm/drm.h>` — brak zależności linkowania od `libdrm`.

To jest ogólny wzorzec dla narzędzia DRM w przestrzeni użytkownika, które musi
współistnieć z kompozytorem Wayland: otwórz to, czego potrzebujesz, a potem
zrzuć master zanim wystartuje kompozytor. Sprawdzone przez współistnienie z
Hyprland na tym sprzęcie.

---

## 📁 Układ repozytorium

```
reclocked/
├── LICENSE                         MIT
├── README.md                       ten plik (EN)
├── CZYTAJMIE.md                    ten plik (PL)
├── .gitignore
├── examples/
│   ├── reclockd.conf.pl            domyślny config — komentarze PL
│   └── reclockd.conf.en            domyślny config — komentarze EN
├── reclockd/
│   ├── reclockd.cpp                źródło demona (~3070 linii, C++17)
│   ├── Makefile                    buduje ./reclockd (bez linkowania libdrm)
│   ├── reclockd.conf               domyślny config (profile, progi, [switch])
│   ├── reclockd.service            jednostka systemd (instaluje się do /etc/systemd/system/)
│   └── reclockctl                  wrapper CLI dla systemctl + pstate.sh (+ switchd)
├── patches/
│   ├── 0001-nouveau-auto-reclock.patch   polityka auto-reclocku w jądrze nouveau
│   ├── 0002-mesa-nvc0-sched-data.patch   dane latencji schedulera Mesa nvc0
│   ├── 0003-reclockd-caps-ceiling.patch  reclockd v4.4 polityka per-klasowa [caps]
│   ├── 0004-reclockd-s3-selfheal.patch   reclockd v4.5 samouzdrawianie liczników busy po S3
│   ├── 0005-reclockd-compiler-fan.patch  reclockd v4.6 wykryty kompilator → wiatraki 100%
│   ├── 81-nouveau-kepler.rules           reguła udev: wymuś nouveau (omija blacklist nvidia-utils)
│   ├── reclockd.conf-caps.diff           diff reclockd.conf — [caps] Discord 0a/0e busy-gated
│   ├── reclockd.conf-compiler.diff       diff reclockd.conf — [compiler] boost wiatraków
│   └── kernel/                           seria kernelowa MacBook Pro 11,3 0002-0014
├── install-udev-rule.sh            instaluje powyższą regułę udev
├── pstate.sh                       inspekcja/wymuszanie pstate przez debugfs (+ iGPU RPS / coretemp)
├── build-mesa.sh                   budowanie patchowanego Mesa (tylko nouveau)
├── mesa-manage.sh                  instalacja/rollback patchowanego sterownika Mesa
├── recover-gpu.sh                  ratunkowe odzyskiwanie, gdy zepsuje się wyświetlanie
├── build-kernel.sh                 budowa + instalacja patchowanego jądra nvkp
├── build-uki-nvkp.sh               budowa UKI (vmlinuz+initramfs+cmdline) dla Limine protocol: efi
└── bench-gpu.sh                    powtarzalny benchmark glmark2 iGPU-vs-dGPU (full-res, offscreen)
```

---

## 📋 Wymagania

- **g++** z C++17 (`-std=c++17`).
- **Jądro Linuxa** z załadowanym modułem **nouveau** i wystawionym pstate w
  debugfs (`/sys/kernel/debug/dri/<bdf>/pstate` zapisywalny/czytelny przez
  root).
- **Nagłówki libdrm** (`<drm/drm.h>`) do definicji ioctl — zazwyczaj z pakietu
  dev `libdrm`. **Brak zależności linkowania od libdrm.**
- **systemd** (opcjonalnie, ale zalecane, dla jednostki usługi).
- **hwmon** wystawiający `temp1_input` z nazwą `nouveau` (opcjonalnie; przy
  braku warunki termiczne są pomijane fail-safe).
- **hyprctl** (opcjonalnie, dla profili zależnych od aplikacji). Bez niego demon
  uruchamia tylko profil `default`.
- **root**, żeby uruchomić demona (mmap BAR0 + zapis pstate w debugfs).

---

## 🔧 Instalacja

### 1. Zbuduj reclockd

```sh
cd reclockd
make
```

To produkuje `reclockd/reclockd`. Nie ma kroku linkowania libdrm; tylko
nagłówki są potrzebne w trakcie kompilacji.

### 2. Instaluj pliki (system-wide)

```sh
sudo install -m755 reclockd/reclockd      /usr/local/bin/reclockd
sudo install -m644 reclockd/reclockd.conf /etc/reclockd.conf
sudo install -m644 reclockd/reclockd.service /etc/systemd/system/reclockd.service
sudo install -m755 reclockd/reclockctl    /usr/local/bin/reclockctl
sudo install -m755 pstate.sh              /usr/local/bin/pstate.sh
```

### 3. Włącz i uruchom usługę

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now reclockd
```

Zweryfikuj:

```sh
reclockctl status          # status systemd + pstate + temp + override
sudo cat /sys/kernel/debug/dri/0000:01:00.0/pstate
journalctl -u reclockd -f  # decyzje na żywo
```

---

## ⚙️ Konfiguracja i parametry

`reclockd` czyta `/etc/reclockd.conf` domyślnie (nadpisz przez `--config`).
Config to mały format linii INI parsowany przez wbudowany parser (brak
zewnętrznych zależności). Flagi CLI nadpisują wartości z configu.

### Sekcja `[preferred]`

Lista klas okien `hyprctl`, które uruchamiają profil `preferred`, gdy są
aktywne (focused), lub gdy działają + busy > `busy-up`. Przykład: przeglądarki.

### Sekcja `[preferred-titles]`

**Tytuły** okien (dopasowanie substring case-insensitive), które uruchamiają
profil `preferred` z **title-priority** — wymuszając `0e` z pominięciem reguły
`busy > busy-up`. Używaj dla kart w przeglądarce, które nie mają własnej klasy
okna (Discord, YouTube). Przy matchu IDLE downshift jest suppressowany, więc
apka trzyma `0e`. Przykład: `Discord`, `YouTube`, ` - YouTube`.

### Sekcja `[caps]` — polityka per-klasowa

Per-klasowe floor / sufit / próg UP-LOAD dla aplikacji z prawdziwą klasą okna
(Electron, PWA Chrome). Składnia: `klasa = floor=0a, max=0e, busy-up=50`
(każdy klucz opcjonalny):

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `floor` | `-` (brak) | Stan spoczynkowy — IDLE nie schodzi poniżej tego stanu drabiny. |
| `max` | sufit profilu | Własny sufit (stan drabiny), nadpisuje cap profilu. |
| `busy-up` | `0` (globalne 80) | Własny próg UP-LOAD (%). `0` = globalne `busy-up`. |

Klasa z wpisem `[caps]` zachowuje się jak `[preferred]` **gdy ma focus**, ale
**nie** jest dopasowywana przez `[preferred-titles]` — bez force-`0e` po tytule.
Używaj dla desktopowego Discorda — obie możliwe klasy (Electron `discord`, PWA
`chrome-discord.com__channels_@me-Default`) dostają tę samą politykę: baza `0a`,
busy > 50 % → `0e`, idle → z powrotem `0a`. TERMAL pozostaje najwyższy (może
zejść poniżej `floor`). Karta Discord w przeglądarce bez wpisu w `[caps]`
zachowuje ścieżkę force-`0e` po tytule bez zmian.

```ini
[caps]
discord = floor=0a, max=0e, busy-up=50
chrome-discord.com__channels_@me-Default = floor=0a, max=0e, busy-up=50
```

### 🛡 Samouzdrawianie po suspend/resume (v4.5)

Głęboki sleep (S3) odcina zasilanie GPU. Po resume nouveau robi pełny
`devinit`, ale konfiguracja liczników busy PMU w BAR0 (`R_IDLE_CTRL` /
`R_IDLE_MASK`) **nie wraca** — `reclockd` inicjuje ją raz przy starcie
(`init_counters()`). Bez niej `sample()` zwraca stale 1000‰, co blokuje
zejście IDLE i GR-idle gate, zostawiając daemon w `0e` (raport 63).

v4.5 samo-leczy się: pętla główna odczytuje `R_IDLE_CTRL` co cykl; gdy
`(ctrl & CTRL_VALUE_MASK) != CTRL_VALUE_ALWAYS` (konfiguracja stracona —
typowo po resume), ponownie inicjuje liczniki, resetuje okno busy i loguje
`PMU busy counters config lost (post-resume?) — reinitializing`.

Hook `system-sleep` jest siatką bezpieczeństwa:
`/usr/lib/systemd/system-sleep/reclockd-resume` restartuje daemon w fazie
`post`, resynchronizując liczniki i indeks pstate. Razem zapobiegają lockupowi
w `0e` po sleepie. Bez zmian w konfiguracji.

**Weryfikacja po `systemctl suspend`:** `reclockctl status` powinien wrócić
do `07` (nie utknąć na `0e`), MainPID daemona powinien być nowy (restart
z hooka), a `journalctl -u reclockd -n 50` może pokazać log reinitu.

### 🌬 Kompilator → wiatraki 100% (v4.6)

Budowanie jest impulsowe: duża kompilacja potrafi zapchać CPU, podczas gdy GPU
bezczynnieje, więc krzywa temp (sterowana temp. GPU) nigdy nie podnosi
wiatraków — a CPU throttle'uje termicznie w trakcie buildu. v4.6 skanuje
`/proc/*/comm` co cykl polla; gdy działa kompilator lub tool budowania,
`Fan::set_boost()` ustawia oba wiatraki na max RPM (lub `fan-max` %) i trzyma
je do końca buildu, po czym wraca do krzywej temperaturowej.

Wykrywane domyślnie: `clang*`, `gcc*`, `g++*` (dowolny sufiks wersji), `cc`,
`c++`, `cc1`, `cc1plus`, `make`, `cmake`, `ninja`, `cargo`, `rustc`, `meson`,
`go`, `javac`, `ld`, `as`, `sccache`, `ccache`. Nazwa procesu z `/proc/*/comm`;
gdy `comm` nie jest prawdziwą nazwą (wątki jądra `[xyz]`), fallback do
basenamu `cmdline`.

Sekcja `[compiler]` (wszystko opcjonalne):

| key       | default | znaczenie                                          |
|-----------|---------|----------------------------------------------------|
| `enable`  | `true`  | 1/0 — wyłącza całą funkcję                         |
| `fan-max` | `100`   | % maksymalnych RPM wiatraków (50 = pół zakresu)     |
| `names`   | —       | dodatkowe nazwy procesów, przecinkami (np. `mycc`) |

Boost działa TYLKO w **auto-mode** — ręczny override (`reclockctl fan-off`,
`/run/reclockd/fan-override`) ma priorytet i nigdy nie jest nadpisywany.
**Weryfikacja:** odpal `make -j` lub `g++` — wiatraki skaczą na max w ~1 s
(`journalctl -u reclockd` zaloguje
`fan: KOMPILATOR wykryty (cc1) -> fan1=... fan2=... (boost 100%)`); po
skończonym buildzie wiatraki wracają do krzywej.

### 🔀 switchd — power-state dGPU + routing renderu (v5.0)

Opcjonalny moduł power-state dla maszyn dual-GPU Apple (iGPU Intel + dGPU
NVIDIA połączone muxem `gmux`). W domyślnej topologii DIS (dGPU prowadzi panel)
działa w trybie **MONITOR** — czyta status, ale nigdy nie dotyka power
(power-off wyczerniłby panel; gate scanout go blokuje). Power executor
aktywuje się dopiero po **boot-time** przełączeniu na IGD (`gpu-power-prefs` =
IGD przez `reclockctl to-igd` + reboot); live przełączanie muxa jest poza
zakresem dla gmux.

Sekcja `[switch]`:

| key | default | znaczenie |
|---|---|---|
| `enable` | `true` | 1/0 — moduł włączony/wyłączony |
| `tick-ms` | `1000` | tick decyzyjny (== `poll-ms`) |
| `dwell-in-ms` | `3000` | dwell wejścia dla miękkiej promocji (busy-gated) |
| `dwell-out-ms` | `5000` | dwell wyjścia dla demote |
| `min-residence-ms` | `20000` | minimalny czas na dGPU po promocji (anti-flapping) |
| `cooldown-ms` | `45000` | cooldown po demote przed kolejną promocją |
| `wait-ready-ms` | `2000` | timeout czekania po power-off |
| `min-switch-gap-ms` | `10000` | minimalny odstęp między power-toggle |
| `temp-gate` | `82` | °C — nie promuj, gdy dGPU gorętszy |
| `busy-enter` | `80` | busy% dla miękkiej promocji |
| `busy-exit` | `40` | busy% dla demote |
| `pstate-settle-ms` | `10000` | nie pisz pstate po power-on — settle zegara GPU po D3hot→D0 (pierwsza zmiana clocka potrafi zawiesić workqueue nouveau) |
| `pstate-write-timeout-ms` | `2000` | zapis pstate w osobnym wątku; po timeoutcie daemon żyje dalej i wstrzymuje zapisy pstate |

`[dpower]` — backend power:

| key | default | znaczenie |
|---|---|---|
| `backend` | `manual` | `manual` (echo ON/OFF do vgaswitcheroo) lub `runpm` (`power/control`) |
| `autosuspend-ms` | `5000` | autosuspend delay dla `runpm` |
| `wait-idle-timeout-ms` | `5000` | skan fd `/proc` przed power-off (otwarte `/dev/dri`) |
| `wait-ready-timeout-ms` | `10000` | timeout reinitu nouveau po power-on (`runtime_status=active`) |

Co promuje / demote'uje:

- `[dgpu-hard]` — klasy okien wymuszające dGPU (twarda promocja, bez busy
  gate): `game`, `blender`, `steam`, `chromium`, `firefox`,
  `google-chrome`, `brave` (v5.2: przeglądarki — focus → dGPU ON + pstate
  `0a/0e` z profilu `[preferred]`; demote po przejściu focusu na terminal).
- `[dgpu-soft]` — klasy z miękką promocją busy-gated (na start puste).
- `[igpu]` — klasy zawsze na iGPU (democja): `foot`, `kitty`, `alacritty`.
- `[dgpu-procs]` — nazwy procesów (skan `/proc/*/comm`) wymagające dGPU:
  `cuda`, `blender`.

**Weryfikacja:** `reclockctl switch-status` pokazuje topologię, power dGPU,
target, ostatnią akcję i `nvram_prefs`. Gdy dGPU jest OFF, `pstate.sh set`
odmawia zapisu — zapis pstate na odciętej od zasilania karcie wiesza nouveau
(patrz seria `patches/kernel/` niżej).

### Sekcja `[low-power]`

Klasy okien, które wymuszają profil `default` (cap `07`) z priorytetem nad
`preferred`, gdy mają focus. Używaj dla terminali (`foot`, `alacritty`,
`kitty`, `ghostty`, `wezterm`, ...). Focus terminala wygrywa `07` nawet, gdy
apka preferred generuje obciążenie w tle.

### Sekcja `[fan]` — kontrola wentylatorów Apple SMC

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `enable` | `true` | Włącz kontrolę wentylatorów applesmc. |
| `temp-min` | `51` | °C, przy/poniżej którego wentylatory siedzą na `fanN_min`. |
| `temp-max` | `91` | °C, przy/powyżej którego wentylatory siedzą na `fanN_max`. |
| `temp-min-igd` | `41` | °C (tylko-iGPU, dGPU OFF), przy/poniżej którego wentylatory siedzą na `fanN_min`. |
| `temp-max-igd` | `91` | °C (tylko-iGPU, dGPU OFF), przy/powyżej którego wentylatory siedzą na `fanN_max`. |

RPM jest interpolowany liniowo między `fanN_min` i `fanN_max`, czytanymi
dynamicznie z sysfs przy starcie. Wyłączane cicho, gdy applesmc nie istnieje.

Gdy dGPU jest OFF (switchd, topologia IGD), hwmon nouveau znika i temp dla
wentylatorów = CPU (`coretemp`) — CPU może się grzać bezpiecznie wyżej niż
dGPU, stąd osobna, cichsza krzywa igd (domyślnie 41/91 °C, v5.1). Przy dGPU
ON działa standardowa krzywa `temp-min`/`temp-max` (51/91 °C). Wybór krzywej
wg **stanu power** dGPU (`sw.dgpu_off()`), nie topologii.

### `[profile default]` — aplikacje nie-preferred (terminal/edytor)

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `max-pstate` | `07` | Sufit pstate (indeks drabiny). Cap dla przejść UP. |
| `temp-down` | `65` | °C — utrzymane powyżej → TERMAL w dół o jeden krok. |
| `temp-up` | `58` | °C — utrzymane poniżej → UP dozwolone (jeśli obciążenie spełnione). |

### `[profile preferred]` — aplikacje preferred (przeglądarka)

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `max-pstate` | `0e` | Sufit pstate. |
| `boost-pstate` | `-1` | Tier boost poza drabiną, lub `-1` by wyłączyć. Domyślnie wyłączony (`0e`/`0a` oba stabilne, `0f` bez widocznego zysku na GT 750M). Ustaw `0f`, by włączyć. |
| `busy-boost` | `85` | % busy > → BOOST UP `0e→0f` po `boost-dwell`. |
| `boost-dwell-ms` | `5000` | Dwell utrzymanego busy > `busy-boost` do wejścia w boost. |
| `temp-down` | `82` | °C thermal down dla preferred. |
| `temp-up` | `75` | °C UP/BOOST dozwolone poniżej tego. |

### Sekcja `[global]` — wspólna polityka

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `interval-ms` | `200` | Okres próbkowania. |
| `poll-ms` | `1000` | Okres odpytywania `hyprctl` (≥ `interval-ms`). |
| `busy-up` | `80` | % busy > → UP o jeden krok (drabina). |
| `busy-down` | `40` | % busy ≤ → liczniki IDLE dwell. Histereza 80/40 = 40 pp. |
| `temp-dwell-ms` | `5000` | Dwell dla warunków termicznych. |
| `idle-dwell-ms` | `5000` | Dwell dla idle downclock. |
| `profile-dwell-ms` | `2000` | Rate-limit przełączeń profilu (ochrona przed alt-tab flap). |
| `win-ms` | `1000` | Okno przesuwne do uśredniania busy. |
| `exit-state` | `0a` | pstate zapisywany przy wyjściu SIGTERM. |
| `vblank-sync` | `true` | Wyrównuj zapisy pstate do vblank. |
| `gr-idle-promille` | `300` | ‰ chwilowego busy, poniżej którego przejścia DOWN są dozwolone (GR-idle gate). 300 = 30 %. `0` paraliżuje DOWN; `1000` wyłącza gate. |

### Flagi CLI

```
--config PATH          plik configu (domyślnie /etc/reclockd.conf)
--exit-state S         pstate przy wyjściu (hex, np. 0a)
--interval MS          okres próbkowania
--poll-ms MS           okres odpytywania hyprctl
--busy-up P            % busy > → UP o jeden krok
--busy-down P          % busy ≤ → idle dwell
--busy-boost P         % busy > → BOOST UP 0e→0f
--boost-dwell-ms MS    dwell dla utrzymanego busy>busy-boost przed wejściem w 0f
--boost-hyst P         pp rezerwa histerezy dla boost
--temp-up C            °C UP/BOOST dozwolone poniżej
--temp-down C          °C TERMAL w dół powyżej
--temp-dwell-ms MS     dwell termiczny
--idle-dwell-ms MS     dwell idle
--profile-dwell-ms MS  rate-limit przełączeń profilu
--win-ms MS            okno wygładzania busy
--vblank-sync / --no-vblank-sync
--probe                20 próbek busy, bez przejść
--dry-run              decyzje bez zapisu pstate
-v                     bardziej szczegółowe logowanie
```

### 🎛 Logika przejść (podsumowanie)

```
UP   07→0a→0e  (UP-LOAD):  g_cur_idx < sufit AND temp < temp_up (temp_dwell)
                          AND (busy > busy_up OR title_pref). Jeden krok/decyzja.
                          title_pref (tytuł aktywny w [preferred-titles])
                          wymusza UP z pominięciem reguły busy>busy_up (UP-TITLE).
                          UP NIE jest GR-idle gate'owane (rosnący zegar bezpieczniejszy).
DOWN 0e→0a→07 (TERMAL|IDLE|CEILING):
                          temp > temp_down (temp_dwell) OR
                          (busy ≤ busy_down (idle_dwell) AND !title_pref) OR
                          g_cur_idx > sufit.
                          DOWN jest GR-idle gate'owane: odraczane, dopóki
                          chwilowe busy > gr-idle-promille ( chroni GR mid-render).
                          TERMAL ma najwyższy priorytet ( > title, > próg gate).
                          IDLE jest suppressowane, dopóki title_pref (apka trzyma 0e).
BOOST 0e→0f  (BOOST-UP):  g_cur_idx == sufit AND busy > busy_boost (boost_dwell)
                          AND temp < temp_up (temp_dwell). Wyłączone, gdy
                          boost-pstate = -1 (domyślnie).
BOOST-DOWN 0f→0e:         temp > temp_up OR temp >= temp_down (INSTANT, priorytet)
                          OR busy < busy_up (histereza obciążenia).
```

Hierarchia priorytetów: **TERMAL > low-power terminal (→07) > title-match (→0e,
suppress IDLE, override busy) > class / running-busy (busy-gated) > IDLE.**

---

## 🖥 Użycie

### `reclockctl` — kontrola demona

```sh
reclockctl start          # systemctl start reclockd
reclockctl stop           # systemctl stop reclockd (przywraca exit-state, auto wentylatorów SMC)
reclockctl status         # status systemd + pstate + temp + override + wentylatory
reclockctl restart        # systemctl restart reclockd
reclockctl reload         # SIGHUP — ponownie wczytaj /etc/reclockd.conf bez restartu
reclockctl logs           # journalctl -u reclockd -f
reclockctl fan-off        # zamroź auto wentylatorów (steruj ręcznie przez sysfs)
reclockctl fan-on         # wznów auto wentylatorów
reclockctl switch-status  # v5.0 switchd: topologia, power dGPU, target, nvram_prefs
reclockctl dgpu-on        # v5.0 switchd override: wymuś power dGPU ON
reclockctl dgpu-off       # v5.0 switchd override: wymuś power dGPU OFF
reclockctl dgpu-auto      # v5.0 switchd: zdejmij override, wróć do polityki auto
reclockctl to-igd         # v5.0: NVRAM gpu-power-prefs=IGD + reboot (przełączenie boot-time)
reclockctl to-dis         # v5.0: NVRAM gpu-power-prefs=DIS + reboot
```

### `pstate.sh` — ręczna inspekcja / override pstate

```sh
pstate.sh status          # pstate + temp + override + stan demona
pstate.sh set 0a          # wymuś 0a, utwórz override (zamraża auto demona)
pstate.sh set ac:0a       # wymuś 0a, override
pstate.sh auto            # czyści override (demon wznawia auto)
```

Pstate na GT 750M (rdzeń / pamięć, MHz):

```
07: 270-405 / 838      0a: 270-925 / 1560
0e: 270-925 / 4000     0f: 270-925 / 5016
```

`0a / 0e / 0f` współdzielą ten sam zakres rdzenia (max 925 MHz); różnią się
taktem pamięci. `0e` i `0f` to agresywna zmiana taktowania pamięci — testuj
ostrożnie (znane zawieszenia na NVE0 przy max pstate). Ustawienia są
runtime-only; restart resetuje.

### Bezpośredni odczyt z debugfs

```sh
sudo cat /sys/kernel/debug/dri/0000:01:00.0/pstate
```

---

## 🩹 Łatki

### `patches/0001-nouveau-auto-reclock.patch`

Dodaje politykę auto-reclocku w jądrze do `clk/base.c` nouveau i
`include/nvkm/subdev/clk.h`: `nvkm_alarm` jako timer próbkujący, EMA obciążenia
oraz liczniki progów up/down, żeby nouveau mógł reclockować sam, bez demona w
przestrzeni użytkownika. To upstream-able wersja tego, co `reclockd` robi w
przestrzeni użytkownika. Aplikuj na drzewo źródłowe jądra Linux:

```sh
cd /path/to/linux
patch -p1 < /path/to/0001-nouveau-auto-reclock.patch
# zbuduj i zainstaluj moduł nouveau
```

### `patches/0002-mesa-nvc0-sched-data.patch`

Doprecyzowuje dane latencji schedulera codegenu Mesa nvc0 w
`nv50_ir_emit_nvc0.cpp` i `nv50_ir_target_nvc0.cpp`, żeby pasowały do latencji
wykonania NAK sm30: latencja `OP_EXIT`/`OP_RET` 14 → 15, wait `sched 0x00`
(JOIN/SYNC) 32 → 16 cykli, occupancy `OPCLASS_TEXTURE` 18 → 17 cykli, plus
reguła `OP_MEMBAR` 16 cykli memory-pipe busy. Efekt netto dla obciążeń
shader-bound na Keplerze: około +10-30%. Aplikuj na drzewo źródłowe Mesa (patrz
`build-mesa.sh`):

```sh
cd /path/to/mesa
patch -p1 < /path/to/0002-mesa-nvc0-sched-data.patch
```

### `patches/81-nouveau-kepler.rules` + `install-udev-rule.sh`

Reguła udev, która wymusza załadowanie modułu `nouveau` dla GT 750M (GK107, PCI
`10de:0fe9`) przy booście. Obchodzi pułapkę na Omarchy/Arch: `nvidia-utils` —
twarda zależność Hyprlanda i aquamarine — dostarcza
`/usr/lib/modprobe.d/nvidia-utils.conf` z `blacklist nouveau`, nawet gdy
proprietarny moduł `nvidia` **nie** jest zainstalowany. Blacklist blokuje
auto-load przez alias PCI, więc dGPU wstaje bez sterownika,
`/sys/kernel/debug/dri/*/pstate` nie istnieje, a `reclockd` kręci się w pustym.
Jawne `modprobe nouveau` po nazwie modułu omija blacklist (blokuje tylko
auto-load przez alias) — właśnie to ta reguła udev wywołuje przy dodaniu
urządzenia. Reguła przetrwa aktualizacje `nvidia-utils`, bo leży w `/etc`
(nadpisuje `/usr/lib`). Instalacja:

```sh
sudo ./install-udev-rule.sh
# albo ręcznie:
sudo install -m644 patches/81-nouveau-kepler.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
# reboot, albo natychmiast: sudo modprobe nouveau
```

### `patches/kernel/` — seria kernelowa MacBook Pro 11,3 (0002-0014)

Seria patchy kernelowych dla dual-GPU MacBooka Pro 15" Late 2013 (iGPU Intel +
NVIDIA GT 750M na muxie `gmux`). Nakładana w kolejności przez `build-kernel.sh`
na drzewo jądra Linux v7.1.8 (`0001` z `patches/` najpierw, potem `0002`-`0014`):

| patch | obszar | fix |
|---|---|---|
| `0002` | gmux/i915 | callback reinit vga_switcheroo dla panela |
| `0003` | gmux/i915 | apple-gmux power-cycle |
| `0004` | gmux/i915 | minimum jasności → off |
| `0005` | gmux/i915 | retry DPCD linku eDP |
| `0006` | nouveau | runtime PM hybrid scanout gate |
| `0007` | nouveau | reinit GK107 po power-cut (power-on) |
| `0008` | nouveau | ACPI bez PR3 → fallback D3hot |
| `0009` | nouveau | retrain linku PCIe po resume |
| `0010` | nouveau | GR-idle gate dla runtime suspend |
| `0011` | nouveau | poll linku PCIe przed D0 |
| `0012` | nouveau | timeout reply PMU (`gt215_pmu_send`) |
| `0013` | nouveau | timeout pstate calc (`nvkm_pstate_calc`) |
| `0014` | nouveau | IRQ_HANDLED dla prywatnej linii MSI (fix `Disabling IRQ #91`) |

`0011`-`0013` naprawiają realny hang: po power-on (gmux power-cut → D3hot→D0)
`pci_power_up` czyta PMCSR zanim link PCIe się wytrenuje, dostaje `0xFFFFFFFF`
i zwraca `-EIO`; PMU nigdy nie odpowiada i oba `gt215_pmu_send` oraz
`nvkm_pstate_calc` wiszą w `wait_event` — wątki jądra w D, czarny ekran.
`0011` polluje link do 2 s przed D0 w obu ścieżkach resume; `0012`/`0013`
dodają 2 s timeouty na reply PMU i workqueue pstate jako siatkę bezpieczeństwa.

`0014` zgłasza prywatną linię MSI jako obsłużoną: na MBP 11,3 nonstall fence
notify jest routowany na osobny przerwań NRHOST (leaf 1), który nie ma
handlera w `gk104_mc_intrs[]`. Burza nonstall (np. glmark2 `--frame-end none`
przy ~8000 FPS) sprawia, że `nvkm_intr()` zwraca `IRQ_NONE`, a kernelowy
detektor spurious-interrupt wyłącza linię („irq 91: nobody cared ...
Disabling IRQ #91") po 100k IRQ_NONE — dGPU bezużyteczny do reboota. Blokada
źródeł i tak wykonała się wcześniej i zostaje; linia jest tylko zgłaszana jako
obsłużona, żeby detektor się nią nie zajmował.

---

## 🛠 Skrypty

### `pstate.sh`

Inspekcja lub wymuszenie pstate GPU przez debugfs, zintegrowane z plikiem flagi
override demona. `set` tworzy `/run/reclockd/override` i zamraża tryb auto;
`auto` czyści. Wymaga root dla zapisu w debugfs. Rozszerzenia v5.0: gdy dGPU
jest OFF (switchd), `status` pokazuje fallback temperatury CPU (`coretemp`)
oraz monitoring RPS iGPU (read-only), a `set` odmawia zapisu pstate — zapis
pstate na odciętej od zasilania karcie wiesza nouveau. Dodatek v5.1: `status`
drukuje też sekcję `=== wentylatory ===` — aktywną krzywą fan
(`igd | dga | compiler | override | off`), jej zakres temperaturowy i aktualne
obroty, czytane z `/run/reclockd/status` (fallback do sysfs applesmc, gdy
daemon nie działa).

### `build-mesa.sh`

Instaluje zależności budowania (Arch pacman), nakłada
`0002-mesa-nvc0-sched-data.patch` na drzewo Mesa w `tmp/mesa`, konfiguruje
budowę meson nouveau-only (bez Vulkan/OpenCL/rust/llvm) i kompiluje. **Nie**
instaluje do systemu — uruchamiaj aplikacje z `LIBGL_DRIVERS_PATH` wskazującym
na wynik budowania, żeby porównać patchowane vs niepatchowane.

```sh
git clone https://gitlab.freedesktop.org/mesa/mesa.git tmp/mesa
./build-mesa.sh
```

### `mesa-manage.sh`

Chirurgiczna instalacja/rollback patchowanego sterownika Mesa nouveau bez
zakłócania innych sterowników (iris, swrast, ...). Działa względem układu
Arch Mesa 26.x „dril": kopiuje patchowany samodzielny `libdril_dri.so` jako
`/usr/lib/dri/nouveau_dri_patched.so` i przepina symlink `nouveau_dri.so` na
niego. Rollback przywraca symlink na systemowy `libdril_dri.so` i usuwa plik
patchowany. Komendy: `status | backup | install | restore | diff`. Flagi:
`--yes`, `--force`, `--dry-run`.

### `recover-gpu.sh`

Ratunkowy revert, gdy eksperyment GPU zepsuje wyświetlanie. Idempotentny: tylko
usuwa to, co znajdzie, nigdy nie aplikuje nowej konfiguracji GPU. Cofa
`AQ_DRM_DEVICES` w `~/.config/uwsm/default`, przywraca autologin SDDM z backupu,
a (z `--dedicated`) cofa multiplekser panelu `gpu-switch` z powrotem na dGPU.
Uruchom z SSH lub TTY, gdy ekran jest czarny.

```sh
./recover-gpu.sh                 # revert, bez restartu
./recover-gpu.sh --reboot        # revert, potem restart
./recover-gpu.sh --dedicated     # dodatkowo cofnij mux gpu-switch
```

### `build-kernel.sh`

Buduje i instaluje patchowane jądro nvkp (`LOCALVERSION=-nvkp`) jako
**osobny** wpis boot — nigdy nie nadpisuje działającego jądra: moduły lecą do
`/lib/modules/<rev>-nvkp`, `/boot` dostaje `vmlinuz-linux-nvkp`,
`initramfs-linux-nvkp.img` i `System.map-linux-nvkp`; wpis w bootloaderze to
GRUB (`grub-mkconfig`) lub Limine (`--limine`). Nakłada całą serię patchy
(`0001` → `0013`, patrz `patches/kernel/`). Opcje: `--jobs=N`, `--no-install`,
`--clean`, `--full-tree`. Ciepło buildu pokrywa boost wiatraków `[compiler]`
demona.

```sh
./build-kernel.sh --full-tree --limine
```

### `build-uki-nvkp.sh`

Buduje Unified Kernel Image (UKI) z jądra nvkp: regeneruje initramfs z hookiem
`encrypt` + keyfile LUKS, pakuje vmlinuz + initramfs + wbudowany cmdline w jeden
`/boot/EFI/Linux/nvkp.efi` i rejestruje wpis Limine `protocol: efi` (`//nvkp`).
Konieczne, bo Limine `protocol: efi` **nie** przekazuje initramfs — a legacy
`protocol: linux` bootuje na tym sprzęcie do czarnego ekranu (konsola VGA na
wyłączonym dGPU vs efifb na iGPU).

### `bench-gpu.sh`

Powtarzalny benchmark glmark2 porównujący iGPU (Intel Iris Pro 5200) z dGPU
(GT 750M) przy PEŁNEJ rozdzielczości panela (domyślnie `2880x1800`). Tryb
offscreen (`--frame-end none --swap-mode immediate`) usuwa copyback PRIME i
vsync kompozytora, które fałszują pomiary on-screen; pstate dGPU jest
przypinany przez debugfs (`0e`/`0a`/`07`) przy zatrzymanym daemonie. Wiatraki
wymuszone na 100%, `trap` przywraca wszystko przy wyjściu. `--dry-run` pokazuje
komendy bez wykonywania.

```sh
sudo ./bench-gpu.sh -g both -m offscreen -p all
```

---

## ⚙️ Jak to działa

Co `interval-ms` (domyślnie 200 ms), `reclockd`:

1. Próbkuje busy% GPU z liczników idle PMU BAR0 (`R_IDLE_COUNT` dla
   busy i total, reset po odczycie) — wartość 0-1000 promil niezależna od
   jakiegokolwiek wskazania sysfs sterownika.
2. Odczytuje temperaturę z hwmon `nouveau` (`temp1_input`).
3. Wygładza busy w oknie przesuwnym (`win-ms`, domyślnie 1 s).
4. Odpytuje `hyprctl` co `poll-ms` (domyślnie 1 s) o klasę i **tytuł** okna
   aktywnego i uruchomionych; aktualizuje aktywny profil (`default` vs
   `preferred`) z rate-limitingiem `profile-dwell`. Re-wykrywa Hyprlanda co
   cykl poll, dopóki sesja nie wstanie (demon startuje przed sesją
   użytkownika). Match tytułu (`[preferred-titles]`) ustawia `title_pref` i
   wymusza `0e` z pominięciem reguły busy; focus terminala (`[low-power]`)
   wymusza `default`.
5. Sprawdza liczniki dwell (temp-low, temp-high, idle, boost-up) względem
   progów aktywnego profilu.
6. Decyduje o docelowym pstate zgodnie z [logiką przejść](#logika-przejsc-podsumowanie)
   — jeden krok drabiny na decyzję, lub wejście/wyjście z tieru boost.
   Przejścia DOWN są GR-idle gate'owane (odraczane, dopóki chwilowe busy >
   `gr-idle-promille`).
7. Jeśli potrzebne przejście, opcjonalnie czeka na vblank
   (`DRM_IOCTL_WAIT_VBLANK`), a następnie zapisuje 2-hex pstate do pliku
   `pstate` w debugfs.
8. Co cykl poll, jeśli kontrola wentylatorów jest włączona, interpoluje RPM
   wentylatorów SMC z bieżącej temperatury (applesmc), niezależnie od decyzji
   pstate.

Deskryptor DRM do `card0` jest otwierany raz przy starcie do synchronizacji
vblank. Zaraz po `open()`, demon wywołuje `DRM_IOCTL_DROP_MASTER`, więc nigdy
nie trzyma DRM master i nigdy nie blokuje kompozytora. Patrz
[Problem DRM master](#problem-drm-master-i-dlaczego-reclockd-go-zrzuca).

Wszystkie ustawienia są **runtime-only**: restart resetuje GPU do taktowań
rozruchowych, a demon ponownie stosuje politykę przy następnym starcie. Przy
`SIGTERM` demon zapisuje `--exit-state` (domyślnie `0a`), przywraca poprzednią
wartość `power/control` i przywraca auto wentylatorów SMC (`fanN_manual=0`).

---

## 🛡 Bezpieczeństwo i ryzyka

- ⚠️ **Zawieszenia przy max pstate `0f`**: układy Keplera klasy NVE0 znane są
  z zawieszania się przy undervolcie na maksymalnym pstate. `0f` jest
  off-ladder w `reclockd` i włączany tylko przy utrzymanym obciążeniu + niskiej
  temperaturze; zabezpieczenie termiczne zrzuca go natychmiast. Jeśli w ogóle
  nie chcesz `0f`, pozostaw `boost-pstate` nieustawione w `reclockd.conf` (lub
  ustaw na wartość ≤ `max-pstate`, co wyłącza boost).
- 🎛 **Agresywna zmiana taktowania pamięci**: `0e` i `0f` agresywnie zmieniają
  taktowanie pamięci. Testuj ostrożnie na swojej konkretnej karcie. Bezpieczna
  drabina (`07 / 0a / 0e`) domyślnie omija najgorszy przypadek.
- 🌡 **Termika**: per-profil obniżenie termiczne ma priorytet nad obciążeniem.
  Jeśli hwmon jest niedostępny, warunki termiczne są pomijane **fail-safe**
  (nigdy emergency UP) — ale tracisz ochronę termiczną, więc monitoruj temp
  ręcznie.
- 🆘 **`recover-gpu.sh`** jest dostarczany na wypadek, gdy eksperyment GPU
  zepsuje wyświetlanie. Uruchom z SSH lub TTY.
- 🔐 **Demon root**: `reclockd` wymaga roota (mmap BAR0 + zapis w debugfs).
  Przejrzyj źródło przed uruchomieniem. To pojedyncza jednostka translacji
  C++17; cała polityka jest w `reclockd.cpp`.

---

## 📜 Licencja

MIT — patrz [LICENSE](LICENSE).