import subprocess, math, os, tempfile, shutil
from PIL import Image, ImageDraw
R='/Users/stuey3d/VibeSDR'; G='#66E07C'
sq_base=open('out/icon-ios.svg').read(); rd_base=open('out/legacy-round.svg').read()
def glyph(k,cx,cy,s=1.0):
    def p(v): return f'{v:.1f}'
    if k=='watch': return f'''<g fill="none" stroke="{G}" stroke-width="{13*s}" stroke-linecap="round" stroke-linejoin="round" transform="translate({cx} {cy}) scale({s})">
     <rect x="-50" y="-70" width="100" height="140" rx="26"/><path d="M-34 -70 L-28 -104 L28 -104 L34 -70"/><path d="M-34 70 L-28 104 L28 104 L34 70"/>
     <line x1="50" y1="-14" x2="62" y2="-14"/><line x1="-24" y1="-20" x2="24" y2="-20"/><line x1="-24" y1="4" x2="24" y2="4"/><line x1="-24" y1="28" x2="10" y2="28"/></g>'''
    if k=='phone': return f'''<g transform="translate({cx} {cy}) scale({s})"><rect x="-58" y="-100" width="116" height="200" rx="26" fill="none" stroke="{G}" stroke-width="13"/>
     <rect x="-22" y="-80" width="44" height="13" rx="6.5" fill="{G}"/><rect x="-24" y="74" width="48" height="9" rx="4.5" fill="{G}"/></g>'''
    if k=='node': return f'''<g transform="translate({cx} {cy}) scale({s})"><g stroke="{G}" stroke-width="13" stroke-linecap="round"><line x1="-62" y1="-50" x2="62" y2="-50"/><line x1="-62" y1="-50" x2="0" y2="62"/><line x1="62" y1="-50" x2="0" y2="62"/></g>
     <g fill="{G}"><circle cx="-62" cy="-50" r="24"/><circle cx="62" cy="-50" r="24"/><circle cx="0" cy="62" r="24"/></g></g>'''
    if k=='binary': return f'''<g fill="{G}" font-family="Menlo, monospace" font-weight="bold" font-size="66" text-anchor="middle" transform="translate({cx} {cy}) scale({s})"><text x="0" y="-14">010</text><text x="0" y="60">101</text></g>'''
def svg(base,k,X,Y,W):
    plate=f'<rect x="{X}" y="{Y}" width="{W}" height="{W}" rx="{W*46/264:.1f}" fill="#040605" stroke="{G}" stroke-width="10"/>'
    return base.replace('</svg>', f'<g filter="url(#glow)">{plate}{glyph(k,X+W/2,Y+W/2,W/264)}</g></svg>')
def render(s,size):
    fd,svgp=tempfile.mkstemp(suffix='.svg'); os.write(fd,s.encode()); os.close(fd)
    out=svgp+'.png'; subprocess.run(['rsvg-convert','-w',str(size),'-h',str(size),svgp,'-o',out],check=True)
    return Image.open(out).convert('RGBA')
def rgb(im): b=Image.new('RGB',im.size,(3,4,3)); b.paste(im,(0,0),im); return b
def se_alpha(im,n=5.0):
    s=im.size[0]; S=s*4; m=Image.new('L',(S,S),0); a=S/2; c=S/2; pts=[]
    for i in range(1440):
        t=2*math.pi*i/1440; co=math.cos(t); si=math.sin(t)
        pts.append((c+a*math.copysign(abs(co)**(2/n),co), c+a*math.copysign(abs(si)**(2/n),si)))
    ImageDraw.Draw(m).polygon(pts,fill=255); o=im.copy(); o.putalpha(m.resize((s,s),Image.LANCZOS)); return o
