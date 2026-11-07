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
BOLD = os.path.join(repo, "assets/fonts/AtkinsonHyperlegible-Bold.ttf")
GREEN = (0x5C, 0xE0, 0x7A)      # the icon's green (vibe-splash BAR_ON)
GREEN_DIM = (0x2E, 0x70, 0x3D)
TXT = (0xE8, 0xF5, 0xEC)
TXT_DIM = (0xA8, 0xB8, 0xAC)

# ★★★ THE CONTROLS ON THE BOOT SCREEN, EVERY BOOT (Stuart, 2026-11-07: "a little glyph of the R36S next to the VibeSDR
#     logo with the controls explained … make it the permanent boot screen then anybody goes back to it gets a
#     reminder. Keep the status bar"; "a green outline of the R36S and its controls with arrows pointing to the
#     controls explaining how they work, including the arrows to indicate how to use the analogue sticks").
# ★★ What each control does is the native UI's (handheld-ui/src/app_impl.h press / pollInput / holdSweep, and
#    sec_controller.h's CONTROLLER GUIDE). If a button moves there, fix this picture too (AGENTS.md).
# ★ Drawn at 2× and scaled down: the outline and the arrows are antialiased like the font.
S = 2
big = Image.new("RGB", (W * S, H * S), BG)
d = ImageDraw.Draw(big)
def F(path, px): return ImageFont.truetype(path, px * S)
f_key, f_txt, f_word, f_small = F(BOLD, 15), F(FONT, 14), F(BOLD, 30), F(FONT, 13)

# ── the header: the icon + the wordmark, side by side ──
ic = icon.resize((44 * S, 44 * S), Image.LANCZOS)
word = "VibeSDR"
ww = d.textlength(word, font=f_word)
hx = (W * S - (44 * S + 12 * S + ww)) / 2
big.paste(ic, (int(hx), 10 * S))
d.text((hx + 56 * S, 14 * S), word, font=f_word, fill=TXT)

# ── the R36S (portrait: screen, D-pad + face buttons, the two sticks, SELECT / START; shoulders on top) ──
DX, DY, DW, DH = 238, 82, 164, 270
def R(v): return [c * S for c in v]
def rr(x, y, w, h, r, width=2, fill=None, outline=GREEN):
    d.rounded_rectangle(R([x, y, x + w, y + h]), radius=r * S, outline=outline, width=width * S, fill=fill)
def circ(cx, cy, r, width=2, outline=GREEN, fill=None):
    d.ellipse(R([cx - r, cy - r, cx + r, cy + r]), outline=outline, width=width * S, fill=fill)
