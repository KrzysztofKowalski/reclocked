#!/usr/bin/env bash
# build-uki-nvkp.sh — zbuduj UKI z jądra nvkp i bootuj przez protocol: efi (jak Omarchy)
#
# DLACZEGO: jądro nvkp bootowało przez `protocol: linux` (legacy) → Limine nie
# znajdował GOP na firmware Apple → screen_info wyzerowane → vgacon na WYŁĄCZONYM
# dGPU → czarny ekran. Działające jądro Omarchy to UKI (omarchy_linux.efi) przez
# `protocol: efi` → jądrowy EFI stub sam pobiera GOP → efifb na iGPU → widoczny.
#
# NIE wystarczy zmienić protocol: linux → efi na gołym vmlinuz: Limine chainload
# NIE przekazuje initramfs → jądro nie zamontuje roota (LUKS). Trzeba UKI.
#
# Co robi: 1) regeneruje initramfs (config z encrypt + keyfile)
#          2) buduje UKI (vmlinuz + initramfs + cmdline w jednym .efi)
#          3) limine-entry-tool --add-uki (wpis protocol: efi)
#          4) usuwa cmdline: z wpisu nvkp (użyj embedded cmdline, nie drop-inów
#             z quiet/loglevel=0, które by ukryły output)
#          5) usuwa stary wpis //linux-nvkp (protocol: linux, zepsuty)
# Reboot i wybór wpisu 'nvkp' — Ty.
set -euo pipefail

PROJ="$(cd "$(dirname "$0")" && pwd)"
KREL="7.1.8-nvkp-dirty"          # uname -r po booth (CONFIG_LOCALVERSION_AUTO=y + dirty tree)
KNAME="linux-nvkp"              # pliki w /boot: vmlinuz-linux-nvkp, initramfs-linux-nvkp.img
UKI_NAME="nvkp"                 # nazwa wpisu w menu Limine (//nvkp)
CONF="/etc/mkinitcpio-nvkp.conf"
INITRAMFS="/boot/initramfs-${KNAME}.img"
VMLINUZ="/boot/vmlinuz-${KNAME}"
UKI="/boot/EFI/Linux/${UKI_NAME}.efi"
MODULES_DIR="/usr/lib/modules/${KREL}"
TMP="$PROJ/tmp/uki-build"
CMDLINE_FILE="$TMP/nvkp-cmdline.txt"

# ----------------------------------------------------------------------------
# cmdline (verbose — diagnostyczny). Po potwierdzeniu stabilności zamień na
# wersję quiet (patrz komentarz na dole pliku).
# ----------------------------------------------------------------------------
CMDLINE="cryptdevice=PARTUUID=13318437-d122-4e79-890d-cf8f3b2332c5:root root=/dev/mapper/root zswap.enabled=0 rootflags=subvol=@ rw rootfstype=btrfs resume=/dev/mapper/root resume_offset=1932841 initramfs_async=0 cryptkey=rootfs:/crypto_keyfile.bin loglevel=7 systemd.show_status=true"

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
yellow(){ printf '\033[33m%s\033[0m\n' "$*"; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }

bold "=== build-uki-nvkp.sh — UKI z jądra nvkp (protocol: efi) ==="
echo "Kernel: ${KREL}  (moduły: ${MODULES_DIR})"
echo "UKI:    ${UKI}"
echo

# --- wstępne checki ----------------------------------------------------------
if ! sudo -n true 2>/dev/null; then
  red "BŁĄD: sudo -n nie działa (passwordless sudo wymagane)."
  exit 1
fi
for c in mkinitcpio limine-entry-tool; do
  command -v "$c" >/dev/null 2>&1 || { red "BŁĄD: $c nie istnieje."; exit 1; }
done
sudo -n test -d "${MODULES_DIR}" || { red "BŁĄD: ${MODULES_DIR} nie istnieje — moduły nvkp nie są zainstalowane."; exit 1; }
sudo -n test -f "${VMLINUZ}" || { red "BŁĄD: ${VMLINUZ} nie istnieje."; exit 1; }
sudo -n test -f "${CONF}" || { red "BŁĄD: ${CONF} nie istnieje — odpal najpierw fix-initramfs-nvkp.sh."; exit 1; }
sudo -n test -f /crypto_keyfile.bin || { yellow "UWAGA: /crypto_keyfile.bin nie istnieje — auto-unlock LUKS nie zadziała (będzie prompt hasła)."; }
green "OK: mkinitcpio + limine-entry-tool + moduły + vmlinuz + config obecne."
echo

# --- 1. cmdline file ----------------------------------------------------------
bold "[1/5] cmdline (verbose) → ${CMDLINE_FILE}"
mkdir -p "${TMP}"
cat > "${CMDLINE_FILE}" <<EOF
# nvkp UKI cmdline — verbose (diagnostyczny). Usuń loglevel=7/systemd.show_status
# i dodaj quiet splash loglevel=0 po potwierdzeniu stabilności.
${CMDLINE}
EOF
green "    Zapisano cmdline (verbose)."
echo

