#!/usr/bin/env bash
# bench-gpu.sh — benchmarker glmark2 porównujący iGPU (Intel Iris Pro 5200) i
# dGPU (NVIDIA GT 750M, nouveau — Kepler GK107) na MacBooku Pro 11,3.
#
# Cel: powtarzalne porównanie wydajności przy PEŁNEJ rozdzielczości panela
# (domyślnie 2880x1800). Poprzednie testy (raport 78) na 800x600 nie obciążały
# GPU — pełny rozmiar dopiero pokazuje różnice między kartami.
#
# Metodologia:
#   * FULL-RES (2880x1800) — natywny rozmiar panela; 800x600 to za mało, żeby
#     obciążyć GPU (raport 78).
#   * OFFSCREEN (FBO) — pomiary on-screen dGPU są zafałszowane copybackiem
#     PRIME: każda klatka renderowana w VRAM dGPU jest kopiowana do iGPU
#     (DRI_PRIME); sufit ~1300 FPS przy 800x600, ~140 FPS przy 2880x1800.
#     Offscreen = zero copybacku, zero vsync.
#   * --frame-end none — domyślnie glmark2 robi glFinish() na koniec każdej
#     klatki, co serializuje pipeline i zaniża wynik (2023.01 ma --frame-end,
#     nie ma --swap-interval).
#   * --swap-mode immediate — zamiast vsync; on-screen bez tego łapie lock
#     kompozytora (raport 78, powtórka: 148 pkt vs 1000+ bez locka — fokus
#     okna na Xwayland zmienia vsync).
#   * ON-SCREEN: zablokuj kompozytor Omarchy/Hyprland przed biegiem (np.
#     hyprctl dispatch lock) — screen jest z założenia mniej wiarygodny.
#   * iGPU RPS — NIE pinujemy (auto). dGPU pinujemy debugfs pstate przy
#     zatrzymanym daemonie — inaczej reclocked negocjuje ceiling w trakcie biegu
#     i realny pstate ≠ ustawiony (raport 78, anomalia).
#   * Wiatraki manual 100% — fan1_max=6156, fan2_max=5700 RPM (SEPARATNIE,
#     maksima są różne; nie wpisujemy tej samej wartości).
#
# Użycie:
#   sudo ./bench-gpu.sh [opcje]
#     -g | --gpu igd|dgpu|both          (default both)
#     -m | --mode offscreen|screen|both (default offscreen)
#     -s | --size WxH|auto              (default 2880x1800; auto = hyprctl,
#                                        fallback 2880x1800)
#     -p | --pstate 0e|0a|07|all        (default 0e; all = 07 0a 0e)
#     -f | --frame-end none|default     (default none)
#     -F | --fans 100|auto              (default 100)
#     -o | --outdir DIR                 (default tmp/bench)
#     -n | --dry-run                    (tylko pokaż komendy, nie wykonuj)
#     -h | --help
#
# Restore (trap EXIT, najwyższy priorytet): wiatraki → auto, reclocked → active,
# dGPU → Off. Skrypt idempotentny: daemon start/dgpu-on tylko gdy trzeba.
set -uo pipefail

# PROJ = ROOT repo (skrypt w scripts/ — dirname $0 = scripts, stąd /..)
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
RECLOCKCTL="${RECLOCKCTL:-$PROJ/src/reclockctl}"
APPLESMC="/sys/devices/platform/applesmc.768"
VGASW="/sys/kernel/debug/vgaswitcheroo/switch"
PSTATE_FILE="/sys/kernel/debug/dri/0000:01:00.0/pstate"
XDISPLAY=":0"                       # Xwayland — raport 78 działał na DISPLAY=:0

# --- domyślne opcje --------------------------------------------------------
GPU_RAW="both"                      # igd|dgpu|both
MODE_RAW="offscreen"                # offscreen|screen|both
SIZE="2880x1800"                    # WxH lub auto (hyprctl, fallback natywny)
PSTATE_RAW="0e"                     # 0e|0a|07|all (pstate dGPU)
FRAME_END="none"                    # none|default
FANS="100"                          # 100|auto
OUTDIR="$PROJ/tmp/bench"
DRY=0
GPU_LIST=""; MODE_LIST=""; PSTATE_LIST=""
RUN_LOGS=""                         # logi bieżącego uruchomienia (do summary)
ERRORS=0                            # 1 = był błąd → kod wyjścia 1
RESTORED=0

