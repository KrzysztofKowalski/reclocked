#!/usr/bin/env bash
# mesa-manage.sh — chirurgiczna instalacja/cofanie łatej Mesy (nouveau/nvc0, Kepler GK107)
#
# Strategia (Mesa 26.x, architektura "dril"):
#   /usr/lib/dri/*_dri.so  ->  libdril_dri.so   (wspólny dispatcher ~96 KB)
#   /usr/lib/dri/libdril_dri.so  dlopenuje  /usr/lib/libgallium-26.1.8-arch1.1.so  (~54 MB)
#   /usr/lib/dri/nouveau_dri.so  = SYMLINK  -> libdril_dri.so
#
# Łatana Mesa (nouveau-only) buduje self-contained libdril_dri.so (~2.8 MB) ze
# statycznie zlinkowanym nouveau. Kopiujemy je jako REALNY plik
# /usr/lib/dri/nouveau_dri_patched.so i przestawiamy symlink
# nouveau_dri.so -> nouveau_dri_patched.so. Pozostałe drivery (iris, swrast, ...)
# zostają na systemowym libdril_dri.so — nietknięte.
#
# Rollback = przywrócenie symlinku nouveau_dri.so -> libdril_dri.so + usunięcie
# nouveau_dri_patched.so. Ostatecznie: `sudo pacman -S mesa` (reinstalacja).
#
# Komendy: status | backup | install | restore | diff
# Flagi:   --yes (bez potwierdzeń), --force (nadpisz backup), --dry-run
#
# NIE używa systemowego /tmp — scratch w /home/k/Projects/nv-kepler/tmp/.
set -euo pipefail

# PROJ = ROOT repo (skrypt w scripts/ — dirname $0 = scripts, stąd /..)
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
MESA="$PROJ/tmp/mesa"
BUILD="$MESA/build-nouveau"
DRIL_DIR="$BUILD/src/gallium/targets/dril"      # tu leży zbudowany nouveau_dri.so -> libdril_dri.so
BUILT_DRIL="$DRIL_DIR/libdril_dri.so"           # self-contained nouveau driver (realny plik)
PATCH="$PROJ/patches/0002-mesa-nvc0-sched-data.patch"
SCRATCH="$PROJ/tmp/mesa-manage-scratch"

DRI_DIR="/usr/lib/dri"
SYS_NOUVEAU="$DRI_DIR/nouveau_dri.so"           # symlink (system)
SYS_LIBDRIL="$DRI_DIR/libdril_dri.so"           # dispatcher (system)
# Auto-detect system libgallium version (e.g. libgallium-26.1.8-arch1.1.so).
# Override with: SYS_LIBGALLIUM=/path/to/libgallium-*.so mesa-manage.sh ...
if [ -z "${SYS_LIBGALLIUM:-}" ]; then
    SYS_LIBGALLIUM="$(ls /usr/lib/libgallium-*.so 2>/dev/null | head -1)"
    [ -z "$SYS_LIBGALLIUM" ] && SYS_LIBGALLIUM="/usr/lib/libgallium.so"
fi
PATCHED_NAME="nouveau_dri_patched.so"           # nazwa pliku łatanego drivera w /usr/lib/dri/
PATCHED_PATH="$DRI_DIR/$PATCHED_NAME"

# --- kolory -----------------------------------------------------------------
if [ -t 1 ]; then
    C_B='\033[1m'; C_R='\033[31m'; C_G='\033[32m'; C_Y='\033[33m'; C_C='\033[36m'; C_D='\033[2m'; C_0='\033[0m'
else
    C_B=''; C_R=''; C_G=''; C_Y=''; C_C=''; C_D=''; C_0=''
fi

# --- flagi -------------------------------------------------------------------
YES=0; FORCE=0; DRY=0
for arg in "$@"; do
    case "$arg" in
        --yes|-y) YES=1 ;;
        --force)  FORCE=1 ;;
        --dry-run) DRY=1 ;;
    esac
