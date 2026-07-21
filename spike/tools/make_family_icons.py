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
import os
import sys
from PIL import Image, ImageDraw, ImageFont

_HERE = os.path.dirname(os.path.abspath(__file__))
ATKINSON = os.path.join(_HERE, "AtkinsonHyperlegible-Regular.ttf")

GREEN = (86, 255, 132)
TILE_BLACK = (5, 5, 4)     # the actual tile/screen black — NOT the greener bg, or the fill shows as a box

JR = "spike/WristSDR/WristSDR/Assets.xcassets/AppIcon.appiconset/AppIcon.png"

# The black rounded tile bottom-right of Jr's icon (measured from the actual near-black pixels). We
# refill its INTERIOR (just inside the border) then draw the sub-brand glyph, CENTRED on the true tile.
TILE = (583, 526, 857, 906)          # outer tile bounds
TILE_CX, TILE_CY = 720, 716          # true tile centre — glyphs centre here (not the old 710,705)
FILL = (588, 530, 852, 902)          # interior to clear (covers the whole watch incl. crown nub)
FILL_R = 40


def clear_tile(d):
    d.rounded_rectangle(FILL, radius=FILL_R, fill=TILE_BLACK)


def draw_iphone(d):
    """A phone: green rounded body, black screen showing TWO content lines (matching the app's phone
    glyph / echoing Jr's watch list), plus a home bar."""
    cx = (FILL[0] + FILL[2]) // 2
    bw, bh = 158, 322                                                    # a touch wider than before
    x0, y0 = cx - bw // 2, 546
    x1, y1 = cx + bw // 2, y0 + bh
    d.rounded_rectangle((x0, y0, x1, y1), radius=32, fill=GREEN)          # phone body
    sb, tb, bb = 22, 40, 42                                              # side / top / bottom bezel
    sx0, sy0, sx1, sy1 = x0 + sb, y0 + tb, x1 - sb, y1 - bb
    d.rounded_rectangle((sx0, sy0, sx1, sy1), radius=14, fill=TILE_BLACK)  # screen
    # The two marks that say "modern iPhone": a Dynamic Island pill up top, a home bar at the bottom.
    d.rounded_rectangle((cx - 30, sy0 + 14, cx + 30, sy0 + 32), radius=9, fill=GREEN)   # island
    d.rounded_rectangle((cx - 32, sy1 - 26, cx + 32, sy1 - 16), radius=5, fill=GREEN)   # home bar


def _mono(size):
    for p in ("/System/Library/Fonts/Menlo.ttc",
              "/System/Library/Fonts/Monaco.ttf",
              "/System/Library/Fonts/SFNSMono.ttf"):
        try:
            return ImageFont.truetype(p, size)
        except Exception:
            pass
    return ImageFont.load_default()


def draw_binary(d):
    """VibeDSP: a stream of 0s and 1s (binary data) in the corner tile, in place of the device."""
    font = _mono(70)
    rows = ["1 0 1", "0 1 1", "1 0 0", "0 1 0"]        # a short cascading stream
    cx = (FILL[0] + FILL[2]) // 2
    y0, dy = 556, 76
    for i, r in enumerate(rows):
        w = d.textlength(r, font=font)
        d.text((cx - w / 2, y0 + i * dy), r, font=font, fill=GREEN)


