#!/usr/bin/env python3
"""VibeSDR sub-brand app-icon family.

The base is Jr's icon (the green line-art radio + faint waterfall background). Every Vibe sub-brand
keeps that exact base and swaps only the INSET GLYPH inside the black rounded tile bottom-right:
  Jr        → smartwatch   (the original)
  Buddy     → iPhone
  VibeDSP   → a stream of 0s/1s pouring from the radio (binary data)
  VibeServer→ our triangle-node mark
VibeSDR itself keeps its own (funnel) icon — it is the master brand, not a sub-brand.

We composite onto Jr's PNG so the radio is pixel-identical to Jr; only the tile contents change.
"""
import sys
from PIL import Image, ImageDraw

GREEN = (86, 255, 132)
TILE_BLACK = (6, 10, 7)

JR = "spike/WristSDR/WristSDR/Assets.xcassets/AppIcon.appiconset/AppIcon.png"

# The black rounded tile bottom-right of Jr's icon (measured). We refill its INTERIOR (staying just
# inside the tile edge so its rounded border survives) then draw the sub-brand glyph.
TILE = (578, 518, 842, 892)          # outer tile bounds
FILL = (590, 530, 830, 880)          # interior to clear (covers the whole watch, inside the border)
FILL_R = 34


def clear_tile(d):
    d.rounded_rectangle(FILL, radius=FILL_R, fill=TILE_BLACK)


def draw_iphone(d):
    """A bold, unmistakable phone: green rounded body, black screen, speaker notch + home bar."""
    cx = (FILL[0] + FILL[2]) // 2
    # Portrait body — slender so it reads as a phone (vs the watch's squarer body), tall enough to
    # fill the tile like the watch straps did.
    bw, bh = 138, 322
    x0, y0 = cx - bw // 2, 546
    x1, y1 = cx + bw // 2, y0 + bh
    d.rounded_rectangle((x0, y0, x1, y1), radius=30, fill=GREEN)          # phone body
    # Screen: inset, with generous top/bottom bezels to carry the notch + home bar.
    sb, tb, bb = 20, 44, 44                                              # side / top / bottom bezel
    d.rounded_rectangle((x0 + sb, y0 + tb, x1 - sb, y1 - bb), radius=14, fill=TILE_BLACK)
    # Speaker notch (top bezel) + camera dot beside it.
    nw = 46
    d.rounded_rectangle((cx - nw // 2, y0 + 19, cx + nw // 2, y0 + 29), radius=5, fill=GREEN)
    d.ellipse((cx + nw // 2 + 10, y0 + 19, cx + nw // 2 + 20, y0 + 29), fill=GREEN)
    # Home indicator (bottom bezel).
    hw = 60
    d.rounded_rectangle((cx - hw // 2, y1 - 26, cx + hw // 2, y1 - 18), radius=4, fill=GREEN)


GLYPHS = {
    "buddy": draw_iphone,
}


def build(variant, out):
    im = Image.open(JR).convert("RGBA")
    d = ImageDraw.Draw(im)
    clear_tile(d)
    GLYPHS[variant](d)
    im.convert("RGB").save(out)
    print("wrote", out)


if __name__ == "__main__":
    variant = sys.argv[1] if len(sys.argv) > 1 else "buddy"
    out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/icon_%s.png" % variant
    build(variant, out)