done
# usuń flagi z $@ by zostawić komendę
ARGS=()
for a in "$@"; do case "$a" in --yes|-y|--force|--dry-run) ;; *) ARGS+=("$a") ;; esac; done
set -- "${ARGS[@]}"
CMD="${1:-}"

# --- helpers -----------------------------------------------------------------
say()  { printf "%b\n" "$*"; }
ok()   { say "${C_G}✔ $*${C_0}"; }
err()  { say "${C_R}✖ $*${C_0}" >&2; }
warn() { say "${C_Y}⚠ $*${C_0}" >&2; }
hdr()  { say "${C_C}${C_B}=== $* ===${C_0}"; }
die()  { err "$*"; exit 1; }

confirm() {
    [ "$YES" = 1 ] && return 0
    local q="$1"
    printf "%b" "${C_Y}${q} [y/N] ${C_0}" >&2
    read -r ans
    [ "$ans" = "y" ] || [ "$ans" = "Y" ]
}

sha() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }

# Najnowszy katalog backupu (jeśli istnieje)
latest_backup() {
    # zwraca najnowszy katalog backupu lub pusty string; NIGDY nie exituje
    local d
    d="$(ls -1d "$PROJ/tmp/mesa-backup-"* 2>/dev/null | sort | tail -1 || true)"
    echo "$d"
}

# Stan systemowego symlinku nouveau_dri.so
# drukuje: "symlink -> <cel>" albo "file <rozmiar>" albo "missing"
describe_nouveau_dri() {
    if [ -L "$SYS_NOUVEAU" ]; then
        printf "symlink -> %s" "$(readlink "$SYS_NOUVEAU")"
    elif [ -e "$SYS_NOUVEAU" ]; then
        printf "file %s bytes" "$(stat -c '%s' "$SYS_NOUVEAU")"
    else
        printf "missing"
    fi
}

# Czy łatany driver jest aktualnie aktywny?
is_patched_active() {
    [ -L "$SYS_NOUVEAU" ] && [ "$(readlink "$SYS_NOUVEAU")" = "$PATCHED_NAME" ] && [ -f "$PATCHED_PATH" ]
}

# Wersja paczki mesa
mesa_ver() { pacman -Qi mesa 2>/dev/null | awk -F': ' '/^Version/{print $2; exit}'; }

# Sprawdzenie zależności
check_built() {
    [ -f "$BUILT_DRIL" ] || die "Nie znaleziono zbudowanego drivera: $BUILT_DRIL
   Najpierw uruchom: ./build-mesa.sh"
}

# ============================================================================
# status
# ============================================================================
do_status() {
    hdr "Pakiet mesa (pacman)"
    echo "  Wersja:           $(mesa_ver)"
    echo "  Plik paczki:      $(pacman -Qo "$SYS_LIBDRIL" 2>/dev/null | sed 's/^ *//')"
    echo
    hdr "Systemowy nouveau DRI"
    echo "  Ścieżka:          $SYS_NOUVEAU"
    echo "  Typ:              $(describe_nouveau_dri)"
    if [ -L "$SYS_NOUVEAU" ]; then
        local tgt; tgt="$(readlink "$SYS_NOUVEAU")"
        local abs="$DRI_DIR/$tgt"
        [ -f "$abs" ] || abs="$tgt"
        echo "  Cel (realny):     $abs"
        echo "  sha256 celu:      $(sha "$abs" 2>/dev/null || echo '(brak)')"
    fi
    echo "  libgallium:       $SYS_LIBGALLIUM"
    echo "  sha256 libgall.:  $(sha "$SYS_LIBGALLIUM" 2>/dev/null || echo '(brak)')"
    echo
    hdr "Zbudowany driver (łataney)"
    if [ -f "$BUILT_DRIL" ]; then
        echo "  Ścieżka:          $BUILT_DRIL"
        echo "  Rozmiar:          $(stat -c '%s' "$BUILT_DRIL") bytes"
        echo "  sha256:           $(sha "$BUILT_DRIL")"
        echo "  SONAME:           $(readelf -d "$BUILT_DRIL" 2>/dev/null | awk '/SONAME/{print $NF}' | tr -d '[]')"
    else
        warn "  Brak zbudowanego drivera ($BUILT_DRIL)"
        echo "  → uruchom: ./build-mesa.sh"
    fi
    echo
    hdr "Aktualnie aktywny"
    if is_patched_active; then
        ok "PATCHED (nouveau_dri.so -> $PATCHED_NAME)"
        echo "  sha256 aktywny:   $(sha "$PATCHED_PATH")"
    else
        say "${C_G}STOCK (oryginalny systemowy)${C_0}"
    fi
    echo
    hdr "Backup"
    local bk; bk="$(latest_backup)"
    if [ -n "$bk" ]; then
        echo "  Najnowszy:        $bk"
        echo "  Utworzony:        $(stat -c '%y' "$bk" 2>/dev/null | cut -d'.' -f1)"
        if [ -f "$bk/manifest.txt" ]; then
            echo "  Manifest:         $bk/manifest.txt"
        fi
    else
        warn "  Brak backupu. Uruchom: $0 backup"
    fi
    echo
    hdr "Patch (git diff --stat w tmp/mesa)"
    if git -C "$MESA" diff --quiet 2>/dev/null; then
        warn "  Drzewo czyste — patch NIE nałożony?"
    else
        git -C "$MESA" diff --stat 2>/dev/null | sed 's/^/  /'
    fi
}

