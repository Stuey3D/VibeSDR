"""Rebrand icon builder — Stuart's set (Claude on the web, 2026-10-03), with the rim drawn on EACH mask's own curve.
iOS: Apple's continuous corner (superellipse n=5) · Play: Google's 20 % rounded square · Android adaptive: circle
(the 72 dp viewport's inscribed circle — complete and evenly lit under every launcher mask) · legacy Android: square
and round, drawn whole."""
import math, re, subprocess, os, sys
SRC=os.environ['SRC']; OUT=os.environ['OUT']
os.makedirs(OUT, exist_ok=True)
def superellipse(cx, cy, a, n=5.0, steps=720):
    pts=[]
    for i in range(steps):
        t=2*math.pi*i/steps; c=math.cos(t); s=math.sin(t)
        x=cx+a*math.copysign(abs(c)**(2/n), c); y=cy+a*math.copysign(abs(s)**(2/n), s)
        pts.append(f'{x:.2f},{y:.2f}')
    return 'M'+' L'.join(pts)+' Z'
ios=open(f'{SRC}/icon-ios.svg').read()
RIM_OLD='<rect x="12" y="12" width="1000" height="1000" rx="222"/>'
assert ios.count(RIM_OLD)==2  # the clip AND the drawn rim
# ★ Rim and clip both follow Apple's mask, inset 12 (the set's own inset) — an even gap all the way round.
se=superellipse(512,512,500)
ios_se=ios.replace(RIM_OLD, f'<path d="{se}"/>')
open(f'{OUT}/icon-ios.svg','w').write(ios_se)
# Play: the same art, rim on a 25 % rounded square, inset 40 (2026-10-05). ★ At Google's 20 % with the set's 12 px inset
#   the rim's stroke and glow reached the mask, and Play's listing preview CLIPPED all four corners (Stuart: "we need
#   one with more rounded corners to avoid the clipping"). 40 px of background is left for Play's mask to trim.
INSET=40; r=0.25*1024-INSET
play=ios.replace(RIM_OLD, f'<rect x="{INSET}" y="{INSET}" width="{1024-2*INSET}" height="{1024-2*INSET}" rx="{r:.1f}"/>')
open(f'{OUT}/icon-play.svg','w').write(play)
# Android adaptive background: the set's background plus a circle rim at the viewport's edge, inset 12.
bg=open(f'{SRC}/adaptive-background.svg').read()
glow='<filter id="rimglow" x="-30%" y="-30%" width="160%" height="160%"><feGaussianBlur stdDeviation="9" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>'
bg=bg.replace('</defs>', glow+'</defs>',1)
# ★ The window and its outline end AT the ring, as the iOS icon's end at its rim — on a squircle launcher they
#   otherwise run on past the ring into the corners.
bg=bg.replace('</defs>','<clipPath id="ringclip"><circle cx="512" cy="512" r="500"/></clipPath></defs>',1)
bg=bg.replace('<polygon points="47,-256 313,571 711,571 977,-256" fill="url(#win)"/>','<polygon points="47,-256 313,571 711,571 977,-256" fill="url(#win)" clip-path="url(#ringclip)"/>',1)
bg=bg.replace('<polyline points="47,-256 313,571 711,571 977,-256" stroke-linejoin="round"/>','<polyline points="47,-256 313,571 711,571 977,-256" stroke-linejoin="round" clip-path="url(#ringclip)"/>',1)
assert bg.count('ringclip')==3
bg=bg.replace('</svg>','<circle cx="512" cy="512" r="500" fill="none" stroke="#66E07C" stroke-width="13" filter="url(#rimglow)"/></svg>')
open(f'{OUT}/adaptive-background.svg','w').write(bg)
# Legacy (pre-Android 8) launcher icons: drawn whole — a rounded square, and a circle on transparency.
sq=ios.replace(RIM_OLD, '<rect x="12" y="12" width="1000" height="1000" rx="180"/>')
open(f'{OUT}/legacy-square.svg','w').write(sq.replace('<svg ', '<svg ',1))
circ=ios.replace(RIM_OLD, '<circle cx="512" cy="512" r="500"/>')
open(f'{OUT}/legacy-round.svg','w').write(circ)
def png(svg, out, size):
    subprocess.run(['rsvg-convert','-w',str(size),'-h',str(size),svg,'-o',out],check=True)
png(f'{OUT}/icon-ios.svg', f'{OUT}/icon-ios-1024.png', 1024)
png(f'{OUT}/icon-play.svg', f'{OUT}/icon-play-512.png', 512)
png(f'{OUT}/adaptive-background.svg', f'{OUT}/adaptive-background-1024.png', 1024)
png(f'{SRC}/adaptive-foreground.svg', f'{OUT}/adaptive-foreground-1024.png', 1024)
png(f'{SRC}/adaptive-monochrome.svg', f'{OUT}/adaptive-monochrome-1024.png', 1024)
png(f'{OUT}/legacy-square.svg', f'{OUT}/legacy-square-1024.png', 1024)
png(f'{OUT}/legacy-round.svg', f'{OUT}/legacy-round-1024.png', 1024)
print('built')
