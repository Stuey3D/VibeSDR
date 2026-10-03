import os, subprocess
from PIL import Image, ImageDraw
R='/Users/stuey3d/VibeSDR'; O='out'
def render(svg,size,dst=None):
    tmp=f'/tmp/_r_{size}.png'; subprocess.run(['rsvg-convert','-w',str(size),'-h',str(size),svg,'-o',tmp],check=True)
    return Image.open(tmp).convert('RGBA')
def rgb(im): bg=Image.new('RGB',im.size,(3,4,3)); bg.paste(im,(0,0),im); return bg
def mask_rr(s,r):
    S=s*4; m=Image.new('L',(S,S),0); ImageDraw.Draw(m).rounded_rectangle([0,0,S-1,S-1],radius=int(r*4),fill=255); return m.resize((s,s),Image.LANCZOS)
def mask_circ(s):
    S=s*4; m=Image.new('L',(S,S),0); ImageDraw.Draw(m).ellipse([0,0,S-1,S-1],fill=255); return m.resize((s,s),Image.LANCZOS)
ios=rgb(render(f'{O}/icon-ios.svg',1024))
for p in ['assets/icon.png','ios/VibeSDR/Images.xcassets/AppIcon.appiconset/App-Icon-1024x1024@1x.png',
          'tvos/VibeSDR/Images.xcassets/AppIcon.appiconset/App-Icon-1024x1024@1x.png','android/app/src/main/res/drawable-mdpi/assets_icon.png']:
    ios.save(f'{R}/{p}', optimize=True)
ios.resize((512,512),Image.LANCZOS).save(f'{R}/website/assets/icon.png', optimize=True)
for p in ['assets/favicon.png','website/assets/favicon.png']:
    ios.resize((64,64),Image.LANCZOS).save(f'{R}/{p}', optimize=True)
SET='/private/var/folders/x1/pd9fgtrd3k5_7f7jfqhjxqn40000gn/T/iconset/vibesdr-icon-set/source'
bg=render(f'{O}/adaptive-background.svg',1024); fg=render(f'{SET}/adaptive-foreground.svg',1024); mono=render(f'{SET}/adaptive-monochrome.svg',1024)
rgb(bg).save(f'{R}/assets/android-icon-background.png'); fg.save(f'{R}/assets/android-icon-foreground.png'); mono.save(f'{R}/assets/android-icon-monochrome.png')
sq=render(f'{O}/legacy-square.svg',1024); rd=render(f'{O}/legacy-round.svg',1024)
dens={'mdpi':1,'hdpi':1.5,'xhdpi':2,'xxhdpi':3,'xxxhdpi':4}
for d,k in dens.items():
    a=round(108*k); l=round(48*k); base=f'{R}/android/app/src/main/res/mipmap-{d}'
    bg.resize((a,a),Image.LANCZOS).convert('RGB').save(f'{base}/ic_launcher_background.webp','WEBP',quality=95)
    fg.resize((a,a),Image.LANCZOS).save(f'{base}/ic_launcher_foreground.webp','WEBP',quality=95)
    mono.resize((a,a),Image.LANCZOS).save(f'{base}/ic_launcher_monochrome.webp','WEBP',quality=95)
    s=sq.resize((l,l),Image.LANCZOS); s.putalpha(mask_rr(l,l*(192/1024))); s.save(f'{base}/ic_launcher.webp','WEBP',quality=95)
    c=rd.resize((l,l),Image.LANCZOS); c.putalpha(mask_circ(l)); c.save(f'{base}/ic_launcher_round.webp','WEBP',quality=95)
os.makedirs('/Users/stuey3d/Desktop/VibeSDR-icon-previews',exist_ok=True)
rgb(render(f'{O}/icon-play.svg',512)).save('/Users/stuey3d/Desktop/VibeSDR-icon-previews/play-store-icon-512.png')
print('applied')