# ============================================================================
# backup
# ============================================================================
do_backup() {
    check_built
    local ts; ts="$(date +%Y%m%d-%H%M%S)"
    local bk="$PROJ/tmp/mesa-backup-$ts"

    # idempotentncja: jeśli jakikolwiek backup istnieje i nie ma --force
    local existing; existing="$(latest_backup)"
    if [ -n "$existing" ] && [ "$FORCE" != 1 ]; then
        warn "Backup już istnieje: $existing"
        warn "Użyj --force by utworzyć nowy (stary pozostanie)."
        if confirm "Utworzyć nowy backup obok istniejącego?"; then
            : # idziemy dalej
        else
            say "Pominięto."
            return 0
        fi
    fi

    mkdir -p "$bk" "$SCRATCH"

    hdr "Backup stanu systemowego → $bk"

    # 1. Metadane symlinku nouveau_dri.so (cel + typ)
    local nv_type="missing" nv_target=""
    if [ -L "$SYS_NOUVEAU" ]; then
        nv_type="symlink"
        nv_target="$(readlink "$SYS_NOUVEAU")"
    elif [ -e "$SYS_NOUVEAU" ]; then
        nv_type="file"
    fi
    printf '%s\n' "$nv_target" > "$bk/nouveau_dri.linktarget"

    # 2. Jeśli symlink → zapisz też skąd skopiować oryginalny cel (libdril_dri.so)
    if [ "$nv_type" = "symlink" ] && [ -n "$nv_target" ]; then
        local abs_tgt="$DRI_DIR/$nv_target"
        [ -f "$abs_tgt" ] || abs_tgt="$nv_target"
        if [ -f "$abs_tgt" ]; then
            cp -a "$abs_tgt" "$bk/$(basename "$abs_tgt")"
            echo "  Zbackupowano cel symlinku: $(basename "$abs_tgt")"
        fi
    elif [ "$nv_type" = "file" ]; then
        cp -a "$SYS_NOUVEAU" "$bk/nouveau_dri.so"
        echo "  Zbackupowano plik: nouveau_dri.so"
    fi

    # 3. sha256 libgallium (dla pełnego obrazu — nie przywracamy go, ale weryfikujemy)
    if [ -f "$SYS_LIBGALLIUM" ]; then
        sha "$SYS_LIBGALLIUM" > "$bk/libgallium.sha256"
    fi

    # 4. Pełna lista plików paczki mesa
    pacman -Ql mesa > "$bk/mesa-files.txt" 2>/dev/null || true

    # 5. Manifest (czytelny dla człowieka)
    {
        echo "# mesa-manage.sh backup — $ts"
        echo "backup_created=$ts"
        echo "mesa_version=$(mesa_ver)"
        echo "nouveau_dri_path=$SYS_NOUVEAU"
        echo "nouveau_dri_type=$nv_type"
        echo "nouveau_dri_target=$nv_target"
        echo "sys_libdril=$SYS_LIBDRIL"
        echo "sys_libdril_sha256=$(sha "$SYS_LIBDRIL" 2>/dev/null)"
        echo "sys_libgallium=$SYS_LIBGALLIUM"
        echo "sys_libgallium_sha256=$(sha "$SYS_LIBGALLIUM" 2>/dev/null)"
        echo "built_dril=$BUILT_DRIL"
        echo "built_dril_sha256=$(sha "$BUILT_DRIL" 2>/dev/null)"
        echo "patched_install_path=$PATCHED_PATH"
        echo "patched_install_name=$PATCHED_NAME"
        echo
        echo "# Rollback:"
        echo "#   $0 restore"
        echo "# Ostatecznie (reinstalacja paczki):"
        echo "#   sudo pacman -S mesa"
    } > "$bk/manifest.txt"

    ok "Backup utworzony: $bk"
    echo "  Manifest: $bk/manifest.txt"
    say "  Aby zainstalować łaty: ${C_C}$0 install${C_0}"
}

