#!/bin/sh
# Build data/fonts/NotoSansCJKjp-Subset.otf: the UI's fallback font for Japanese.
# Keeps every JIS X 0208 character plus kana, CJK punctuation, full-width forms,
# and ASCII/Latin-1/general punctuation (mpv's captions use this font alone for
# Japanese tracks): ~7,700 codepoints, ~1.5 MB. The full Noto Sans CJK JP is ~16 MB.
# Needs fonttools (pip install fonttools) and a Noto Sans CJK source font:
#   tools/make_cjk_font.sh [/path/to/NotoSansCJK-Regular.ttc] [font-number]
# (font-number 0 = the JP face of the Debian/Ubuntu fonts-noto-cjk .ttc; use 0 for
# a standalone NotoSansCJKjp-Regular.otf too).
set -e
SRC=${1:-/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc}
NUM=${2:-0}
OUT=$(dirname "$0")/../data/fonts/NotoSansCJKjp-Subset.otf
TMP=$(mktemp)
python3 - "$TMP" <<'PY'
import sys
cps = set()
for b1 in range(0xA1, 0xFF):                     # every JIS X 0208 row (via EUC-JP)
    for b2 in range(0xA1, 0xFF):
        try: cps.update(map(ord, bytes([b1, b2]).decode('euc_jp')))
        except UnicodeDecodeError: pass
for a, b in [(0x20, 0x7E), (0xA0, 0xFF), (0x2000, 0x206F),  # Latin-1, punctuation
             (0x3000, 0x30FF), (0x31F0, 0x31FF),
             (0x3200, 0x32FF), (0xFF00, 0xFFEF)]:
    cps.update(range(a, b + 1))
open(sys.argv[1], 'w').write(','.join('U+%04X' % c for c in sorted(cps)))
PY
pyftsubset "$SRC" --font-number="$NUM" --unicodes-file="$TMP" \
    --layout-features='' --no-hinting --name-IDs='*' --output-file="$OUT"
rm -f "$TMP"
ls -l "$OUT"
