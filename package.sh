#!/usr/bin/env bash
# Combine cosmo-smooth's 32-bit EXEs with your original Cosmo's Cosmic Adventure
# episode data (.STN and .VOL files, or original .zip archives) into ready-to-play
# DOSBox Pure / RetroArch ZIP packages in dist/.
#
# Usage:
#   ./package.sh [gamedata_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DATA_DIR="${1:-$ROOT/gamedata}"
BIN_DIR="$ROOT/bin"
DIST_DIR="$ROOT/dist"

if [ ! -d "$DATA_DIR" ]; then
    echo "error: episode data directory not found: $DATA_DIR" >&2
    exit 1
fi

for EP in 1 2 3; do
    if [ ! -f "$BIN_DIR/COSMO${EP}.EXE" ]; then
        echo "error: missing $BIN_DIR/COSMO${EP}.EXE (run ./build.sh first)" >&2
        exit 1
    fi
done

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$DIST_DIR"

# Find an episode asset file (case-insensitive) either unpacked in DATA_DIR
# or inside any .zip archive in DATA_DIR, and copy it into STAGE/UPPERNAME.
fetch_asset() {
    local target="$1"   # e.g. COSMO1.STN
    local found=""

    # 1. Check unpacked files directly inside DATA_DIR (depth 2).
    found="$(find "$DATA_DIR" -maxdepth 2 -type f -iname "$target" | head -n 1 || true)"
    if [ -n "$found" ]; then
        cp "$found" "$STAGE/$target"
        return 0
    fi

    # 2. Check inside .zip archives matching *cosmo* first (fast on large folders),
    #    then fall back to any other .zip in DATA_DIR.
    for pattern in "*[Cc][Oo][Ss][Mm][Oo]*.zip" "*.zip"; do
        while IFS= read -r -d '' archive; do
            local entry
            entry="$(unzip -Z1 "$archive" 2>/dev/null | grep -iE "(^|/)${target}$" | head -n 1 || true)"
            if [ -n "$entry" ]; then
                unzip -p "$archive" "$entry" > "$STAGE/$target"
                return 0
            fi
        done < <(find "$DATA_DIR" -maxdepth 2 -type f -name "$pattern" -print0)
    done

    return 1
}

FOUND_EPS=()
ALL_FILES=()

for EP in 1 2 3; do
    STN="COSMO${EP}.STN"
    VOL="COSMO${EP}.VOL"
    if fetch_asset "$STN" && fetch_asset "$VOL"; then
        cp "$BIN_DIR/COSMO${EP}.EXE" "$STAGE/COSMO${EP}.EXE"
        OUT_ZIP="$DIST_DIR/cosmo-smooth-ep${EP}.zip"
        rm -f "$OUT_ZIP"
        (cd "$STAGE" && zip -q "$OUT_ZIP" "COSMO${EP}.EXE" "$STN" "$VOL")
        echo "Created $OUT_ZIP"
        FOUND_EPS+=("$EP")
        ALL_FILES+=("COSMO${EP}.EXE" "$STN" "$VOL")
    fi
done

if [ "${#FOUND_EPS[@]}" -eq 0 ]; then
    echo "error: no Cosmo episode data found in $DATA_DIR" >&2
    echo "Place COSMO1.STN + COSMO1.VOL (or your original episode .zip files)" >&2
    echo "into gamedata/ and re-run ./package.sh." >&2
    exit 1
fi

if [ "${#FOUND_EPS[@]}" -ge 2 ]; then
    ALL_ZIP="$DIST_DIR/cosmo-smooth-all-episodes.zip"
    rm -f "$ALL_ZIP"
    (cd "$STAGE" && zip -q "$ALL_ZIP" "${ALL_FILES[@]}")
    echo "Created $ALL_ZIP"
fi

echo ""
echo "Packaged ${#FOUND_EPS[@]} episode(s) into $DIST_DIR:"
ls -lh "$DIST_DIR"/cosmo-smooth-*.zip
