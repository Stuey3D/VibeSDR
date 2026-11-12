#!/bin/sh
# The native map, offscreen (test/map_test.cpp): vibe-hhui's map code over the real map files, Mesa llvmpipe in
# Docker linux/arm64 (the R36S's architecture and compiler family). Silent: no audio is opened.
#   docker build --platform linux/arm64 -t vibe-hh-map:trixie handheld-ui/test/docker-map   (once; needs vibe-hh-wf:trixie)
#   handheld-ui/test/run_map_test.sh [outDir] [scenario…]      scenarios: zoom spots (default both), or: shot lat lon z [profile]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${1:-$HOME/R36S-debug/map-test}
shift 2>/dev/null || true
mkdir -p "$OUT"
SCEN=${*:-zoom spots}
docker run --rm --platform linux/arm64 -e MAP_TEST_BPS -e MAP_TEST_RENDER_CAP -e VIBE_HHUI_MAP_NOMSAA -v "$REPO/handheld-ui:/w:ro" -v "$REPO/assets:/a:ro" -v "$OUT:/o" vibe-hh-map:trixie sh -c "
  set -e
  g++ -std=c++17 -O2 -ffp-contract=off -Wall -Wno-unused-parameter -I/w/src \$(pkg-config --cflags sdl2) -o /tmp/map_test /w/test/map_test.cpp \
      \$(pkg-config --libs sdl2 SDL2_ttf SDL2_image) -lEGL -lGLESv2 -lpthread
  export SDL_VIDEODRIVER=offscreen EGL_PLATFORM=surfaceless SDL_AUDIODRIVER=dummy VIBE_HHUI_FONTS=/a/fonts
  case '$SCEN' in
    shot*) /tmp/map_test shot /a/mapdata/v1 /o/\$(echo '$SCEN' | cut -d' ' -f2- | tr ' ' '_').png \$(echo '$SCEN' | cut -d' ' -f2-) ;;
    *) rc=0; for s in $SCEN; do echo \"== \$s\"; /tmp/map_test \$s /a/mapdata/v1 /o 2>/o/\$s.log || rc=1; done; exit \$rc ;;
  esac"
