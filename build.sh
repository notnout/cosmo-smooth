#!/usr/bin/env bash
# Build self-contained 32-bit DOS executables (COSMO1.EXE, COSMO2.EXE, COSMO3.EXE)
# using Open Watcom v2 + CauseWay DOS extender.
#
# Usage:
#   WATCOM=/path/to/watcom ./build.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
OUT="$ROOT/bin"
mkdir -p "$BUILD" "$OUT"

# Locate Open Watcom v2 installation.
if [ -z "${WATCOM:-}" ]; then
    for candidate in /opt/watcom "$HOME/watcom" "$ROOT/../toolchain/watcom"; do
        if [ -d "$candidate/h" ]; then
            WATCOM="$candidate"
            break
        fi
    done
fi

if [ -z "${WATCOM:-}" ] || [ ! -d "$WATCOM/h" ]; then
    echo "error: Open Watcom v2 not found." >&2
    echo "Set WATCOM=/path/to/watcom (download ow-snapshot.tar.xz from" >&2
    echo "https://github.com/open-watcom/open-watcom-v2/releases/tag/Current-build)." >&2
    exit 1
fi

# Pick the native Open Watcom host binary directory.
OS="$(uname -s)"
ARCH="$(uname -m)"
HOST_BIN=""
case "$OS:$ARCH" in
    Darwin:arm64)          HOST_BIN="$WATCOM/armo64" ;;
    Darwin:x86_64)         HOST_BIN="$WATCOM/bino64" ;;
    Linux:x86_64)          HOST_BIN="$WATCOM/binl64" ;;
    Linux:aarch64|Linux:arm64) HOST_BIN="$WATCOM/arml64" ;;
    Linux:i*86)            HOST_BIN="$WATCOM/binl" ;;
    MINGW*:x86_64|MSYS*:x86_64|CYGWIN*:x86_64) HOST_BIN="$WATCOM/binnt64" ;;
    *)                     HOST_BIN="$WATCOM/binl64" ;;
esac

if [ ! -x "$HOST_BIN/wcc386" ]; then
    echo "error: wcc386 not found in $HOST_BIN" >&2
    exit 1
fi

export WATCOM
export PATH="$HOST_BIN:$WATCOM/binw:$PATH"
export INCLUDE="$WATCOM/h"
export EDPATH="$WATCOM/eddat"

# Open Watcom 32-bit flat protected-mode DOS flags:
# -bt=dos : target MS-DOS
# -mf     : flat 32-bit memory model
# -5r     : Pentium register calling convention
# -ox     : full optimization
# -za99   : C99 mode (stdint.h, stdbool.h)
COMMON_FLAGS=(
    -bt=dos -mf -5r -ox -za99 -zq
    -i="$ROOT/include"
    -i="$ROOT/src/game"
    -i="$WATCOM/h"
)

echo "=== Compiling 32-bit DOS platform layer ($OS/$ARCH -> DOS32) ==="
wcc386 "${COMMON_FLAGS[@]}" -fo="$BUILD/dos_hw.obj"       "$ROOT/src/platform/dos_hw.c"
wcc386 "${COMMON_FLAGS[@]}" -fo="$BUILD/ega.obj"          "$ROOT/src/platform/ega.c"
wcc386 "${COMMON_FLAGS[@]}" -fo="$BUILD/dos_video.obj"    "$ROOT/src/platform/dos_video.c"
wcc386 "${COMMON_FLAGS[@]}" -fo="$BUILD/dos_platform.obj" "$ROOT/src/platform/dos_platform.c"

for EP in 1 2 3; do
    echo "=== Building COSMO${EP}.EXE (Episode ${EP}) ==="
    wcc386 "${COMMON_FLAGS[@]}" -w0 -dEPISODE="${EP}" \
        -fi="$ROOT/include/cosmo/dos_compat.h" \
        -fo="$BUILD/game1_ep${EP}.obj" "$ROOT/src/game/game1.c"

    wcc386 "${COMMON_FLAGS[@]}" -w0 -dEPISODE="${EP}" \
        -fi="$ROOT/include/cosmo/dos_compat.h" \
        -fo="$BUILD/game2_ep${EP}.obj" "$ROOT/src/game/game2.c"

    wcc386 "${COMMON_FLAGS[@]}" -w0 -dEPISODE="${EP}" \
        -fi="$ROOT/include/cosmo/dos_compat.h" \
        -fo="$BUILD/lowlevel_ep${EP}.obj" "$ROOT/src/platform/lowlevel_ega.c"

    wlink system causeway \
        option quiet \
        option stack=262144 \
        name "$OUT/COSMO${EP}.EXE" \
        file "$BUILD/dos_hw.obj" \
        file "$BUILD/ega.obj" \
        file "$BUILD/dos_video.obj" \
        file "$BUILD/dos_platform.obj" \
        file "$BUILD/lowlevel_ep${EP}.obj" \
        file "$BUILD/game1_ep${EP}.obj" \
        file "$BUILD/game2_ep${EP}.obj"
done

echo "=== Built cosmo-smooth 32-bit DOS executables ==="
ls -lh "$OUT"/COSMO*.EXE
