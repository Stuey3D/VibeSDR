#!/usr/bin/env python3
"""parse-pub117.py — NGA Pub. 117 (Radio Navigational Aids), Chapter 3, 300C.1 / 300C.2 "NAVTEX Stations"
   → scripts/navtex-stations/navtex-stations.json, the ONE source of truth (scripts/gen-navtex-stations.mjs makes the
   C++ and TypeScript readers from it).

★ WHY PUB 117: a US Government work, public domain. The Admiralty List of Radio Signals is NOT used (copyright).
★ INPUT: the WORD COORDINATES of the chapter as served by msi.nga.mil, from poppler:
      pdftotext -bbox 117chapter3.pdf ch3.html
      python3 -I parse-pub117.py ch3.html "<edition / fetched date>" > navtex-stations.json
★★ HOW THE TABLE IS READ. Columns by x: country (≈67), station + call sign (110–215), position (220–300), range
   (≈307), kHz (≈343), B1 letter (370–420), broadcast times (≥ 445). Each ENTRY is a B1 letter with its broadcast times
   on the same line; its kHz is the kHz word on that line, or the kHz cell centred over it. A station's entries are
   laid out symmetrically about its position line, so within a stretch of the table (between MMSI / Phone / page
   lines) the entries are split into one CONTIGUOUS group per station, the split minimising the distance between each
   group's mean height and its station's line (a small dynamic programme). Anything that does not fit is REPORTED on
   stderr and left out — never guessed.
"""
import json, re, sys
from html import unescape

WORD = re.compile(r'<word xMin="([\d.]+)" yMin="([\d.]+)" xMax="([\d.]+)" yMax="([\d.]+)">([^<]*)</word>')
POSLAT = re.compile(r'^(\d{2})-(\d{2})([NS])$')
POSLON = re.compile(r'^(\d{3})-(\d{2})([EW])$')
SEPARATOR = ('MMSI', 'Phone', 'Telex', 'E-mail', 'Remotely', 'Fax', 'Website', 'Tel:', 'Email')
KHZ = {'490', '518', '424', '4209.5'}

def pages(path):
    html = open(path, encoding='utf-8').read()
    for p in re.split(r'<page ', html)[1:]:
        ws = [(float(a), float(b), float(c), float(d), unescape(t)) for a, b, c, d, t in WORD.findall(p)]
        yield ws

