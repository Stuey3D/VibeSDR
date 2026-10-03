#!/usr/bin/env python3
"""Strip privacy metadata from images, LOSSLESSLY (Stuart + NickB, 2026-10-03: "strip any exif").

JPEG: drops APP1 (EXIF — incl. GPS — and XMP), APP13 (IPTC) and COM segments; keeps APP0 (JFIF), APP2 (ICC
colour profile) and every image segment byte for byte — nothing is re-compressed. A photo whose EXIF
Orientation is not 1 is ROTATED for real first (re-encoded at quality 95), or dropping the tag would turn it.
PNG: drops eXIf, tEXt, iTXt, zTXt and tIME chunks; keeps iCCP/sRGB/gAMA and the pixels.

  python3 scripts/strip-image-metadata.py <file|dir>...      (no arguments = every tracked image but test fixtures)
  python3 scripts/strip-image-metadata.py --check            exit 1 if any tracked image still carries metadata
"""
import os, struct, subprocess, sys, zlib
from PIL import Image, ImageOps

def jpeg_strip(data):
    assert data[:2] == b'\xff\xd8'
    out = bytearray(b'\xff\xd8'); i = 2
    while i < len(data):
        if data[i] != 0xFF: raise ValueError('bad marker')
        m = data[i+1]
        if m == 0xDA:                       # start of scan: the rest is image data
            out += data[i:]; break
        if 0xD0 <= m <= 0xD7 or m == 0x01:  # markers without a length
            out += data[i:i+2]; i += 2; continue
        ln = struct.unpack('>H', data[i+2:i+4])[0]; seg = data[i:i+2+ln]
        if m in (0xE1, 0xED, 0xFE):         # APP1 EXIF/XMP, APP13 IPTC, COM
            pass
        else:
            out += seg
        i += 2 + ln
    return bytes(out)

def png_strip(data):
    sig = b'\x89PNG\r\n\x1a\n'; assert data[:8] == sig
    out = bytearray(sig); i = 8
    while i < len(data):
        ln = struct.unpack('>I', data[i:i+4])[0]; typ = data[i+4:i+8]
        chunk = data[i:i+12+ln]
        if typ not in (b'eXIf', b'tEXt', b'iTXt', b'zTXt', b'tIME'): out += chunk
        i += 12 + ln
    return bytes(out)

def dirty(path):
    im = Image.open(path)
    return bool(im.getexif()) or any(k in im.info for k in ('exif', 'xmp', 'XML:com.adobe.xmp', 'Comment')) \
        or (path.lower().endswith('.png') and any(k in im.info for k in ('Creation Time', 'Software', 'Author')))

def strip(path):
    low = path.lower()
    if low.endswith(('.jpg', '.jpeg')):
        im = Image.open(path); o = im.getexif().get(0x0112, 1)
        if o not in (1, None):
            fixed = ImageOps.exif_transpose(im); icc = im.info.get('icc_profile')
            fixed.save(path, quality=95, icc_profile=icc) if icc else fixed.save(path, quality=95)
        data = open(path, 'rb').read(); new = jpeg_strip(data)
    elif low.endswith('.png'):
        data = open(path, 'rb').read(); new = png_strip(data)
    else:
        return False
    if new != data: open(path, 'wb').write(new); return True
    return False

def tracked():
    fs = subprocess.run(['git', 'ls-files'], capture_output=True, text=True).stdout.split('\n')
    return [f for f in fs if f.lower().endswith(('.jpg', '.jpeg', '.png')) and not f.startswith('test/fixtures/')]

if __name__ == '__main__':
    args = sys.argv[1:]
    if args == ['--check']:
        bad = [f for f in tracked() if dirty(f)]
        for f in bad: print('metadata:', f)
        sys.exit(1 if bad else 0)
    paths = []
    for a in args or tracked():
        if os.path.isdir(a):
            for r, _, fs in os.walk(a): paths += [os.path.join(r, f) for f in fs]
        else: paths.append(a)
    n = sum(strip(p) for p in paths if p.lower().endswith(('.jpg', '.jpeg', '.png')))
    print(f'stripped {n} file(s)')
