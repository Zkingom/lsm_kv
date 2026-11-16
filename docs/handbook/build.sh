#!/usr/bin/env bash
# Rebuilds the handbook PDFs from the Markdown sources.
#   Toolchain: python3 (diagram SVGs) -> headless Chromium via Playwright (SVG -> PNG)
#              pandoc (Markdown -> HTML) -> headless Chromium (HTML -> A4 PDF)
#   Fonts:     WenQuanYi Zen Hei (CJK), Liberation Sans / Mono (Latin)
#   Usage:     docs/handbook/build.sh        # needs pandoc, node + playwright, the fonts above
# Set NODE_PATH if playwright is installed globally, e.g. NODE_PATH=$(npm root -g).
set -euo pipefail
cd "$(dirname "$0")"

fc-list : family | grep "WenQuanYi Zen Hei" > /dev/null || { echo "missing CJK font: WenQuanYi Zen Hei" >&2; exit 1; }

python3 diagrams/gen_diagrams.py
for svg in diagrams/*.svg; do
  node render.js png "$svg" "${svg%.svg}.png"
done

build() {  # $1 = language suffix, $2 = footer text
  pandoc "handbook_$1.md" -f markdown -t html5 --standalone --columns=100000 --css style.css -o "handbook_$1.html"
  node render.js pdf "handbook_$1.html" "handbook_$1.pdf" "$2"
  rm -f "handbook_$1.html"
  pdfinfo "handbook_$1.pdf" | grep Pages
}
build zh "lsm-kv 复习手册"
build en "lsm-kv review handbook"
