/**
 * The landscape deck's geometry (src/constants/meters.ts landscapeDeck) — faceplates brief §9, §11, §4.1.
 *
 * ★★★ NEVER TALLER THAN TODAY'S BAR: at every landscape width from the SE in Display Zoom (568) to a
 * 13" iPad (1366), for every chassis × meter × shared, the band is today's, the four keys are one
 * height, the columns never go negative or overlap, the drums keep their 80 pt, and the frequency
 * window still holds a real Nixie tube row without clipping a dome.
 *
 * Run: node --no-warnings scripts/test_faceplate_landscape.ts   (run-tests.sh does)
 */
import { landscapeDeck, LAND, type MeterKind } from '../src/constants/meters.ts';
import { nixieGeometry, nixieSpec, stackHeight, TUBE_DESIGN, PIP_H } from '../src/constants/nixie.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);

type Plate = { screws: boolean; gloss: boolean } | null;
const CHASSIS: [string, Plate][] = [['default', null], ['silver', { screws: true, gloss: false }],
                                    ['black', { screws: false, gloss: true }]];
const METERS: MeterKind[] = ['bar', 'vu', 'edge'];
// Phones: 568 (SE Display Zoom), 667 (SE), 740, 844, 932 (Pro Max); tablets: 1024, 1366.
const WIDTHS: [number, boolean][] = [[568, false], [667, false], [740, false], [844, false], [932, false],
                                     [1024, true], [1366, true]];

for (const [W, tablet] of WIDTHS) {
  const scale = Math.max(0.58, Math.min(1.45, W / 926));      // useUiScale, landscape
  const r = (n: number) => Math.round(n * scale);
  // TODAY's LandscapeBar, before this row: BAND_H = max(DRUM_H 44, SIG_H 40 / tablet 62), KEY_H half of it.
  const todayBand = Math.max(r(44), r(tablet ? 62 : 40));
  for (const [cname, plate] of CHASSIS) {
    const heights = new Set<number>();
    for (const meter of METERS) for (const shared of [false, true]) for (const singleDrum of [false, true]) {
      // ★ shared never reaches the geometry: the banner lives in the status row (§9) — so the call
      //   takes no `shared` at all, and the loop proves the deck cannot depend on it.
      void shared;
      const d = landscapeDeck({ plate, meter, tablet, W, scale, r, singleDrum });
      const tag = `${W} ${cname} ${meter}${shared ? '+shared' : ''}${singleDrum ? ' 1-drum' : ''}`;
      heights.add(d.bandH);
      ok(`${tag}: never taller than today's band (${d.bandH} ≤ ${todayBand})`, d.bandH <= todayBand);
      eq(`${tag}: the band IS today's`, d.bandH, todayBand);
      // §11: four identical keys, two rows and the gap exactly fill the band.
      eq(`${tag}: two keys + the row gap = the band`, 2 * d.keyH + d.rowGap, d.bandH);
      ok(`${tag}: keys are positive`, d.keyH > 0);
      // Columns: nothing negative, nothing overlapping, everything inside the plate.
      const drums = singleDrum ? 1 : 2;
      const inner = W - 2 * LAND.screenMargin - 2 * d.padH;
      ok(`${tag}: every column is positive`, d.keyW > 0 && d.dispW > 0 && d.drumW > 0 && d.colGap >= 0);
      ok(`${tag}: the columns fill the plate exactly (no overlap)`,
         Math.abs(drums * d.drumW + 2 * d.keyW + d.dispW + (drums + 2) * d.colGap - inner) < 1e-9);
      ok(`${tag}: the drums keep their ${LAND.drumMin} pt (drum ${d.drumW.toFixed(1)})`, d.drumW >= LAND.drumMin);
      ok(`${tag}: …and the bar's own minWidth s.r(80)`, d.drumW >= r(LAND.drumMin));
      // The display column never grows past the band.
      if (d.meter === 'bar') {
        ok(`${tag}: the bar frame fits the band`, d.barH > 0 && d.barH <= d.bandH);
        if (!plate) eq(`${tag}: default bar = today's frame`, d.barH, r(tablet ? 62 : 40));
        else eq(`${tag}: metal bar fills the band`, d.barH, d.bandH);
      } else {
        eq(`${tag}: frequency + gap + meter = the band`, d.freqH + d.meterGap + d.housingH, d.bandH);
        ok(`${tag}: the frequency window keeps its floor (${d.freqH} ≥ ${r(LAND.minFreq)})`, d.freqH >= r(LAND.minFreq));
        ok(`${tag}: the digits fit their window`, d.digit > 0 && d.digit * 1.12 + 2 <= d.freqH + 1e-9);
        ok(`${tag}: mode + reading fit the window`,
           Math.round(d.modeFont * 1.15) + Math.round(d.readingFont * 1.15) <= d.freqH);
        if (d.meter === 'vu') {
          eq(`${tag}: the LED housing adds up`,
             d.ledPadTop + d.ledH + d.labelGap + d.labelH + d.ledPadBottom, d.housingH);
          if (W < LAND.smallW) eq(`${tag}: §9 TRAP — below 740 the strip has no labels`, d.labelH, 0);
        } else {
          eq(`${tag}: the edgewise housing adds up`, 2 * d.edgePad + d.edgeWindow, d.housingH);
          ok(`${tag}: the edgewise window keeps its floor`, d.edgeWindow >= r(LAND.minEdgeWindow));
          ok(`${tag}: the print covers the window`,
             d.edgePrintTop <= 0 && d.edgePrintTop + d.edgePrintH >= d.edgeWindow - 1e-9);
        }
        // ★ Real tubes in the window: the stack fits, the pip is inside, the glass is never taller
        //   than meterLand — for every radio's fixed row, in the width the window really has.
        const winW = d.dispW - r(LAND.modeBox) - Math.round(r(10) * 2.6);
        for (const layout of ['hf', 'wide', 'fm'] as const) {
          const g = nixieGeometry(winW, d.freqH, nixieSpec(layout), TUBE_DESIGN.meterLand, { bar: false, scale });
          const t = `${tag} ${layout} tubes (window ${d.freqH})`;
          ok(`${t}: the stack fits`, stackHeight(g.glassH, scale) <= d.freqH + 1e-9);
          ok(`${t}: the pip is inside`, g.collarY - g.glassH - PIP_H * scale >= -1e-9);
          ok(`${t}: glass ≤ meterLand`, g.glassH <= TUBE_DESIGN.meterLand.th * scale + 1e-9);
          const last = g.tubes[g.tubes.length - 1];
          ok(`${t}: the row fits the width`, last.x + last.w <= winW + 1e-6);
        }
      }
      // Black's gloss panel wraps the column OUTSIDE it, inside the gaps — never adding height.
      if (plate?.gloss) ok(`${tag}: gloss sits in the column gap`, d.glossOut > 0 && d.glossOut <= d.colGap);
      else eq(`${tag}: no gloss`, d.glossOut, 0);
    }
    eq(`${W} ${cname}: ONE band across meter × shared × drums`, heights.size, 1);
  }
}