log()  { printf '[bench] %s\n' "$*"; }
fail() { ERRORS=1; printf '[bench] BŁĄD: %s\n' "$*"; }
die()  { printf 'BŁĄD: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Użycie:
  sudo ./bench-gpu.sh [opcje]
    -g | --gpu igd|dgpu|both          (default both)
    -m | --mode offscreen|screen|both (default offscreen)
    -s | --size WxH|auto              (default 2880x1800; auto = hyprctl,
                                       fallback 2880x1800)
    -p | --pstate 0e|0a|07|all        (default 0e; all = 07 0a 0e)
    -f | --frame-end none|default     (default none)
    -F | --fans 100|auto              (default 100)
    -o | --outdir DIR                 (default tmp/bench)
    -n | --dry-run                    (tylko pokaż komendy, nie wykonuj)
    -h | --help
EOF
}

# --- dry-run: drukuj zamiast wykonywać --------------------------------------
cmd() { # <komenda...>
  if [ "$DRY" = 1 ]; then printf 'DRY-RUN: %s\n' "$*"; else "$@"; fi
}
write_sysfs() { # <plik> <wartość>
  if [ "$DRY" = 1 ]; then
    printf 'DRY-RUN: echo %s > %s\n' "$2" "$1"
  else
    printf '%s\n' "$2" >"$1" 2>/dev/null || log "BŁĄD: zapis $1 = $2"
  fi
}
sleep_if_real() { # <sekundy> <opis>
  if [ "$DRY" = 1 ]; then
    printf 'DRY-RUN: sleep %s (%s)\n' "$1" "$2"
  else
    sleep "$1"
  fi
}

# --- daemon reclocked ---------------------------------------------------------
daemon_active() { pgrep -x reclocked >/dev/null 2>&1; }

daemon_wait_active() { # <timeout-s>
  local tmo="$1" i=0
  if [ "$DRY" = 1 ]; then
    printf 'DRY-RUN: czekam na aktywny reclocked (max %s s)\n' "$tmo"
    return 0
  fi
  while [ "$i" -lt "$tmo" ]; do
    daemon_active && return 0
    sleep 1
    i=$((i+1))
  done
  return 1
}

# --- vgaswitcheroo ------------------------------------------------------------
# Format linii DIS: 2:DIS: :Pwr:0000:01:00.0  |  2:DIS: :Off:0000:01:00.0
vgasw_line() { cat "$VGASW" 2>/dev/null | grep '^2:DIS:' | head -1; }
vgasw_is() { # <Pwr|Off>
  local line
  line="$(vgasw_line)"
  [ -n "$line" ] && case "$line" in *":$1:"*) return 0 ;; esac
  return 1
}
wait_vgasw() { # <Pwr|Off> <timeout-s>
  local want="$1" tmo="$2" i=0
  if [ "$DRY" = 1 ]; then
    printf 'DRY-RUN: poll vgaswitcheroo aż 2:DIS: :%s (max %s s)\n' "$want" "$tmo"
    return 0
  fi
  while [ "$i" -lt "$tmo" ]; do
    vgasw_is "$want" && return 0
    sleep 1
    i=$((i+1))
  done
  return 1
}

# --- wiatraki (applesmc) -------------------------------------------------------
fans_manual_100() {
  local f1 f2
  f1="$(cat "$APPLESMC/fan1_max" 2>/dev/null || echo 6156)"
  f2="$(cat "$APPLESMC/fan2_max" 2>/dev/null || echo 5700)"
  log "wiatraki manual 100% (fan1=$f1, fan2=$f2 RPM)"
  write_sysfs "$APPLESMC/fan1_manual" 1
  write_sysfs "$APPLESMC/fan1_output" "$f1"
  write_sysfs "$APPLESMC/fan2_manual" 1
  write_sysfs "$APPLESMC/fan2_output" "$f2"
}
fans_auto() {
  log "wiatraki → auto (SMC przejmuje)"
  write_sysfs "$APPLESMC/fan1_manual" 0
  write_sysfs "$APPLESMC/fan2_manual" 0
}

