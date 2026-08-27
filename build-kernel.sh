#!/usr/bin/env bash
# build-kernel.sh — budowa + instalacja jądra z patchami nv-kepler (switchd v5.0)
#
# Buduje OSOBNY wpis jądra z LOCALVERSION=-nvkp — NIE nadpisuje działającego jądra:
#   - moduły       → /lib/modules/<rev>-nvkp
#   - /boot        → vmlinuz-linux-nvkp, initramfs-linux-nvkp.img, System.map-linux-nvkp
#   - bootloader   → GRUB (grub-mkconfig) albo Limine (wpis automatyczny z --limine)
#
# Kluczowy patch: 0007-nouveau-reinit-gk107-after-power-cut.patch — reinit GK107
# po power-cut (power-on switchd v5.0). Kolejność patchy: 0001 → 0014.
# 0011-0013 = fix zawieszenia pstate po D3hot→D0 (poll linku PCIe przed D0,
#             timeout reply PMU, timeout pstate work — raport 77).
# 0014 = IRQ_HANDLED dla prywatnej linii MSI — fix 'Disabling IRQ #91' przy burzy
#        nonstall fence notify (patrz NOTES.md, sekcja IRQ #91).
#
# Termika: kompilacja grzeje — reclockd [compiler] wykrywa gcc/make i podkręca
# wentylatory do 100% samoczynnie → pełne -j$(nproc) jest bezpieczne.
#
# Użycie:
#   ./build-kernel.sh                — pełna ścieżka (config → build → moduły → /boot → bootloader)
#   ./build-kernel.sh --jobs=N       — ograniczenie równoległości (default: $(nproc))
#   ./build-kernel.sh --no-install   — tylko build (bez modułów//boot/initramfs/bootloadera)
#   ./build-kernel.sh --clean        — make mrproper przed buildem
#   ./build-kernel.sh --full-tree    — zamień sparse-checkout na PEŁNE drzewo (wymagane do builda)
#   ./build-kernel.sh --limine       — przy bootloaderze Limine: dołóż wpis do /boot/limine.conf
set -euo pipefail

PROJ="$(cd "$(dirname "$0")" && pwd)"
SRC="${SRC:-$PROJ/tmp/linux-nouveau}"
PATCH_KERNEL="$PROJ/patches/kernel"
PATCH_GENERIC="$PROJ/patches"
LOCALVERSION="-nvkp"
BOOT_PREFIX="linux-nvkp"            # pliki w /boot: vmlinuz-linux-nvkp, initramfs-linux-nvkp.img
LOG="${LOG:-$PROJ/tmp/build-kernel.log}"
PATCH_STAMP="$SRC/.nvkp-patches.stamp"   # znacznik „patche nałożone” — idempotencja re-runów

# ----------------------------------------------------------------------------
# Argumenty
# ----------------------------------------------------------------------------
CLEAN=false
NO_INSTALL=false
FULL_TREE=false
LIMINE_APPLY=false
JOBS="$(nproc 2>/dev/null || echo 4)"

usage() {
  sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --jobs=*)       JOBS="${1#*=}" ;;
    --clean)        CLEAN=true ;;
    --no-install)   NO_INSTALL=true ;;
    --full-tree)    FULL_TREE=true ;;
    --limine)       LIMINE_APPLY=true ;;
    -h|--help)      usage; exit 0 ;;
    *) echo "BŁĄD: nieznany argument: $1" >&2; usage; exit 1 ;;
  esac
  shift
done
[ "$JOBS" -ge 1 ] 2>/dev/null || JOBS="$(nproc 2>/dev/null || echo 4)"

die() { printf 'BŁĄD: %s\n' "$*" >&2; exit 1; }
log() { printf '%s\n' "$*"; }

# ----------------------------------------------------------------------------
# Pomocnicze
# ----------------------------------------------------------------------------
need_sudo() {
  # Wszystkie uprzywilejowane kroki przez `sudo -n` (passwordless na tej maszynie).
  # Sprawdź raz na starcie, żeby nie schodzić do połowy i dopiero potem paść.
  if ! sudo -n true 2>/dev/null; then
    echo "BŁĄD: sudo -n (passwordless) nie działa — potrzebne do instalacji." >&2
    echo "      Dodaj NOPASSWD w /etc/sudoers.d albo najpierw uruchom: sudo -v" >&2
    exit 1
  fi
}

