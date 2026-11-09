#!/usr/bin/env bash
# export_figures.sh — SVG sources -> PDF (for LaTeX), PNG at 2x, and outlined SVG (text as paths, for the web)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE/../figures"
"$HERE/../build/venv/bin/python" "$HERE/make_diagrams.py" .
for f in quicx-architecture apool-architecture; do
  for v in dark light; do
    rsvg-convert -f pdf "$f-$v.svg" -o "$f-$v.pdf"
    rsvg-convert -w 3840 -h 2160 "$f-$v.svg" -o "$f-$v.png"
    rsvg-convert -f svg "$f-$v.svg" -o "$f-$v.outlined.svg"
  done
done
ls -la