# ============================================================================
# install
# ============================================================================
do_install() {
    check_built
    local bk; bk="$(latest_backup)"
    [ -n "$bk" ] || die "Brak backupu. Najpierw: $0 backup"

    if is_patched_active; then
        warn "Łatany driver już jest aktywny."
        if ! confirm "Kontynuować (przekopiować plik ponownie)?"; then
            say "Pominięto."; return 0
        fi
    fi

    local built_sha; built_sha="$(sha "$BUILT_DRIL")"

    hdr "Plan instalacji (dry-run)"
    echo "  1. Kopiuj:  $BUILT_DRIL"
    echo "              → $PATCHED_PATH   (jako realny plik: $PATCHED_NAME)"
    echo "  2. Przestaw symlink:"
    echo "              $SYS_NOUVEAU  →  $PATCHED_NAME"
    echo "              (obecnie: $(describe_nouveau_dri))"
    echo "  3. Pozostałe *_dri.so nietknięte (wciąż → systemowy libdril_dri.so)."
    echo "  Backup:     $bk"
    echo "  sha256 zbudowanego: $built_sha"
    echo

    if [ "$DRY" = 1 ]; then say "${C_Y}--dry-run: nic nie zmieniono.${C_0}"; return 0; fi

    confirm "Wykonać instalację?" || die "Anulowano."

    hdr "Instalacja"
    echo "  [1/3] Kopiowanie zbudowanego drivera..."
    sudo install -m 0755 "$BUILT_DRIL" "$PATCHED_PATH"
    local inst_sha; inst_sha="$(sha "$PATCHED_PATH")"
    [ "$inst_sha" = "$built_sha" ] || die "sha256 po kopii ≠ zbudowany. NIE kontynuuj."
    ok "  Skopiowano. sha256 OK."

    echo "  [2/3] Przestawianie symlinku nouveau_dri.so..."
    sudo ln -sfn "$PATCHED_NAME" "$SYS_NOUVEAU"
    ok "  Symlink: $(readlink "$SYS_NOUVEAU")"

    echo "  [3/3] Weryfikacja..."
    is_patched_active && ok "PATCHED aktywne" || die "Symlink nie wskazuje na patched — sprawdź ręcznie."

    echo
    hdr "Test renderera (opcjonalny)"
    if command -v glxinfo >/dev/null 2>&1; then
        say "  Sprawdź renderer (nowa sesja/apka, by załadował nowy .so):"
        say "  ${C_C}LIBGL_DRIVERS_PATH=$DRI_DIR DRI_PRIME=0 glxinfo | grep -i renderer${C_0}"
        say "  ${C_C}LIBGL_DRIVERS_PATH=$DRI_DIR DRI_PRIME=1 glxinfo | grep -i renderer${C_0}"
        say "  Renderer NVE7 / nouveau = OK. (DRI_PRIME=0 = dGPU nouveau na tej maszynie.)"
    else
        warn "  glxinfo niedostępne — zainstaluj: sudo pacman -S mesa-utils"
    fi

    echo
    hdr "Cofnięcie"
    say "  ${C_C}$0 restore${C_0}   — przywraca stock symlink + usuwa patched plik"
    say "  ${C_C}sudo pacman -S mesa${C_0}  — ostateczna reinstalacja (gwarancja stocka)"
}