# kernel_release — realny numer wydania drzewa (z LOCALVERSION).
# Po zbudowaniu scripts/ istnieje → make kernelrelease działa.
# Przed buildem (tylko poglądowo) fallback: parsuj Makefile.
kernel_release() {
  local out
  if out="$(make -s kernelrelease LOCALVERSION="$LOCALVERSION" 2>/dev/null)" && [ -n "$out" ]; then
    printf '%s\n' "$out"
    return 0
  fi
  local v p s e
  v="$(awk '$1=="VERSION" {print $3; exit}' Makefile)"
  p="$(awk '$1=="PATCHLEVEL" {print $3; exit}' Makefile)"
  s="$(awk '$1=="SUBLEVEL" {print $3; exit}' Makefile)"
  e="$(awk '$1=="EXTRAVERSION" {print $3; exit}' Makefile)"
  printf '%s.%s.%s%s%s\n' "$v" "$p" "$s" "$e" "$LOCALVERSION"
}

# cfg_state — wypisz stan symbolu CONFIG_* z .config (tylko raport).
cfg_state() {
  local sym="$1"
  local v
  v="$(grep -E "^$sym=" .config 2>/dev/null | head -1 || true)"
  if [ -n "$v" ]; then printf '  %-26s %s\n' "$sym" "${v#*=}"; else printf '  %-26s (nie ustawione)\n' "$sym"; fi
}

# --- patche ---
# Kolejność 0001→0014 z patches/kernel/; 0001 leży w patches/ (nie kernel/) —
# szukaj w obu miejscach.
PATCH_NAMES=(
  0001-nouveau-auto-reclock.patch
  0002-gmux-i915-switcheroo-vga-switcheroo-reinit-callback.patch
  0003-gmux-i915-switcheroo-apple-gmux-power-cycle.patch
  0004-gmux-i915-switcheroo-apple-gmux-backlight-min-to-off.patch
  0005-gmux-i915-switcheroo-i915-edp-dpcd-retry.patch
  0006-nouveau-runtime-pm-hybrid-scanout-gate.patch
  0007-nouveau-reinit-gk107-after-power-cut.patch
  0008-nouveau-acpi-no-pr3-d3hot-fallback.patch
  0009-nouveau-pcie-link-retrain-after-resume.patch
  0010-nouveau-gr-idle-gate-runtime-suspend.patch
  0011-nouveau-pcie-link-wait-before-d0.patch
  0012-nouveau-pmu-send-reply-timeout.patch
  0013-nouveau-pstate-calc-timeout.patch
  0014-nouveau-irq-msi-handled.patch
)

find_patch() {
  local name="$1"
  if [ -f "$PATCH_KERNEL/$name" ]; then printf '%s\n' "$PATCH_KERNEL/$name"; return 0; fi
  if [ -f "$PATCH_GENERIC/$name" ]; then printf '%s\n' "$PATCH_GENERIC/$name"; return 0; fi
  return 1
}

patches_digest() {
  # Hasz treści wszystkich patchy + nazw plików. Zmiana dowolnego patcha → nowy
  # digest → następny run re-aplikuje wszystkie na czystym drzewie.
  ( cat "$@" 2>/dev/null; printf '%s\n' "$@" ) | sha256sum | cut -d' ' -f1
}

apply_patch() {
  local p="$1" name
  name="$(basename "$p")"
  if git apply --check "$p" 2>/dev/null; then
    git apply "$p"
    echo "    OK   $name — nałożony"
  elif git apply --check --reverse "$p" 2>/dev/null; then
    echo "    SKIP $name — już nałożony"
  else
    echo "BŁĄD: $name nie aplikuje się czysto i nie jest nałożony." >&2
    echo "      Nie używam --3way ani --reject — rozstrzygnij ręcznie." >&2
    exit 1
  fi
}

