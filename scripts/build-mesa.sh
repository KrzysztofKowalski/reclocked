#!/usr/bin/env bash
# build-mesa.sh — instaluje zależności + buduje łatanej Mesę (nouveau/nvc0, Kepler GK107)
# Patch: patches/0002-mesa-nvc0-sched-data.patch (latencje NAK sm30 → SchedDataCalculator).
# NIE instaluje do systemu — build idzie do tmp/mesa/build-nouveau, uruchamiasz przez env.
#
# Użycie:  ./build-mesa.sh
# Rebuild: ./build-mesa.sh   (inkrementalny, pomija setup jeśli build istnieje)
set -euo pipefail

PROJ="$(cd "$(dirname "$0")" && pwd)"
MESA="$PROJ/tmp/mesa"
PATCH="$PROJ/patches/0002-mesa-nvc0-sched-data.patch"
BUILD="$MESA/build-nouveau"

# ----------------------------------------------------------------------------
# 1. Zależności budowania (tylko brakujące przez --needed)
# ----------------------------------------------------------------------------
echo ">>> [1/5] Instalacja zależności (sudo pacman --needed)..."
sudo pacman -S --needed --noconfirm \
    meson python-mako python-ply \
    libdrm wayland-protocols pkgconf \
    flex bison

# ----------------------------------------------------------------------------
# 2. Drzewo źródłowe Mesy
# ----------------------------------------------------------------------------
echo ">>> [2/5] Sprawdzanie tmp/mesa..."
if [ ! -d "$MESA/.git" ]; then
    echo "BŁĄD: $MESA nie istnieje. Najpierw:"
    echo "  git clone https://gitlab.freedesktop.org/mesa/mesa.git $MESA"
    exit 1
fi
cd "$MESA"

# ----------------------------------------------------------------------------
# 3. Patch (idempotentny — jeśli już nałożony, pomija)
# ----------------------------------------------------------------------------
echo ">>> [3/5] Patch NAK→SchedDataCalculator..."
if git diff --quiet -- src/gallium/drivers/nouveau/codegen/nv50_ir_emit_nvc0.cpp \
                    src/gallium/drivers/nouveau/codegen/nv50_ir_target_nvc0.cpp; then
    # drzewo czyste na plikach docelowych → nałóż patch
    if git apply --check "$PATCH" 2>/dev/null; then
        git apply "$PATCH"
        echo "    nałożony."
    else
        echo "BŁĄD: git apply --check nie przeszedł — patch nie pasuje do tego drzewa."
        echo "       Odśwież patch (rebase) lub zresetuj drzewo: (cd $MESA && git checkout -- .)"
        exit 1
    fi
else
    echo "    już nałożony (pliki docelowe zmodyfikowane w drzewie)."
fi

# ----------------------------------------------------------------------------
# 4. Konfiguracja meson — TYLKO nouveau, minimalne, bez vulkan/opencl/rust/llvm
#    (patch dotyczy klasycznego drivera gallium nvc0 w C++, nie NAK/rusticl)
# ----------------------------------------------------------------------------
# Ważny build dir = ma meson-private/coredata.dat. Jeśli nie (np. setup padł
# przy poprzednim uruchomieniu) — wipe i świeży setup.
if [ -d "$BUILD" ] && [ -f "$BUILD/meson-private/coredata.dat" ]; then
    echo ">>> [4/5] build dir OK — pomijam setup (inkrementalnie)."
else
    echo ">>> [4/5] meson setup (nouveau-only)..."
    rm -rf "$BUILD"
    # Opcje zweryfikowane pod mesa 26.x (meson.options):
    #   gallium-opencl / gallium-vdpau NIE ISTNIEJĄ (clover usunięte) — nie używaj.
    #   gallium-rusticl = boolean (default false), gallium-va = feature, llvm = feature.
    meson setup "$BUILD" \
        -Dgallium-drivers=nouveau \
        -Dvulkan-drivers= \
        -Dgallium-rusticl=false \
        -Dgallium-va=disabled \
        -Dgles1=disabled \
        -Dglx=dri \
        -Dplatforms=wayland,x11 \
        -Dllvm=disabled
    # UWAGA: jeśli meson wymaga llvm, zainstaluj: sudo pacman -S llvm
    # i usuń -Dllvm=disabled powyżej, potem: rm -rf "$BUILD" && ./build-mesa.sh
fi

# ----------------------------------------------------------------------------
# 5. Build
# ----------------------------------------------------------------------------
echo ">>> [5/5] meson compile (to chwilę potrwa)..."
meson compile -C "$BUILD"

# ----------------------------------------------------------------------------
# 6. Jak używać (bez nadpisywania systemowej Mesy)
# ----------------------------------------------------------------------------
DRI="$(find "$BUILD/src/gallium" -name 'nouveau_dri.so' 2>/dev/null | head -1)"
[ -z "$DRI" ] && DRI="$(find "$BUILD" -name 'libgallium_dri.so' 2>/dev/null | head -1)"
DRIDIR="$(dirname "$DRI")"

cat <<EOF

=== BUILD OK ===
Łatana Mesa (nouveau/nvc0) zbudowana w:
  $BUILD

Uruchom aplikacje z nią (bez nadpisywania systemowej Mesy):
  export LIBGL_DRIVERS_PATH="$DRIDIR"

Test (renderer powinien pokazać NVE7 / nouveau, nie Intel):
  DRI_PRIME=0 glxinfo | grep -i renderer
  DRI_PRIME=1 glxinfo | grep -i renderer

Aplikacja:
  LIBGL_DRIVERS_PATH="$DRIDIR" DRI_PRIME=1 firefox

Porównanie shader-bound przed/po patchu (ten sam scene):
  # bez patcha (systemowa Mesa):
  DRI_PRIME=1 glxgears -geometry 800x600   # notuj FPS
  # z patchem:
  LIBGL_DRIVERS_PATH="$DRIDIR" DRI_PRIME=1 glxgears -geometry 800x600

Aby zainstalować do systemu (NIE zalecane bez backupu — bramka usera):
  sudo meson install -C "$BUILD"
EOF