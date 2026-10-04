#!/usr/bin/env python3
"""colour_icons.py — the app icon and the Now Playing art in every illumination colour (Stuart, 2026-10-04).

The icon and album art were green only, and green drew criticism; the user picks their buttons' colour, so the
icon and art can follow. Each colour is recoloured from the SAME masters as the shipped icon (icon-ios.svg,
artwork.svg), with the mapping Stuart approved on the contact sheet ("they look good"):
  • the lit strokes (#66E07C)   → the app's own LED core for that colour (src/constants/faceplate.ts LED);
  • the pale tints (L > 0.8)    → that LED's white-hot centre;
  • the faintly tinted darks    → the LED's hue at the original lightness, a little richer (white: barely tinted).
GREEN is the shipped icon and art, untouched — nobody who never picks a colour sees any change.

Writes (run from the repo root):  python3 assets/brand/colour_icons.py
  ios/VibeSDR/Images.xcassets/AppIcon-<Colour>.appiconset   iOS alternate icons (1024, no alpha)
  ios/VibeSDR/Images.xcassets/artwork_<colour>.imageset     Now Playing art base, 600 x 600
  ios/VibeSDR/Images.xcassets/logo_vibeserver[_<colour>].imageset   the 11.0 VibeServer mark inlaid on that art
  ios/VibeSDRWatch/Assets.xcassets/AppIcon.appiconset/AppIcon.png  Buddy's watch icon, fixed amber
Needs rsvg-convert (brew install librsvg) and Pillow.
"""
import colorsys, json, os, re, subprocess, tempfile
from PIL import Image

R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BRAND = os.path.join(R, 'assets', 'brand')
# The app's LEDs (faceplate.ts LED): core = the lit colour, hot = its white-hot centre. GREEN is the shipped art.
LED = {'red': ('#ff3a2e', '#ffcbc6'), 'amber': ('#ffae1a', '#ffe4b3'), 'blue': ('#3d9bff', '#d0e7ff'),
       'white': ('#eef3ff', '#ffffff'), 'teal': ('#46ffd7', '#c8fff2'), 'neon': ('#ff7a26', '#ffc48a')}


def h2r(h): h = h.lstrip('#'); return tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))
def r2h(r): return '#%02x%02x%02x' % tuple(round(max(0, min(1, c)) * 255) for c in r)


def recolour(svg, colour):
    core, hot = LED[colour]
    ch = colorsys.rgb_to_hls(*h2r(core))[0]
    def sub(m):
        src = m.group(0); _, ll, ss = colorsys.rgb_to_hls(*h2r(src))
        if ss < 0.02: return src
        if src.lower() == '#66e07c': return core
        if ll > 0.8: return hot
        return r2h(colorsys.hls_to_rgb(ch, ll, min(1, ss * (0.4 if colour == 'white' else 1.6))))
    return re.sub(r'#[0-9a-fA-F]{6}', sub, svg)


def render(svg_text, size):
    fd, p = tempfile.mkstemp(suffix='.svg'); os.write(fd, svg_text.encode()); os.close(fd)
    out = p + '.png'
    subprocess.run(['rsvg-convert', '-w', str(size), '-h', str(size), p, '-o', out], check=True)
    im = Image.open(out).convert('RGBA'); os.remove(p); os.remove(out)
    bg = Image.new('RGB', im.size, (3, 4, 3)); bg.paste(im, (0, 0), im)   # ★ App Store icons: no alpha
    return bg


def icon_shape(im, n=5.0):
    """The icon's own outline (Apple's continuous corner, superellipse n = 5 — family_icons.py se_alpha), so the
    inlay reads as a small app icon on the art, not a dark square."""
    from PIL import ImageDraw
    import math
    sz = im.size[0]; S = sz * 4; m = Image.new('L', (S, S), 0); a = S / 2; pts = []
    for i in range(1440):
        t = 2 * math.pi * i / 1440; co, si = math.cos(t), math.sin(t)
        pts.append((a + a * math.copysign(abs(co) ** (2 / n), co), a + a * math.copysign(abs(si) ** (2 / n), si)))
    ImageDraw.Draw(m).polygon(pts, fill=255)
    o = im.convert('RGBA'); o.putalpha(m.resize((sz, sz), Image.LANCZOS)); return o


