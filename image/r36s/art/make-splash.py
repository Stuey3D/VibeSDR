#!/usr/bin/env python3
# make-splash.py <repo> <out-dir> — render the R36S boot splash at build time (needs python3-pil).
#
# Out: splash.bgrx.z   640×480 XRGB8888 (bytes B,G,R,X), zlib — vibe-splash copies it straight into a DRM dumb buffer
#      splash-font.bin  a glyph atlas (VSF1: height, then for ASCII 32..126 a width byte + width×height alpha bytes)
#      splash-preview.png   for people (README, review)
# ★ The artwork is the 11.0 app icon (assets/icon.png) on black — the same mark the page's own start screen shows — and
#   the wordmark in Atkinson Hyperlegible (assets/fonts), the client's legible UI face. 640×480 is the panel.
import os, sys, zlib
from PIL import Image, ImageDraw, ImageFont

repo, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
W, H = 640, 480
BAR_X, BAR_Y, BAR_W, BAR_H = 160, 372, 320, 6      # must match vibe-splash
FONT = os.path.join(repo, "assets/fonts/AtkinsonHyperlegible-Regular.ttf")

icon = Image.open(os.path.join(repo, "assets/icon.png")).convert("RGB")
# ★ the screen is the icon's own corner colour (7,9,7 — not pure black), so the icon has no visible square around it
BG = icon.getpixel((2, 2))
icon = icon.resize((232, 232), Image.LANCZOS)
img = Image.new("RGB", (W, H), BG)
img.paste(icon, ((W - 232) // 2, 64))
d = ImageDraw.Draw(img)
word = ImageFont.truetype(FONT, 40)
t = "VibeSDR"
tw = d.textlength(t, font=word)
d.text(((W - tw) / 2, 306), t, font=word, fill=(0xE8, 0xF5, 0xEC))
img.save(os.path.join(out, "splash-base.png"))

raw = bytearray()
for r, g, b in img.getdata():
    raw += bytes((b, g, r, 0xFF))
with open(os.path.join(out, "splash.bgrx.z"), "wb") as f:
    f.write(zlib.compress(bytes(raw), 9))

# ── the glyph atlas ──
font = ImageFont.truetype(FONT, 20)
asc, desc = font.getmetrics()
gh = asc + desc
atlas = bytearray(b"VSF1" + bytes([gh]))
for ch in range(32, 127):
    c = chr(ch)
    w = max(1, int(round(font.getlength(c))))
    g = Image.new("L", (w, gh), 0)
    ImageDraw.Draw(g).text((0, 0), c, font=font, fill=255)
    atlas += bytes([w]) + g.tobytes()
with open(os.path.join(out, "splash-font.bin"), "wb") as f:
    f.write(atlas)

# ── a preview of a boot in progress ──
p = img.copy()
pd = ImageDraw.Draw(p)
pd.rectangle([BAR_X, BAR_Y, BAR_X + BAR_W - 1, BAR_Y + BAR_H - 1], fill=(0x1C, 0x33, 0x22))
pd.rectangle([BAR_X, BAR_Y, BAR_X + int(BAR_W * 0.6) - 1, BAR_Y + BAR_H - 1], fill=(0x5C, 0xE0, 0x7A))
m = "Radio settings reset"
pd.text(((W - pd.textlength(m, font=font)) / 2, 400), m, font=font, fill=(0xE8, 0xF5, 0xEC))
p.save(os.path.join(out, "splash-preview.png"))
print("splash: %d bytes compressed, font %d px tall, %d bytes" % (os.path.getsize(os.path.join(out, "splash.bgrx.z")), gh, len(atlas)))
