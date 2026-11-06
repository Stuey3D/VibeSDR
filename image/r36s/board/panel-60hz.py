#!/usr/bin/env python3
# panel-60hz.py IN.dtbo OUT.dtbo — make a 60 Hz mode the panel's default.
# ✗✗✗ NOT USED (2026-11-06): on Stuart's R36S neither 60 Hz mode displays correctly (23.04 MHz: picture ~80 px left;
#     22.92 MHz: left half only, columns of noise). ROCKNIX's 80 Hz default is centred and clean, and the image ships
#     that overlay unchanged (build-image.sh). Kept as the record of what was tried.
#
# ★★★ WHY THE OVERLAY AND NOT COG. The ST7703 panel's overlay (ROCKNIX's generic-dsi description, generated for this
#     board — see board/PROVENANCE) lists twelve 640×480 modes and marks the 80.046 Hz one `default=1`, which the
#     driver turns into DRM_MODE_TYPE_PREFERRED. Cog's DRM platform takes the PREFERRED mode; every mode is named
#     "640x480", so COG_PLATFORM_DRM_VIDEO_MODE cannot pick one, and COG_PLATFORM_DRM_MODE_MAX=…@60 would pick the
#     FIRST mode under 60 (49.95 Hz). So `default=1` moves to "M clock=22920 horizontal=640,46,2,44
#     vertical=480,16,11,14": 22.92 MHz / (732 × 521) = 60.10 Hz, one of the modes ROCKNIX generated for this board.
# ★★★ THE HORIZONTAL TIMING MUST STAY 640,46,2,44 — the 80 Hz default's, the one proven on the panel. The first image
#     (2026-11-06) took the 23.04 MHz mode, horizontal 640,46,20,44: on the R36S the whole picture sat ~80 px left with a
#     black band on the right (the splash AND Cog). This mode differs from the proven one only in the vertical padding.
# ★★ BYTE FOR BYTE THE SAME OTHERWISE: the 10 bytes " default=1" move from one string to another inside the same
#    property, so the property's length, the structure block and every offset stay as they were. The script refuses
#    unless it finds exactly the two strings it expects, and checks that the output differs only by that move.
import sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
old80 = b"M clock=30000 horizontal=640,46,2,44 vertical=480,16,2,14 default=1\0"
new80 = b"M clock=30000 horizontal=640,46,2,44 vertical=480,16,2,14\0"
old60 = b"M clock=22920 horizontal=640,46,2,44 vertical=480,16,11,14\0"
new60 = b"M clock=22920 horizontal=640,46,2,44 vertical=480,16,11,14 default=1\0"
if data.count(old80) != 1 or data.count(old60) != 1 or data.count(b"default=1") != 1:
    sys.exit("panel-60hz: %s is not the overlay this script was written for" % src)
a, b = data.index(old80), data.index(old60)
assert a < b, "mode order changed"
mid = data[a + len(old80):b]
out = data[:a] + new80 + mid + new60 + data[b + len(old60):]
assert len(out) == len(data)
assert out.count(b"default=1") == 1 and new60 in out
open(dst, "wb").write(out)
print("panel-60hz: default mode 80.046 Hz -> 60.10 Hz (%d bytes, same length)" % len(out))