# --- pstate dGPU (debugfs) ------------------------------------------------------
# Zapis dozwolony TYLKO przy dGPU w Pwr (inaczej hang w nouveau — raport 77).
# Format pliku: "0e: core 270-925 MHz memory 4000 MHz" + "*" na aktywnym +
# linia "AC:" z user-state dla zasilania AC.
pstate_mem() { # <07|0a|0e> → MHz pamięci (echo)
  case "$1" in 0e) echo 4000 ;; 0a) echo 1560 ;; 07) echo 838 ;; *) return 1 ;; esac
}
pin_pstate() { # <07|0a|0e>
  local ps="$1" mem cur
  mem="$(pstate_mem "$ps")" || { fail "nieznany pstate '$ps'"; return 1; }
  vgasw_is Pwr || { fail "dGPU nie w Pwr — NIE pinuję pstate (zawieszenie!)"; return 1; }
  if [ "$DRY" = 1 ]; then
    printf 'DRY-RUN: echo %s > %s\n' "$ps" "$PSTATE_FILE"
    printf 'DRY-RUN: weryfikacja: grep -E "^%s:.*\\*" %s  +  grep -E "^AC:.*memory %s"\n' \
      "$ps" "$PSTATE_FILE" "$mem"
    return 0
  fi
  log "pin pstate=$ps (AC memory ${mem} MHz)"
  printf '%s\n' "$ps" >"$PSTATE_FILE" 2>/dev/null \
    || { fail "zapis pstate=$ps nie powiódł się"; return 1; }
  sleep 2   # czas na przełączenie zegarów (patche 0011-0013; reclocked timeout 2 s)
  cur="$(cat "$PSTATE_FILE" 2>/dev/null)" \
    || { fail "nie mogę odczytać $PSTATE_FILE"; return 1; }
  if ! grep -qE "^${ps}:.*\*" <<<"$cur"; then
    fail "pstate=$ps nieaktywny (brak '*' na linii ${ps}:)"
    return 1
  fi
  if ! grep -qE "^AC:.*memory ${mem}([^0-9]|$)" <<<"$cur"; then
    fail "AC nie trzyma memory ${mem} MHz — pstate=$ps niesprawdzony"
    return 1
  fi
  log "pstate=$ps potwierdzony (AC: core … memory ${mem} MHz)"
  return 0
}
check_pstate_still() { # <ps> — weryfikacja po biegu (pstate się nie zmienił?)
  local ps="$1" cur
  [ "$DRY" = 1 ] && return 0
  cur="$(cat "$PSTATE_FILE" 2>/dev/null)" || return 0
  if ! grep -qE "^${ps}:.*\*" <<<"$cur"; then
    log "UWAGA: pstate po biegu ≠ $ps — pstate się zmienił (ktoś nadpisał?)"
  fi
}

# --- power dGPU (przez reclockctl — flag-file wykonywany przez daemon) ---------
dgpu_on() {
  if vgasw_is Pwr; then
    log "dGPU już Pwr — pomijam dgpu-on"
  else
    cmd "$RECLOCKCTL" dgpu-on
    wait_vgasw Pwr 30 || { fail "dGPU nie przeszedł w Pwr po 30 s"; return 1; }
  fi
  sleep_if_real 5 "settle po power-on (PCIe link / PMU — patche 0011-0013)"
  return 0
}
dgpu_off() {
  if ! vgasw_is Pwr; then
    log "dGPU już Off — pomijam dgpu-off"
  else
    cmd "$RECLOCKCTL" dgpu-off
    wait_vgasw Off 30 || fail "dGPU nie przeszedł w Off po 30 s"
  fi
}

# --- glmark2 --------------------------------------------------------------------
glmark2_run() { # <env-prefix> <log> <args...>
  local prefix="$1" logf="$2"; shift 2
  if [ "$DRY" = 1 ]; then
    printf 'DRY-RUN: env %s glmark2 %s 2>&1 | tee %s\n' "$prefix" "$*" "$logf"
    return 0
  fi
  RUN_LOGS="$RUN_LOGS $logf"
  log "glmark2 → $logf"
  # shellcheck disable=SC2086  # prefix celowo niesplittowany: DISPLAY=:0 DRI_PRIME=1
  if env $prefix glmark2 "$@" 2>&1 | tee "$logf"; then
    log "glmark2 OK → $logf"
  else
    fail "glmark2 kod $? — $logf"
  fi
}

