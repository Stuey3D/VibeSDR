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

# ★★★ THE CONTROLS ON THE BOOT SCREEN, EVERY BOOT (Stuart, 2026-11-07: "make it the permanent boot screen then anybody
#     goes back to it gets a reminder. Keep the status bar"). 2026-11-11: no longer a portrait outline of the R36S with
#     callouts (an odd shape on a landscape panel, and the shoulder labels never had room) — the logo on the left, a
#     row per control on the right with a small drawing of that control (see the LANDSCAPE note below).
# ★★ What each control does is the native UI's (handheld-ui/src/app_impl.h press / pollInput / holdSweep, and
#    sec_controller.h's CONTROLLER GUIDE). If a button moves there, fix this picture too (AGENTS.md).
# ★ Drawn at 2× and scaled down: the outlines are antialiased like the font.
S = 2
big = Image.new("RGB", (W * S, H * S), BG)
d = ImageDraw.Draw(big)
def F(path, px): return ImageFont.truetype(path, px * S)
f_key, f_txt, f_word, f_small = F(BOLD, 15), F(FONT, 14), F(BOLD, 30), F(FONT, 13)

# ★★★ LANDSCAPE (Stuart, 2026-11-11: "the glyph of the R36S is a bit of an odd shape for a Landscape screen. Maybe Have
#     the VibeSDR logo on the left and a list of the controls on the right with little glyphs for the actual controls
#     instead"). The logo + wordmark fill the left; each control is a row on the right: a small drawing of THAT control,
#     its name in green, what it does. Order and words = sec_controller.h's CONTROLLER GUIDE (L1 · R1 zoom, L2 · R2 tune
#     a step / hold to sweep, the sticks turn like dials) + the power button, the power-save leaf and the boot reset
#     (vibe-bootkeys). Everything sits above the bar (BAR_Y) and the message under it (vibe-splash msg_y).
import math
def R(v): return [c * S for c in v]
def rr(x, y, w, h, r, width=2, fill=None, outline=GREEN):
    d.rounded_rectangle(R([x, y, x + w, y + h]), radius=r * S, outline=outline, width=max(1, int(width * S)), fill=fill)
def circ(cx, cy, r, width=2, outline=GREEN, fill=None):
    d.ellipse(R([cx - r, cy - r, cx + r, cy + r]), outline=outline, width=max(1, int(width * S)), fill=fill)