# --- sprawdzenie kompletności drzewa źródłowego ---
# Repo nv-kepler trzyma tmp/linux-nouveau jako SPARSE checkout (tylko drzewo DRM,
# do pisania patchy). Do builda potrzebne jest PEŁNE drzewo.
check_full_tree() {
  local sparse
  sparse="$(git sparse-checkout list 2>/dev/null || true)"
  if [ -n "$sparse" ]; then
    if [ "$FULL_TREE" = true ]; then
      echo ">>> Drzewo jest sparse-checkout — materializuję PEŁNE drzewo..."
      echo "    Uwaga: klon to blob:none — git dociągnie brakujące pliki z git.kernel.org"
      echo "    (może to być >1 GB i wymaga sieci; jednorazowo)."
      git sparse-checkout disable
      sparse="$(git sparse-checkout list 2>/dev/null || true)"
      [ -n "$sparse" ] && die "git sparse-checkout disable nie zadziałało"
    else
      echo "BŁĄD: drzewo źródłowe jest sparse-checkout — nie da się go zbudować." >&2
      echo "      Widoczne tylko:" >&2
      printf '%s\n' "$sparse" | sed 's/^/        /' >&2
      echo "      Przygotuj pełne drzewo:  ./build-kernel.sh --full-tree" >&2
      exit 1
    fi
  fi
  if [ "$(git rev-parse --is-shallow-repository 2>/dev/null)" = "true" ]; then
    echo "UWAGA: klon jest płytki (shallow) — ok do builda, ale git describe może nie mieć tagów."
  fi
}

# ==========================================================================
echo "=== nv-kepler: budowa jądra z LOCALVERSION=$LOCALVERSION (switchd v5.0, patch 0007 = reinit GK107, 0011-0013 = fix D3hot→D0 hang, 0014 = IRQ MSI handled)"
echo "=== src: $SRC"

[ -d "$SRC/.git" ] || die "$SRC nie jest klonem git (brak .git)"
cd "$SRC"

# pełne drzewo (sparse → pełne), zanim cokolwiek zrobimy
check_full_tree

# po materializacji pełnego drzewa scripts/ MUSI istnieć
[ -d scripts ] || { echo "BŁĄD: brak scripts/ w $SRC — drzewo niekompletne." >&2; exit 1; }

# --------------------------------------------------------------------------
# 1. [opcjonalnie] make mrproper
# --------------------------------------------------------------------------
if [ "$CLEAN" = true ]; then
  echo ">>> [1/9] make mrproper (czyszczenie)..."
  make mrproper
else
  echo ">>> [1/9] bez --clean (drzewo bez czyszczenia)"
fi

# --------------------------------------------------------------------------
# 2. Patchowanie (git apply, idempotentnie)
# --------------------------------------------------------------------------
echo ">>> [2/9] Aplikacja patchy z $PATCH_KERNEL (i $PATCH_GENERIC)..."
digest="$(patches_digest "${PATCH_NAMES[@]}")"
if [ -f "$PATCH_STAMP" ] && [ "$(cat "$PATCH_STAMP" 2>/dev/null)" = "$digest" ]; then
  echo "    SKIP wszystkie ${#PATCH_NAMES[@]} patchy — już nałożone (stamp zgodny: $PATCH_STAMP)"
else
  # Per-patch detekcja „już nałożony” (git apply --check --reverse) NIE działa dla
  # tego zestawu: patche 0006/0007/0008/0010/0011 ruszają ten sam plik
  # (nouveau_drm.c), więc reverse-check wcześniejszego patcha pada, gdy
  # późniejszy zmienił kontekst.
  # Dlatego: czyste drzewo → nałóż wszystkie w kolejności → zapisz stamp.
  if ! git diff --quiet; then
    echo "    Drzewo ma zmiany (wcześniejsze patche?) — resetuję do czystego base: git checkout -f"
    git checkout -f
  fi
  for name in "${PATCH_NAMES[@]}"; do
    p="$(find_patch "$name")" || die "nie znaleziono patcha $name w $PATCH_KERNEL ani $PATCH_GENERIC"
    apply_patch "$p"
  done
  printf '%s\n' "$digest" > "$PATCH_STAMP"
  echo "    Stamp zapisany: $PATCH_STAMP (re-run pominie aplikację patchy)"
fi

# --------------------------------------------------------------------------
# 3. Konfiguracja
# --------------------------------------------------------------------------
echo ">>> [3/9] Konfiguracja .config..."
if [ ! -f .config ]; then
  if [ -r /proc/config.gz ]; then
    echo "    brak .config → baza = konfiguracja działającego jądra Arch (/proc/config.gz)"
    zcat /proc/config.gz > .config
  else
    echo "UWAGA: /proc/config.gz niedostępny — make defconfig (ryzykowne, sprawdź ręcznie)"
    make defconfig
  fi
else
  echo "    .config istnieje — zostawiam, tylko olddefconfig"
fi
make olddefconfig

