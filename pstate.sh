#!/usr/bin/env bash
# pstate.sh — manualna zmiana pstate karty GT 750M (nouveau, Kepler GK107)
# Zapis przez debugfs: /sys/kernel/debug/dri/0000:01:00.0/pstate (wymaga root).
# USTAWIENIA SĄ RUNTIME-ONLY: reboot ZAWSZE resetuje do stanu startowego.
#
# v4: integracja z reclockd — `set` włącza manual override (daemon zamraża auto),
#     `auto` zdejmuje override, `status` pokazuje też stan daemona i override.
#
# Użycie:
#   ./pstate.sh status           — pstate + temp + override + daemon
#   ./pstate.sh set 0a           — ustaw 0a (AC+DC) + włącz override (zamraża daemon)
#   ./pstate.sh set ac:0a        — ustaw 0a tylko AC + override
#   ./pstate.sh auto             — zdejmij override (daemon wznawia auto)
#
# Pstany (core / mem, MHz):
#   07: 270-405 / 838       0a: 270-925 / 1560
#   0e: 270-925 / 4000      0f: 270-925 / 5016
# 0a/0e/0f mają TEN SAM zakres core (max 925 MHz) — różni je tylko pamięć.
# ⚠️ 0e/0f = agresywny reclock pamięci — testować ostrożnie (historia zawieszek).

PSTATE=/sys/kernel/debug/dri/0000:01:00.0/pstate
VALID="07 0a 0e 0f"
OVERRIDE_DIR=/run/reclockd
OVERRIDE_FILE=/run/reclockd/override

find_hwmon() {
  for h in /sys/class/hwmon/hwmon*; do
    if [ -r "$h/name" ] && [ "$(cat "$h/name")" = "nouveau" ]; then
      echo "$h"
      return 0
    fi
  done
  return 1
}

usage() {
  cat <<'EOF'
Użycie:
  ./pstate.sh status           — pstate + temp + override + daemon
  ./pstate.sh set 0a           — ustaw 0a (AC+DC) + włącz override (zamraża daemon)
  ./pstate.sh set ac:0a        — ustaw 0a tylko AC + override
  ./pstate.sh auto             — zdejmij override (daemon wznawia auto)

Pstany (core / mem, MHz):
  07: 270-405 / 838       0a: 270-925 / 1560
  0e: 270-925 / 4000      0f: 270-925 / 5016
0a/0e/0f mają TEN SAM zakres core (max 925 MHz) — różni je tylko pamięć.
⚠️ 0e/0f = agresywny reclock pamięci — testować ostrożnie.
Ustawienia są runtime-only: REBOOT RESETUJE do stanu startowego.
EOF
}

daemon_pid() {
  # Zwraca PID reclockd lub pusty ciąg (testować niepustość, NIE exit status
  # potoku — `pgrep | head` ma status z `head`, zawsze 0; patrz raport 15).
  pgrep -x reclockd 2>/dev/null | head -1
}

daemon_status() {
  local pid
  pid=$(daemon_pid)
  if [ -n "$pid" ]; then
    echo "daemon: RUNNING (PID $pid)"
  else
    echo "daemon: nie działa"
  fi
}

override_status() {
  local pid content
  pid=$(daemon_pid)
  if sudo -n test -f "$OVERRIDE_FILE" 2>/dev/null; then
    content=$(sudo -n cat "$OVERRIDE_FILE" 2>/dev/null)
    if [ -n "$pid" ]; then
      echo "override: AKTYWNY ($content — pstate zamrożony, daemon PID $pid)"
    else
      echo "override: $content (flaga ustawiona, ale daemon NIE działa — flaga bez efektu)"
    fi
  else
    echo "override: brak (auto)"
  fi
}

status() {
  echo "=== pstate (debugfs) ==="
  sudo cat "$PSTATE"
  local hw
  if hw=$(find_hwmon); then
    printf "=== temperatura: %.1f °C (hwmon: %s) ===\n" \
      "$(awk '{printf "%.1f", $1/1000}' "$hw/temp1_input")" "$(basename "$hw")"
  fi
  echo "=== reclockd ==="
  daemon_status
  override_status
}

set_override() {
  local val="$1"
  sudo -n mkdir -p "$OVERRIDE_DIR" 2>/dev/null
  local ts
  ts=$(date '+%Y-%m-%d %H:%M:%S')
  echo "$val @ $ts" | sudo -n tee "$OVERRIDE_FILE" >/dev/null
}

set_state() {
  [ $# -ge 1 ] || { usage; return 2; }
  local arg="$1"
  local val="${arg##*:}"
  case " $VALID " in
    *" $val "*) : ;;
    *) echo "BŁĄD: nieznany pstate '$arg'. Dozwolone: $VALID (opcjonalnie z prefiksem ac:/dc:)" >&2
       return 2 ;;
  esac
  sudo -n test -f "$PSTATE" || { echo "BŁĄD: brak $PSTATE — czy debugfs zamontowany? (sprawdź: sudo cat $PSTATE)" >&2; return 1; }
  case "$val" in
    0e|0f) echo "⚠️  $val = agresywny reclock pamięci — testuj ostrożnie!" >&2 ;;
  esac
  printf '%s' "$arg" | sudo tee "$PSTATE" >/dev/null || return 1
  # Włącz manual override — daemon zamrozi auto dopóki flag-file istnieje.
  set_override "$arg"
  echo "override włączony (/run/reclockd/override) — daemon zamraża auto"
  if pgrep -x reclockd >/dev/null 2>&1; then
    echo "daemon reclockd działa → hold na $val (auto wznowione po: ./pstate.sh auto)"
  fi
  sleep 1
  echo "=== po zmianie ==="
  status
}

auto_override() {
  if sudo -n test -f "$OVERRIDE_FILE" 2>/dev/null; then
    sudo -n rm -f "$OVERRIDE_FILE"
    echo "override zdjęty — daemon wznawia auto"
  else
    echo "override nieaktywny (nic do zrobienia)"
  fi
  status
}

case "${1:-status}" in
  status) status ;;
  set) shift; set_state "$@" ;;
  auto) auto_override ;;
  -h|--help) usage ;;
  *) usage; exit 1 ;;
esac