#!/usr/bin/env python3
"""VibeSDR Jr app icon — the SHIPPING radio glyph, with an Apple Watch badged on its corner.

★ The radio is LIFTED FROM assets/icon.png, not redrawn. Stuart: "keep the radio icon identical."
  A redraw that is 95% right is worse than useless for a family mark — the eye reads the
  difference without being able to name it, and the tie to VibeSDR is exactly what this glyph is
  carrying. So it is extracted by colour mask, scaled, and composited.

No trapezoid (Stuart): it competed with the badge for the same corners. The family tie is the
glyph itself, the green, and the scanline wash.
"""
from PIL import Image, ImageDraw, ImageFilter

SRC = "/Users/stuey3d/VibeSDR/assets/icon.png"
OUTDIR = "/private/tmp/claude-501/-Users-stuey3d-VibeSDR/7b7e24cd-79a9-47ac-92d6-74a06c52d572/scratchpad"
OUT = 1024
GREEN = (86, 255, 132)
BG = (5, 5, 4)
GLYPH_BOX = (378, 141, 646, 422)          # measured, not guessed

# ── extract the glyph as a clean alpha mask ──────────────────────────────────────────────
src = Image.open(SRC).convert("RGB").crop(GLYPH_BOX)
mask = Image.new("L", src.size, 0)
sp, mp = src.load(), mask.load()
for y in range(src.size[1]):
    for x in range(src.size[0]):
        r, g, b = sp[x, y]
        # the glyph is the bright green; the faint glow behind it is not
        mp[x, y] = 255 if (g > 150 and g - r > 60 and g - b > 40) else 0

# ── canvas: same near-black + faint green wash and scanlines as the phone icon ───────────
im = Image.new("RGB", (OUT, OUT), BG)
d = ImageDraw.Draw(im, "RGBA")
for y in range(0, OUT, 2):
    t = max(0.0, (y - 380) / 644.0)
    d.rectangle([0, y, OUT, y + 2], fill=(6, int(10 + 26 * t), 7))
for x in range(40, OUT, 97):
    h = 200 + (x * 37) % 420
    d.rectangle([x, 520, x + 3, 520 + h], fill=(255, 255, 255, 12))

# ── the radio, scaled up and centred slightly high-left to leave the badge its corner ────
GW = 596
gh = int(GW * mask.size[1] / mask.size[0])
gm = mask.resize((GW, gh), Image.LANCZOS)
gx, gy = 138, 214
glyph = Image.new("RGB", (GW, gh), GREEN)
im.paste(glyph, (gx, gy), gm)

# ── the watch, badged over the radio's bottom-right corner ───────────────────────────────
BX, BY, BW = 706, 716, 196          # centre + width
bh = BW * 1.20
sw = max(3.0, BW * 0.100)
x0, y0, x1, y1 = BX - BW / 2, BY - bh / 2, BX + BW / 2, BY + bh / 2

# Knockout: a background-filled pad so the radio's strokes stop cleanly at the watch and it
# reads as sitting ON TOP, rather than tangling with the grille bars behind it.
pad = BW * 0.13
d.rounded_rectangle([x0 - pad, y0 - bh * 0.20 - pad, x1 + pad + BW * 0.14, y1 + bh * 0.20 + pad],
                    radius=BW * 0.34, fill=BG)

bw_band = BW * 0.56
d.line([BX, y0 - bh * 0.19, BX, y0 + 4], fill=GREEN, width=int(bw_band))
d.line([BX, y1 - 4, BX, y1 + bh * 0.19], fill=GREEN, width=int(bw_band))
d.rounded_rectangle([x0, y0, x1, y1], radius=BW * 0.30, fill=BG, outline=GREEN, width=int(sw))
d.line([x1 + sw * 0.3, BY - bh * 0.07, x1 + BW * 0.12, BY - bh * 0.07],
       fill=GREEN, width=int(sw * 0.95))
for i in range(3):                                   # a hint of waterfall on the screen
    yy = BY - bh * 0.13 + i * bh * 0.135
    d.line([BX - BW * 0.20, yy, BX + BW * 0.20, yy], fill=GREEN, width=int(sw * 0.62))

im.save(f"{OUTDIR}/jr_icon6.png")

mask_c = Image.new("L", (OUT, OUT), 0)
ImageDraw.Draw(mask_c).ellipse([0, 0, OUT, OUT], fill=255)
prev = Image.new("RGB", (OUT, OUT), (0, 0, 0))
prev.paste(im, (0, 0), mask_c)
prev.save(f"{OUTDIR}/jr_icon6_round.png")
print("wrote jr_icon6.png + jr_icon6_round.png")
