#!/usr/bin/env bash
# recover-gpu.sh — revert GPU experiments if the display breaks after testing
# AQ_DRM_DEVICES (Intel-primary) / gpu-switch.
#
# Reverts:
#   1. AQ_DRM_DEVICES (Option 2) in ~/.config/uwsm/default  -> Hyprland back on nouveau
#   2. SDDM autologin                                         -> restore from .bak
#   3. (optional, --dedicated) gpu-switch mux back to dGPU
#
# Run from SSH (e.g. ssh -t user@<host>) or a TTY when the screen is black.
# Safe & idempotent: only removes what it finds, never applies new GPU config.
#
#   ./recover-gpu.sh                 # revert 1+2, no reboot
#   ./recover-gpu.sh --reboot        # revert 1+2, then reboot
#   ./recover-gpu.sh --dedicated     # also undo gpu-switch panel mux (-> dGPU)
#   ./recover-gpu.sh --dedicated --reboot

set -uo pipefail

REBOOT=0; DEDICATED=0
for a in "$@"; do
  case "$a" in
    --reboot) REBOOT=1;;
    --dedicated) DEDICATED=1;;
    *) echo "unknown arg: $a";;
  esac
done

UWSM_DEFAULT="$HOME/.config/uwsm/default"
AUTOLOGIN=/etc/sddm.conf.d/autologin.conf
AUTOLOGIN_BAK=/etc/sddm.conf.d/autologin.conf.bak
# Path to the gpu-switch binary (panel mux). Override with GPU_SWITCH env var.
# Default: look next to this script, then fall back to /usr/local/bin/gpu-switch.
GPU_SWITCH="${GPU_SWITCH:-$(dirname "$(readlink -f "$0")")/gpu-switch/gpu-switch}"
[ -x "$GPU_SWITCH" ] || GPU_SWITCH="/usr/local/bin/gpu-switch"

echo "== GPU recovery =="
[ "$EUID" = 0 ] || echo "(some steps use sudo)"

# 1. Revert AQ_DRM_DEVICES (Option 2)
if [ -f "$UWSM_DEFAULT" ] && grep -q AQ_DRM_DEVICES "$UWSM_DEFAULT" 2>/dev/null; then
  mv "$UWSM_DEFAULT" "$UWSM_DEFAULT.reverted.$(date +%s)"
  echo "  [OK] removed AQ_DRM_DEVICES from $UWSM_DEFAULT (backup saved)"
else
  echo "  [--] no AQ_DRM_DEVICES in $UWSM_DEFAULT (Option 2 not active)"
fi

# 2. Restore SDDM autologin from backup if present
if [ -f "$AUTOLOGIN_BAK" ]; then
  sudo cp "$AUTOLOGIN_BAK" "$AUTOLOGIN" && echo "  [OK] restored $AUTOLOGIN from .bak"
else
  echo "  [--] no $AUTOLOGIN_BAK (nothing to restore)"
fi

# 3. Undo gpu-switch panel mux -> dGPU
if [ "$DEDICATED" = 1 ]; then
  if [ -x "$GPU_SWITCH" ]; then
    sudo "$GPU_SWITCH" --dedicated && echo "  [OK] gpu-switch -> dedicated (applies at next reboot)"
  else
    echo "  [!!] $GPU_SWITCH not found/executable"
  fi
fi

echo
if [ "$REBOOT" = 1 ]; then
  echo "Rebooting in 3s... (Ctrl-C to cancel)"; sleep 3; sudo systemctl reboot
else
  echo "Done. Reboot to apply:  sudo systemctl reboot"
fi