# --- sekcje benchmarku -----------------------------------------------------------
bench_igd() {
  log "== iGPU — Intel Iris Pro 5200 (i915, RPS auto — nie pinujemy) =="
  if daemon_active; then
    cmd "$RECLOCKCTL" stop || fail "reclockctl stop nie powiódł się"
  else
    log "daemon nieaktywny — pomijam stop"
  fi
  [ "$FANS" = 100 ] && fans_manual_100
  for mode in $MODE_LIST; do
    local args=(--size "$SIZE" --frame-end "$FRAME_END")
    [ "$mode" = offscreen ] && args+=(--off-screen)
    args+=(--swap-mode immediate)
    glmark2_run "DISPLAY=$XDISPLAY" \
      "$OUTDIR/bench-igd-${mode}-${SIZE}-${FRAME_END}.log" "${args[@]}"
  done
}

bench_dgpu() {
  log "== dGPU — NVIDIA GT 750M (nouveau, GK107) =="
  # 1. daemon aktywny? start + czekaj; potem dgpu-on (flag-file) + poll Pwr
  if daemon_active; then
    log "daemon aktywny — pomijam start"
  else
    log "daemon nieaktywny → start"
    cmd "$RECLOCKCTL" start || { fail "reclockctl start nie powiódł się"; return 1; }
    daemon_wait_active 15 || { fail "daemon nie wstał w 15 s"; return 1; }
  fi
  dgpu_on || return 1
  # 2. stop daemon — nie będzie negocjował pstate/wiatraków w trakcie biegów
  cmd "$RECLOCKCTL" stop || fail "reclockctl stop nie powiódł się"
  # 3. wiatraki manual 100%
  [ "$FANS" = 100 ] && fans_manual_100
  # 4. pstate × mode
  for ps in $PSTATE_LIST; do
    if ! pin_pstate "$ps"; then
      fail "pin pstate=$ps nieudany — pomijam ten pstate"
      continue
    fi
    for mode in $MODE_LIST; do
      local args=(--size "$SIZE" --frame-end "$FRAME_END")
      [ "$mode" = offscreen ] && args+=(--off-screen)
      args+=(--swap-mode immediate)
      glmark2_run "DISPLAY=$XDISPLAY DRI_PRIME=1" \
        "$OUTDIR/bench-dgpu-${mode}-${SIZE}-${ps}-${FRAME_END}.log" "${args[@]}"
      check_pstate_still "$ps"
    done
  done
}

# --- weryfikacja rendererów --------------------------------------------------------
verify_renderers() {
  log "== WERYFIKACJA RENDERERÓW =="
  local logs
  logs="$(printf '%s\n' $RUN_LOGS | grep '/bench-igd-' || true)"
  if [ -n "$logs" ]; then
    if grep -H "GL_RENDERER" $logs 2>/dev/null | grep -q "Mesa Intel.*P5200"; then
      log "iGPU: renderer OK — Mesa Intel … P5200"
    else
      fail "brak oczekiwanego renderera iGPU (Mesa Intel … P5200)"
    fi
  else
    log "brak biegów iGPU w tej sesji"
  fi
  logs="$(printf '%s\n' $RUN_LOGS | grep '/bench-dgpu-' || true)"
  if [ -n "$logs" ]; then
    if grep -H "GL_RENDERER" $logs 2>/dev/null | grep -q "NVE7"; then
      log "dGPU: renderer OK — NVE7 (nvc0)"
    else
      fail "brak oczekiwanego renderera dGPU (NVE7 / nvc0)"
    fi
  else
    log "brak biegów dGPU w tej sesji"
  fi
}

# --- restore / summary -------------------------------------------------------------
restore() {
  [ "$RESTORED" = 1 ] && return 0
  RESTORED=1
  log "== RESTORE (najwyższy priorytet) =="
  fans_auto
  if daemon_active; then
    log "daemon już aktywny — pomijam start"
  else
    log "daemon nieaktywny → start"
    cmd "$RECLOCKCTL" start || fail "reclockctl start nie powiódł się"
    daemon_wait_active 15 || fail "daemon nie wstał po restorze"
  fi
  dgpu_off
  print_summary
}