def vibeserver_mark(base_svg, colour=None):
    """The 11.0 VibeServer mark — the app icon with the glowing node plate — built exactly as family_icons.py builds
    assets/vibeserver-icon.png (the green rebuild matches it pixel for pixel), then recoloured whole. Stuart
    (2026-10-04): "the full 11 artwork including on the inlaid art" — it replaced the older radio-and-node mark."""
    G = '#66E07C'; X, Y, W = 676, 668, 264; cx, cy, sc = X + W / 2, Y + W / 2, W / 264
    glyph = (f'<g transform="translate({cx} {cy}) scale({sc})"><g stroke="{G}" stroke-width="13" stroke-linecap="round">'
             f'<line x1="-62" y1="-50" x2="62" y2="-50"/><line x1="-62" y1="-50" x2="0" y2="62"/><line x1="62" y1="-50" x2="0" y2="62"/></g>'
             f'<g fill="{G}"><circle cx="-62" cy="-50" r="24"/><circle cx="62" cy="-50" r="24"/><circle cx="0" cy="62" r="24"/></g></g>')
    plate = f'<rect x="{X}" y="{Y}" width="{W}" height="{W}" rx="{W*46/264:.1f}" fill="#040605" stroke="{G}" stroke-width="10"/>'
    svg = base_svg.replace('</svg>', f'<g filter="url(#glow)">{plate}{glyph}</g></svg>')
    return recolour(svg, colour) if colour else svg


def buddy_icon(base_svg, colour=None):
    """Buddy (the iPhone app's watch companion): the round master with the phone glyph — family_icons.py's 'phone'
    on legacy-round.svg; the green rebuild matches the shipped icon pixel for pixel. ★ watchOS has NO alternate
    icons, so Buddy cannot follow the user's pick; it is fixed AMBER (Stuart, 2026-10-04: "the opposite choice in
    the app against the green") so it stands apart from Jr's green on the watch."""
    G = '#66E07C'; X, Y, W = 596, 598, 232; cx, cy, sc = X + W / 2, Y + W / 2, W / 264
    glyph = (f'<g transform="translate({cx} {cy}) scale({sc})"><rect x="-58" y="-100" width="116" height="200" rx="26" fill="none" stroke="{G}" stroke-width="13"/>'
             f'<rect x="-22" y="-80" width="44" height="13" rx="6.5" fill="{G}"/><rect x="-24" y="74" width="48" height="9" rx="4.5" fill="{G}"/></g>')
    plate = f'<rect x="{X}" y="{Y}" width="{W}" height="{W}" rx="{W*46/264:.1f}" fill="#040605" stroke="{G}" stroke-width="10"/>'
    svg = base_svg.replace('</svg>', f'<g filter="url(#glow)">{plate}{glyph}</g></svg>')
    return recolour(svg, colour) if colour else svg


def main():
    icon, art = open(os.path.join(BRAND, 'icon-ios.svg')).read(), open(os.path.join(BRAND, 'artwork.svg')).read()
    xc = os.path.join(R, 'ios', 'VibeSDR', 'Images.xcassets')
    # ★ The green inlay is the 11.0 mark too (it was the older radio-and-node art until 2026-10-04).
    icon_shape(render(vibeserver_mark(icon), 1024).resize((320, 320), Image.LANCZOS)).save(
        os.path.join(xc, 'logo_vibeserver.imageset', 'vibeserver.png'), optimize=True)
    # ★ Buddy, fixed amber (see buddy_icon).
    render(buddy_icon(open(os.path.join(BRAND, 'legacy-round.svg')).read(), 'amber'), 1024).save(
        os.path.join(R, 'ios', 'VibeSDRWatch', 'Assets.xcassets', 'AppIcon.appiconset', 'AppIcon.png'), optimize=True)
    for c in LED:
        d = os.path.join(xc, f'AppIcon-{c.capitalize()}.appiconset'); os.makedirs(d, exist_ok=True)
        render(recolour(icon, c), 1024).save(os.path.join(d, 'App-Icon-1024x1024@1x.png'), optimize=True)
        json.dump({'images': [{'filename': 'App-Icon-1024x1024@1x.png', 'idiom': 'universal', 'platform': 'ios',
                               'size': '1024x1024'}], 'info': {'version': 1, 'author': 'xcode'}},
                  open(os.path.join(d, 'Contents.json'), 'w'), indent=2)
        d = os.path.join(xc, f'artwork_{c}.imageset'); os.makedirs(d, exist_ok=True)
        render(recolour(art, c), 600).save(os.path.join(d, f'artwork_{c}.png'), optimize=True)
        json.dump({'images': [{'filename': f'artwork_{c}.png', 'idiom': 'universal'}],
                   'info': {'version': 1, 'author': 'xcode'}}, open(os.path.join(d, 'Contents.json'), 'w'), indent=2)
        # ★ The inlaid VibeServer mark on the art follows the colour too (Stuart); other server types keep their own.
        d = os.path.join(xc, f'logo_vibeserver_{c}.imageset'); os.makedirs(d, exist_ok=True)
        icon_shape(render(vibeserver_mark(icon, c), 1024).resize((320, 320), Image.LANCZOS)).save(
            os.path.join(d, f'logo_vibeserver_{c}.png'), optimize=True)
        json.dump({'images': [{'filename': f'logo_vibeserver_{c}.png', 'idiom': 'universal'}],
                   'info': {'version': 1, 'author': 'xcode'}}, open(os.path.join(d, 'Contents.json'), 'w'), indent=2)
        print('wrote', c)


if __name__ == '__main__':
    main()
