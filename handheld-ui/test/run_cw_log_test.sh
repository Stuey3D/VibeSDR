#!/bin/sh
# The CW text box's reading (cw_log_test.cpp): follow, hold, EXPLAIN ↔ RAW, CLR, a signal's history — through the box's
# own code, offscreen (SDL offscreen + Mesa llvmpipe) in the arm64 trixie image with the dev libraries (vibe-hh-map,
# or any image with libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libdrm-dev libgles-dev). Silent: no audio.
#   handheld-ui/test/run_cw_log_test.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
IMG=${IMG:-vibe-hh-map:trixie}
docker run --rm --platform linux/arm64 -v "$REPO/handheld-ui:/w:ro" -v "$REPO/assets/fonts:/fonts:ro" "$IMG" sh -c '
  set -e; cd /w; T=$(mktemp -d)
  g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc $(pkg-config --cflags sdl2 SDL2_ttf glesv2 libdrm) -o $T/t \
      test/cw_log_test.cpp src/wf/*.cpp src/kb/*.cpp $(pkg-config --libs sdl2 SDL2_ttf SDL2_image glesv2 libdrm) -lEGL -lpthread
  SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy VIBE_HHUI_FONTS=/fonts $T/t'
