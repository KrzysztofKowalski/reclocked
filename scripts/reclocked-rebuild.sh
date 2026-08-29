#!/usr/bin/env bash
# Rekompilacja reclocked (wymóg: ZERO warningów) + restart daemona systemd + status daemona.
# Użycie: reclocked-rebuild.sh [--reload]
#   (bez arg.)  rebuild + restart + status;   --reload  SIGHUP bez rebuilda
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="reclocked"

# Buduje $1; przy błędzie kompilacji lub jakimkolwiek "warning:" w output → exit 1 bez restartu.
build_one() {
    local dir="$1" out
    echo "== build $dir =="
    if out="$(make -C "$dir" 2>&1)"; then
        echo "$out"
    else
        echo "== BŁĄD: build $dir nieudany — bez restartu ==" >&2
        echo "$out" >&2
        exit 1
    fi
    if grep -q "warning:" <<<"$out"; then
        echo "== BŁĄD: warningi w buildzie $dir (wymóg projektu: ZERO warningów) — bez restartu ==" >&2
        echo "$out" | grep "warning:" >&2
        exit 1
    fi
}

restart_daemon() {
    echo "== restart reclocked =="
    sudo systemctl restart reclocked
    sleep 3
    if ! systemctl is-active --quiet reclocked; then
        echo "== BŁĄD: reclocked nie jest active po restarcie ==" >&2
        systemctl status reclocked --no-pager >&2 || true
        exit 1
    fi
    echo "== log startu =="
    sudo journalctl -u reclocked --since "10 sec ago" --no-pager | grep -E "start v|BŁĄD|error" | tail -5 || true
    echo "== status =="
    sudo cat /run/reclocked/status || true
}

case "${1:-}" in
    "")
        build_one "$BUILD_DIR"
        restart_daemon
        ;;
    --reload)
        echo "== reload reclocked (SIGHUP, bez rebuilda) =="
        sudo systemctl kill -s HUP reclocked
        sleep 1
        sudo journalctl -u reclocked --since "5 sec ago" --no-pager | grep -E "SIGHUP|config" | tail -3 || true
        ;;
    *)
        echo "Użycie: $0 [--reload]" >&2
        exit 1
        ;;
esac

echo "== gotowe =="
exit 0