# shoulders: L2 behind L1 (taller), R2 behind R1
rr(DX + 10, DY - 14, 46, 20, 6); rr(DX + 14, DY - 7, 40, 14, 5, fill=BG)
rr(DX + DW - 56, DY - 14, 46, 20, 6); rr(DX + DW - 54, DY - 7, 40, 14, 5, fill=BG)
rr(DX, DY, DW, DH, 18, width=3, fill=BG)                       # the body
SX, SY, SW, SH = DX + 14, DY + 14, DW - 28, 104                 # the screen (4:3)
rr(SX, SY, SW, SH, 4, width=2, outline=GREEN_DIM)
mi = icon.resize((60 * S, 60 * S), Image.LANCZOS)               # the mark on the little screen
big.paste(mi, ((SX + SW // 2 - 30) * S, (SY + SH // 2 - 30) * S))
# D-pad (a plus)
PX, PY = DX + 40, DY + 152
d.polygon([(c * S) for xy in [(PX - 6, PY - 18), (PX + 6, PY - 18), (PX + 6, PY - 6), (PX + 18, PY - 6), (PX + 18, PY + 6),
                                (PX + 6, PY + 6), (PX + 6, PY + 18), (PX - 6, PY + 18), (PX - 6, PY + 6), (PX - 18, PY + 6),
                                (PX - 18, PY - 6), (PX - 6, PY - 6)] for c in xy], outline=GREEN, width=2 * S)
# face buttons (a diamond); BOTTOM and RIGHT are the two the receiver uses — filled
FX, FY, FR, FG2 = DX + DW - 40, DY + 152, 7, 14
face = {"top": (FX, FY - FG2), "left": (FX - FG2, FY), "right": (FX + FG2, FY), "bottom": (FX, FY + FG2)}
for k, (cx, cy) in face.items():
    circ(cx, cy, FR, fill=GREEN if k in ("bottom", "right") else None)
# the sticks
LX, LY, RX, RY, SR = DX + 52, DY + 214, DX + DW - 52, DY + 214, 15
for cx, cy in ((LX, LY), (RX, RY)):
    circ(cx, cy, SR); circ(cx, cy, 7, width=2, outline=GREEN_DIM)
# SELECT / START
BX1, BX2, BY = DX + DW // 2 - 22, DX + DW // 2 + 6, DY + DH - 22
rr(BX1, BY, 16, 7, 3); rr(BX2, BY, 16, 7, 3)

# ── the turn arrows round each stick: clockwise = up (tune up / zoom in) ──
import math
def turn_arrow(cx, cy, r):
    a0, a1 = 215, 505                                          # degrees, clockwise on screen (PIL: 0 = 3 o'clock, +cw)
    d.arc(R([cx - r, cy - r, cx + r, cy + r]), a0, a1 - 360 if a1 > 360 else a1, fill=GREEN, width=2 * S)
    for end, sgn in ((a1, 1), (a0, -1)):                       # a head at each end: turn either way
        t = math.radians(end)
        ex, ey = cx + r * math.cos(t), cy + r * math.sin(t)
        tx, ty = -math.sin(t) * sgn, math.cos(t) * sgn          # the direction of travel there
        nx, ny = math.cos(t), math.sin(t)
        L = 10
        pts = [(ex + tx * L, ey + ty * L), (ex - tx * 2 + nx * 7, ey - ty * 2 + ny * 7), (ex - tx * 2 - nx * 7, ey - ty * 2 - ny * 7)]
        d.polygon([c * S for p_ in pts for c in p_], fill=GREEN)
turn_arrow(LX, LY, SR + 9)
turn_arrow(RX, RY, SR + 9)

# ── the callouts: a label at the side, a line to the control with a dot on it ──
def callout(side, ly, tx, ty, key, what):
    """side 'l' / 'r'; (tx, ty) = the control; the label's first line sits at ly."""
    edge = DX - 14 if side == "l" else DX + DW + 14
    lx_text_end = edge - 6 if side == "l" else edge + 6
    kw = d.textlength(key, font=f_key) / S
    wl = [w_ for w_ in what.split("\n")]
    if side == "l":
        d.text(((lx_text_end - kw) * S, ly * S), key, font=f_key, fill=GREEN)
        for i, l in enumerate(wl):
            d.text(((lx_text_end - d.textlength(l, font=f_txt) / S) * S, (ly + 18 + i * 16) * S), l, font=f_txt, fill=TXT)
    else:
        d.text((lx_text_end * S, ly * S), key, font=f_key, fill=GREEN)
        for i, l in enumerate(wl):
            d.text((lx_text_end * S, (ly + 18 + i * 16) * S), l, font=f_txt, fill=TXT)
    ay = ly + 9
    d.line(R([edge, ay, edge + (8 if side == "l" else -8), ay]), fill=GREEN_DIM, width=2 * S)
    d.line(R([edge + (8 if side == "l" else -8), ay, tx, ty]), fill=GREEN_DIM, width=2 * S)
    circ(tx, ty, 3, width=1, fill=GREEN)

callout("l", 58,  DX + 30, DY - 10, "L1 · L2", "zoom out · tune down\nhold L2 to sweep down")
callout("l", 126, PX - 18, PY, "D-PAD", "move the highlight\nleft / right adjust")
callout("l", 196, LX - SR - 9, LY, "LEFT STICK", "turn it like a dial to tune\nclockwise = up")
callout("l", 272, BX1 + 2, BY + 4, "SELECT", "the menu")
callout("r", 58,  DX + DW - 30, DY - 10, "R1 · R2", "zoom in · tune up\nhold R2 to sweep up")
callout("r", 112, FX + FG2 + FR, FY, "RIGHT BUTTON", "back · close")
callout("r", 160, FX + 4, FY + FG2 + FR - 2, "BOTTOM BUTTON", "press · select")
callout("r", 214, RX + SR + 9, RY, "RIGHT STICK", "turn it to zoom\nclockwise = in")
# the power button (on the case's edge, so a note rather than an arrow)
pw = "POWER: tap = screen off / on · hold = switch off"
d.text(((W * S - d.textlength(pw, font=f_small)) / 2, 356 * S), pw, font=f_small, fill=TXT_DIM)

img = big.resize((W, H), Image.LANCZOS)
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