// ── The mockup's numbers come out exactly where today's band IS the mockup's 62 (a tablet at scale 1) ──
{
  const r = (n: number) => n;
  const d = (meter: MeterKind, plate: Plate) => landscapeDeck({ plate, meter, tablet: true, W: 932, scale: 1, r });
  const silver = { screws: true, gloss: false };
  eq('band 62', d('vu', silver).bandH, 62);
  eq('keys 28 / 28, gap 6', [d('vu', silver).keyH, d('vu', silver).rowGap], [28, 6]);
  eq('grid 62 360 62, gap 8', [d('vu', silver).keyW, d('vu', silver).dispW, d('vu', silver).colGap], [62, 360, 8]);
  eq('silver 24 side padding', d('vu', silver).padH, 24);
  eq('LEDs 9, labels 6.5', [d('vu', silver).ledH, d('vu', silver).labelH], [9, 6.5]);
  eq('LED frequency window 33.5', d('vu', silver).freqH, 33.5);
  eq('analogue: 24 pt window, 28 print at -2', [d('edge', silver).edgeWindow, d('edge', silver).edgePrintH,
                                                 d('edge', silver).edgePrintTop], [24, 28, -2]);
  eq('analogue frequency window 28', d('edge', silver).freqH, 28);
  eq('digits 25', d('vu', silver).digit, 25);
  eq('legends 78 % on caps, today\'s on default', [d('vu', silver).legendScale, d('vu', null).legendScale], [0.78, 1]);
  eq('metal bar = the mockup\'s 62', d('bar', silver).barH, 62);
}
// ── Today's default bar deck, unchanged (§13.1 / "default + bar landscape stays as today") ──
{
  const r = (n: number) => n;
  const d = landscapeDeck({ plate: null, meter: 'bar', tablet: false, W: 932, scale: 1, r });
  eq('default bar: band 44, frame 40, keys 19, gap 6', [d.bandH, d.barH, d.keyH, d.rowGap], [44, 40, 19, 6]);
  eq('default bar: today\'s 56 / 340 / 6 columns', [d.keyW, d.dispW, d.colGap], [56, 340, 6]);
}
// ── §9 TRAP: on the SE (667, a 32 pt band) the mode box cannot be read over a meter — the bar, as the
//    brief allows; from 740 up the LED strip (no labels on a phone) and the analogue card are kept ──
{
  const at = (W: number, meter: MeterKind) => {
    const scale = Math.max(0.58, W / 926);
    return landscapeDeck({ plate: null, meter, tablet: false, W, scale, r: (n: number) => Math.round(n * scale) });
  };
  eq('568 (SE Display Zoom): LED → bar', at(568, 'vu').meter, 'bar');
  eq('667 (SE): LED strip kept, without labels', [at(667, 'vu').meter, at(667, 'vu').labelH], ['vu', 0]);
  eq('667 (SE): analogue → bar', at(667, 'edge').meter, 'bar');
  eq('740: analogue → bar (a 13 pt card under a 16 pt window)', at(740, 'edge').meter, 'bar');
  eq('844 / 932: analogue kept', [at(844, 'edge').meter, at(932, 'edge').meter], ['edge', 'edge']);
  eq('932: LED strip without labels (a phone band cannot hold them)', at(932, 'vu').labelH, 0);
}
// Every compact result anywhere is legible.
for (const [W, tablet] of WIDTHS) for (const [, plate] of CHASSIS) for (const meter of ['vu', 'edge'] as const) {
  const scale = Math.max(0.58, Math.min(1.45, W / 926));
  const d = landscapeDeck({ plate, meter, tablet, W, scale, r: (n: number) => Math.round(n * scale) });
  if (d.meter !== 'bar') ok(`${W} ${meter}: the mode box is legible (${d.modeFont} / ${d.readingFont})`,
                            d.modeFont >= LAND.minModeFont && d.readingFont >= LAND.minReadingFont);
}

console.log(`${fails ? 'FAIL' : 'ok'}  faceplate landscape: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
