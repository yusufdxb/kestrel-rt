#!/usr/bin/env bash
# Render docs/figures/*.dot to SVG and PNG (Graphviz). Committed outputs are reviewed images.
set -euo pipefail
cd "$(dirname "$0")/../docs/figures"
for f in *.dot; do
  dot -Tsvg "$f" -o "${f%.dot}.svg"
  dot -Tpng -Gdpi=160 "$f" -o "${f%.dot}.png"
done
ls -1 *.svg
