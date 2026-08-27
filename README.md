# reclocked

> A userspace pstate governor daemon (`reclockd`) plus kernel and Mesa patches
> that keep an old NVIDIA GT 750M (Kepler, GK107) running at usable clocks
> under the open-source **nouveau** driver — instead of sitting at boot clocks
> and losing 2-4x performance.

> Polski: [CZYTAJMIE.md](CZYTAJMIE.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: Linux](https://img.shields.io/badge/Platform-Linux-6c757d.svg)](#installation)
[![Language: C++17](https://img.shields.io/badge/C%2B%2B-17-00599c.svg)](#requirements)

`reclocked` is a small, self-contained C++17 userspace daemon plus kernel and
Mesa patches that bring automatic pstate reclocking to NVIDIA Kepler (GK107)
under nouveau — without any proprietary driver, kernel governor, or reclock
from the upstream stack.

---

## 🖥 About

The upstream nouveau driver exposes manual pstate reclocking through debugfs, but
it has **no automatic governor**: after boot the GPU stays at the lowest pstate
(`07` = 270-405 MHz core / 838 MHz mem) until someone manually writes a higher
pstate. On a GT 750M that means a 2-4x performance loss in everything from browser
compositing to shader-bound games, with no thermal or load-aware scaling.

`reclockd` fills that gap. It is a userspace daemon that polls GPU busy% (via
BAR0 PMU idle counters) and temperature (via hwmon), and writes pstate transitions
through the nouveau debugfs interface — a load-and-thermal-aware governor that
nouveau upstream never shipped for Kepler. It runs as a systemd service, coexists
with a Wayland compositor (see [The DRM master problem](#the-drm-master-problem-and-why-reclockd-drops-it)),
and supports app-aware profiles (e.g. cap low for a terminal, allow boost for a
browser).

This is **not** a replacement for the missing nouveau kernel governor — it is a
practical, runtime-only workaround for hardware that nouveau has effectively
abandoned. Reboot resets everything to boot clocks; the daemon re-applies policy
on next start.

### ♻️ Why this project exists: from e-waste to usable

This project is tested on a **MacBook Pro 15" Late 2013 with the NVIDIA GeForce GT
750M** (GK107, Kepler, NVE7, sm_30). That machine is legendarily problematic on
open-source graphics:

- The Kepler dGPU on nouveau has no auto-reclock, so it sits at boot clocks
  (2-4x slower than its capable clocks).
- The dual-GPU layout (Intel iGPU + NVIDIA dGPU joined by an Apple `gmux` mux)
  makes live GPU switching unstable.
- Max pstate (`0f`) is known to lock up on NVE0-class parts under undervolt.

The combination means this generation of MacBook Pros is being retired as
**e-waste** because "you can't use the dGPU properly on Linux." `reclocked` is
the attempt to change that: a working auto-reclock stack (daemon + kernel patch +
Mesa patch) that keeps a 2013 Kepler laptop usable as a daily driver in 2026.
From e-waste to usable.

---

## 🐧 Hardware target & Assumptions

**Tested on:** MacBook Pro 15" Late 2013, NVIDIA GeForce GT 750M (GK107 / Kepler /
NVE7 / sm_30), Arch Linux + nouveau. The Linux environment is
[**Omarchy**](https://omarchy.dev) (Arch-based, Hyprland/Wayland) — **recommended**
for this hardware, and the combo on which the DRM-master / black-screen problem
was debugged and the `DROP_MASTER` fix verified against the Hyprland compositor.

**Assumptions:**

- A NVIDIA Kepler dGPU (GK107, NVE7) driven by the **nouveau** kernel module.
  Other Kepler parts (GK104/GK106/GK208) or non-Kepler GPUs will need adaptation of
  the LADDER pstate values, the BAR0 PMU register map, and the hwmon lookup.
- The nouveau module is loaded and the card is **runtime-active** (not
  runtime-suspended). `reclockd` sets `power/control=on` on the PCI device for
  the duration of its run and restores the previous value on exit.
- **debugfs pstate access**: the kernel must expose
  `/sys/kernel/debug/dri/<bdf>/pstate` (nouveau built with
  `CONFIG_DRM_NOUVEAU_DEBUG=on`, which is the default for distro kernels). The
  daemon writes pstate through this file, which requires **root**.
- A dual-GPU laptop (Intel i915 primary + NVIDIA nouveau dGPU) where `card0` is
  the nouveau dGPU driving the eDP panel (e.g. via `gmux` mux). The daemon opens
  `/dev/dri/card0` for vblank sync only, then immediately drops DRM master (see
  below). Single-GPU nouveau desktops work too, as long as `card0` is the
  reclocked nouveau device.
- **systemd** (for the provided `.service` unit) or manual launch. `hyprctl`
  is optional — when present, app-aware profiles are enabled; when absent, the
  daemon falls back to the `default` profile.
- The dGPU PCI BDF in `reclockd.cpp` defaults to `0000:01:00.0`. Adjust
  `PCI_RESOURCE`, `PSTATE_FILE`, `POWER_CTRL`, and `DRM_CARD` in the source if
  your card lives at a different address.

**Not for:** non-Kepler GPUs without code adaptation, and anyone uncomfortable
running a root daemon that rewrites GPU clocks every 200 ms. Read
[Safety / Risks](#safety--risks) first.

---

## ✨ Features

- 🎛 **3-step safe LADDER** (`07 ↔ 0a ↔ 0e`): one pstate step per decision, never a
  jump `07 → 0e`. Transitions are load- and thermal-gated with dwell counters.
- 🪟 **App-aware profiles**: `default` (cap `07`, conservative thermal — for
  terminal/editor) vs `preferred` (cap `0e` — for browser). Profile is chosen
  from the focused or running window class reported by `hyprctl`, **or** from
  the focused window title (see title-priority below). `profile-dwell`
  rate-limits switching so alt-tab doesn't flap the ceiling.
- 🔁 **Hyprland re-detect**: the daemon starts as a system service *before* the
  user session, so the `hyprctl` socket is not yet up at startup. Instead of a
  one-shot check that leaves the daemon stuck on `default` for the whole
  session, `reclockd` re-detects Hyprland every poll cycle until the session
  appears — app-aware profiling activates within ~1 s of login.
- 🛑 **GR-idle gate**: a live memory downclock while the GR engine is mid-render
  can wedge the compositor (Kepler rewrites framebuffer timings mid-frame →
  render-target PROP traps). vblank sync only protects scanout, not the GR
  engine. DOWN transitions are therefore **deferred** until the instantaneous
  busy falls below `gr-idle-promille` (default 300 ‰ = 30%). UP transitions
  are not gated (a rising clock is safer mid-render).
- 🏷 **Title-priority (Discord / YouTube)**: browser tabs like Discord and
  YouTube have no window class of their own — their identity shows up in the
  window *title*. `[preferred-titles]` matches the focused title
  case-insensitively and, on match, **forces `0e` overriding the `busy > 80%`
  rule** (these workloads are memory-bound, with GR busy often 16-36 %, so the
  normal UP-LOAD path never fires). IDLE downshift is suppressed while the
  title matches, so the app holds `0e`. Terminal focus still wins `07` (see
  low-power below) — both key off the same focused window, so they don't
  conflict. Priority order: TERMAL > low-power terminal > title-match > class
  / running-busy > IDLE.
- 💻 **Low-power terminals**: `[low-power]` lists window classes (e.g.
  `foot`, `alacritty`, `kitty`, `ghostty`, `wezterm`). When a terminal has
  focus, the daemon forces the `default` profile (cap `07`) with priority over
  `preferred` — even if a browser is generating load in the background.
- 🚀 **Boost tier (`0f`, disabled by default)**: `0f` is intentionally
  **off-ladder** and is now **off by default** (`boost-pstate = -1`). On this
  GT 750M, `0e` and `0a` are both stable and `0f` shows no visible gain over
  `0e`, so boost is opt-in. Enable it by setting `boost-pstate = 0f` in
  `reclockd.conf`: it only engages at the ladder ceiling (`0e`) under
  sustained busy > `busy-boost` for `boost-dwell` with temp < `temp-up`, and
  the thermal guard drops it instantly. NVE0 lockups at max pstate are known.
- 🎯 **Per-class `[caps]` policy (v4.4)**: a per-class `floor` / `max` /
  `busy-up` (`class = floor=0a, max=0e, busy-up=50`) — `floor` = resting state
  (IDLE never goes below), `max` = own ceiling, `busy-up` = own UP-LOAD
  threshold (0 = the global 80 %). A class with a `[caps]` entry is
  *preferred while focused* but is **not** caught by title-priority — a
  Discord *tab* in a browser (chromium/firefox, not in `[caps]`) still gets the
  force-`0e` title path. Example — Desktop Discord: base `0a`, busy > 50 % →
  `0e`, idle → back to `0a`. TERMAL remains top priority (may drop below
  `floor`).
- 🛡 **Self-healing after suspend/resume (v4.5)**: deep sleep (S3) cuts the
  GPU power and drops the PMU busy-counter config in BAR0 — busy sampling then
  returns a constant 1000‰, which blocks the IDLE downshift and the GR-idle
  gate, leaving the daemon stuck at `0e`. v4.5 readbacks `R_IDLE_CTRL` every
  cycle and, when the config is lost, re-initializes the counters and resets
  the busy window (logged to the journal). Complements the `system-sleep`
  hook (daemon restart on post-resume) as a safety net. No config change.
- 🌬 **Compiler → fans 100% (v4.6)**: a running compiler or build tool
  (`clang*`, `gcc*`, `g++*`, `cc`, `c++`, `cc1`, `cc1plus`, `make`, `cmake`,
  `ninja`, `cargo`, `rustc`, `meson`, `go`, `javac`, `ld`, `as`, `sccache`,
  `ccache`, ...) drives the fans to max RPM — no thermal throttling mid-build.
  Detection scans `/proc/*/comm` (cmdline fallback) every poll cycle and also
  matches version prefixes (`gcc-14`, `clang-18`, ...). Boost applies only in
  auto-mode — a manual fan override keeps priority. When the build ends, fans
  return to the temperature curve. Config: `[compiler]` (`enable` / `fan-max`
  / `names`).
- 🔀 **switchd — dGPU power-state + render routing (v5.0)**: an optional
  power-state module for dual-GPU Apple machines (Intel iGPU + NVIDIA dGPU on
  a `gmux` mux). In DIS topology (dGPU drives the panel) it runs in MONITOR
  mode — no power changes (power-off is blocked by the scanout gate). The
  power executor activates after a **boot-time** switch to IGD
  (`gpu-power-prefs` via `reclockctl to-igd` + reboot; live mux switching is
  out of scope). Soft promotion is busy-gated, hard promotion comes from
  `[dgpu-hard]` window classes / `[dgpu-procs]` processes, demotion from
  `[igpu]`. `pstate-settle-ms` waits out the GPU clock settle after power-on
  (first clock change after D3hot→D0 can hang the nouveau workqueue),
  `pstate-write-timeout-ms` timeouts a stuck pstate write. CLI:
  `reclockctl switch-status | dgpu-on | dgpu-off | dgpu-auto | to-igd | to-dis`.
- 🌬 **iGPU-only fan curve (v5.1)**: with only the iGPU active (dGPU OFF in
  IGD topology) the nouveau hwmon disappears, so the fan driver reads the CPU
  (`coretemp`) temperature — which can safely run hotter than the dGPU. A
  separate, quieter curve applies: `temp-min-igd` / `temp-max-igd` (defaults
  41/91 °C) — fans hit max at 91 °C instead of 67 °C. The curve is chosen by
  the dGPU **power state** (`sw.dgpu_off()`), not the topology: OFF → iGPU
  curve, ON → the standard 40/67 °C. `pstate.sh status` now also prints an
  `=== wentylatory ===` section — the active curve + range + RPMs from
  `/run/reclockd/status` (applesmc sysfs fallback when the daemon is down).
- 🔀 **Browsers force the dGPU on (v5.2)**: browser classes (`chromium`,
  `firefox`, `google-chrome`, `brave`) were added to `[dgpu-hard]` — browser
  focus promotes the dGPU (power-on) with the `[preferred]` profile (pstate
  `0a/0e`); focus moving to a terminal demotes back (`[igpu]`/`[low-power]`).
  Config-only — the daemon picks it up live via `reclockctl reload` (SIGHUP),
  no rebuild.
- 🌡 **Per-profile thermal guard**: thermal downclock is **per-profile**, not
  global. `default` throttles at 65°C / recovers below 58°C; `preferred`
  throttles at 82°C / recovers below 75°C. Thermal down is prioritized over
  load (and over title-priority) in both profiles.
- 🌀 **Fan control (applesmc)**: on Apple laptops, `reclockd` also drives the
  SMC fans via `/sys/devices/platform/applesmc.768/`. A linear temp→RPM curve
  is interpolated between `fanN_min` and `fanN_max`, which are **read
  dynamically from sysfs at startup** (not hardcoded). Default band 40-67 °C,
  updated every poll cycle, independent of pstate. `reclockctl fan-off`
  freezes auto (drive fans manually); `fan-on` resumes. Fail-safe restores SMC
  auto (`manual=0`) on exit.
- 🖥 **vblank sync**: pstate writes are aligned to vblank via
  `DRM_IOCTL_WAIT_VBLANK` to avoid mid-scanout glitches.
- 🔓 **DROP_MASTER**: the daemon drops DRM master immediately after opening
  `card0` so it never blocks a Wayland compositor from taking KMS. See
  [The DRM master problem](#the-drm-master-problem-and-why-reclockd-drops-it).
- ✋ **Manual override**: `pstate.sh set <pstate>` creates a flag-file at
  `/run/reclockd/override` that freezes auto mode; `pstate.sh auto` clears it.
  Useful for benchmarking or pinning a pstate.
- 🔔 **SIGHUP live reload**: send `SIGHUP` (or `reclockctl reload`) to re-read
  `/etc/reclockd.conf` without restarting the daemon.
- 🛡 **Fail-safe**: missing hwmon → thermal conditions skipped (never an emergency
  UP); missing applesmc → fan control disabled silently; SIGTERM → restore
  `--exit-state` and SMC fan auto; CLI overrides win over config.
- 📦 **No libdrm link dependency**: uses raw `<drm/drm.h>` ioctls only.

---

## 🔓 The DRM master problem (and why reclockd drops it)

This section documents a design problem that affects **any** userspace DRM
utility that needs to coexist with a Wayland compositor (Hyprland, Sway, KDE,
etc.). It is described here as a general pattern, not as a quirk of one machine.

### ⚠️ The problem

When a process opens a DRM-primary device node (`/dev/dri/card0`) `O_RDWR`, the
kernel makes the **first opener** the implicit DRM master. If a reclocking
daemon opens `card0` first and holds master, then the display server (Hyprland /
SDDM / logind / libseat) starts later and tries to acquire the same device via
`TakeDevice` — the device is busy (`EBUSY`) because the daemon still holds
master → libseat cannot open KMS → the compositor finds no GPUs → it crashes →
the user gets a **black screen** on session start.

The symptom is a black screen right after login, with the reclocking daemon
running happily in the background holding master it never uses for rendering.

### ✅ The fix: DROP_MASTER

In `drm_open()`, `reclockd` opens `card0` with `O_RDWR | O_CLOEXEC` and then
**immediately** calls:

```c
ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
```

This drops master privileges (whether the daemon was the default master or not),
leaving it as an ordinary non-master fd. Reclocking still works without master:
the vblank wait uses `_DRM_VBLANK_RELATIVE` + `DRM_IOCTL_WAIT_VBLANK`, which does
not require master. `EINVAL` / `ENODEV` (returned when the daemon is not master,
e.g. the compositor already is) are expected and ignored. The ioctls come from
`<drm/drm.h>` directly — no `libdrm` link dependency.

This is the general pattern for a userspace DRM utility that must coexist with a
Wayland compositor: open what you need, then drop master before the compositor
starts. Proven by coexistence with Hyprland on this hardware.

---

## 📁 Repository layout

```
reclocked/
├── LICENSE                         MIT
├── README.md                       this file
├── .gitignore
├── examples/
│   ├── reclockd.conf.pl            default config — Polish comments
│   └── reclockd.conf.en            default config — English comments
├── reclockd/
│   ├── reclockd.cpp                daemon source (~3070 lines, C++17)
│   ├── Makefile                    builds ./reclockd (no libdrm link)
│   ├── reclockd.conf               default config (profiles, thresholds, [switch])
│   ├── reclockd.service            systemd unit (installs to /etc/systemd/system/)
│   └── reclockctl                  CLI wrapper for systemctl + pstate.sh (+ switchd)
├── patches/
│   ├── 0001-nouveau-auto-reclock.patch   nouveau kernel auto-reclock policy
│   ├── 0002-mesa-nvc0-sched-data.patch   Mesa nvc0 scheduler latency data
│   ├── 0003-reclockd-caps-ceiling.patch  reclockd v4.4 per-class [caps] policy
│   ├── 0004-reclockd-s3-selfheal.patch   reclockd v4.5 self-healing busy counters after S3
│   ├── 0005-reclockd-compiler-fan.patch  reclockd v4.6 compiler detected → fans 100%
│   ├── 81-nouveau-kepler.rules           udev rule: force-load nouveau (bypass nvidia-utils blacklist)
│   ├── reclockd.conf-caps.diff           reclockd.conf diff — [caps] Discord 0a/0e busy-gated
│   ├── reclockd.conf-compiler.diff       reclockd.conf diff — [compiler] fan boost
│   └── kernel/                           MacBook Pro 11,3 kernel series 0002-0014
├── install-udev-rule.sh            installs the udev rule above
├── pstate.sh                       inspect/force pstate via debugfs (+ iGPU RPS / coretemp)
├── build-mesa.sh                   build patched Mesa (nouveau-only)
├── mesa-manage.sh                  install/rollback patched Mesa driver
├── recover-gpu.sh                  emergency recovery if display breaks
├── build-kernel.sh                 build + install the patched nvkp kernel
├── build-uki-nvkp.sh               build UKI (vmlinuz+initramfs+cmdline) for Limine protocol: efi
└── bench-gpu.sh                    repeatable glmark2 iGPU-vs-dGPU benchmark (full-res, offscreen)
```

---

## 📋 Requirements

- **g++** with C++17 (`-std=c++17`).
- **Linux kernel** with the **nouveau** module loaded and debugfs pstate
  exposed (`/sys/kernel/debug/dri/<bdf>/pstate` readable/writable by root).
- **libdrm headers** (`<drm/drm.h>`) for ioctl definitions — typically provided
  by the `libdrm` dev package. **No link-time dependency on libdrm.**
- **systemd** (optional but recommended, for the service unit).
- **hwmon** exposing `temp1_input` with name `nouveau` (optional; if absent,
  thermal conditions are skipped fail-safe).
- **hyprctl** (optional, for app-aware profiles). Without it, the daemon runs
  the `default` profile only.
- **root** to run the daemon (mmap BAR0 + write debugfs pstate).

---

## 🔧 Installation

### 1. Build reclockd

```sh
cd reclockd
make
```

This produces `reclockd/reclockd`. There is no libdrm link step; only the headers
are needed at compile time.

### 2. Install files (system-wide)

```sh
sudo install -m755 reclockd/reclockd      /usr/local/bin/reclockd
sudo install -m644 reclockd/reclockd.conf /etc/reclockd.conf
sudo install -m644 reclockd/reclockd.service /etc/systemd/system/reclockd.service
sudo install -m755 reclockd/reclockctl    /usr/local/bin/reclockctl
sudo install -m755 pstate.sh              /usr/local/bin/pstate.sh
```

### 3. Enable and start the service

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now reclockd
```

Verify:

```sh
reclockctl status          # systemd status + pstate + temp + override
sudo cat /sys/kernel/debug/dri/0000:01:00.0/pstate
journalctl -u reclockd -f  # live decisions
```

---

## ⚙️ Configuration & Parameters

`reclockd` reads `/etc/reclockd.conf` by default (override with `--config`).
The config is a tiny INI-line format parsed by a built-in parser (no external
deps). CLI flags override config values.

### `[preferred]` section

List of `hyprctl` window classes that trigger the `preferred` profile when
focused, or when running + busy > `busy-up`. Example: browsers.

### `[preferred-titles]` section

Window **titles** (case-insensitive substring match) that trigger the
`preferred` profile with **title-priority** — forcing `0e` overriding the
`busy > busy-up` rule. Use this for browser tabs that have no window class of
their own (Discord, YouTube). On match, IDLE downshift is also suppressed so
the app holds `0e`. Example: `Discord`, `YouTube`, ` - YouTube`.

### `[caps]` section — per-class policy

Per-class floor / ceiling / UP-LOAD threshold for apps that have a real window
class (Electron, Chrome PWA). Syntax: `class = floor=0a, max=0e, busy-up=50`
(each key optional):

| Key | Default | Meaning |
|---|---|---|
| `floor` | `-` (off) | Resting pstate — IDLE never goes below this ladder state. |
| `max` | profile ceiling | Own ceiling (ladder state), overrides the profile cap. |
| `busy-up` | `0` (global 80) | Own UP-LOAD threshold (%). `0` = the global `busy-up`. |

A class with a `[caps]` entry behaves like `[preferred]` **while focused**, but
is **not** matched by `[preferred-titles]` — no force-`0e` on title match. Use
it for Desktop Discord — both possible classes (Electron `discord`, PWA
`chrome-discord.com__channels_@me-Default`) get the same policy: base `0a`,
busy > 50 % → `0e`, idle → back `0a`. TERMAL stays top priority (may go below
`floor`). A Discord **tab** in a browser has no `[caps]` entry and keeps the
force-`0e` title path unchanged.

```ini
[caps]
discord = floor=0a, max=0e, busy-up=50
chrome-discord.com__channels_@me-Default = floor=0a, max=0e, busy-up=50
```

### 🛡 Suspend/resume self-healing (v4.5)

Deep sleep (S3) cuts the GPU power rail. On resume, nouveau runs a full
`devinit`, but the PMU busy-counter config in BAR0 (`R_IDLE_CTRL` /
`R_IDLE_MASK`) is **not restored** — `reclockd` initializes it once at
startup (`init_counters()`). Without it, `sample()` returns a constant
1000‰, which blocks the IDLE downshift and the GR-idle gate, leaving the
daemon stuck at `0e` (report 63).

v4.5 self-heals: the main loop readbacks `R_IDLE_CTRL` every cycle; when
`(ctrl & CTRL_VALUE_MASK) != CTRL_VALUE_ALWAYS` (config lost — typically
after resume), it re-runs `init_counters()`, resets the busy window and
logs `PMU busy counters config lost (post-resume?) — reinitializing`.

The `system-sleep` hook is the safety net:
`/usr/lib/systemd/system-sleep/reclockd-resume` restarts the daemon in the
`post` phase, re-synchronizing the counters and the pstate index. Together
they prevent the `0e` lockup after sleep. No config change needed.

**Verify after `systemctl suspend`:** `reclockctl status` should settle back
to `07` (not stick at `0e`), the daemon MainPID should be new (hook
restart), and `journalctl -u reclockd -n 50` may show the reinit log.

### 🌬 Compiler → fans 100% (v4.6)

Builds are bursty: a big compile can peg the CPU while the GPU idles, so the
GPU-temp-driven curve never raises the fans — and the CPU thermal-throttles
mid-build. v4.6 scans `/proc/*/comm` every poll cycle; when a compiler or
build tool is running, `Fan::set_boost()` drives both fans to their max RPM
(or `fan-max` %) and holds them until the build ends, then falls back to the
temperature curve.

Detected by default: `clang*`, `gcc*`, `g++*` (any version suffix), `cc`,
`c++`, `cc1`, `cc1plus`, `make`, `cmake`, `ninja`, `cargo`, `rustc`, `meson`,
`go`, `javac`, `ld`, `as`, `sccache`, `ccache`. Process name comes from
`/proc/*/comm`; when `comm` is not a real name (kernel threads `[xyz]`), it
falls back to the `cmdline` basename.

`[compiler]` section (all optional):

| key       | default | meaning                                              |
|-----------|---------|------------------------------------------------------|
| `enable`  | `true`  | 1/0 — turn the whole feature off                     |
| `fan-max` | `100`   | % of max fan RPM to apply (50 = half range)          |
| `names`   | —       | extra process names, comma-separated (e.g. `mycc`)   |

Boost only applies in **auto-mode** — a manual override (`reclockctl fan-off`,
`/run/reclockd/fan-override`) keeps priority and is never overwritten.
**Verify:** start a `make -j` or `g++` build — fans jump to max within ~1 s
(`journalctl -u reclockd` logs
`fan: KOMPILATOR wykryty (cc1) -> fan1=... fan2=... (boost 100%)`); when the
build finishes, fans return to the curve.

### 🔀 switchd — dGPU power-state + render routing (v5.0)

Optional power-state module for dual-GPU Apple machines (Intel iGPU + NVIDIA
dGPU joined by a `gmux` mux). In the default DIS topology (dGPU drives the
panel) it runs in **MONITOR** mode — it reads status but never touches power
(a power-off would black the panel; the scanout gate blocks it). The power
executor activates only after a **boot-time** switch to IGD
(`gpu-power-prefs` = IGD via `reclockctl to-igd` + reboot); live mux switching
is out of scope for the gmux.

`[switch]` section:

| key | default | meaning |
|---|---|---|
| `enable` | `true` | 1/0 — module on/off |
| `tick-ms` | `1000` | decision tick (== `poll-ms`) |
| `dwell-in-ms` | `3000` | entry dwell for soft promotion (busy-gated) |
| `dwell-out-ms` | `5000` | exit dwell for demote |
| `min-residence-ms` | `20000` | minimum time on the dGPU after promotion (anti-flapping) |
| `cooldown-ms` | `45000` | cooldown after demote before a new promotion |
| `wait-ready-ms` | `2000` | timeout for the wait after power-off |
| `min-switch-gap-ms` | `10000` | minimum gap between power toggles |
| `temp-gate` | `82` | °C — don't promote when the dGPU is hotter |
| `busy-enter` | `80` | busy% for soft promotion |
| `busy-exit` | `40` | busy% for demote |
| `pstate-settle-ms` | `10000` | don't write pstate after power-on — GPU clock settle after D3hot→D0 (the first clock change can hang the nouveau workqueue) |
| `pstate-write-timeout-ms` | `2000` | pstate write runs in a separate thread; on timeout the daemon stays alive and pauses pstate writes |

`[dpower]` — power backend:

| key | default | meaning |
|---|---|---|
| `backend` | `manual` | `manual` (echo ON/OFF to vgaswitcheroo) or `runpm` (`power/control`) |
| `autosuspend-ms` | `5000` | autosuspend delay for `runpm` |
| `wait-idle-timeout-ms` | `5000` | `/proc` fd scan before power-off (open `/dev/dri`) |
| `wait-ready-timeout-ms` | `10000` | nouveau reinit timeout after power-on (`runtime_status=active`) |

What promotes / demotes:

- `[dgpu-hard]` — window classes that force the dGPU on (hard promotion, no
  busy gate): `game`, `blender`, `steam`, `chromium`, `firefox`,
  `google-chrome`, `brave` (v5.2: browsers — focus → dGPU ON + pstate `0a/0e`
  from `[preferred]`; demote when focus moves to a terminal).
- `[dgpu-soft]` — classes with busy-gated soft promotion (empty by default).
- `[igpu]` — classes always kept on the iGPU (demotion): `foot`, `kitty`,
  `alacritty`.
- `[dgpu-procs]` — process names (`/proc/*/comm` scan) that require the dGPU:
  `cuda`, `blender`.

**Verify:** `reclockctl switch-status` shows topology, dGPU power, target, last
action and `nvram_prefs`. When the dGPU is OFF, `pstate.sh set` refuses to
write — a pstate write on a power-cut card hangs nouveau (see the
`patches/kernel/` series below).

### `[low-power]` section

Window classes that force the `default` profile (cap `07`) with priority over
`preferred` when focused. Use this for terminals (`foot`, `alacritty`,
`kitty`, `ghostty`, `wezterm`, ...). Terminal focus wins `07` even if a
preferred app is busy in the background.

### `[fan]` section — Apple SMC fan control

| Key | Default | Meaning |
|---|---|---|
| `enable` | `true` | Enable applesmc fan control. |
| `temp-min` | `40` | °C at/below which fans sit at `fanN_min`. |
| `temp-max` | `67` | °C at/above which fans sit at `fanN_max`. |
| `temp-min-igd` | `41` | °C (iGPU-only, dGPU OFF) at/below which fans sit at `fanN_min`. |
| `temp-max-igd` | `91` | °C (iGPU-only, dGPU OFF) at/above which fans sit at `fanN_max`. |

RPM is linearly interpolated between `fanN_min` and `fanN_max`, which are read
dynamically from sysfs at startup. Disabled silently if applesmc is absent.

When the dGPU is OFF (switchd, IGD topology) the nouveau hwmon disappears and
the fan temperature becomes the CPU (`coretemp`) — which can safely run hotter
than the dGPU, hence the separate quieter iGPU curve (defaults 41/91 °C, v5.1).
With the dGPU ON the standard `temp-min`/`temp-max` (40/67 °C) curve applies.
The curve is chosen by the dGPU **power state** (`sw.dgpu_off()`), not the
topology.

### `[profile default]` — non-preferred apps (terminal/editor)

| Key | Default | Meaning |
|---|---|---|
| `max-pstate` | `07` | Ceiling pstate (ladder index). Cap for UP transitions. |
| `temp-down` | `65` | °C — sustained above → TERMAL down one step. |
| `temp-up` | `58` | °C — sustained below → UP allowed (if load met). |

### `[profile preferred]` — preferred apps (browser)

| Key | Default | Meaning |
|---|---|---|
| `max-pstate` | `0e` | Ceiling pstate. |
| `boost-pstate` | `-1` | Off-ladder boost tier, or `-1` to disable. Default disabled (`0e`/`0a` both stable, `0f` no visible gain on GT 750M). Set `0f` to opt in. |
| `busy-boost` | `85` | % busy > → BOOST UP `0e→0f` after `boost-dwell`. |
| `boost-dwell-ms` | `5000` | Sustained busy > `busy-boost` dwell to enter boost. |
| `temp-down` | `82` | °C thermal down for preferred. |
| `temp-up` | `75` | °C UP/BOOST allowed below this. |

### `[global]` section — common policy

| Key | Default | Meaning |
|---|---|---|
| `interval-ms` | `200` | Sampling period. |
| `poll-ms` | `1000` | `hyprctl` polling period (≥ `interval-ms`). |
| `busy-up` | `80` | % busy > → UP one step (ladder). |
| `busy-down` | `40` | % busy ≤ → IDLE dwell counts. Hysteresis band 80/40 = 40 pp. |
| `temp-dwell-ms` | `5000` | Dwell for thermal conditions. |
| `idle-dwell-ms` | `5000` | Dwell for idle downclock. |
| `profile-dwell-ms` | `2000` | Rate-limit on profile switches (alt-tab flap guard). |
| `win-ms` | `1000` | Sliding window for busy averaging. |
| `exit-state` | `0a` | pstate written on SIGTERM exit. |
| `vblank-sync` | `true` | Align pstate writes to vblank. |
| `gr-idle-promille` | `300` | ‰ instantaneous busy below which DOWN transitions are allowed (GR-idle gate). 300 = 30 %. `0` paralyzes DOWN; `1000` disables the gate. |

### CLI flags

```
--config PATH          config file (default /etc/reclockd.conf)
--exit-state S         pstate on exit (hex, e.g. 0a)
--interval MS          sampling period
--poll-ms MS           hyprctl poll period
--busy-up P            % busy > → UP one step
--busy-down P          % busy ≤ → idle dwell
--busy-boost P         % busy > → BOOST UP 0e→0f
--boost-dwell-ms MS    dwell for sustained busy>busy-boost before entering 0f
--boost-hyst P         pp hysteresis reserve for boost
--temp-up C            °C UP/BOOST allowed below
--temp-down C          °C TERMAL down above
--temp-dwell-ms MS     thermal dwell
--idle-dwell-ms MS     idle dwell
--profile-dwell-ms MS  profile switch rate-limit
--win-ms MS            busy smoothing window
--vblank-sync / --no-vblank-sync
--probe                20 busy samples, no transitions
--dry-run              decisions without writing pstate
-v                     more verbose logging
```

### 🎛 Transition logic (summary)

```
UP   07→0a→0e  (UP-LOAD):  g_cur_idx < ceiling AND temp < temp_up (temp_dwell)
                          AND (busy > busy_up OR title_pref). One step/decision.
                          title_pref (focused title in [preferred-titles])
                          forces UP overriding the busy>busy_up rule (UP-TITLE).
                          UP is NOT GR-idle gated (rising clock is safer).
DOWN 0e→0a→07 (TERMAL|IDLE|CEILING):
                          temp > temp_down (temp_dwell) OR
                          (busy ≤ busy_down (idle_dwell) AND !title_pref) OR
                          g_cur_idx > ceiling.
                          DOWN is GR-idle gated: deferred until instantaneous
                          busy ≤ gr-idle-promille ( protects GR mid-render).
                          TERMAL has top priority ( > title, > gate threshold).
                          IDLE is suppressed while title_pref (app holds 0e).
BOOST 0e→0f  (BOOST-UP):  g_cur_idx == ceiling AND busy > busy_boost (boost_dwell)
                          AND temp < temp_up (temp_dwell). Disabled when
                          boost-pstate = -1 (default).
BOOST-DOWN 0f→0e:         temp > temp_up OR temp >= temp_down (INSTANT, priority)
                          OR busy < busy_up (load hysteresis).
```

Priority hierarchy: **TERMAL > low-power terminal (→07) > title-match (→0e,
suppress IDLE, override busy) > class / running-busy (busy-gated) > IDLE.**

---

## 🖥 Usage

### `reclockctl` — daemon control

```sh
reclockctl start          # systemctl start reclockd
reclockctl stop           # systemctl stop reclockd (restores exit-state, SMC fan auto)
reclockctl status         # systemd status + pstate + temp + override + fans
reclockctl restart        # systemctl restart reclockd
reclockctl reload         # SIGHUP — re-read /etc/reclockd.conf without restart
reclockctl logs           # journalctl -u reclockd -f
reclockctl fan-off        # freeze auto fan control (drive fans manually via sysfs)
reclockctl fan-on         # resume auto fan control
reclockctl switch-status  # v5.0 switchd: topology, dGPU power, target, nvram_prefs
reclockctl dgpu-on        # v5.0 switchd override: force dGPU power ON
reclockctl dgpu-off       # v5.0 switchd override: force dGPU power OFF
reclockctl dgpu-auto      # v5.0 switchd: clear override, back to auto policy
reclockctl to-igd         # v5.0: NVRAM gpu-power-prefs=IGD + reboot (boot-time switch)
reclockctl to-dis         # v5.0: NVRAM gpu-power-prefs=DIS + reboot
```

### `pstate.sh` — manual pstate inspection / override

```sh
pstate.sh status          # pstate + temp + override + daemon state
pstate.sh set 0a          # force 0a, create override (freezes daemon auto)
pstate.sh set ac:0a       # force 0a, override
pstate.sh auto            # clear override (daemon resumes auto)
```

Pstates on the GT 750M (core / mem, MHz):

```
07: 270-405 / 838      0a: 270-925 / 1560
0e: 270-925 / 4000     0f: 270-925 / 5016
```

`0a / 0e / 0f` share the same core range (max 925 MHz); they differ in memory
clock. `0e` and `0f` are aggressive memory reclocks — test carefully (known
lockups on NVE0 at max pstate). Settings are runtime-only; reboot resets.

### Direct debugfs check

```sh
sudo cat /sys/kernel/debug/dri/0000:01:00.0/pstate
```

---

## 🩹 Patches

### `patches/0001-nouveau-auto-reclock.patch`

Adds a kernel-side auto-reclock policy to nouveau's `clk/base.c` and
`include/nvkm/subdev/clk.h`: a `nvkm_alarm` sampling timer, EMA of busy time, and
up/down threshold counters, so nouveau can reclock on its own without a
userspace daemon. This is the upstream-able version of what `reclockd` does in
userspace. Apply to a Linux kernel source tree:

```sh
cd /path/to/linux
patch -p1 < /path/to/0001-nouveau-auto-reclock.patch
# build and install the nouveau module
```

### `patches/0002-mesa-nvc0-sched-data.patch`

Tightens the Mesa nvc0 codegen scheduler latency data in
`nv50_ir_emit_nvc0.cpp` and `nv50_ir_target_nvc0.cpp` to match the NAK sm30
execution latencies: `OP_EXIT`/`OP_RET` latency 14 → 15, `sched 0x00` (JOIN/SYNC)
wait 32 → 16 cycles, `OPCLASS_TEXTURE` occupancy 18 → 17 cycles, plus a
`OP_MEMBAR` 16-cycle memory-pipe busy rule. Net effect on shader-bound workloads
on Kepler: roughly +10-30%. Apply to a Mesa source tree (see `build-mesa.sh`):

```sh
cd /path/to/mesa
patch -p1 < /path/to/0002-mesa-nvc0-sched-data.patch
```

### `patches/81-nouveau-kepler.rules` + `install-udev-rule.sh`

A udev rule that force-loads the `nouveau` module for the GT 750M (GK107, PCI
`10de:0fe9`) at boot. This works around a nasty trap on Omarchy/Arch:
`nvidia-utils` — a hard dependency of Hyprland and aquamarine — ships
`/usr/lib/modprobe.d/nvidia-utils.conf` with `blacklist nouveau`, even when the
proprietary `nvidia` kernel module is **not** installed. The blacklist suppresses
auto-loading via the PCI alias, so the dGPU comes up without a driver,
`/sys/kernel/debug/dri/*/pstate` is absent, and `reclockd` spins idle. A direct
`modprobe nouveau` by name bypasses the blacklist (it only blocks alias
auto-load), which is exactly what this udev rule triggers on device add. The rule
survives `nvidia-utils` updates because it lives in `/etc` (overrides
`/usr/lib`). Install:

```sh
sudo ./install-udev-rule.sh
# albo ręcznie:
sudo install -m644 patches/81-nouveau-kepler.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
# reboot, albo natychmiast: sudo modprobe nouveau
```

### `patches/kernel/` — MacBook Pro 11,3 kernel series (0002-0014)

Kernel patch series for the dual-GPU MacBook Pro 15" Late 2013 (Intel iGPU +
NVIDIA GT 750M on a `gmux` mux). Applied in order by `build-kernel.sh` to a
Linux v7.1.8 tree (`0001` from `patches/` first, then `0002`-`0014`):

| patch | area | fix |
|---|---|---|
| `0002` | gmux/i915 | vga_switcheroo reinit callback for the panel |
| `0003` | gmux/i915 | apple-gmux power-cycle |
| `0004` | gmux/i915 | backlight minimum → off |
| `0005` | gmux/i915 | eDP link DPCD retry |
| `0006` | nouveau | runtime PM hybrid scanout gate |
| `0007` | nouveau | reinit GK107 after power-cut (power-on) |
| `0008` | nouveau | ACPI no-PR3 → D3hot fallback |
| `0009` | nouveau | PCIe link retrain after resume |
| `0010` | nouveau | GR-idle gate for runtime suspend |
| `0011` | nouveau | poll PCIe link before D0 |
| `0012` | nouveau | PMU reply timeout (`gt215_pmu_send`) |
| `0013` | nouveau | pstate calc timeout (`nvkm_pstate_calc`) |
| `0014` | nouveau | IRQ_HANDLED for the private MSI line (fix `Disabling IRQ #91`) |

`0011`-`0013` fix a real hang: after a power-on (gmux power-cut → D3hot→D0),
`pci_power_up` reads PMCSR before the PCIe link has trained, gets
`0xFFFFFFFF` and returns `-EIO`; the PMU then never answers and both
`gt215_pmu_send` and `nvkm_pstate_calc` wait forever in `wait_event` — D-state
kernel threads, black screen. `0011` polls the link up to 2 s before D0 in
both resume paths; `0012`/`0013` add 2 s timeouts to the PMU reply and the
pstate workqueue as a safety net.

`0014` claims the private MSI line as handled: on the MBP 11,3 the nonstall
fence notify is routed to the separate NRHOST interrupt (leaf 1), which has no
handler in `gk104_mc_intrs[]`. A nonstall flood (e.g. glmark2
`--frame-end none` at ~8000 FPS) makes `nvkm_intr()` return `IRQ_NONE`, and
the kernel's spurious-interrupt detector disables the line ("irq 91: nobody
cared ... Disabling IRQ #91") after 100k IRQ_NONE — the dGPU is then unusable
until reboot. The source block still runs above as protection; the line is
only claimed as handled so the detector stays out of it.

---

## 🛠 Scripts

### `pstate.sh`

Inspect or force the GPU pstate through debugfs, integrated with the daemon's
override flag-file. `set` creates `/run/reclockd/override` and freezes auto
mode; `auto` clears it. Requires root for the debugfs write. v5.0 additions:
when the dGPU is OFF (switchd), `status` falls back to the CPU `coretemp`
temperature and also shows the iGPU RPS monitoring (read-only), and `set`
refuses to write pstate — a pstate write on a power-cut card hangs nouveau.
v5.1 addition: `status` also prints an `=== wentylatory ===` section — the
active fan curve (`igd | dga | compiler | override | off`), its temperature
range and the current RPMs, read from `/run/reclockd/status` (applesmc sysfs
fallback when the daemon is down).

### `build-mesa.sh`

Installs build dependencies (Arch pacman), applies
`0002-mesa-nvc0-sched-data.patch` to a Mesa tree at `tmp/mesa`, configures a
nouveau-only meson build (no Vulkan/OpenCL/rust/llvm), and compiles. Does **not**
install to the system — run apps with `LIBGL_DRIVERS_PATH` pointing at the build
output to compare patched vs unpatched.

```sh
git clone https://gitlab.freedesktop.org/mesa/mesa.git tmp/mesa
./build-mesa.sh
```

### `mesa-manage.sh`

Surgical install/rollback of the patched Mesa nouveau driver without disturbing
other drivers (iris, swrast, ...). Works against the Arch Mesa 26.x "dril"
layout: copies the patched self-contained `libdril_dri.so` as
`/usr/lib/dri/nouveau_dri_patched.so` and repoints the `nouveau_dri.so` symlink
at it. Rollback restores the symlink to the system `libdril_dri.so` and removes
the patched file. Commands: `status | backup | install | restore | diff`. Flags:
`--yes`, `--force`, `--dry-run`.

### `recover-gpu.sh`

Emergency revert if a GPU experiment breaks the display. Idempotent: only
removes what it finds, never applies new GPU config. Reverts `AQ_DRM_DEVICES`
in `~/.config/uwsm/default`, restores SDDM autologin from backup, and (with
`--dedicated`) undoes a `gpu-switch` panel mux back to the dGPU. Run from SSH or
a TTY when the screen is black.

```sh
./recover-gpu.sh                 # revert, no reboot
./recover-gpu.sh --reboot        # revert, then reboot
./recover-gpu.sh --dedicated     # also undo gpu-switch mux
```

### `build-kernel.sh`

Builds and installs the patched nvkp kernel (`LOCALVERSION=-nvkp`) as a
**separate** boot entry — never overwrites the running kernel: modules go to
`/lib/modules/<rev>-nvkp`, `/boot` gets `vmlinuz-linux-nvkp`,
`initramfs-linux-nvkp.img` and `System.map-linux-nvkp`; the bootloader entry is
GRUB (`grub-mkconfig`) or Limine (`--limine`). Applies the full patch series
(`0001` → `0013`, see `patches/kernel/`). Options: `--jobs=N`, `--no-install`,
`--clean`, `--full-tree`. Build heat is covered by the daemon's `[compiler]`
fan boost.

```sh
./build-kernel.sh --full-tree --limine
```

### `build-uki-nvkp.sh`

Builds a Unified Kernel Image (UKI) from the nvkp kernel: regenerates the
initramfs with the `encrypt` hook + LUKS keyfile, packs vmlinux + initramfs +
embedded cmdline into one `/boot/EFI/Linux/nvkp.efi`, and registers a Limine
`protocol: efi` entry (`//nvkp`). Needed because Limine's `protocol: efi` does
**not** pass initramfs — and the legacy `protocol: linux` path boots to a black
screen on this hardware (VGA console on a powered-off dGPU vs efifb on the
iGPU).

### `bench-gpu.sh`

Repeatable glmark2 benchmark comparing the iGPU (Intel Iris Pro 5200) and the
dGPU (GT 750M) at full panel resolution (default `2880x1800`). Offscreen mode
(`--frame-end none --swap-mode immediate`) removes the PRIME copyback and the
compositor vsync that corrupt on-screen numbers; the dGPU pstate is pinned via
debugfs (`0e`/`0a`/`07`) with the daemon stopped. Fans forced to 100%, a
`trap` restores everything on exit. `--dry-run` prints the commands.

```sh
sudo ./bench-gpu.sh -g both -m offscreen -p all
```

---

## ⚙️ How it works

Every `interval-ms` (default 200 ms), `reclockd`:

1. Samples GPU busy% from the BAR0 PMU idle counters (`R_IDLE_COUNT` for busy
   and total, reset after read) — a 0-1000 per-mille figure independent of any
   driver sysfs gauge.
2. Reads temperature from the `nouveau` hwmon (`temp1_input`).
3. Smooths busy over a sliding window (`win-ms`, default 1 s).
4. Polls `hyprctl` every `poll-ms` (default 1 s) for the focused and running
   window class **and title**; updates the active profile (`default` vs
   `preferred`) with `profile-dwell` rate-limiting. Re-detects Hyprland each
   poll until the session is up (the daemon starts before the user session).
   Title matches (`[preferred-titles]`) set `title_pref` and force `0e`
   overriding the busy rule; terminal focus (`[low-power]`) forces `default`.
5. Evaluates dwell counters (temp-low, temp-high, idle, boost-up) against the
   active profile's thresholds.
6. Decides a target pstate per the [transition logic](#transition-logic-summary)
   — one ladder step per decision, or a boost tier entry/exit. DOWN
   transitions are GR-idle-gated (deferred until instantaneous busy ≤
   `gr-idle-promille`).
7. If a transition is needed, optionally waits for vblank
   (`DRM_IOCTL_WAIT_VBLANK`), then writes the 2-hex pstate to the debugfs
   `pstate` file.
8. Every poll cycle, if fan control is enabled, interpolates the SMC fan RPM
   from the current temperature (applesmc), independent of pstate decisions.

The DRM FD to `card0` is opened once at startup for vblank sync. Immediately
after `open()`, the daemon calls `DRM_IOCTL_DROP_MASTER` so it never holds DRM
master and never blocks the compositor. See
[The DRM master problem](#the-drm-master-problem-and-why-reclockd-drops-it).

All settings are **runtime-only**: a reboot resets the GPU to boot clocks and
the daemon re-applies policy on next start. On `SIGTERM`, the daemon writes
`--exit-state` (default `0a`), restores the prior `power/control` value, and
restores SMC fan auto (`fanN_manual=0`).

---

## 🛡 Safety / Risks

- ⚠️ **Max pstate `0f` lockups**: NVE0-class Kepler parts are known to lock up
  under undervolt at the max pstate. `0f` is off-ladder in `reclockd` and only
  entered under sustained load + cool temp; thermal guard drops it instantly.
  If you do not want `0f` at all, leave `boost-pstate` unset in
  `reclockd.conf` (or set it to a value ≤ `max-pstate`, which disables boost).
- 🎛 **Aggressive memory reclock**: `0e` and `0f` reclock memory aggressively. Test
  carefully on your specific card. The safe ladder (`07 / 0a / 0e`) avoids the
  worst case by default.
- 🌡 **Thermal**: per-profile thermal down is prioritized over load. If hwmon is
  unavailable, thermal conditions are skipped **fail-safe** (never an emergency
  UP) — but you lose thermal protection, so monitor temp manually.
- 🆘 **`recover-gpu.sh`** is provided for the case where a GPU experiment breaks
  the display. Run it from SSH or a TTY.
- 🔐 **Root daemon**: `reclockd` requires root (mmap BAR0 + write debugfs). Audit
  the source before running it. It is a single C++17 translation unit; the whole
  policy is in `reclockd.cpp`.

---

## 📜 License

MIT — see [LICENSE](LICENSE).