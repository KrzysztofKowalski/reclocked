#!/usr/bin/env bash
# pstate.sh — manualna zmiana pstate karty GT 750M (nouveau, Kepler GK107)
# Zapis przez debugfs: /sys/kernel/debug/dri/0000:01:00.0/pstate (wymaga root).
# USTAWIENIA SĄ RUNTIME-ONLY: reboot ZAWSZE resetuje do stanu startowego.
#
# v4: integracja z reclocked — `set` włącza manual override (daemon zamraża auto),
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
OVERRIDE_DIR=/run/reclocked
OVERRIDE_FILE=/run/reclocked/override

find_hwmon() {
  for h in /sys/class/hwmon/hwmon*; do
    if [ -r "$h/name" ] && [ "$(cat "$h/name")" = "nouveau" ]; then
      echo "$h"
      return 0
    fi
  done
  return 1
}

find_coretemp() {
  # CPU temp ("Package id 0") — coretemp NIE znika przy power-cycle dGPU
  # (w przeciwieństwie do hwmon nouveau). Fallback dla pstate.sh gdy dGPU OFF.
  for h in /sys/class/hwmon/hwmon*; do
    if [ -r "$h/name" ] && [ "$(cat "$h/name")" = "coretemp" ] && [ -r "$h/temp1_input" ]; then
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
  # Zwraca PID reclocked lub pusty ciąg (testować niepustość, NIE exit status
  # potoku — `pgrep | head` ma status z `head`, zawsze 0; patrz raport 15).
  pgrep -x reclocked 2>/dev/null | head -1
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

# ---------------------------------------------------------------------------
# iGPU (Intel Iris Pro 5200, i915 RPS) — monitoring READ-ONLY (raport 75, Opcja A;
# zapis/cap w v5.1+). Tu NIE zapisujemy niczego — tylko odczyt sysfs.
# ---------------------------------------------------------------------------

IGPU_BDF="0000:00:02.0"

find_igpu_card() {
  # Numer cardN zmienia się między bootami — znajdź kartę przez PCI BDF
  # (/sys/class/drm/cardN/device -> readlink daje BDF), nie hardcode cardN.
  local card bdf
  for card in /sys/class/drm/card*; do
    [ -e "$card" ] || continue
    bdf="$(basename "$(readlink "$card/device" 2>/dev/null)")"
    if [ "$bdf" = "$IGPU_BDF" ]; then
      printf '%s\n' "${card##*/}"
      return 0
    fi
  done
  return 1
}

igpu_line() {
  # key: wartość — wyrównana kolumna (jak sekcja dGPU).
  printf '  %-22s %s %s\n' "$1" "$2" "${3:-}"
}

igpu_rc6_residency() {
  # rc6_residency_ms jest sumaryczne od boota — pokaż mS + % od /proc/uptime.
  local d="$1" res up
  res="$(cat "$d/rc6_residency_ms" 2>/dev/null)" || res=""
  if [ -n "$res" ]; then
    up="$(awk '{print $1}' /proc/uptime 2>/dev/null)" || up=""
    if [ -n "$up" ]; then
      igpu_line "rc6_residency_ms:" "$res ms" \
        "$(awk -v r="$res" -v u="$up" 'BEGIN { if (u>0) printf "(≈%.1f%% od boota)", r/(u*1000)*100 }')"
    else
      igpu_line "rc6_residency_ms:" "$res ms" ""
    fi
  else
    igpu_line "rc6_residency_ms:" "niedostępne" ""
  fi
}

igpu_rps() {
  # Wspólny odczyt pól RPS; $1 = katalog sysfs, $2 = prefiks nazw (rps_|gt_).
  local d="$1" p="$2" cur
  cur="$(cat "$d/${p}cur_freq_mhz" 2>/dev/null)" || cur="n/d"
  igpu_line "${p}cur_freq_mhz:" "$cur MHz" "* aktualny"
  igpu_line "${p}max_freq_mhz:" "$(cat "$d/${p}max_freq_mhz" 2>/dev/null || echo n/d) MHz"
  igpu_line "${p}min_freq_mhz:" "$(cat "$d/${p}min_freq_mhz" 2>/dev/null || echo n/d) MHz"
  igpu_line "${p}boost_freq_mhz:" "$(cat "$d/${p}boost_freq_mhz" 2>/dev/null || echo n/d) MHz"
  igpu_line "${p}RP0_freq_mhz:" "$(cat "$d/${p}RP0_freq_mhz" 2>/dev/null || echo n/d) MHz"
  igpu_line "${p}RP1_freq_mhz:" "$(cat "$d/${p}RP1_freq_mhz" 2>/dev/null || echo n/d) MHz"
  igpu_line "${p}RPn_freq_mhz:" "$(cat "$d/${p}RPn_freq_mhz" 2>/dev/null || echo n/d) MHz"
}

igpu_status() {
  local card base d rc6
  card="$(find_igpu_card)" || {
    echo "=== iGPU (Intel Iris Pro 5200) — RPS ==="
    echo "iGPU ($IGPU_BDF): nie znaleziono"
    return 0
  }
  base="/sys/class/drm/$card"
  echo "=== iGPU (Intel Iris Pro 5200) — RPS ==="
  igpu_line "karta:" "$card ($IGPU_BDF)"

  if [ -d "$base/gt/gt0" ]; then
    d="$base/gt/gt0"
    igpu_line "tryb:" "gt/gt0 (nowy interfejs)"
    igpu_rps "$d" "rps_"
    if [ -r "$d/rc6_enable" ]; then
      igpu_line "rc6_enable:" "$(cat "$d/rc6_enable" 2>/dev/null || echo n/d)"
      igpu_rc6_residency "$d"
    fi
  elif [ -r "$base/gt_cur_freq_mhz" ]; then
    d="$base"
    igpu_line "tryb:" "gt_* (stary interfejs)"
    igpu_rps "$d" "gt_"
    rc6="$base/power"
    if [ -r "$rc6/rc6_enable" ]; then
      igpu_line "rc6_enable:" "$(cat "$rc6/rc6_enable" 2>/dev/null || echo n/d)"
      igpu_rc6_residency "$rc6"
    fi
  else
    igpu_line "rps:" "brak interfejsu iGPU RPS"
  fi
}

fan_status() {
  # v5.1: aktualna krzywa fan + obroty z daemona (/run/reclocked/status, JSON).
  # Krzywa wybiera daemon (igd/dga/compiler/override) — tu tylko odczyt.
  # Fallback bez daemona: obroty z sysfs applesmc (krzywa nieznana).
  local curve tmin tmax r1 r2 note
  echo "=== wentylatory ==="
  if [ -r /run/reclocked/status ]; then
    curve=$(sed -n 's/.*"fan_curve": *"\([^"]*\)".*/\1/p' /run/reclocked/status)
    tmin=$(sed -n 's/.*"fan_tmin": *\([0-9]*\).*/\1/p' /run/reclocked/status)
    tmax=$(sed -n 's/.*"fan_tmax": *\([0-9]*\).*/\1/p' /run/reclocked/status)
    r1=$(sed -n 's/.*"fan_rpm1": *\([0-9]*\).*/\1/p' /run/reclocked/status)
    r2=$(sed -n 's/.*"fan_rpm2": *\([0-9]*\).*/\1/p' /run/reclocked/status)
    case "$curve" in
      igd)      note="tylko-iGPU (dGPU OFF)" ;;
      dga)      note="dGPU ON" ;;
      compiler) note="boost kompilator" ;;
      override) note="override (ręczne)" ;;
      off)      note="fan wyłączony" ;;
      *)        note="nieznana (${curve:-brak danych})" ;;
    esac
    printf '  %-22s %s\n' "krzywa:" "$curve ($tmin-$tmax°C) — $note"
    printf '  %-22s fan1=%s RPM, fan2=%s RPM\n' "obroty:" "${r1:-n/d}" "${r2:-n/d}"
  else
    local f1 f2
    f1=$(cat /sys/devices/platform/applesmc.768/fan1_input 2>/dev/null)
    f2=$(cat /sys/devices/platform/applesmc.768/fan2_input 2>/dev/null)
    printf '  %-22s %s\n' "krzywa:" "daemon nie działa — nieznana"
    printf '  %-22s fan1=%s RPM, fan2=%s RPM (sysfs)\n' "obroty:" "${f1:-n/d}" "${f2:-n/d}"
  fi
}