# --- 2. initramfs + UKI (mkinitcpio -U) ---------------------------------------
bold "[2/5] mkinitcpio -k ${KREL} -c ${CONF} -g ${INITRAMFS} -U ${UKI}"
echo "    (to NIE jest rebuild jądra — tylko initramfs + UKI, ~1-2 min)"
sudo -n mkinitcpio -k "${KREL}" -c "${CONF}" -g "${INITRAMFS}" -U "${UKI}" --cmdline "${CMDLINE_FILE}"
sudo -n test -f "${UKI}" || { red "BŁĄD: UKI nie powstał (${UKI})."; exit 1; }
green "    UKI wygenerowany: ${UKI}"
echo

# --- 3. weryfikacja UKI (sekcje .linux/.initrd/.cmdline) ----------------------
bold "[3/5] Weryfikacja UKI (sekcje systemd-stub)"
if command -v objdump >/dev/null 2>&1; then
  sudo -n objdump -h "${UKI}" 2>/dev/null | grep -E '\.(linux|initrd|cmdline|osrel)' | sed 's/^/      /' \
    || red "    UWAGA: nie znaleziono sekcji .linux/.initrd/.cmdline — UKI może być zły."
else
  yellow "    objdump brak — pomijam weryfikację sekcji (sprawdź rozmiar: $(sudo -n stat -c%s "${UKI}") B)."
fi
echo

# --- 4. wpis Limine (--add-uki) + usunięcie cmdline: z wpisu ------------------
bold "[4/5] limine-entry-tool --add-uki ${UKI_NAME}"
sudo -n limine-entry-tool --add-uki "${UKI_NAME}" "${UKI}" \
  --comment "nv-kepler ${KREL} (UKI, verbose)" \
  --quiet
green "    Wpis //${UKI_NAME} dodany (protocol: efi)."
echo
bold "    Usuwam cmdline: z wpisu //${UKI_NAME} (użyj embedded cmdline, nie drop-inów z quiet)"
# Drop-iny limine-entry-tool (omarchy-defaults.conf) dodają do KAŻDEGO wpisu
# `quiet splash loglevel=0 ...` — to by nadpisało nasz embedded loglevel=7.
# Usuwamy linię cmdline: z wpisu //nvkp, żeby systemd-stub użył embedded cmdline.
sudo -n cp /boot/limine.conf /boot/limine.conf.bak-uki
sudo -n awk -v name="${UKI_NAME}" '
  $0 == "  //" name { in_entry=1; print; next }
  in_entry && /^  cmdline:/ { next }
  in_entry && !/^  [^ ]/ { in_entry=0 }
  { print }
' /boot/limine.conf > "${TMP}/limine.conf.new"
sudo -n cp "${TMP}/limine.conf.new" /boot/limine.conf
green "    cmdline: usunięty z wpisu //${UKI_NAME} (backup: /boot/limine.conf.bak-uki)."
echo

# --- 5. usuń stary wpis //linux-nvkp (protocol: linux, zepsuty) ---------------
bold "[5/5] Usuwam stary wpis //linux-nvkp (protocol: linux)"
if limine-entry-tool --tree 2>/dev/null | grep -q 'linux-nvkp'; then
  sudo -n limine-entry-tool --remove-kernel "${KNAME}" --quiet \
    && green "    Stary wpis //${KNAME} usunięty." \
    || yellow "    Nie udało się usunąć //${KNAME} — usuń ręcznie: sudo limine-entry-tool --remove-kernel ${KNAME}"
else
  yellow "    Wpis //${KNAME} nie istnieje — pomijam."
fi
echo

# --- podsumowanie -------------------------------------------------------------
bold "=== Struktura menu Limine ==="
limine-entry-tool --tree 2>&1 | sed 's/^/    /'
echo
bold "=== Gotowe — następny krok: reboot ==="
echo "1. Zrestartuj: sudo reboot"
echo "2. W menu Limine wybierz '${UKI_NAME}' (UKI, protocol: efi — jak Omarchy)."
echo "3. Oczekiwane: widoczny boot (efifb na iGPU) + auto-unlock LUKS (bez hasła)."
echo "4. Po booth weryfikuj:"
echo "     uname -r                          → ${KREL}"
echo "     lsmod | grep nouveau               → załadowany"
echo "     journalctl -b | grep -iE 'nouveau.*(error|fail)'"
echo
yellow "Jeśli nvkp nie wstanie: wybierz 'linux' (Omarchy) — nadal działa."
yellow "Backup limine.conf: /boot/limine.conf.bak-uki (przywróć: sudo cp ...)."
yellow "Usunięcie wpisu UKI: sudo limine-entry-tool --remove-uki ${UKI_NAME}"

# ----------------------------------------------------------------------------
# Wersja quiet (po potwierdzeniu stabilności) — zamień CMDLINE na:
#   cryptdevice=PARTUUID=13318437-d122-4e79-890d-cf8f3b2332c5:root root=/dev/mapper/root zswap.enabled=0 rootflags=subvol=@ rw rootfstype=btrfs resume=/dev/mapper/root resume_offset=1932841 initramfs_async=0 cryptkey=rootfs:/crypto_keyfile.bin quiet splash loglevel=0 systemd.show_status=false rd.udev.log_level=0 vt.global_cursor_default=0
# i odpal skrypt ponownie (przebuduje UKI z nowym cmdline).
# ----------------------------------------------------------------------------
