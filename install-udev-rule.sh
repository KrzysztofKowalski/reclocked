#!/usr/bin/env bash
# install-udev-rule.sh — instaluje regułę udev wymuszającą nouveau dla GT 750M.
# Omija blacklist z pakietu nvidia-utils (Hyprland go wymaga, ale blob nvidia
# nie jest zainstalowany). Patrz patches/81-nouveau-kepler.rules.
set -euo pipefail

RULE=patches/81-nouveau-kepler.rules
DEST=/etc/udev/rules.d/81-nouveau-kepler.rules

if [ ! -f "$RULE" ]; then
  echo "BŁĄD: nie znaleziono $RULE (uruchom z katalogu repo)" >&2
  exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "Uruchom z sudo: sudo $0 $*" >&2
  exec sudo -E "$0" "$@"
fi

install -m644 "$RULE" "$DEST"
udevadm control --reload-rules
echo "Zainstalowano: $DEST"
echo "Przeładuj reguły udev: udevadm control --reload-rules (wykonano)"
echo "Następny reboot załaduje nouveau automatycznie."
echo "Test bez rebootu: sudo modprobe nouveau"