echo "    --- kluczowe opcje (tylko raport, NIE zmieniam) ---"
cfg_state CONFIG_DRM_NOUVEAU
cfg_state CONFIG_DRM_APPLE_GMUX
cfg_state CONFIG_APPLE_GMUX
cfg_state CONFIG_VGA_SWITCHEROO
cfg_state CONFIG_DRM_I915

# --------------------------------------------------------------------------
# 4. Build
# --------------------------------------------------------------------------
echo ">>> [4/9] Build: make -j$JOBS LOCALVERSION=$LOCALVERSION (log: $LOG)"
mkdir -p "$(dirname "$LOG")"
echo "=== make -j$JOBS LOCALVERSION=$LOCALVERSION ($(date)) ===" > "$LOG"
if ! make -j"$JOBS" LOCALVERSION="$LOCALVERSION" 2>&1 | tee -a "$LOG"; then
  echo "BŁĄD: kompilacja nieudana — log: $LOG (ostatnie 15 linii):" >&2
  tail -15 "$LOG" >&2 || true
  exit 1
fi
echo "    build OK"

# --------------------------------------------------------------------------
# 5. Kernelrelease (rzeczywisty numer — może mieć sufiks -g<hash>[-dirty],
#    bo CONFIG_LOCALVERSION_AUTO=y w konfigu Arch)
# --------------------------------------------------------------------------
KREL="$(kernel_release)"
echo ">>> [5/9] kernelrelease: $KREL"

# --------------------------------------------------------------------------
# 6. modules_install
# --------------------------------------------------------------------------
if [ "$NO_INSTALL" = false ]; then
  need_sudo
  echo ">>> [6/9] sudo -n make modules_install LOCALVERSION=$LOCALVERSION"
  sudo -n make modules_install LOCALVERSION="$LOCALVERSION"
  [ -d "/lib/modules/$KREL" ] || die "moduły nie wylądowały w /lib/modules/$KREL"
  echo "    moduły w /lib/modules/$KREL"
else
  echo ">>> [6/9] pomijam modules_install (--no-install)"
fi

# --------------------------------------------------------------------------
# 7. /boot: bzImage + System.map (bez make install — kontrolowane kopiowanie)
# --------------------------------------------------------------------------
if [ "$NO_INSTALL" = false ]; then
  echo ">>> [7/9] Instalacja do /boot (kopiowanie bzImage/System.map)..."
  [ -f arch/x86/boot/bzImage ] || die "brak arch/x86/boot/bzImage po buildzie"
  sudo -n install -m 0644 arch/x86/boot/bzImage "/boot/vmlinuz-$BOOT_PREFIX"
  if [ -f System.map ]; then
    sudo -n install -m 0644 System.map "/boot/System.map-$BOOT_PREFIX"
  fi
  echo "    /boot/vmlinuz-$BOOT_PREFIX + /boot/System.map-$BOOT_PREFIX"
else
  echo ">>> [7/9] pomijam /boot (--no-install)"
fi

# --------------------------------------------------------------------------
# 8. initramfs (mkinitcpio)
# --------------------------------------------------------------------------
if [ "$NO_INSTALL" = false ]; then
  echo ">>> [8/9] initramfs: mkinitcpio -k $KREL..."
  # MUSI być config z hookiem `encrypt` (udev + encrypt + keyfile), NIE systemowy
  # /etc/mkinitcpio.conf (systemd, bez encrypt). Bez dm-crypt LUKS nie wstaje →
  # czarny ekran. Dodatkowo `-c` wyłącza drop-iny /etc/mkinitcpio.conf.d/ (gdzie
  # omarchy_hooks.conf dodaje encrypt), więc jawnie wskazujemy config nvkp.
  # Config tworzy fix-initramfs-nvkp.sh (udev + autodetect + encrypt + FILES keyfile).
  if ! sudo -n test -f /etc/mkinitcpio-nvkp.conf; then
    die "/etc/mkinitcpio-nvkp.conf nie istnieje — odpal najpierw fix-initramfs-nvkp.sh"
  fi
  sudo -n mkinitcpio -k "$KREL" -c /etc/mkinitcpio-nvkp.conf -g "/boot/initramfs-$BOOT_PREFIX.img"
  sudo -n test -f "/boot/initramfs-$BOOT_PREFIX.img" || die "initramfs nie powstał"
  echo "    /boot/initramfs-$BOOT_PREFIX.img"
