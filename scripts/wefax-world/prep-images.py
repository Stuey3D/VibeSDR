#!/usr/bin/env python3
"""prep-images.py — turn published radiofax products into harness charts (2026-10-07).

Each product is converted to 8-bit greyscale and scaled to the WEFAX line at 120 LPM / IOC 576 — 1809 px wide,
aspect kept (IOC 576 has square pixels) — the way it would fill a received line. Long pages are cut to MAX_LINES.
★ This is the WORST case for the aligner: whatever white surround the transmitter's own scan adds on air is not
  there, so every map reaches both ends of the line.

Usage: prep-images.py <out_dir> <src> [<src> ...]     (needs Pillow; used once, offline — the harness reads PNG)
"""
import os
import sys

from PIL import Image

W = 1809
MAX_LINES = 1600


def prep(src: str, out_dir: str) -> str:
    im = Image.open(src).convert('L')
    h = round(im.height * W / im.width)
    im = im.resize((W, h), Image.LANCZOS)
    if h > MAX_LINES:
        im = im.crop((0, 0, W, MAX_LINES))
    name = os.path.splitext(os.path.basename(src))[0] + '.png'
    out = os.path.join(out_dir, name)
    # ★ 16 greys, stored as a 4-bit palette PNG: a line chart is 1-bit at source and loses nothing; a satellite
    #   image keeps more grey levels than a received HF line resolves (σ 30 of noise is added by the harness anyway),
    #   at a quarter of the size in the repo.
    im = im.point(lambda v: (v + 8) // 17)
    pal = Image.frombytes('P', im.size, im.tobytes())
    pal.putpalette([c for v in range(16) for c in (v * 17, v * 17, v * 17)])
    pal.save(out, optimize=True, bits=4)
    return out


if __name__ == '__main__':
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    for s in sys.argv[2:]:
        print(prep(s, out_dir))
