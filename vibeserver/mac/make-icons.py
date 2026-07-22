#!/usr/bin/env python3
"""Derive the Mac app's icons from the VibeServer family icon.

Two outputs, and they are different KINDS of image:

  AppIcon.icns   — the full-colour family icon (green radio + triangle-node tile), for the Finder,
                   the Dock and About. Straight from spike/tools/make_family_icons.py.

  MenuBar*.png   — a macOS TEMPLATE image: pure black with an alpha channel and NO colour of its
                   own. macOS tints it automatically — black on a light menu bar, white on a dark
                   one — which is why it must not be the green artwork. Anything coloured here
                   would stay coloured and look wrong in one appearance or the other.

The template is derived from the GREEN line art only. The icon's black tile already cuts a notch
out of the radio, so the triangle-node sits in its own space without us drawing a separator.

★ Menu-bar icons are ~18pt. A straight downscale of 12px strokes leaves them nearly invisible, so
the mask is dilated before resizing — the glyph survives at 1x instead of dissolving into grey.

  python3 vibeserver/mac/make-icons.py
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GEN = os.path.join(ROOT, "spike", "tools", "make_family_icons.py")

# The family green. Everything near it is the artwork; everything else is background or tile.
GREEN = (86, 255, 132)


def source_icon(path):
    """Render the VibeServer family icon (green radio + triangle-node tile)."""
    subprocess.run([sys.executable, GEN, "vibeserver", path], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)
    return Image.open(path).convert("RGBA")


def green_mask(im):
    """Alpha mask of the green line art — the shape of the glyph, with the tile's notch included."""
    px = im.load()
    w, h = im.size
    mask = Image.new("L", (w, h), 0)
    mp = mask.load()
    for y in range(h):
        for x in range(w):
            r, g, b, _ = px[x, y]
            # Generous: the artwork is antialiased, so accept anything clearly green-dominant.
            if g > 120 and g > r + 40 and g > b + 30:
                mp[x, y] = 255
    return mask


def trim(mask):
    """Crop to the artwork so the glyph fills its box instead of floating in the icon's margins."""
    box = mask.getbbox()
    return mask.crop(box) if box else mask


def template(mask, size, thicken):
    """Black + alpha at `size`, strokes thickened first so they survive the downscale."""
    m = mask.filter(ImageFilter.MaxFilter(thicken)) if thicken >= 3 else mask
    # Square it on the longer edge so the aspect ratio is kept and it centres in the menu bar.
    w, h = m.size
    side = max(w, h)
    sq = Image.new("L", (side, side), 0)
    sq.paste(m, ((side - w) // 2, (side - h) // 2))
    sq = sq.resize((size, size), Image.LANCZOS)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.putalpha(sq)          # black pixels, alpha = the glyph. macOS supplies the colour.
    return out


def main():
    res = os.path.join(HERE, "Resources")
    os.makedirs(res, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        src_png = os.path.join(tmp, "vibeserver.png")
        im = source_icon(src_png)

        # ── Menu-bar template, at the three scale factors macOS may ask for ───────────────
        mask = trim(green_mask(im))
        for scale, px, thicken in ((1, 18, 9), (2, 36, 5), (3, 54, 3)):
            name = "MenuBar.png" if scale == 1 else f"MenuBar@{scale}x.png"
            template(mask, px, thicken).save(os.path.join(res, name))
            print("wrote", name)

        # ── App icon (.icns) ─────────────────────────────────────────────────────────────
        iconset = os.path.join(tmp, "AppIcon.iconset")
        os.makedirs(iconset)
        for px in (16, 32, 64, 128, 256, 512, 1024):
            im.resize((px, px), Image.LANCZOS).save(os.path.join(iconset, f"icon_{px}x{px}.png"))
            if px <= 512:   # @2x companions
                im.resize((px * 2, px * 2), Image.LANCZOS).save(
                    os.path.join(iconset, f"icon_{px}x{px}@2x.png"))
        icns = os.path.join(HERE, "AppIcon.icns")
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", icns], check=True)
        print("wrote AppIcon.icns")


if __name__ == "__main__":
    main()