else
  echo ">>> [8/9] pomijam initramfs (--no-install)"
fi

# --------------------------------------------------------------------------
# 9. Bootloader (GRUB albo Limine — na tej maszynie jest Limine)
# --------------------------------------------------------------------------
bootloader_entry() {
  # Blok wpisu Limine (protokół linux) dla nowego jądra. Linux protocol → osobne
  # kernel+initramfs (bez UKI); boot():/vmlinuz-linux-nvkp to plik na ESP (=/boot).
  local cmdline
  cmdline="$(tr '\n' ' ' < /proc/cmdline 2>/dev/null || true)"
  printf '%s\n' \
    "/+Linux-nvkp" \
    "### nv-kepler: jądro $KREL z patchami switchd v5.0" \
    "comment: Kernel $KREL (nv-kepler patched)" \
    "protocol: linux" \
    "kernel_path: boot():/vmlinuz-$BOOT_PREFIX" \
    "module_path: boot():/initramfs-$BOOT_PREFIX.img" \
    "cmdline: $cmdline"
}

if [ "$NO_INSTALL" = false ]; then
  echo ">>> [9/9] Aktualizacja wpisu bootloadera..."
  # /boot na tym MBP to ESP FAT32 montowane z fmask=0077 (drwx------) → zwykły user
  # NIE MA odczytu /boot. Detekcja bootloadera musi iść przez `sudo -n test ...`,
  # a nie przez `[ -f ... ]` (testowy user zawsze zwróci false i wpadnie do else).
  # need_sudo już wołany w kroku 6 (jeśli NO_INSTALL=false) — sudo -n działa tu pewnie.
  if command -v grub-mkconfig >/dev/null 2>&1 && sudo -n test -d /boot/grub; then
    need_sudo
    echo "    GRUB wykryty: grub-mkconfig -o /boot/grub/grub.cfg"
    sudo -n grub-mkconfig -o /boot/grub/grub.cfg
    echo "    Nowy wpis vmlinuz-$BOOT_PREFIX dodany — wybierz go w menu GRUB."
  elif sudo -n test -f /boot/limine.conf; then
    echo "    Wykryto Limine (nie GRUB) — grub-mkconfig nie istnieje na tej maszynie."
    echo
    echo "    Proponowany wpis do /boot/limine.conf (kernel+initramfs osobno):"
    bootloader_entry | sed 's/^/      /'
    echo
    if [ "$LIMINE_APPLY" = true ]; then
      need_sudo
      echo "    --limine: dopisuję wpis do /boot/limine.conf (backup: limine.conf.bak-nvkp)"
      sudo -n cp /boot/limine.conf /boot/limine.conf.bak-nvkp
      { echo; echo "# --- nvkp entry (auto, $(date '+%F %T')) ---"; bootloader_entry; } \
        | sudo -n tee -a /boot/limine.conf >/dev/null
      echo "    Wpis dopisany — w menu Limine wybierz 'Linux-nvkp'. Stare wpisy bez zmian."
    else
      echo "    Aby dopisać automatycznie: ./build-kernel.sh --limine (po zakończeniu tego buildu)."
    fi
  else
    echo "    UWAGA: nie wykryto GRUB-a ani Limine — dodaj wpis bootloadera ręcznie."
  fi
else
  echo ">>> [9/9] pomijam bootloader (--no-install)"
fi

# --------------------------------------------------------------------------
# Podsumowanie
# --------------------------------------------------------------------------
echo
echo "=== PODSUMOWANIE ==="
echo "kernelrelease: $KREL"
if [ "$NO_INSTALL" = false ]; then
  echo "--- nowe pliki w /boot ---"
  sudo -n ls -la /boot/vmlinuz-"$BOOT_PREFIX"* /boot/initramfs-"$BOOT_PREFIX"* /boot/System.map-"$BOOT_PREFIX"* 2>/dev/null || true
  echo "--- stare jądro (bez zmian) ---"
  sudo -n ls -la /boot/vmlinuz-* /boot/initramfs-* 2>/dev/null | grep -v "$BOOT_PREFIX" || true
  echo "Na tym systemie stare jądro to UKI w /boot/EFI/Linux/omarchy_linux.efi — nie ruszone."
fi
echo
echo "Reboot i w menu boot wybierz nowy wpis. Po testach wróć do starego jądra:"
echo "  wybierz stary wpis w menu albo (Limine) zmień default_entry w /boot/limine.conf."
