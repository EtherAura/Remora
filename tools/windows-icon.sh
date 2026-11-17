#!/usr/bin/env bash
# Make packaging/windows/io.github.EtherAura.Remora.ico, the icon the Windows executables carry
# (packaging/windows/remora.rc), their windows show, and the installer and Start-menu entries use
# (bd remora-7oih):
#
#   tools/windows-icon.sh            # needs rsvg-convert, ImageMagick and python3
#
# The icon is a one-colour silhouette, baked white for a dark scheme and #232629 for a light one
# (packaging/*.svg). Windows shows one .ico on every background it has: a light Start menu, a dark
# taskbar, Explorer in either mode. So it is the dark silhouette on a light halo that follows the
# outline, which reads on all of them. From 64 px up the halo fills the artwork's holes too, which
# gives back its own black on white; below that the circuit detail only averages to grey, so the
# small sizes are the solid silhouette, which is what a taskbar or a title bar can show.
#
# A stroke in the SVG would be simpler, but it traces every hole as well as the outline and
# swamps the small sizes. So each size is drawn at four times its size from the vector, the halo is
# made from the outline by dilating it, and the result is scaled down.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/packaging/io.github.EtherAura.Remora.light-scheme.svg"
OUT="$ROOT/packaging/windows/io.github.EtherAura.Remora.ico"
for tool in rsvg-convert magick python3; do
    command -v "$tool" >/dev/null || { echo "windows-icon: needs $tool" >&2; exit 1; }
done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pngs=()
for size in 16 20 24 32 40 48 64 128 256; do
    big=$((size * 4))
    # Halo in pixels of the final size: 1 up to 32 px, then growing with it, at most 4.
    halo=$(awk -v s="$size" 'BEGIN { h = s / 32; if (h < 1) h = 1; if (h > 4) h = 4; print h * 4 }')
    rsvg-convert -w "$big" -h "$big" "$SRC" -o "$TMP/art.png"
    # The outline, holes filled: whatever the corner cannot reach through transparency.
    magick "$TMP/art.png" -alpha extract -threshold 5% -bordercolor black -border 1 \
        -fill gray50 -draw 'color 0,0 floodfill' -fill white +opaque gray50 \
        -fill black -opaque gray50 -shave 1x1 "$TMP/outline.png"
    magick "$TMP/outline.png" -morphology Dilate "Disk:$halo" "$TMP/halo.png"
    if ((size >= 64)); then
        magick \( "$TMP/halo.png" -alpha copy -fill '#fcfcfc' -colorize 100 \) "$TMP/art.png" \
            -composite -resize "${size}x${size}" "$TMP/$size.png"
    else
        magick \( "$TMP/halo.png" -alpha copy -fill '#fcfcfc' -colorize 100 \) \
            \( "$TMP/outline.png" -alpha copy -fill '#232629' -colorize 100 \) \
            -composite -resize "${size}x${size}" "$TMP/$size.png"
    fi
    pngs+=("$TMP/$size.png")
done
# Every size stored as the PNG it was rendered as, which Windows has read natively since Vista: a
# fraction of the size of bitmaps, and nothing re-encodes the render. The header, then one 16-byte
# directory entry per image (0 means 256), then the images.
python3 -I - "$OUT" "${pngs[@]}" <<'PY'
import struct, sys
out, files = sys.argv[1], sys.argv[2:]
images = [open(f, 'rb').read() for f in files]
blob = struct.pack('<HHH', 0, 1, len(images))
offset = 6 + 16 * len(images)
for data in images:
    w, h = struct.unpack('>II', data[16:24])
    blob += struct.pack('<BBBBHHII', w % 256, h % 256, 0, 0, 1, 32, len(data), offset)
    offset += len(data)
open(out, 'wb').write(blob + b''.join(images))
PY
echo "windows-icon: $OUT ($(stat -c %s "$OUT") bytes)"
