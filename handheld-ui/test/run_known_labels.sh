#!/bin/sh
# The handheld SIGNAL LABELS: the golden test (src/known_labels.h against the app's src/utils/knownSignals.ts) and the
# offscreen render (known_labels_render.cpp → a PNG of three crowded spectra). Both in Docker linux/arm64 (aarch64 g++,
# SDL offscreen + Mesa llvmpipe), as the R36S is. Silent: no audio anywhere.
#   handheld-ui/test/run_known_labels.sh [outDir] [png]
# Needs node ≥ 22 and the vibe-hh-map:trixie image (SDL2, SDL2_ttf, SDL2_image, GLES).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${1:-$HOME/R36S-debug/known-labels}
PNG=${2:-$OUT/R36S-spectrum-labels.png}
mkdir -p "$OUT"
cd "$HERE"
node --no-warnings --import ./ts_resolve.mjs gen_known_labels_golden.ts "$OUT/known_labels_golden.txt"
cp known_labels_inputs.json "$OUT/"
FONTS="$REPO/assets/fonts"
docker run --rm --platform linux/arm64 -v "$REPO/handheld-ui:/w" -v "$OUT:/o" -v "$FONTS:/fonts:ro" vibe-hh-map:trixie sh -c "
  set -e; cd /w
  g++ -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter -o /o/known_labels_test test/known_labels_test.cpp \$(pkg-config --cflags sdl2) -lGLESv2
  /o/known_labels_test /o/known_labels_inputs.json /o/known_labels_golden.txt
  g++ -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter -o /o/known_labels_render test/known_labels_render.cpp \
      \$(pkg-config --cflags --libs sdl2 SDL2_ttf SDL2_image glesv2)
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy VIBE_HHUI_FONTS=/fonts /o/known_labels_render /o/known_labels_inputs.json /o/known_labels.png"
cp "$OUT/known_labels.png" "$PNG"
echo "  screenshot: $PNG"