# ── the left: the icon and the wordmark ──
LW = 200                                             # the left column's width
ic = icon.resize((132 * S, 132 * S), Image.LANCZOS)
big.paste(ic, ((LW // 2 - 66) * S, 96 * S))
f_big = F(BOLD, 38)
word = "VibeSDR"
d.text(((LW * S - d.textlength(word, font=f_big)) / 2, 240 * S), word, font=f_big, fill=TXT)
sub = "the radio in your pocket"
d.text(((LW * S - d.textlength(sub, font=f_small)) / 2, 288 * S), sub, font=f_small, fill=TXT_DIM)
d.line(R([LW + 2, 22, LW + 2, 350]), fill=GREEN_DIM, width=S)

# ── the glyphs (each fits a 34 × 26 box at (x, y)) ──
f_btn = F(BOLD, 11)          # ★ the button names INSIDE the glyphs (Stuart: the shoulders' words never read at 9 px)
def pill(x, y, w, label, h=15):
    rr(x, y, w, h, h / 2, width=1.5)
    d.text(((x + w / 2) * S - d.textlength(label, font=f_btn) / 2, (y + (h - 13) / 2) * S), label, font=f_btn, fill=GREEN)
def tab(x, y, w, label):
    # a shoulder button: rounded on top, square underneath (the R36S's L / R tabs)
    d.rounded_rectangle(R([x, y, x + w, y + 17]), radius=6 * S, outline=GREEN, width=int(1.5 * S), corners=(True, True, False, False))
    d.text(((x + w / 2) * S - d.textlength(label, font=f_btn) / 2, (y + 2) * S), label, font=f_btn, fill=GREEN)
def g_shoulders(x, y, a_, b_):
    tab(x, y + 5, 24, a_); tab(x + 28, y + 5, 24, b_)
def g_dpad(x, y):
    cx, cy, a_, b_ = x + 26, y + 13, 4, 11
    pts = [(cx - a_, cy - b_), (cx + a_, cy - b_), (cx + a_, cy - a_), (cx + b_, cy - a_), (cx + b_, cy + a_), (cx + a_, cy + a_),
           (cx + a_, cy + b_), (cx - a_, cy + b_), (cx - a_, cy + a_), (cx - b_, cy + a_), (cx - b_, cy - a_), (cx - a_, cy - a_)]
    d.polygon([c * S for p_ in pts for c in p_], outline=GREEN, width=int(1.6 * S))
def g_face(x, y, solid):
    cx, cy, g_, r_ = x + 26, y + 13, 8, 4.2
    for k, (dx, dy) in {"top": (0, -g_), "left": (-g_, 0), "right": (g_, 0), "bottom": (0, g_)}.items():
        circ(cx + dx, cy + dy, r_, width=1.4, fill=GREEN if k == solid else None)
def g_stick(x, y):
    cx, cy = x + 26, y + 13
    circ(cx, cy, 7, width=1.6); circ(cx, cy, 3, width=1.2, outline=GREEN_DIM)
    r_ = 11.5; a0, a1 = 200, 340                     # a clockwise arc over the top, its head on the right
    d.arc(R([cx - r_, cy - r_, cx + r_, cy + r_]), a0, a1, fill=GREEN, width=int(1.6 * S))
    t = math.radians(a1); ex, ey = cx + r_ * math.cos(t), cy + r_ * math.sin(t)
    tx, ty = -math.sin(t), math.cos(t); nx, ny = math.cos(t), math.sin(t)
    pts = [(ex + tx * 5, ey + ty * 5), (ex - tx * 1 + nx * 3.5, ey - ty * 1 + ny * 3.5), (ex - tx * 1 - nx * 3.5, ey - ty * 1 - ny * 3.5)]
    d.polygon([c * S for p_ in pts for c in p_], fill=GREEN)
def g_select(x, y):
    pill(x + 2, y + 5, 48, "SELECT")
def g_start(x, y):
    pill(x + 2, y + 5, 48, "START")
def g_power(x, y):
    cx, cy, r_ = x + 26, y + 14, 8
    d.arc(R([cx - r_, cy - r_, cx + r_, cy + r_]), -60, 240, fill=GREEN, width=int(1.8 * S))
    d.line(R([cx, cy - r_ - 2, cx, cy - 1]), fill=GREEN, width=int(1.8 * S))
def g_leaf(x, y):
    # the status row's leaf (vibe-hhui): two halves either side of a midrib, tip up-right, a short stem
    k = 1.25; x += 17; y += 3
    bx, by, tx, ty = x + 3 * k, y + 13 * k, x + 14 * k, y + 2 * k
    d.polygon([c * S for c in (bx, by, tx, ty, x + 4 * k, y + 4 * k)], fill=GREEN)
    d.polygon([c * S for c in (bx, by, tx, ty, x + 13 * k, y + 12 * k)], fill=GREEN)
    d.line(R([bx + 1, by - 1, tx - 2, ty + 2]), fill=BG, width=max(1, int(1.2 * S)))
    d.line(R([x + 1, y + 15 * k, bx + 0.5, by - 0.5]), fill=GREEN, width=int(1.6 * S))
def g_reset(x, y):
    pill(x + 2, y - 2, 48, "START", h=14); pill(x + 2, y + 14, 48, "SELECT", h=14)

ROWS = [
    (lambda x, y: g_shoulders(x, y, "L1", "R1"), "L1 · R1", "zoom out · zoom in"),
    (lambda x, y: g_shoulders(x, y, "L2", "R2"), "L2 · R2", "tune down · up a step — hold to sweep"),
    (g_dpad, "D-PAD", "move the highlight · left / right adjust"),
    (lambda x, y: g_face(x, y, "bottom"), "BOTTOM BUTTON", "press the highlighted control"),
    (lambda x, y: g_face(x, y, "right"), "RIGHT BUTTON", "back · close"),
    (g_stick, "LEFT STICK", "turn it like a dial to tune · clockwise = up"),
    (g_stick, "RIGHT STICK", "turn it like a dial to zoom · clockwise = in"),
    (g_select, "SELECT", "the menu (again to close it)"),
    (g_start, "START", "type a frequency · START again to tune"),
    (g_power, "POWER", "tap: screen off / on · hold: switch off"),
    (g_leaf, "POWER SAVE", "untouched a minute: the waterfall slows"),
    (g_reset, "START + SELECT", "held while switching on: reset the radio"),
]
GX, TX, Y0, RH = LW + 12, LW + 72, 4, 30   # ★ twelve rows above the bar (BAR_Y 372)
TMAX = W - 8 - TX
for i, (glyph, key, what) in enumerate(ROWS):
    y = Y0 + i * RH
    glyph(GX, y + 2)
    d.text((TX * S, y * S), key, font=f_key, fill=GREEN)
    fw = f_small
    if d.textlength(what, font=fw) / S > TMAX:
        raise SystemExit("splash: row %r too wide (%d > %d px)" % (what, d.textlength(what, font=fw) / S, TMAX))
    d.text((TX * S, (y + 16) * S), what, font=fw, fill=TXT)
if Y0 + len(ROWS) * RH > BAR_Y - 4:
    raise SystemExit("splash: the rows run into the bar")

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
