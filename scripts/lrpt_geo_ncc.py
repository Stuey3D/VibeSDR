"""lrpt_geo_ncc.py — the LRPT map outline's residual, measured (docs/v12/LRPT-MAP-OVERLAY.md §Measured).

  python3 scripts/lrpt_geo_ncc.py --prep CACHE_DIR       write coast_eu.txt and land.pgm (Natural Earth 10m, public
                                                          domain) into the working directory for tool-lrpt-geo-fit
  python3 scripts/lrpt_geo_ncc.py IMAGE.pgm MASK.pgm x0,y0,x1,y1 [R]
      normalised cross-correlation of a channel (thermal channels 4–6 inverted: land is cold) against the land mask
      tool-lrpt-geo-fit predicts, over shifts of ±R px in a box; prints the shift (sub-pixel) where it peaks = how far
      the picture's land sits from where the model put it (+dx = right in the picture, +dy = later rows).

Needs numpy and Pillow. A developer tool (not run by run-tests.sh: it needs a real pass's picture).
"""
import sys
import numpy as np
from PIL import Image


def prep(cache):
    import json
    from PIL import ImageDraw
    # coastline, densified to 0.005°, Europe
    with open('coast_eu.txt', 'w') as out:
        for f in ['ne_10m_coastline.geojson', 'ne_10m_minor_islands_coastline.geojson']:
            d = json.load(open(cache + '/' + f))
            for ft in d['features']:
                g = ft['geometry']; c = g['coordinates']
                for l in ([c] if g['type'] == 'LineString' else c):
                    if not any(35 <= p[1] <= 75 and -35 <= p[0] <= 45 for p in l): continue
                    pts = []
                    for i in range(len(l)):
                        if i > 0:
                            a, b = l[i - 1], l[i]; k = int(max(abs(b[0] - a[0]) * 0.6, abs(b[1] - a[1])) / 0.005)
                            for j in range(1, k + 1): pts.append((a[0] + (b[0] - a[0]) * j / (k + 1), a[1] + (b[1] - a[1]) * j / (k + 1)))
                        pts.append(tuple(l[i]))
                    out.write('%d %s\n' % (len(pts), ' '.join('%.5f %.5f' % p for p in pts)))
    # land raster at 0.01°
    LAT0, LAT1, LON0, LON1, R = 40.0, 72.0, -20.0, 35.0, 0.01
    W = int((LON1 - LON0) / R); H = int((LAT1 - LAT0) / R)
    im = Image.new('L', (W, H), 0); dr = ImageDraw.Draw(im)
    xy = lambda p: ((p[0] - LON0) / R, (LAT1 - p[1]) / R)
    for f, fill in [('ne_10m_land.geojson', 255), ('lakes.geojson', 0)]:
        for ft in json.load(open(cache + '/' + f))['features']:
            g = ft['geometry']
            for poly in ([g['coordinates']] if g['type'] == 'Polygon' else g['coordinates']):
                ring = poly[0]
                if not any(LAT0 - 5 <= p[1] <= LAT1 + 5 and LON0 - 5 <= p[0] <= LON1 + 5 for p in ring): continue
                dr.polygon([xy(p) for p in ring], fill=fill)
                if fill: [dr.polygon([xy(p) for p in hole], fill=0) for hole in poly[1:]]
    im.save('land.pgm')
    print('wrote coast_eu.txt and land.pgm')


if len(sys.argv) >= 3 and sys.argv[1] == '--prep':
    prep(sys.argv[2]); sys.exit(0)

img = np.asarray(Image.open(sys.argv[1]), dtype=np.float64)
if any('apid%d' % a in sys.argv[1] for a in (67, 68, 69)): img = np.where(img > 0, 256 - img, 0)
msk = np.asarray(Image.open(sys.argv[2]), dtype=np.float64)
x0, y0, x1, y1 = map(int, sys.argv[3].split(','))
R = int(sys.argv[4]) if len(sys.argv) > 4 else 30
best = (-2, 0, 0); grid = {}
for dy in range(-R, R + 1):
    for dx in range(-R, R + 1):
        a = img[y0 + dy:y1 + dy, x0 + dx:x1 + dx]
        m = msk[y0:y1, x0:x1]
        if a.shape != m.shape: continue
        ok = (a > 0) & (m != 128)          # black = no data; 128 = off the Earth
        if ok.sum() < 500: continue
        av = a[ok]; mv = m[ok]
        if mv.std() == 0 or av.std() == 0: continue
        c = np.corrcoef(av, mv)[0, 1]
        grid[(dx, dy)] = c
        if c > best[0]: best = (c, dx, dy)
c0 = grid.get((0, 0), float('nan'))
c, bx, by = best
g = lambda dx, dy: grid.get((dx, dy), c)
sx = bx + 0.5 * (g(bx - 1, by) - g(bx + 1, by)) / (g(bx - 1, by) - 2 * c + g(bx + 1, by) + 1e-12)
sy = by + 0.5 * (g(bx, by - 1) - g(bx, by + 1)) / (g(bx, by - 1) - 2 * c + g(bx, by + 1) + 1e-12)
print("box %s: ncc at 0 %+.3f, best %+.3f at dx %+.1f dy %+.1f" % (sys.argv[3], c0, c, sx, sy))
