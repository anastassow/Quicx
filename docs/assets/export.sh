#!/usr/bin/env bash
# export.sh — the README assets: SVG (from make_assets.py) -> PNG at 2x
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PY="$HERE/../report/build/venv/bin/python"; [ -x "$PY" ] || PY=python3
"$PY" "$HERE/make_assets.py"
for f in banner benchmarks results; do
  for v in dark light; do
    rsvg-convert -z 2 "$HERE/$f-$v.svg" -o "$HERE/$f-$v.png"
  done
done
ls -la "$HERE"/*.png
