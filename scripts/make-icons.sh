#!/usr/bin/env bash
#
# SDD-016: derive every CpapDash icon from the one master asset.
#
#   frontend/public/logo.png (512x512 RGBA)
#       -> desktop/qt/resources/*.png     tray / menu bar, four states
#       -> desktop/qt/resources/cpapdash.ico    Windows exe + installer
#       -> desktop/qt/resources/cpapdash.icns   macOS bundle
#
# The OUTPUTS ARE COMMITTED. This script is how they are reproduced, not a build
# step: ImageMagick is not on the GitHub Windows runner, Inno reads SetupIconFile
# out of the source tree before CMake has run at all, and a signed release should
# not depend on which rasteriser version a runner happened to ship.
#
# Requires: ImageMagick 7 (`brew install imagemagick`). Run from the repo root.
set -euo pipefail

SRC="frontend/public/logo.png"
OUT="desktop/qt/resources"

[ -f "$SRC" ] || { echo "missing $SRC (run from the repo root)" >&2; exit 1; }
command -v magick >/dev/null || { echo "ImageMagick 7 not found: brew install imagemagick" >&2; exit 1; }

mkdir -p "$OUT"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
TMPDIR_ICONS="$TMP"

# ── The small-size problem ──────────────────────────────────────────────────
#
# The master is a rounded square holding a moon-and-star mark ABOVE the words
# "CPAP Dash". At 16-22px -- which is exactly the size a menu bar or a system
# tray uses -- that text is three grey smudges, and downsampling the whole
# square spends most of the pixel budget on it.
#
# So small sizes get the EMBLEM ONLY: crop the mark out, pad back to square, and
# let the moon fill the frame. The full lockup is kept for sizes big enough to
# read it (the app icon, the installer, Explorer's large views).
EMBLEM_CROP="330x240+95+95"

emblem() {   # emblem <size> <output>
    magick "$SRC" -crop "$EMBLEM_CROP" +repage \
        -background none -gravity center -extent 340x340 \
        -filter Lanczos -resize "${1}x${1}" \
        -strip "$2"
}

full() {     # full <size> <output>
    magick "$SRC" -filter Lanczos -resize "${1}x${1}" -strip "$2"
}

# ── Tray / menu bar, four states, in TWO families ───────────────────────────
#
# The icon carries status, so a glance answers "is it running?" without opening
# the menu. @2x variants exist because Qt picks per-DPI from the sizes it is
# given, and one bitmap scaled by the compositor looks soft on every Retina
# display and most modern Windows laptops.
#
# Windows and Linux get COLOUR, which is what their tray areas expect.
#
# macOS gets a TEMPLATE image: a black silhouette with alpha, which the system
# tints itself -- black on a light menu bar, white on a dark one, and correct
# again when the user switches appearance without restarting. Shipping the
# colour icon there produces a small navy tile that ignores the system theme and
# looks like a foreign object next to every other menu bar item.
#
# The consequence is that macOS states cannot use colour, so they use alpha and
# shape instead: the red dot that marks a problem on Windows becomes a
# monochrome badge with a bite taken out of it, which survives being tinted.

# The mark alone, thresholded off its navy backing, trimmed so the glyph fills
# the frame rather than floating in the middle of the emblem's padding.
MONO_MASTER="$TMPDIR_ICONS/mono-master.png"
magick "$SRC" -crop "$EMBLEM_CROP" +repage \
    -colorspace Gray -level 30%,70% -threshold 50% -transparent black \
    -fill black -opaque white \
    -trim +repage -background none -gravity center \
    -extent '%[fx:max(w,h)]x%[fx:max(w,h)]' \
    "$MONO_MASTER"