def draw_node(d):
    """VibeServer: our triangle-node mark — three nodes joined into an EQUILATERAL triangle,
    centred on the true tile centre."""
    cx = TILE_CX
    base = 200                                  # as wide as the tile allows (node radius included)
    h = int(base * 0.866)                       # equilateral height
    ytop = TILE_CY - h // 2 + 18                  # centre, nudged down: the apex-down mass reads high,
    #                                              so a small drop removes the black "chin" below it.
    # APEX DOWN — two nodes on top, one below — to match the in-app node glyph.
    tl = (cx - base // 2, ytop)
    tr = (cx + base // 2, ytop)
    bot = (cx, ytop + h)
    for a, b in ((tl, tr), (tl, bot), (tr, bot)):
        d.line([a, b], fill=GREEN, width=16)
    for p in (tl, tr, bot):
        d.ellipse((p[0] - 34, p[1] - 34, p[0] + 34, p[1] + 34), fill=GREEN)


# ── VibeDSP: no tile. The engine's binary streams out of the bottom of the radio. Website-only, so it
#    reconstructs the radio's right side (hidden under Jr's watch tile) rather than needing a clean base.
def _erase_to_bg(im, box):
    """Repaint a box with the icon's vertical background gradient (near-black → dark green)."""
    px = im.load()
    x0, y0, x1, y1 = box
    for y in range(y0, y1):
        g = min(36, int(10 + max(0, y - 300) * 0.036))
        row = (6, g, 7, 255)
        for x in range(x0, x1):
            px[x, y] = row


def _fill_gradient(im, x0, x1):
    px = im.load()
    for y in range(im.size[1]):
        g = min(36, int(10 + max(0, y - 300) * 0.036))
        for x in range(x0, x1):
            px[x, y] = (6, g, 7, 255)


def build_vibedsp(im):
    """Website-only (per Stuart's sketch). The bitstream starts INSIDE the radio (over the bars) and
    streams right OUT of it, onto a slightly WIDER canvas. Radio kept complete; digits in Atkinson
    Hyperlegible, radio-green, on the same gradient/streak background as the rest of the family."""
    EXTRA = 300
    W = 1024 + EXTRA
    canvas = Image.new("RGBA", (W, 1024), (6, 10, 7, 255))
    canvas.paste(im, (0, 0))
    # Extend the background by tiling Jr's own pure-bg columns, so the gradient + faint streaks carry
    # across the new strip (rather than a flat fill that wouldn't match).
    strip = im.crop((852, 0, 1022, 1024))
    x = 1024
    while x < W:
        canvas.paste(strip, (x, 0)); x += strip.width
    d = ImageDraw.Draw(canvas)
    _erase_to_bg(canvas, (558, 500, 882, 908))           # remove the watch tile
    # Rebuild the radio COMPLETE (the digits overlap it, as drawn).
    d.rectangle((711, 396, 733, 838), fill=GREEN)        # right edge
    d.rectangle((558, 816, 733, 838), fill=GREEN)        # bottom edge
    for bx in (593, 672):                                # the two hidden speaker bars
        d.rectangle((bx - 10, 503, bx + 10, 757), fill=GREEN)
    # The bitstream — Atkinson Hyperlegible, radio-green, vertically centred ~y716, starting inside the
    # radio's lower-right and running out to the right.
    font = ImageFont.truetype(ATKINSON, 190)
    s = "010101"
    bb = d.textbbox((0, 0), s, font=font)
    tx, ty = 512, 716 - (bb[1] + bb[3]) // 2
    pb = d.textbbox((tx, ty), s, font=font)              # placed bbox
    # A black backing so the stream reads cleanly over the radio's bars/edges instead of merging.
    d.rounded_rectangle((pb[0] - 16, pb[1] - 12, pb[2] + 16, pb[3] + 12), radius=18, fill=TILE_BLACK)
    d.text((tx, ty), s, font=font, fill=GREEN)
    return canvas


# Tile sub-brands: clear the corner tile, draw the inset glyph.
GLYPHS = {
    "buddy": draw_iphone,
    "vibeserver": draw_node,
}
# Full custom builders (im, draw) — they own the whole composite (e.g. VibeDSP removes the tile).
CUSTOM = {
    "vibedsp": build_vibedsp,
}


def build(variant, out):
    im = Image.open(JR).convert("RGBA")
    if variant in CUSTOM:
        im = CUSTOM[variant](im)                 # may return a new (wider) canvas
    else:
        d = ImageDraw.Draw(im)
        clear_tile(d)
        GLYPHS[variant](d)
    im.convert("RGB").save(out)
    print("wrote", out)


if __name__ == "__main__":
    variant = sys.argv[1] if len(sys.argv) > 1 else "buddy"
    out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/icon_%s.png" % variant
    build(variant, out)
