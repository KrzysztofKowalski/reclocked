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
  terminal/editor) vs `preferred` (cap `0e` with boost tier — for browser).
  Profile is chosen from the focused or running window class reported by
  `hyprctl`. `profile-dwell` rate-limits switching so alt-tab doesn't flap the
  ceiling.
- 🚀 **Boost tier (`0f`)**: `0f` is intentionally **off-ladder** — it never appears
  as a step in the auto ladder. It only engages when the GPU is already at the
  ladder ceiling (`0e`), busy > `busy-boost` for `boost-dwell`, and temp <
  `temp-up` for `temp-dwell`. Thermal guard drops it instantly.
- 🌡 **Per-profile thermal guard**: thermal downclock is **per-profile**, not
  global. `default` throttles at 65°C / recovers below 58°C; `preferred`
  throttles at 82°C / recovers below 75°C. Thermal down is prioritized over
  load in both profiles.
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
  UP); SIGTERM → restore `--exit-state`; CLI overrides win over config.
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
├── reclockd/
│   ├── reclockd.cpp                daemon source (~1200 lines, C++17)
│   ├── Makefile                    builds ./reclockd (no libdrm link)
│   ├── reclockd.conf               default config (profiles, thresholds)
│   ├── reclockd.service            systemd unit (installs to /etc/systemd/system/)
│   └── reclockctl                  CLI wrapper for systemctl + pstate.sh
├── patches/
│   ├── 0001-nouveau-auto-reclock.patch   nouveau kernel auto-reclock policy
│   └── 0002-mesa-nvc0-sched-data.patch   Mesa nvc0 scheduler latency data
├── pstate.sh                       inspect/force pstate via debugfs (+ override)
├── build-mesa.sh                   build patched Mesa (nouveau-only)
├── mesa-manage.sh                  install/rollback patched Mesa driver
└── recover-gpu.sh                  emergency recovery if display breaks
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
| `boost-pstate` | `0f` | Off-ladder boost tier (must be off-ladder and > ceiling). |
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
                          AND busy > busy_up. One step per decision.
DOWN 0e→0a→07 (TERMAL|IDLE|CEILING):
                          temp > temp_down (temp_dwell) OR
                          busy ≤ busy_down (idle_dwell) OR
                          g_cur_idx > ceiling.
BOOST 0e→0f  (BOOST-UP):  g_cur_idx == ceiling AND busy > busy_boost (boost_dwell)
                          AND temp < temp_up (temp_dwell).
BOOST-DOWN 0f→0e:         temp > temp_up OR temp >= temp_down (INSTANT, priority)
                          OR busy < busy_up (load hysteresis).
```

---

## 🖥 Usage

### `reclockctl` — daemon control

```sh
reclockctl start     # systemctl start reclockd
reclockctl stop      # systemctl stop reclockd (restores exit-state)
reclockctl status    # systemd status + pstate + temp + override
reclockctl restart   # systemctl restart reclockd
reclockctl reload    # SIGHUP — re-read /etc/reclockd.conf without restart
reclockctl logs      # journalctl -u reclockd -f
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

---

## 🛠 Scripts

### `pstate.sh`

Inspect or force the GPU pstate through debugfs, integrated with the daemon's
override flag-file. `set` creates `/run/reclockd/override` and freezes auto
mode; `auto` clears it. Requires root for the debugfs write.

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

---

## ⚙️ How it works

Every `interval-ms` (default 200 ms), `reclockd`:

1. Samples GPU busy% from the BAR0 PMU idle counters (`R_IDLE_COUNT` for busy
   and total, reset after read) — a 0-1000 per-mille figure independent of any
   driver sysfs gauge.
2. Reads temperature from the `nouveau` hwmon (`temp1_input`).
3. Smooths busy over a sliding window (`win-ms`, default 1 s).
4. Polls `hyprctl` every `poll-ms` (default 1 s) for the focused and running
   window classes; updates the active profile (`default` vs `preferred`) with
   `profile-dwell` rate-limiting.
5. Evaluates dwell counters (temp-low, temp-high, idle, boost-up) against the
   active profile's thresholds.
6. Decides a target pstate per the [transition logic](#transition-logic-summary)
   — one ladder step per decision, or a boost tier entry/exit.
7. If a transition is needed, optionally waits for vblank
   (`DRM_IOCTL_WAIT_VBLANK`), then writes the 2-hex pstate to the debugfs
   `pstate` file.

The DRM FD to `card0` is opened once at startup for vblank sync. Immediately
after `open()`, the daemon calls `DRM_IOCTL_DROP_MASTER` so it never holds DRM
master and never blocks the compositor. See
[The DRM master problem](#the-drm-master-problem-and-why-reclockd-drops-it).

All settings are **runtime-only**: a reboot resets the GPU to boot clocks and
the daemon re-applies policy on next start. On `SIGTERM`, the daemon writes
`--exit-state` (default `0a`) and restores the prior `power/control` value.

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