def main():
    path, edition = sys.argv[1], sys.argv[2]
    stations, problems, audit = [], [], []
    country = ''
    for ws in pages(path):
        text = ' '.join(w[4] for w in ws)
        if not ('NAVTEX Stations' in text and ('300C.1' in text or '300C.2' in text)): continue
        # header bottom: the "(B1)" / "Character" header words; table starts below them
        heads = [w for w in ws if w[4] in ('Character', '(NM)')]
        top = max((w[3] for w in heads), default=0) + 1
        # ★ the columns from THIS page's own header (the U.S. table is laid out differently from the worldwide one)
        hx = {w[4]: w[0] for w in ws if w[4] in ('kHz', 'Position', 'Range', 'Broadcast', 'Identification')}
        if not all(k in hx for k in ('kHz', 'Position', 'Range', 'Broadcast')):
            problems.append('a station page without its column header'); continue
        KX = (hx['kHz'] - 10, hx['kHz'] + 30)
        LX = (hx['kHz'] + 30, hx['Broadcast'] - 8)
        TX = hx['Broadcast'] - 12
        RX = (hx['Range'] - 10, hx['Range'] + 30)
        NX = (95 if 'Country' in text else 40, hx['Position'] - 5)
        body = [w for w in ws if w[1] > top and not re.fullmatch(r'3\s*-\s*\d+', w[4])]
        # ★ the page number is "3-11"; a range of 300 / 350 NM is NOT one (an optional dash here once ate every range
        #   that starts with a 3)
        footer = [w for w in body if re.fullmatch(r'3', w[4]) and w[1] > 700]
        if footer: body = [w for w in body if w[1] < min(f[1] for f in footer) - 2]
        # rows by y (words on one text line share yMin within ~2 pt)
        def row_of(w): return round(w[1] / 2.0)
        # anchors: a latitude word + longitude word on one line
        anchors = []
        for w in body:
            m = POSLAT.match(w[4])
            if not m: continue
            lon = [v for v in body if abs(v[1] - w[1]) < 3 and v[0] > w[0] and POSLON.match(v[4])]
            if not lon: continue
            ml = POSLON.match(lon[0][4])
            lat = int(m.group(1)) + int(m.group(2)) / 60.0
            lo = int(ml.group(1)) + int(ml.group(2)) / 60.0
            if m.group(3) == 'S': lat = -lat
            if ml.group(3) == 'W': lo = -lo
            rng = [v for v in body if abs(v[1] - w[1]) < 3 and RX[0] < v[0] < RX[1] and re.match(r'^\d', v[4])]
            anchors.append({'y': w[1], 'lat': round(lat, 3), 'lon': round(lo, 3), 'range': rng[0][4] if rng else ''})
        # entries: a lone capital in the letter column with broadcast times on its line
        entries = []
        for w in body:
            if not (LX[0] <= w[0] <= LX[1] and re.fullmatch(r'[A-Z]', w[4])): continue
            times = [v for v in body if abs(v[1] - w[1]) < 3 and v[0] >= TX]
            note = [v for v in body if abs(v[1] - w[1]) < 3 and w[0] < v[0] < TX and v[4].startswith('(')]
            if not times and not note:
                # ★ the times may sit a line away when the station's name wraps (Ho Chi Minh Ville): accept a times row
                #   within 12 pt that has no letter of its own
                near = [v for v in body if 3 <= abs(v[1] - w[1]) < 12 and v[0] >= TX]
                own = lambda y: any(abs(u[1] - y) < 3 and LX[0] <= u[0] <= LX[1] and re.fullmatch(r'[A-Z]', u[4]) for u in body)
                if not any(not own(v[1]) for v in near): continue
            # ★★ AUDIT: a B1 letter OWNS a 10-minute slot in every 4 hours (A = 0000, B = 0010 … X = 0350), so the last
            #   broadcast time (" … 16, 2010") must fall in this letter's slot. A mismatch means the letter was read off
            #   the wrong row — reported, so a misread table cannot pass silently.
            rows = sorted((v for v in body if abs(v[1] - w[1]) < 12 and v[0] >= TX and re.fullmatch(r'\d{4}', v[4])),
                          key=lambda v: abs(v[1] - w[1]))
            entries.append({'y': w[1], 'b1': w[4], 'last': rows[0][4] if rows else ''})
        khzw = [w for w in body if KX[0] <= w[0] <= KX[1] and w[4] in KHZ]
        seps = sorted(w[1] for w in body if w[0] < hx['Range'] and w[4].startswith(SEPARATOR))
        # country labels (x < 110), joined when stacked
        clab = sorted((w[1], w[4], w[0]) for w in body if w[0] < NX[0] and re.match(r'^[A-Za-z][a-zA-Z.\'()-]*$', w[4])) if 'Country' in text else []
        # ★ a name word: in the name column, on no separator line (MMSI / Phone …) and no note line ("Ice included in
        #   warnings during ice season" sits in the name column under a Canadian station)
        seprow = lambda y: any(abs(v[1] - y) < 3 and v[4].startswith(SEPARATOR) for v in body)
        # ★ a NOTE line under a station: "Ice included …", "NOTE: …", "Broadcasts are remotely controlled …", "relayed by
        #   …", "in English and Chinese", and the Kodiak area labels ("EAST of Kodiak" / "WEST of Kodiak")
        def noterow(y):
            line = sorted((v for v in body if abs(v[1] - y) < 3 and NX[0] <= v[0] < NX[1]), key=lambda v: v[0])
            if not line: return False
            first = line[0][4]
            return (first in ('Ice', 'NOTE:', 'Broadcasts') or first[:1].islower()
                    or (len(line) > 1 and first in ('EAST', 'WEST', 'NORTH', 'SOUTH') and line[1][4] == 'of'))
        names = [w for w in body if NX[0] <= w[0] < NX[1] and not w[4].startswith(SEPARATOR) and not POSLAT.match(w[4])
                 and not seprow(w[1]) and not noterow(w[1])]
        # split into stretches between separators
        bounds = [-1e9] + seps + [1e9]
        for i in range(len(bounds) - 1):
            lo_, hi_ = bounds[i], bounds[i + 1]
            A = sorted([a for a in anchors if lo_ < a['y'] < hi_], key=lambda a: a['y'])
            E = sorted([e for e in entries if lo_ < e['y'] < hi_], key=lambda e: e['y'])
            if not A and not E: continue
            if not A or len(E) < len(A):
                if E: problems.append(f'entries without a station near y={E[0]["y"]:.0f} ({[e["b1"] for e in E]}) on a page with {[a["lat"] for a in anchors][:3]}')
                continue
            # DP: E split into len(A) contiguous non-empty groups, cost = sum |mean(group) - anchor|
            n, m = len(E), len(A)
            INF = float('inf')
            cost = [[INF] * (m + 1) for _ in range(n + 1)]; back = [[0] * (m + 1) for _ in range(n + 1)]
            cost[0][0] = 0
            for j in range(1, m + 1):
                for k in range(j, n + 1):
                    for s in range(j - 1, k):
                        if cost[s][j - 1] == INF: continue
                        mean = sum(e['y'] for e in E[s:k]) / (k - s)
                        c = cost[s][j - 1] + abs(mean - A[j - 1]['y'])
                        if c < cost[k][j]: cost[k][j] = c; back[k][j] = s
            k = n; groups = []
            for j in range(m, 0, -1):
                s = back[k][j]; groups.append(E[s:k]); k = s
            groups.reverse()
            for a, g in zip(A, groups):
                mean = sum(e['y'] for e in g) / len(g)
                if abs(mean - a['y']) > 12:
                    problems.append(f'station at y={a["y"]:.0f} lat {a["lat"]}: its entries are not centred on it ({mean:.0f})')
                    continue
                # country: the label stack nearest above or on this line
                # ★ the label GROUP (stacked lines, gap < 14 pt) that starts at or above this station's line — all of
                #   it, below included ("Nether-" / "lands"), hyphenation joined
                groups = []
                for c in sorted(clab, key=lambda c: (round(c[0]), c[2])):
                    if groups and c[0] - groups[-1][-1][0] < 14: groups[-1].append(c)
                    else: groups.append([c])
                cands = [gr for gr in groups if gr[0][0] <= a['y'] + 12]
                if cands:
                    words = [c[1] for c in cands[-1]]
                    country = re.sub(r'-\s+', '', ' '.join(words))
                if 'Country' not in text: country = 'United States'
                # name: words in the name column on lines within this station's own span
                ys = [e['y'] for e in g] + [a['y']]
                span = (min(ys) - 3, max(ys) + 3)
                # ★★ a name that WRAPS sits on the lines above and below its position line ("Petropavlovsk" / position /
                #   "(UBE-2)"). A name line on no station's line goes to the NEAREST station line in this stretch; on a
                #   tie (one line between two stations) a "(call sign)" continues the station above, a word starts the
                #   one below ("Manila (DZS)" / "Puerto Princesa" / position / "(DVS)").
                def owner(w):
                    near = sorted(A, key=lambda b: (abs(b['y'] - w[1]), 0 if (b['y'] < w[1]) == w[4].startswith('(') else 1))
                    return near[0] if near and abs(near[0]['y'] - w[1]) < 14 else None
                nm = sorted([w for w in names if lo_ < w[1] < hi_ and (owner(w) is a or (owner(w) is None and span[0] <= w[1] <= span[1]))],
                            key=lambda w: (round(w[1]), w[0]))
                rng = [w for w in body if RX[0] < w[0] < RX[1] and re.fullmatch(r'\d+', w[4]) and span[0] - 8 <= w[1] <= span[1] + 8]
                a['range'] = a['range'] or (min(rng, key=lambda w: abs(w[1] - a['y']))[4] if rng else '')
                name = ' '.join(w[4] for w in nm if w[4] != 'Areas')   # ★ "Kodiak (NOX) Areas" — the area labels' word
                tx = []
                for e in g:
                    same = [w for w in khzw if abs(w[1] - e['y']) < 3]
                    if same: khz = same[0][4]
                    else:
                        # ★ only a CENTRED (merged) kHz cell — one on no other entry's line — may serve this entry.
                        #   A kHz word that sits on another entry's line is that entry's; the cell here is BLANK in
                        #   the source (Pub. 117 leaves Hamburg's S without one) and is reported, never guessed.
                        free = [w for w in khzw if span[0] - 8 <= w[1] <= span[1] + 8
                                and not any(abs(w[1] - o['y']) < 3 for o in g)]
                        if not free: problems.append(f'{name}: {e["b1"]} has no kHz in the source — left out'); continue
                        khz = min(free, key=lambda w: abs(w[1] - e['y']))[4]
                    tx.append({'khz': float(khz), 'b1': e['b1']})
                    if e['last']:
                        hhmm = int(e['last']); mins = ((hhmm // 100) * 60 + hhmm % 100) % 240
                        if mins // 10 != ord(e['b1']) - 65:
                            audit.append(f'{country} / {name}: {khz} {e["b1"]} but its times end {e["last"]} '
                                         f'(slot {chr(65 + mins // 10) if mins // 10 < 24 else "?"})')
                stations.append({'country': country, 'name': re.sub(r'\s+', ' ', name).strip(), 'lat': a['lat'], 'lon': a['lon'],
                                 'rangeNm': a['range'], 'tx': tx})
    json.dump({'source': 'NGA Pub. 117 Radio Navigational Aids, Chapter 3, 300C.1 and 300C.2 (NAVTEX Stations). '
               'A US Government work, public domain. https://msi.nga.mil — Publications → Radio Navigational Aids → Chapter 3.',
               'edition': edition, 'stations': stations}, sys.stdout, indent=1, ensure_ascii=False)
    sys.stdout.write('\n')
    for p in problems: print('PROBLEM: ' + p, file=sys.stderr)
    for p in audit: print('SLOT: ' + p, file=sys.stderr)
    print(f'{len(stations)} stations, {sum(len(s["tx"]) for s in stations)} transmitters, {len(problems)} problems', file=sys.stderr)

main()