print_summary() {
  log "== STAN KOŃCOWY =="
  if daemon_active; then log "reclocked: active"; else log "reclocked: NIEAKTYWNY"; fi
  if vgasw_is Pwr; then log "dGPU: Pwr"; elif vgasw_is Off; then log "dGPU: Off"; else log "dGPU: stan nieznany"; fi
  local m1 m2
  m1="$(cat "$APPLESMC/fan1_manual" 2>/dev/null || echo '?')"
  m2="$(cat "$APPLESMC/fan2_manual" 2>/dev/null || echo '?')"
  log "wiatraki: fan1_manual=$m1 fan2_manual=$m2 (0 = auto)"
  log "== TABELA WYNIKÓW (Score per run) =="
  if [ -n "$RUN_LOGS" ]; then
    grep -HE "Surface Size|glmark2 Score" $RUN_LOGS 2>/dev/null \
      || log "brak wyników w logach biegu"
  else
    log "brak logów (nic nie biegnięto?)"
  fi
}

# --- preflight ---------------------------------------------------------------------
preflight() {
  [ "$EUID" = 0 ] || die "uruchom przez sudo: sudo $0 [opcje]"
  command -v glmark2 >/dev/null 2>&1 || die "brak glmark2 w PATH"
  if [ -x "$RECLOCKCTL" ]; then
    :
  elif command -v reclockctl >/dev/null 2>&1; then
    RECLOCKCTL="$(command -v reclockctl)"
  else
    die "brak reclockctl: $RECLOCKCTL (sprawdź PATH lub ścieżkę projektu)"
  fi
  [ -r "$VGASW" ] || die "brak dostępu do $VGASW (debugfs zamontowany?)"
  [ -e "$PSTATE_FILE" ] || die "brak $PSTATE_FILE (debugfs / moduł nouveau?)"
  for f in fan1_manual fan2_manual fan1_output fan2_output; do
    [ -w "$APPLESMC/$f" ] || die "brak zapisu do $APPLESMC/$f"
  done
  if dmesg 2>/dev/null | grep -qi "nobody cared"; then
    log "OSTRZEŻENIE: kernel wyłączył linię IRQ (nobody cared) — dGPU po power-on"
    log "           może stallić na fence'ach; rozważ reboot przed testem dGPU"
  fi
}