status() {
  echo "=== pstate (debugfs) ==="
  sudo cat "$PSTATE"
  local hw ct
  if hw=$(find_hwmon); then
    # dGPU OFF (switchd v5.0): odczyt temp1_input pada EINVAL — nie crashuj.
    if t=$(cat "$hw/temp1_input" 2>/dev/null); then
      printf "=== temperatura: %.1f °C (hwmon: %s) ===\n" \
        "$(awk '{printf "%.1f", $1/1000}' <<<"$t")" "$(basename "$hw")"
    elif ct=$(find_coretemp); then
      # dGPU OFF → pokaż temp CPU (coretemp) — to samo źródło, którego używa
      # reclocked dla wentylatorów gdy dGPU OFF (max(dGPU,coretemp)).
      printf "=== temperatura: %.1f °C (dGPU OFF — CPU coretemp: %s) ===\n" \
        "$(awk '{printf "%.1f", $1/1000}' "$ct/temp1_input")" "$(basename "$ct")"
    else
      echo "=== temperatura: NIEDOSTĘPNA (dGPU OFF, brak coretemp) ==="
    fi
  fi
  igpu_status
  fan_status
  echo "=== reclocked ==="
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
  # switchd v5.0: dGPU OFF (gmux odciął zasilanie) → write pstate = hang w nouveau
  # (nvkm_pstate_calc — daemon zawisł tak w G1 live). Nie pozwól na to.
  if [ -f /run/reclocked/status ] && grep -q '"dgpu_power": "off"' /run/reclocked/status; then
    echo "BŁĄD: dGPU jest WYŁĄCZONY (switchd v5.0) — zapis pstate by zawiesił kartę." >&2
    echo "      Najpierw włącz dGPU: reclockctl dgpu-on (potem reclockctl dgpu-auto)" >&2
    return 1
  fi
  case "$val" in
    0e|0f) echo "⚠️  $val = agresywny reclock pamięci — testuj ostrożnie!" >&2 ;;
  esac
  printf '%s' "$arg" | sudo tee "$PSTATE" >/dev/null || return 1
  # Włącz manual override — daemon zamrozi auto dopóki flag-file istnieje.
  set_override "$arg"
  echo "override włączony (/run/reclocked/override) — daemon zamraża auto"
  if pgrep -x reclocked >/dev/null 2>&1; then
    echo "daemon reclocked działa → hold na $val (auto wznowione po: ./pstate.sh auto)"
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