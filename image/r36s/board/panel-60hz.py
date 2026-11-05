#!/usr/bin/env python3
# panel-60hz.py IN.dtbo OUT.dtbo — make the panel's 60.0 Hz mode the default one, changing nothing else.
#
# ★★★ WHY THE OVERLAY AND NOT COG. The ST7703 panel's overlay (ROCKNIX's generic-dsi description, generated for this
#     board — see board/PROVENANCE) lists twelve 640×480 modes and marks the 80.046 Hz one `default=1`, which the
#     driver turns into DRM_MODE_TYPE_PREFERRED. Cog's DRM platform takes the PREFERRED mode; every mode is named
#     "640x480", so COG_PLATFORM_DRM_VIDEO_MODE cannot pick one, and COG_PLATFORM_DRM_MODE_MAX=…@60 would pick the
#     FIRST mode under 60 (49.95 Hz). So `default=1` moves to "M clock=23040 horizontal=640,46,20,44
#     vertical=480,16,2,14": 23.04 MHz / (750 × 512) = 60.000 Hz, from the same family ROCKNIX generated for 57.5 / 90
#     / 120 Hz.
# ★★ BYTE FOR BYTE THE SAME OTHERWISE: the 10 bytes " default=1" move from one string to another inside the same
#    property, so the property's length, the structure block and every offset stay as they were. The script refuses
#    unless it finds exactly the two strings it expects, and checks that the output differs only by that move.
import sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
old80 = b"M clock=30000 horizontal=640,46,2,44 vertical=480,16,2,14 default=1\0"
new80 = b"M clock=30000 horizontal=640,46,2,44 vertical=480,16,2,14\0"
old60 = b"M clock=23040 horizontal=640,46,20,44 vertical=480,16,2,14\0"
new60 = b"M clock=23040 horizontal=640,46,20,44 vertical=480,16,2,14 default=1\0"
if data.count(old80) != 1 or data.count(old60) != 1 or data.count(b"default=1") != 1:
    sys.exit("panel-60hz: %s is not the overlay this script was written for" % src)
a, b = data.index(old80), data.index(old60)
assert a < b, "mode order changed"
mid = data[a + len(old80):b]
out = data[:a] + new80 + mid + new60 + data[b + len(old60):]
assert len(out) == len(data)
assert out.count(b"default=1") == 1 and new60 in out
open(dst, "wb").write(out)
print("panel-60hz: default mode 80.046 Hz -> 60.000 Hz (%d bytes, same length)" % len(out))
