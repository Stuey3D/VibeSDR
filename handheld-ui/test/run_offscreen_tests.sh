#!/bin/sh
# The UI tests that need the app's own headers (fonts, GL, the Nav) — offscreen (SDL offscreen + Mesa llvmpipe) in the
# arm64 trixie image with the dev libraries (vibe-hh-map, or any image with libsdl2-dev libsdl2-ttf-dev
# libsdl2-image-dev libdrm-dev libgles-dev). Silent: no audio (SDL_AUDIODRIVER=dummy; nothing here opens audio).
#   cw_log_test       the CW text box: follow, hold, EXPLAIN <-> RAW, CLR, a signal's history
#   slider_hold_test  hold LEFT / RIGHT on a slider: the repeat's timing, release, focus, the latest-value sender
#   handheld-ui/test/run_offscreen_tests.sh [test …]   (default: all)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
IMG=${IMG:-vibe-hh-map:trixie}
TESTS=${*:-cw_log_test slider_hold_test}
docker run --rm --platform linux/arm64 -v "$REPO/handheld-ui:/w:ro" -v "$REPO/assets/fonts:/fonts:ro" "$IMG" sh -c "
  set -e; cd /w; T=\$(mktemp -d)
  for t in $TESTS; do
    g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc \$(pkg-config --cflags sdl2 SDL2_ttf glesv2 libdrm) -o \$T/\$t \
        test/\$t.cpp src/wf/*.cpp src/kb/*.cpp \$(pkg-config --libs sdl2 SDL2_ttf SDL2_image glesv2 libdrm) -lEGL -lpthread
    SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy VIBE_HHUI_FONTS=/fonts \$T/\$t
  done"