# ============================================================================
# restore
# ============================================================================
do_restore() {
    local bk; bk="$(latest_backup)"
    [ -n "$bk" ] || die "Brak backupu. Alternatywa: sudo pacman -S mesa (reinstalacja paczki)."

    [ -f "$bk/manifest.txt" ] || die "Manifest brakuje w $bk — uszkodzony backup."

    # Wczytaj metadane z manifestu
    local orig_target; orig_target="$(awk -F= '/^nouveau_dri_target=/{print $2}' "$bk/manifest.txt")"
    local orig_type;   orig_type="$(awk -F= '/^nouveau_dri_type=/{print $2}'   "$bk/manifest.txt")"
    local sys_libdril_sha; sys_libdril_sha="$(awk -F= '/^sys_libdril_sha256=/{print $2}' "$bk/manifest.txt")"

    hdr "Plan przywrócenia (dry-run)"
    echo "  Backup:      $bk"
    echo "  Typ oryginału: $orig_type"
    if [ "$orig_type" = "symlink" ]; then
        echo "  Cel do przywrócenia: nouveau_dri.so -> $orig_target"
    else
        echo "  Plik do przywrócenia: nouveau_dri.so (z backupu)"
    fi
    echo "  Do usunięcia: $PATCHED_PATH   (jeśli istnieje)"
    echo "  Obecnie:     $(describe_nouveau_dri)"
    echo

    if [ "$DRY" = 1 ]; then say "${C_Y}--dry-run: nic nie zmieniono.${C_0}"; return 0; fi

    confirm "Wykonać przywrócenie?" || die "Anulowano."

    hdr "Przywracanie"

    # 1. Najpierw przywróć symlink/plik nouveau_dri.so
    if [ "$orig_type" = "symlink" ] && [ -n "$orig_target" ]; then
        sudo ln -sfn "$orig_target" "$SYS_NOUVEAU"
        ok "  Symlink przywrócony: nouveau_dri.so -> $(readlink "$SYS_NOUVEAU")"
    elif [ "$orig_type" = "file" ] && [ -f "$bk/nouveau_dri.so" ]; then
        sudo cp -a "$bk/nouveau_dri.so" "$SYS_NOUVEAU"
        ok "  Plik przywrócony z backupu."
    else
        die "Nie wiem jak przywrócić (typ=$orig_type). Sprawdź $bk/manifest.txt"
    fi

    # 2. Usuń plik łatanego drivera
    if [ -f "$PATCHED_PATH" ] || [ -L "$PATCHED_PATH" ]; then
        sudo rm -f "$PATCHED_PATH"
        ok "  Usunięto: $PATCHED_PATH"
    else
        say "  (patched plik nie istniał — pominięto usuwanie)"
    fi

    # 3. Weryfikacja: sha256 systemowego libdril_dri.so == backup
    echo "  [weryfikacja] sha256 system libdril_dri.so vs backup..."
    local cur_sha; cur_sha="$(sha "$SYS_LIBDRIL" 2>/dev/null || echo MISSING)"
    if [ -n "$sys_libdril_sha" ] && [ "$cur_sha" = "$sys_libdril_sha" ]; then
        ok "  sha256 libdril_dri.so zgodny z backupem."
    else
        warn "  sha256 libdril_dri.so RÓŻNI się od backupu ($cur_sha vs $sys_libdril_sha)."
        warn "  Może systemowy mesa został zaktualizowany w międzyczasie."
        warn "  Jeśli nowa wersja paczki — to OK (stock). Jeśli nie — rozważ reinstall."
    fi

    if is_patched_active; then
        err "  Nadal patched aktywne! Ręczna interwencja wymagana."
        die "  Ostatecznie: sudo pacman -S mesa"
    fi
    ok "  Stan: STOCK."

    echo
    hdr "Ostateczne przywrócenie (opcjonalne)"
    say "  Jeśli cokolwiek wygląda źle — reinstalacja paczki = gwarancja stocka:"
    say "  ${C_C}sudo pacman -S mesa${C_0}"
}