for pair in "22 " "44 @2x"; do
    set -- $pair; SZ=$1; SUFFIX=${2:-}
    GLYPH=$(( SZ * 92 / 100 ))

    # ---- colour family (Windows, Linux) ----
    #
    # A TRANSPARENT glyph, not the rounded navy tile. Every other icon in a
    # Windows notification area is a shape on transparency; shipping the logo
    # square puts a dark box in the middle of them that reads as a foreign
    # object, and it fights the taskbar in light theme and again in dark.
    # So the colour family is the same silhouette as the macOS one, filled in
    # the brand's own light blue instead of black.
    magick "$MONO_MASTER" -fill "#7FC3E8" -colorize 100 \
        -filter Lanczos -resize "${GLYPH}x${GLYPH}" \
        -background none -gravity center -extent "${SZ}x${SZ}" \
        "$OUT/tray-running${SUFFIX}.png"

    magick "$OUT/tray-running${SUFFIX}.png" -alpha set \
        -channel A -evaluate multiply 0.55 +channel \
        "$OUT/tray-starting${SUFFIX}.png"

    # Grey rather than faded: "stopped" is a state, not a weaker version of
    # running, and alpha alone reads as "still starting".
    magick "$MONO_MASTER" -fill "#9AA0A6" -colorize 100 \
        -filter Lanczos -resize "${GLYPH}x${GLYPH}" \
        -background none -gravity center -extent "${SZ}x${SZ}" \
        "$OUT/tray-stopped${SUFFIX}.png"

    DOT=$(( SZ * 42 / 100 ))
    CX=$(( SZ - DOT/2 - 1 )); CY=$(( SZ - DOT/2 - 1 ))
    magick "$OUT/tray-stopped${SUFFIX}.png" \
        -fill '#d64545' -stroke none \
        -draw "circle $CX,$CY $CX,$(( CY - DOT/2 ))" \
        "$OUT/tray-problem${SUFFIX}.png"

    # ---- template family (macOS menu bar) ----
    magick "$MONO_MASTER" -filter Lanczos -resize "${GLYPH}x${GLYPH}" \
        -background none -gravity center -extent "${SZ}x${SZ}" \
        "$OUT/mono-running${SUFFIX}.png"

    magick "$OUT/mono-running${SUFFIX}.png" -alpha set \
        -channel A -evaluate multiply 0.55 +channel \
        "$OUT/mono-starting${SUFFIX}.png"

    magick "$OUT/mono-running${SUFFIX}.png" -alpha set \
        -channel A -evaluate multiply 0.30 +channel \
        "$OUT/mono-stopped${SUFFIX}.png"

    magick "$OUT/mono-stopped${SUFFIX}.png" \
        -fill none -stroke none \
        -draw "fill rgba(0,0,0,0) circle $CX,$CY $CX,$(( CY - DOT/2 - 2 ))" \
        -fill black \
        -draw "circle $CX,$CY $CX,$(( CY - DOT/2 ))" \
        "$OUT/mono-problem${SUFFIX}.png"
done

# ── Scrollbar arrows ────────────────────────────────────────────────────────
#
# Qt cannot draw a scrollbar arrow from a stylesheet alone -- border-triangle
# tricks render inconsistently across styles -- so they are real images. Drawn
# in the pale sky tone so they read against the dark navy track in either
# system appearance.
arrow() {   # arrow <up|down> <size> <output>
    local dir=$1 sz=$2 out=$3
    local half=$(( sz / 2 ))
    if [ "$dir" = "up" ]; then
        magick -size "${sz}x${sz}" xc:none -fill "#A8CFE4" -stroke none \
            -draw "polygon $half,$(( sz * 30 / 100 )) $(( sz * 15 / 100 )),$(( sz * 65 / 100 )) $(( sz * 85 / 100 )),$(( sz * 65 / 100 ))" \
            -strip "$out"
    else
        magick -size "${sz}x${sz}" xc:none -fill "#A8CFE4" -stroke none \
            -draw "polygon $half,$(( sz * 70 / 100 )) $(( sz * 15 / 100 )),$(( sz * 35 / 100 )) $(( sz * 85 / 100 )),$(( sz * 35 / 100 ))" \
            -strip "$out"
    fi
}
arrow up   16 "$OUT/arrow-up.png"
arrow up   32 "$OUT/arrow-up@2x.png"
arrow down 16 "$OUT/arrow-down.png"
arrow down 32 "$OUT/arrow-down@2x.png"

# ── Window / about / app icon ───────────────────────────────────────────────
full 256 "$OUT/app-256.png"

# ── Windows .ico ────────────────────────────────────────────────────────────
#
# One file, every size Windows asks for. 256 is PNG-compressed inside the .ico
# (Vista+); the small sizes use the emblem for the same reason the tray does.
for s in 16 20 24 32; do emblem "$s" "$TMP/ico-$s.png"; done
for s in 48 64 128 256; do full "$s" "$TMP/ico-$s.png"; done
magick "$TMP/ico-16.png" "$TMP/ico-20.png" "$TMP/ico-24.png" "$TMP/ico-32.png" \
       "$TMP/ico-48.png" "$TMP/ico-64.png" "$TMP/ico-128.png" "$TMP/ico-256.png" \
       "$OUT/cpapdash.ico"

# ── macOS .icns ─────────────────────────────────────────────────────────────
if command -v iconutil >/dev/null; then
    ICONSET="$TMP/cpapdash.iconset"
    mkdir -p "$ICONSET"
    full 16   "$ICONSET/icon_16x16.png"
    full 32   "$ICONSET/icon_16x16@2x.png"
    full 32   "$ICONSET/icon_32x32.png"
    full 64   "$ICONSET/icon_32x32@2x.png"
    full 128  "$ICONSET/icon_128x128.png"
    full 256  "$ICONSET/icon_128x128@2x.png"
    full 256  "$ICONSET/icon_256x256.png"
    full 512  "$ICONSET/icon_256x256@2x.png"
    full 512  "$ICONSET/icon_512x512.png"
    magick "$SRC" -strip "$ICONSET/icon_512x512@2x.png"   # 1024 would upscale; keep the master
    iconutil -c icns "$ICONSET" -o "$OUT/cpapdash.icns"
else
    echo "note: iconutil is macOS-only; .icns not regenerated" >&2
fi

echo "icons written to $OUT:"
ls -1 "$OUT" | sed 's/^/  /'
