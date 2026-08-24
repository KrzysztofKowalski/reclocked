# reclocked

> Demon gubernatora pstate w przestrzeni użytkownika (`reclockd`) plus patche
> kernela i Mesa, które utrzymują starą kartę NVIDIA GT 750M (Kepler, GK107) na
> użytecznych taktowaniach pod otwartym sterownikiem **nouveau** — zamiast
> tkwić na taktowaniach rozruchowych i tracić 2-4x wydajności.

> English: [README.md](README.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-6c757d.svg)](#instalacja)
[![Language: C++17](https://img.shields.io/badge/C%2B%2B-17-00599c.svg)](#wymagania)

`reclocked` to mały, samodzielny demon w C++17 plus patche kernela i Mesa, które
przynoszą automatyczne re-taktowanie (reclocking) dla NVIDIA Kepler (GK107) pod
nouveau — bez zastrzeżonego sterownika, kernelowego gubernatora i bez wsparcia
reklokowania ze strony upstream.

---

## 🖥 O projekcie

Upstreamowy sterownik nouveau udostępnia ręczne re-taktowanie pstate przez
debugfs, ale **nie ma automatycznego gubernatora**: po rozruchu karta tkwi na
najniższym pstate (`07` = 270-405 MHz rdzeń / 838 MHz pamięć), dopóki ktoś nie
zapisaze wyższego pstate ręcznie. Na GT 750M oznacza to stratę 2-4x wydajności
we wszystkim — od kompozycji w przeglądarce po gry z heavy shader-bound, bez
żadnego skalowania termicznego ani obciążeniowego.

`reclockd` wypełnia tę lukę. To demon w przestrzeni użytkownika, który odpytuje
obciążenie GPU (busy%) przez liczniki bezczynności PMU na BAR0 oraz temperaturę
przez hwmon, i zapisuje przejścia pstate przez interfejs debugfs nouveau —
gubernator świadomy obciążenia i temperatury, którego upstream nouveau nigdy
nie dostarczył dla Keplera. Działa jako usługa systemd, współistnieje z
kompozytorem Wayland (patrz [Problem DRM master](#problem-drm-master-i-dlaczego-reclockd-go-zrzuca)),
i obsługuje profile świadome aplikacji (np. cap niskie dla terminala, pozwól
na boost dla przeglądarki).

To **nie** jest zamiennik brakującego kernelowego gubernatora nouveau — to
praktyczne obejście runtime-only dla sprzętu, który nouveau de facto porzucił.
Restart przywraca wszystko do taktowań rozruchowych; demon ponownie aplikuje
politykę przy następnym starcie.

### ♻️ Po co ten projekt istnieje: z elektrośmieci do używalności

Projekt testowany na **MacBook Pro 15" Late 2013 z NVIDIA GeForce GT 750M**
(GK107, Kepler, NVE7, sm_30). Ta maszyna jest legendarnie problematyczna na
otwartych sterownikach graficznych:

- Kepler dGPU na nouveau nie ma auto-rekloku, więc tkwi na taktowaniach
  rozruchowych (2-4x wolniej niż potrafi).
- Układ dual-GPU (Intel iGPU + NVIDIA dGPU połączony przez mulaplekser Apple
  `gmux`) sprawia, że live-switching GPU jest niestabilny.
- Maksymalny pstate (`0f`) znany jest z zawieszania się układów klasy NVE0 przy
  undervolcie.

W kombinacji te MacBooki Pro z tej generacji są wycofywane jako
**elektrośmieci**, bo „nie da się użyć dGPU porządnie pod Linuksem". `reclocked`
to próba zmiany tego: działający stos auto-rekloku (demon + patch kernela +
patch Mesa), który utrzymuje laptopa z 2013 z Keplerem jako używalną maszynę
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

- dGPU NVIDIA Kepler (GK107, NVE7) sterowany przez moduł kernela **nouveau**.
  Inne układy Keplera (GK104/GK106/GK208) lub karty nie-Kepler będą wymagały
  adaptacji wartości LADDER pstate, mapy rejestrów PMU BAR0 i lookupu hwmon.
- Moduł nouveau jest załadowany, a karta jest **aktywna runtime** (nie
  runtime-suspended). `reclockd` ustawia `power/control=on` na urządzeniu PCI
  na czas swojego działania i przywraca poprzednią wartość przy wyjściu.
- **Dostęp do pstate w debugfs**: kernel musi wystawiać
  `/sys/kernel/debug/dri/<bdf>/pstate` (nouveau zbudowane z
  `CONFIG_DRM_NOUVEAU_DEBUG=on`, co jest domyślne dla kerneli dystrybucyjnych).
  Demon zapisuje pstate przez ten plik, co wymaga **roota**.
- Laptop dual-GPU (Intel i915 primary + NVIDIA nouveau dGPU), gdzie `card0` to
  dGPU nouveau napędzające panel eDP (np. przez multiplekser `gmux`). Demon
  otwiera `/dev/dri/card0` tylko do synchronizacji vblank, a następnie
  natychmiast zrzuca DRM master (patrz niżej). Desktopy single-GPU nouveau też
  działają, o ile `card0` jest urządzeniem nouveau poddanym re-taktowaniu.
- **systemd** (dla dostarczonej jednostki `.service`) lub ręczny start.
  `hyprctl` jest opcjonalny — gdy jest dostępny, profile świadome aplikacji
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
  pstate na decyzję, nigdy skok `07 → 0e`. Przejścia są bramkowane obciążeniem
  i temperaturą z licznikami dwell.
- 🪟 **Profile świadome aplikacji**: `default` (cap `07`, termika konserwatywna
  — dla terminala/edytora) vs `preferred` (cap `0e` z tierem boost — dla
  przeglądarki). Profil jest wybierany z klasy okna aktywgo/uruchomionego
  raportowanej przez `hyprctl`. `profile-dwell` rate-limituje przełączanie,
  żeby alt-tab nie migał sufitem.
- 🚀 **Tier boost (`0f`)**: `0f` jest celowo **poza drabiną** — nigdy nie
  pojawia się jako krok w auto ladder. Włącza się tylko, gdy GPU jest już przy
  suficie drabiny (`0e`), busy > `busy-boost` przez `boost-dwell`, i temp <
  `temp-up` przez `temp-dwell`. Straż termiczny zrzuca go natychmiast.
- 🌡 **Per-profil straż termiczny**: downclock termiczny jest **per-profil**, a
  nie globalny. `default` throttluje przy 65°C / odzyskuje poniżej 58°C;
  `preferred` throttluje przy 82°C / odzyskuje poniżej 75°C. Thermal down ma
  priorytet nad obciążeniem w obu profilach.
- 🖥 **Synchronizacja vblank**: zapisy pstate są wyrównywane do vblank przez
  `DRM_IOCTL_WAIT_VBLANK`, żeby uniknąć glitchy w trakcie scanoutu.
- 🔓 **DROP_MASTER**: demon zrzuca DRM master natychmiast po otwarciu `card0`,
  więc nigdy nie blokuje kompozytora Wayland w przejęciu KMS. Patrz
  [Problem DRM master](#problem-drm-master-i-dlaczego-reclockd-go-zrzuca).
- ✋ **Ręczny override**: `pstate.sh set <pstate>` tworzy plik-flagę w
  `/run/reclockd/override`, który zamraża tryb auto; `pstate.sh auto` go
  czyści. Przydatne do benchmarków lub przypinania pstate.
- 🔔 **Live reload przez SIGHUP**: wyślij `SIGHUP` (lub `reclockctl reload`),
  żeby ponownie wczytać `/etc/reclockd.conf` bez restartu demona.
- 🛡 **Fail-safe**: brak hwmon → warunki termiczne pominięte (nigdy emergency
  UP); SIGTERM → przywróć `--exit-state`; CLI override wygrywa z configiem.
- 📦 **Bez zależności link na libdrm**: używa tylko surowych ioctl z
  `<drm/drm.h>`.

---

## 🔓 Problem DRM master (i dlaczego reclockd go zrzuca)

Ta sekcja dokumentuje problem projektowy, który dotyczy **każdego** narzędzia
DRM w przestrzeni użytkownika, które musi współistnieć z kompozytorem Wayland
(Hyprland, Sway, KDE itp.). Opisany jest tu jako ogólny wzorzec, a nie jako
quirk jednej maszyny.

### ⚠️ Problem

Gdy proces otwiera węzeł urządzenia DRM-primary (`/dev/dri/card0`) `O_RDWR`,
kernel czyni **pierwszego otwierającego** niejawnym DRM master. Jeśli demon
re-taktujący otworzy `card0` pierwszy i utrzyma master, a potem serwer
wyświetlania (Hyprland / SDDM / logind / libseat) wystartuje i spróbuje
przejąć to samo urządzenie przez `TakeDevice` — urządzenie jest zajęte
(`EBUSY`), bo demon wciąż trzyma master → libseat nie może otworzyć KMS →
kompozytor nie znajduje GPU → crashuje się → użytkownik dostaje **czarny
ekran** przy starcie sesji.

Objaw to czarny ekran zaraz po loginie, podczas gdy demon re-taktujący działa
sobie w tle trzymając master, którego nigdy nie używa do renderingu.

### ✅ Fix: DROP_MASTER

W `drm_open()`, `reclockd` otwiera `card0` z `O_RDWR | O_CLOEXEC` i
**natychmiast** wywołuje:

```c
ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
```

To zrzuca uprawnienia master (niezależnie od tego, czy demon był domyślnym
master, czy nie), pozostawiając go jako zwykły fd non-master. Re-taktowanie
nadal działa bez master: czekanie na vblank używa
`_DRM_VBLANK_RELATIVE` + `DRM_IOCTL_WAIT_VBLANK`, co nie wymaga master.
`EINVAL` / `ENODEV` (zwracane, gdy demon nie jest master, np. bo kompozytor
już jest) są oczekiwane i ignorowane. Ioctle pochodzą bezpośrednio z
`<drm/drm.h>` — brak zależności link na `libdrm`.

To jest ogólny wzorzec dla narzędzia DRM w przestrzeni użytkownika, które musi
współistnieć z kompozytorem Wayland: otwórz czego potrzebujesz, a potem zrzuć
master zanim wystartuje kompozytor. Sprawdzone przez współistnienie z Hyprland
na tym sprzęcie.

---

## 📁 Układ repozytorium

```
reclocked/
├── LICENSE                         MIT
├── README.md                       ten plik (EN)
├── CZYTAJMIE.md                    ten plik (PL)
├── .gitignore
├── reclockd/
│   ├── reclockd.cpp                źródło demona (~1200 linii, C++17)
│   ├── Makefile                    buduje ./reclockd (bez link na libdrm)
│   ├── reclockd.conf               domyślny config (profile, progi)
│   ├── reclockd.service            jednostka systemd (instaluje się do /etc/systemd/system/)
│   └── reclockctl                  wrapper CLI dla systemctl + pstate.sh
├── patches/
│   ├── 0001-nouveau-auto-reclock.patch   polityka auto-rekloku kernela nouveau
│   └── 0002-mesa-nvc0-sched-data.patch   dane latencji schedulera Mesa nvc0
├── pstate.sh                       inspekcja/wymuszanie pstate przez debugfs (+ override)
├── build-mesa.sh                   budowanie patchowanego Mesa (tylko nouveau)
├── mesa-manage.sh                  instalacja/rollback patchowanego sterownika Mesa
└── recover-gpu.sh                  ratunkowe odzyskiwanie, gdy zepsuje się wyświetlanie
```

---

## 📋 Wymagania

- **g++** z C++17 (`-std=c++17`).
- **Kernel Linux** z załadowanym modułem **nouveau** i wystawionym pstate w
  debugfs (`/sys/kernel/debug/dri/<bdf>/pstate` zapisywalny/czytelny przez
  root).
- **Nagłówki libdrm** (`<drm/drm.h>`) do definicji ioctl — zazwyczaj z pakietu
  dev `libdrm`. **Brak zależności link-time na libdrm.**
- **systemd** (opcjonalnie, ale zalecane, dla jednostki usługi).
- **hwmon** wystawiający `temp1_input` z nazwą `nouveau` (opcjonalnie; przy
  braku warunki termiczne są pomijane fail-safe).
- **hyprctl** (opcjonalnie, dla profili świadomych aplikacji). Bez niego demon
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
| `boost-pstate` | `0f` | Tier boost poza drabiną (musi być off-ladder i > sufit). |
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
                          AND busy > busy_up. Jeden krok na decyzję.
DOWN 0e→0a→07 (TERMAL|IDLE|CEILING):
                          temp > temp_down (temp_dwell) OR
                          busy ≤ busy_down (idle_dwell) OR
                          g_cur_idx > sufit.
BOOST 0e→0f  (BOOST-UP):  g_cur_idx == sufit AND busy > busy_boost (boost_dwell)
                          AND temp < temp_up (temp_dwell).
BOOST-DOWN 0f→0e:         temp > temp_up OR temp >= temp_down (INSTANT, priorytet)
                          OR busy < busy_up (histereza obciążenia).
```

---

## 🖥 Użycie

### `reclockctl` — kontrola demona

```sh
reclockctl start     # systemctl start reclockd
reclockctl stop      # systemctl stop reclockd (przywraca exit-state)
reclockctl status    # status systemd + pstate + temp + override
reclockctl restart   # systemctl restart reclockd
reclockctl reload    # SIGHUP — ponownie wczytaj /etc/reclockd.conf bez restartu
reclockctl logs      # journalctl -u reclockd -f
```

### `pstate.sh` — ręczna inspekcja / override pstate

```sh
pstate.sh status          # pstate + temp + override + stan demona
pstate.sh set 0a          # wymuś 0a, utwórz override (zamraża auto demona)
pstate.sh set ac:0a       # wymuś 0a, override
pstate.sh auto            # czyści override (demon wznawia auto)
```

Pstate'y na GT 750M (rdzeń / pamięć, MHz):

```
07: 270-405 / 838      0a: 270-925 / 1560
0e: 270-925 / 4000     0f: 270-925 / 5016
```

`0a / 0e / 0f` współdzielą ten sam zakres rdzenia (max 925 MHz); różnią się
taktem pamięci. `0e` i `0f` to agresywne re-taktowania pamięci — testuj
ostrożnie (znane zawieszenia na NVE0 przy max pstate). Ustawienia są
runtime-only; restart resetuje.

### Bezpośredni odczyt z debugfs

```sh
sudo cat /sys/kernel/debug/dri/0000:01:00.0/pstate
```

---

## 🩹 Patche

### `patches/0001-nouveau-auto-reclock.patch`

Dodaje politykę auto-rekloku kernela do `clk/base.c` nouveau i
`include/nvkm/subdev/clk.h`: `nvkm_alarm` jako timer próbkujący, EMA obciążenia
oraz liczniki progów up/down, żeby nouveau mógł re-taktować sam, bez demona w
przestrzeni użytkownika. To upstream-able wersja tego, co `reclockd` robi w
przestrzeni użytkownika. Aplikuj na drzewo źródłowe kernela Linux:

```sh
cd /path/to/linux
patch -p1 < /path/to/0001-nouveau-auto-reclock.patch
# zbuduj i zainstaluj moduł nouveau
```

### `patches/0002-mesa-nvc0-sched-data.patch`

Uszczelnia dane latencji schedulera codegenu Mesa nvc0 w
`nv50_ir_emit_nvc0.cpp` i `nv50_ir_target_nvc0.cpp`, żeby pasowały do latencji
wykonania NAK sm30: latencja `OP_EXIT`/`OP_RET` 14 → 15, wait `sched 0x00`
(JOIN/SYNC) 32 → 16 cykli, occupancy `OPCLASS_TEXTURE` 18 → 17 cykli, plus
reguła `OP_MEMBAR` 16 cykli memory-pipe busy. Efekt netto dla shader-bound
workloadów na Keplerze: około +10-30%. Aplikuj na drzewo źródłowe Mesa (patrz
`build-mesa.sh`):

```sh
cd /path/to/mesa
patch -p1 < /path/to/0002-mesa-nvc0-sched-data.patch
```

---

## 🛠 Skrypty

### `pstate.sh`

Inspekcja lub wymuszenie pstate GPU przez debugfs, zintegrowane z plikiem-flagi
override demona. `set` tworzy `/run/reclockd/override` i zamraża tryb auto;
`auto` czyści. Wymaga root dla zapisu w debugfs.

### `build-mesa.sh`

Instaluje zależności budowania (Arch pacman), aplikuje
`0002-mesa-nvc0-sched-data.patch` na drzewo Mesa w `tmp/mesa`, konfiguruje
budowanie meson nouveau-only (bez Vulkan/OpenCL/rust/llvm) i kompiluje. **Nie**
instaluje do systemu — uruchamiaj aplikacje z `LIBGL_DRIVERS_PATH` wskazującym
na wynik budowania, żeby porównać patchowane vs niepatchowane.

```sh
git clone https://gitlab.freedesktop.org/mesa/mesa.git tmp/mesa
./build-mesa.sh
```

### `mesa-manage.sh`

Chirurgiczna instalacja/rollback patchowanego sterownika Mesa nouveau bez
zakłócania innych sterowników (iris, swrast, ...). Działa względem layoutu
Arch Mesa 26.x „dril": kopiuje patchowany samodzielny `libdril_dri.so` jako
`/usr/lib/dri/nouveau_dri_patched.so` i przepina symlink `nouveau_dri.so` na
niego. Rollback przywraca symlink na systemowy `libdril_dri.so` i usuwa plik
patchowany. Komendy: `status | backup | install | restore | diff`. Flagi:
`--yes`, `--force`, `--dry-run`.

### `recover-gpu.sh`

Ratunkowy revert, gdy eksperyment GPU zepsuje wyświetlanie. Idempotentny: tylko
usuwa to, co znajdzie, nigdy nie aplikuje nowej konfiguracji GPU. Revertuje
`AQ_DRM_DEVICES` w `~/.config/uwsm/default`, przywraca autologin SDDM z backupu,
a (z `--dedicated`) cofa multiplekser panelu `gpu-switch` z powrotem na dGPU.
Uruchom z SSH lub TTY, gdy ekran jest czarny.

```sh
./recover-gpu.sh                 # revert, bez restartu
./recover-gpu.sh --reboot        # revert, potem restart
./recover-gpu.sh --dedicated     # dodatkowo cofnij mux gpu-switch
```

---

## ⚙️ Jak to działa

Co `interval-ms` (domyślnie 200 ms), `reclockd`:

1. Próbkuje busy% GPU z liczników bezczynności PMU BAR0 (`R_IDLE_COUNT` dla
   busy i total, reset po odczycie) — wartość 0-1000 promil niezależna od
   żadnego sysfs gauge'a sterownika.
2. Odczytuje temperaturę z hwmon `nouveau` (`temp1_input`).
3. Wygładza busy w oknie przesuwnym (`win-ms`, domyślnie 1 s).
4. Odpytuje `hyprctl` co `poll-ms` (domyślnie 1 s) o klasy okna aktywnego i
   uruchomionych; aktualizuje aktywny profil (`default` vs `preferred`) z
   rate-limitingiem `profile-dwell`.
5. Ewaluuje liczniki dwell (temp-low, temp-high, idle, boost-up) względem
   progów aktywnego profilu.
6. Decyduje o docelowym pstate zgodnie z [logiką przejść](#logika-przejsc-podsumowanie)
   — jeden krok drabiny na decyzję, lub wejście/wyjście z tieru boost.
7. Jeśli potrzebne przejście, opcjonalnie czeka na vblank
   (`DRM_IOCTL_WAIT_VBLANK`), a następnie zapisuje 2-hex pstate do pliku
   `pstate` w debugfs.

FD DRM do `card0` jest otwierany raz przy starcie do synchronizacji vblank.
Zaraz po `open()`, demon wywołuje `DRM_IOCTL_DROP_MASTER`, więc nigdy nie
trzyma DRM master i nigdy nie blokuje kompozytora. Patrz
[Problem DRM master](#problem-drm-master-i-dlaczego-reclockd-go-zrzuca).

Wszystkie ustawienia są **runtime-only**: restart resetuje GPU do taktowań
rozruchowych, a demon ponownie aplikuje politykę przy następnym starcie. Przy
`SIGTERM` demon zapisuje `--exit-state` (domyślnie `0a`) i przywraca poprzednią
wartość `power/control`.

---

## 🛡 Bezpieczeństwo i ryzyka

- ⚠️ **Zawieszenia przy max pstate `0f`**: układy Keplera klasy NVE0 znane są
  z zawieszania się przy undervolcie na maksymalnym pstate. `0f` jest
  off-ladder w `reclockd` i włączany tylko przy utrzymanym obciążeniu + niskiej
  temperaturze; straż termiczny zrzuca go natychmiast. Jeśli w ogóle nie chcesz
  `0f`, zostaw `boost-pstate` nieustawione w `reclockd.conf` (lub ustaw na
  wartość ≤ `max-pstate`, co wyłącza boost).
- 🎛 **Agresywne re-taktowanie pamięci**: `0e` i `0f` agresywnie re-taktują
  pamięć. Testuj ostrożnie na swojej konkretnej karcie. Bezpieczna drabina
  (`07 / 0a / 0e`) domyślnie omija najgorszy przypadek.
- 🌡 **Termika**: per-profil thermal down ma priorytet nad obciążeniem. Jeśli
  hwmon jest niedostępny, warunki termiczne są pomijane **fail-safe** (nigdy
  emergency UP) — ale tracisz ochronę termiczną, więc monitoruj temp ręcznie.
- 🆘 **`recover-gpu.sh`** jest dostarczany na wypadek, gdy eksperyment GPU
  zepsuje wyświetlanie. Uruchom z SSH lub TTY.
- 🔐 **Demon root**: `reclockd` wymaga roota (mmap BAR0 + zapis w debugfs).
  Przejrzyj źródło przed uruchomieniem. To pojedyncza jednostka translacji
  C++17; cała polityka jest w `reclockd.cpp`.

---

## 📜 Licencja

MIT — patrz [LICENSE](LICENSE).