# --- argumenty ----------------------------------------------------------------------
resolve_size() { # <WxH|auto> → WxH (echo)
  local val="$1" line
  case "$val" in
    auto)
      line="$(hyprctl monitors -j 2>/dev/null | jq -r '.[] | select(.name=="eDP-1") | "\(.width)x\(.height)"' 2>/dev/null | head -1)"
      [ -n "$line" ] || line="$(hyprctl monitors -j 2>/dev/null | jq -r '.[0] | "\(.width)x\(.height)"' 2>/dev/null)"
      [ -n "$line" ] || line="2880x1800"
      log "rozmiar auto → $line (panel eDP-1)"
      printf '%s\n' "$line"
      return 0
      ;;
    [0-9]*x[0-9]*) printf '%s\n' "$val"; return 0 ;;
    *) echo "zły rozmiar: '$val' — oczekiwane WxH (np. 2880x1800) lub auto" >&2; return 1 ;;
  esac
}
dedup() { # <lista> — usuń duplikaty, zachowaj kolejność
  local out="" w
  for w in $1; do
    case " $out " in *" $w "*) : ;; *) out="$out $w" ;; esac
  done
  printf '%s' "${out# }"
}
parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      -g|--gpu)       [ $# -ge 2 ] || die "brak wartości dla $1"; GPU_RAW="$2"; shift 2 ;;
      --gpu=*)        GPU_RAW="${1#*=}"; shift ;;
      -m|--mode)      [ $# -ge 2 ] || die "brak wartości dla $1"; MODE_RAW="$2"; shift 2 ;;
      --mode=*)       MODE_RAW="${1#*=}"; shift ;;
      -s|--size)      [ $# -ge 2 ] || die "brak wartości dla $1"; SIZE="$2"; shift 2 ;;
      --size=*)       SIZE="${1#*=}"; shift ;;
      -p|--pstate)    [ $# -ge 2 ] || die "brak wartości dla $1"; PSTATE_RAW="$2"; shift 2 ;;
      --pstate=*)     PSTATE_RAW="${1#*=}"; shift ;;
      -f|--frame-end) [ $# -ge 2 ] || die "brak wartości dla $1"; FRAME_END="$2"; shift 2 ;;
      --frame-end=*)  FRAME_END="${1#*=}"; shift ;;
      -F|--fans)      [ $# -ge 2 ] || die "brak wartości dla $1"; FANS="$2"; shift 2 ;;
      --fans=*)       FANS="${1#*=}"; shift ;;
      -o|--outdir)    [ $# -ge 2 ] || die "brak wartości dla $1"; OUTDIR="$2"; shift 2 ;;
      --outdir=*)     OUTDIR="${1#*=}"; shift ;;
      -n|--dry-run)   DRY=1; shift ;;
      -h|--help)      usage; exit 0 ;;
      *) die "nieznany argument: $1 (patrz --help)" ;;
    esac
  done
}
validate_args() {
  local g m p list=""
  GPU_LIST=""
  for g in $GPU_RAW; do
    case "$g" in
      igd|dgpu) list="$list $g" ;;
      both)     list="$list igd dgpu" ;;
      *) die "nieznany GPU: '$g' (igd|dgpu|both)" ;;
    esac
  done
  GPU_LIST="$(dedup "$list")"
  [ -n "$GPU_LIST" ] || die "brak GPU do benchmarku"
  list=""; MODE_LIST=""
  for m in $MODE_RAW; do
    case "$m" in
      offscreen|screen) list="$list $m" ;;
      both)             list="$list offscreen screen" ;;
      *) die "nieznany mode: '$m' (offscreen|screen|both)" ;;
    esac
  done
  MODE_LIST="$(dedup "$list")"
  [ -n "$MODE_LIST" ] || die "brak mode do benchmarku"
  list=""; PSTATE_LIST=""
  for p in $PSTATE_RAW; do
    case "$p" in
      all)      list="$list 07 0a 0e" ;;
      07|0a|0e) list="$list $p" ;;
      *) die "nieznany pstate: '$p' (0e|0a|07|all)" ;;
    esac
  done
  PSTATE_LIST="$(dedup "$list")"
  [ -n "$PSTATE_LIST" ] || die "brak pstate do benchmarku"
  case "$FRAME_END" in none|default) ;; *) die "nieznany frame-end: '$FRAME_END' (none|default)" ;; esac
  case "$FANS" in 100|auto) ;; *) die "nieznane fans: '$FANS' (100|auto)" ;; esac
  SIZE="$(resolve_size "$SIZE")" || die "zły rozmiar (szczegóły wyżej)"
}

# --- main -----------------------------------------------------------------------------
main() {
  parse_args "$@"
  validate_args
  if [ "$DRY" = 1 ]; then
    log "DRY-RUN — plan (komendy NIE będą wykonane):"
    log "  GPU=$GPU_LIST  MODE=$MODE_LIST  SIZE=$SIZE  PSTATE=$PSTATE_LIST"
    log "  FRAME_END=$FRAME_END  FANS=$FANS  OUTDIR=$OUTDIR"
    for gpu in $GPU_LIST; do
      case "$gpu" in igd) bench_igd ;; dgpu) bench_dgpu ;; esac
    done
    exit 0
  fi
  preflight
  mkdir -p "$OUTDIR" || die "nie mogę utworzyć $OUTDIR"
  trap 'exit 130' INT TERM
  trap restore EXIT
  log "start: GPU=$GPU_LIST  MODE=$MODE_LIST  SIZE=$SIZE  PSTATE=$PSTATE_LIST"
  log "       FRAME_END=$FRAME_END  FANS=$FANS  OUTDIR=$OUTDIR"
  for gpu in $GPU_LIST; do
    case "$gpu" in
      igd)  bench_igd ;;
      dgpu) bench_dgpu ;;
    esac
  done
  verify_renderers
  if [ "$ERRORS" = 1 ]; then
    log "zakończono z błędami — patrz wyżej"
    exit 1
  fi
  log "koniec — OK (restore przez trap EXIT)"
}
main "$@"
