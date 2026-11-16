#!/bin/sh
# The UI tests that need the app's own headers (fonts, GL, the Nav) — offscreen (SDL offscreen + Mesa llvmpipe) in the
# arm64 trixie image with the dev libraries (vibe-hh-map, or any image with libsdl2-dev libsdl2-ttf-dev
# libsdl2-image-dev libdrm-dev libgles-dev). Silent: no audio (SDL_AUDIODRIVER=dummy; nothing here opens audio).
#   cw_log_test       the CW text box: follow, hold, EXPLAIN <-> RAW, CLR, a signal's history
#   slider_hold_test  hold LEFT / RIGHT on a slider: the repeat's timing, release, focus, the latest-value sender
#   wide_vfo_render_test  V12 DAB's VFO is the 1.536 MHz block (drawOverlays, read back): filled at 7 and 2.048 MHz, edges
#                     drawn, no wash on a view narrower than the block, WFM unchanged
#   ism_bands_test    the ISM box's BANDS list: ismBands with their regions, opens on the server's band, the D-pad walks
#                     and scrolls 17 bands, ENTER queues that band's id (none for the band already on), BACK leaves it
#   wefax_adj_test    WEFAX's ADJ row is pressed into: LEFT / RIGHT walk MARGIN · SLANT · AUTO · RAW until ENTER edits one;
#                     ENTER or BACK finishes (BACK consumed, the box not released); DOWN off it or closing ADJ ends it
#   tuned_label_render_test  V12 the tuned station's label (drawOverlays, read back): beside the needle at the waterfall's
#                     top once the dial rests, flipped left at the right edge, nothing while it moves or on nothing
#   paging_box_test   V12 PAGE: offered only where the engine says, the speaker glyph / VTS line while it runs with the
#                     person's own mute untouched (from on, from muted, from APRS), the page list (parse, newest first,
#                     bounded, ENTER reads one whole), nothing written to disk
#   handheld-ui/test/run_offscreen_tests.sh [test …]   (default: all)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
IMG=${IMG:-vibe-hh-map:trixie}
TESTS=${*:-cw_log_test slider_hold_test wide_vfo_render_test ism_bands_test wefax_adj_test tuned_label_render_test paging_box_test}
docker run --rm --platform linux/arm64 -v "$REPO/handheld-ui:/w:ro" -v "$REPO/assets/fonts:/fonts:ro" "$IMG" sh -c "
  set -e; cd /w; T=\$(mktemp -d)
  for t in $TESTS; do
    g++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc \$(pkg-config --cflags sdl2 SDL2_ttf glesv2 libdrm) -o \$T/\$t \
        test/\$t.cpp src/wf/*.cpp src/kb/*.cpp \$(pkg-config --libs sdl2 SDL2_ttf SDL2_image glesv2 libdrm) -lEGL -lpthread
    SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy VIBE_HHUI_FONTS=/fonts \$T/\$t
  done"