# ============================================================================
# diff
# ============================================================================
do_diff() {
    check_built
    hdr "sha256 — porównanie"
    printf "  %-22s %s\n" "zbudowany libdril:" "$(sha "$BUILT_DRIL")"
    printf "  %-22s %s\n" "system libdril:"    "$(sha "$SYS_LIBDRIL" 2>/dev/null || echo MISSING)"
    if [ -f "$PATCHED_PATH" ]; then
        printf "  %-22s %s\n" "zainstalowany patched:" "$(sha "$PATCHED_PATH")"
    else
        printf "  %-22s %s\n" "zainstalowany patched:" "(nie zainstalowano)"
    fi
    local bk; bk="$(latest_backup)"
    if [ -n "$bk" ] && [ -f "$bk/libgallium.sha256" ]; then
        printf "  %-22s %s\n" "backup libgallium:" "$(cat "$bk/libgallium.sha256")"
    fi
    echo
    hdr "Patch — zmodyfikowane pliki źródłowe (git diff --stat w tmp/mesa)"
    if git -C "$MESA" diff --quiet 2>/dev/null; then
        warn "  Drzewo czyste (patch nie nałożony na źródła?)."
    else
        git -C "$MESA" diff --stat 2>/dev/null | sed 's/^/  /'
        echo
        say "  Pełny diff: ${C_C}git -C $MESA diff${C_0}"
        say "  Patch:      $PATCH"
    fi
}

# ============================================================================
# usage
# ============================================================================
usage() {
    cat <<EOF
${C_B}mesa-manage.sh${C_0} — instalacja/cofanie łatej Mesy (nouveau/nvc0, Kepler GK107)

${C_B}Komendy:${C_0}
  status    Pokaż stan: pacman, systemowy nouveau driver, backup, patched/stock, diff źródeł
  backup    Zbackupuj stan systemowy do tmp/mesa-backup-<data>/ (wymagane przed install)
  install   Zainstaluj łaty driver (wymaga istniejącego backupu)
  restore   Przywróć stan z backupu (symlink + usuń patched plik)
  diff      sha256 zbudowany vs system vs backup + git diff --stat źródeł

${C_B}Flagi:${C_0}
  --yes       Bez potwierdzeń (nie pytaj przed install/restore)
  --force     Nowy backup obok istniejącego (backup normalnie ostrzega)
  --dry-run   Pokaż plan bez modyfikacji (install/restore)

${C_B}Przepływ (bramki usera):${C_0}
  1. ./build-mesa.sh                 # zbuduj łataney driver (już zrobione)
  2. ./mesa-manage.sh status         # zweryfikuj ścieżki i sha
  3. ./mesa-manage.sh backup         # zrób backup systemu (bezpieczne, nie modyfikuje systemu)
  4. ./mesa-manage.sh install        # instaluj (pyta o potwierdzenie)
  5. test: DRI_PRIME=0 glxinfo | grep -i renderer
  6. ./mesa-manage.sh restore        # cofnij gdy coś nie tak
  7. (ostatecznie) sudo pacman -S mesa

${C_B}Kluczowe ścieżki:${C_0}
  System:  $SYS_NOUVEAU  ->  $(readlink "$SYS_NOUVEAU" 2>/dev/null || echo '?')
           $SYS_LIBDRIL  (dispatcher, dlopenuje libgallium)
  Zbudowany: $BUILT_DRIL
  Patched instalowany jako: $PATCHED_PATH
EOF
}

# ============================================================================
# dispatch
# ============================================================================
case "$CMD" in
    status)  do_status ;;
    backup)  do_backup ;;
    install) do_install ;;
    restore) do_restore ;;
    diff)    do_diff ;;
    ""|-h|--help|help) usage ;;
    *) die "Nieznana komenda: $CMD
$(usage)" ;;
esac