SQ=dict(X=676,Y=668,W=264); RD=dict(X=596,Y=598,W=232)
sqv={k:render(svg(sq_base,k,**SQ),1024) for k in ('watch','phone','node','binary')}
rdv={k:render(svg(rd_base,k,**RD),1024) for k in ('watch','phone')}
# ── Jr: watch app (round) + its iPhone stub (square) · Buddy: watch app (round)
rgb(rdv['watch']).save(f'{R}/spike/WristSDR/WristSDR/Assets.xcassets/AppIcon.appiconset/AppIcon.png')
rgb(sqv['watch']).save(f'{R}/spike/WristSDR/WristSDRStub/Assets.xcassets/AppIcon.appiconset/AppIcon.png')
rgb(rdv['phone']).save(f'{R}/ios/VibeSDRWatch/Assets.xcassets/AppIcon.appiconset/AppIcon.png')
# ── VibeServer: the family master, web client art + favicon, the server-type logo in the apps' lists and artwork
vs=rgb(sqv['node']); vs.save(f'{R}/assets/vibeserver-icon.png')
vs.resize((512,512),Image.LANCZOS).save(f'{R}/assets/vibeserver-art.png')
vs.resize((180,180),Image.LANCZOS).save(f'{R}/assets/vibeserver-favicon.png')
for p in ('assets/logo_vibeserver.png','ios/VibeSDRWatch/Assets.xcassets/logo_vibeserver.imageset/logo_vibeserver.png'):
    vs.resize((320,320),Image.LANCZOS).save(f'{R}/{p}')
# ── Website family row
for k,f in (('watch','family-jr'),('phone','family-buddy'),('node','family-vibeserver'),('binary','family-vibedsp')):
    se_alpha(sqv[k].resize((320,320),Image.LANCZOS)).save(f'{R}/website/assets/{f}.png')
# ── Lite (Android 5+, legacy launcher only): rounded square drawn whole, transparent corners
lite=render(svg(open('out/legacy-square.svg').read(),'node',**SQ),1024)
for d,k in {'mdpi':1,'hdpi':1.5,'xhdpi':2,'xxhdpi':3,'xxxhdpi':4}.items():
    l=round(48*k); t=lite.resize((l,l),Image.LANCZOS)
    m=Image.new('L',(l*4,l*4),0); ImageDraw.Draw(m).rounded_rectangle([0,0,l*4-1,l*4-1],radius=int(l*4*192/1024),fill=255)
    t.putalpha(m.resize((l,l),Image.LANCZOS)); t.save(f'{R}/lite/android/app/src/main/res/mipmap-{d}/ic_launcher.png')
# ── Mac app: macOS draws no mask, so the app supplies its shape — the Big Sur grid, 824 body on 1024, soft shadow
body=se_alpha(sqv['node'].resize((824,824),Image.LANCZOS))
mac=Image.new('RGBA',(1024,1024),(0,0,0,0))
sh=Image.new('RGBA',(1024,1024),(0,0,0,0)); shm=body.split()[3].point(lambda a:int(a*.45))
from PIL import ImageFilter
sh.paste((0,0,0,255),(100,112),shm); sh=sh.filter(ImageFilter.GaussianBlur(14))
mac=Image.alpha_composite(mac,sh); mac.paste(body,(100,100),body)
tmp=tempfile.mkdtemp(); iset=os.path.join(tmp,'AppIcon.iconset'); os.makedirs(iset)
for px in (16,32,64,128,256,512,1024):
    mac.resize((px,px),Image.LANCZOS).save(f'{iset}/icon_{px}x{px}.png')
    if px<=512: mac.resize((px*2,px*2),Image.LANCZOS).save(f'{iset}/icon_{px}x{px}@2x.png')
subprocess.run(['iconutil','-c','icns',iset,'-o',f'{R}/vibeserver/mac/AppIcon.icns'],check=True)
# previews
sheet=Image.new('RGB',(1500,420),(28,30,34)); d=ImageDraw.Draw(sheet)
def circ(im,s):
    t=im.resize((s,s),Image.LANCZOS); m=Image.new('L',(s*4,s*4),0); ImageDraw.Draw(m).ellipse([0,0,s*4-1,s*4-1],fill=255); t.putalpha(m.resize((s,s),Image.LANCZOS)); return t
items=[('Jr watch',circ(rdv['watch'],240)),('Buddy watch',circ(rdv['phone'],240)),('Mac app',mac.resize((260,260),Image.LANCZOS)),('Lite (Android)',Image.open(f'{R}/lite/android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png').resize((192,192))),('website VibeDSP',Image.open(f'{R}/website/assets/family-vibedsp.png').convert('RGBA').resize((200,200)))]
x=20
for lbl,im in items:
    sheet.paste(im,(x,60),im); d.text((x,30),lbl,fill=(230,230,230)); x+=im.size[0]+40
sheet.save('family_shapes.png'); shutil.copy('family_shapes.png','/Users/stuey3d/Desktop/VibeSDR-icon-previews/family-shapes.png'); print('ok')
