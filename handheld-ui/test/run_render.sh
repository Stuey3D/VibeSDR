#!/bin/sh
# Renders the 'picture' scenario two ways and compares them:
#   • the APP's own shader (WF_SKSL from WaterfallView.tsx, CanvasKit in node) over the app's TS pipeline — render_app.ts;
#   • the PORT (GLSL ES 3.00 + the C++ pipeline) offscreen in Docker linux/arm64 with Mesa llvmpipe — wf_render.cpp.
# Also builds and runs the golden test with the container's aarch64 GCC (the R36S's compiler family).
# Output: PNGs under $OUT (default ~/R36S-debug/wf-port). Needs the vibe-hh-wf:trixie image:
#   docker build --platform linux/arm64 -t vibe-hh-wf:trixie handheld-ui/test/docker
#   handheld-ui/test/run_render.sh [outDir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${1:-$HOME/R36S-debug/wf-port}
GOLD="$OUT/golden"
mkdir -p "$OUT" "$GOLD"
cd "$HERE"
node --no-warnings --import ./ts_resolve.mjs gen_golden.ts "$GOLD" > /dev/null
PAL=${PAL:-Jet}; H=600
SNAPS="4500 7620 9620 11900"   # mid-drag, mid zoom-in (2nd key), mid zoom-out (2nd key), the end
# 640 = the handheld (a texel boundary every 5th column: nearest-texel TIES that two float implementations break either
#       way); 1024 = one texel a column, no ties — the like-for-like comparison of the shader maths.
for W in 640 1024; do node --no-warnings --import ./ts_resolve.mjs render_app.ts "$GOLD" picture "$OUT" $W $H "$PAL" $SNAPS; done
docker run --rm --platform linux/arm64 -v "$REPO/handheld-ui:/w" -v "$OUT:/o" vibe-hh-wf:trixie sh -c "
  set -e; cd /w
  g++ -std=c++17 -O2 -ffp-contract=off -Wall -o /o/wf_render_arm64 test/wf_render.cpp src/wf/wf_row_place.cpp src/wf/wf_clock.cpp \
      src/wf/wf_pace.cpp src/wf/wf_signal.cpp src/wf/wf_combine.cpp src/wf/wf_palette.cpp src/wf/wf_model.cpp src/wf/wf_gl.cpp -lEGL -lGLESv2
  g++ -std=c++17 -O2 -ffp-contract=off -o /o/wf_golden_arm64 test/wf_golden_test.cpp src/wf/wf_row_place.cpp src/wf/wf_clock.cpp \
      src/wf/wf_pace.cpp src/wf/wf_signal.cpp src/wf/wf_combine.cpp src/wf/wf_palette.cpp src/wf/wf_model.cpp
  echo '  golden test, aarch64 g++:'; /o/wf_golden_arm64 /o/golden | grep -v 'row timing'
  for W in 640 1024; do /o/wf_render_arm64 /o/golden picture /o \$W $H $PAL $SNAPS; done"
for W in 640 1024; do for t in $SNAPS; do node --no-warnings compare_png.ts "$OUT/picture_${W}x${H}_${t}_app.rgba" "$OUT/picture_${W}x${H}_${t}_port.rgba" $W $H 2; done; done
