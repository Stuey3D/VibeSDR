/**
 * The landscape deck's geometry (src/constants/meters.ts landscapeDeck) — faceplates brief §9, §11, §4.1.
 *
 * ★★★ THE BAND GROWS WITH THE HEIGHT, AND ONLY WHERE THERE IS HEIGHT (build 356): at every landscape
 * window from the SE in Display Zoom (568 × 320) to a 13" iPad (1366 × 1024), for every chassis × meter
 * × shared, the band is today's on the default chassis's bar, grows a LITTLE on the SE (B8: landscape had
 * room to spare there), and grows with the window's height in POINTS to the mockup's 62 on a Pro Max, the four keys are one height, the columns never go
 * negative or overlap, the drums keep their 80 pt, and the frequency window still holds a real Nixie
 * tube row without clipping a dome.
 *
 * Run: node --no-warnings scripts/test_faceplate_landscape.ts   (run-tests.sh does)
 */
import { landscapeDeck, landscapeBand, LAND, type MeterKind } from '../src/constants/meters.ts';
import { nixieGeometry, nixieSpec, stackHeight, TUBE_DESIGN, PIP_H, COLLAR_H } from '../src/constants/nixie.ts';
import {
  modeBoxFit, modeTextWidth, stereoWidth, modeLabelCandidates, MODE_BOX, MODE_BOX_MAX_SHARE, MODE_BOX_LAST_SHARE, MODE_MIN_FONT,
  type ModeFace,
} from '../src/constants/modeBox.ts';
import { WHOLE_PROFILE_MODES } from '../src/services/dataModes.ts';

/** Every label the code can compose (constants/modeBox.ts), and the two mode-box typefaces: Atkinson
 *  under Hyperlegible / Nixie / 7-segment, Doto under the VFD. */
const CANDIDATES = modeLabelCandidates(WHOLE_PROFILE_MODES);
const FACES: ModeFace[] = ['hyper', 'doto'];

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
// Landscape windows in POINTS (W × H): 568 × 320 (SE Display Zoom), 667 × 375 (SE), 740 × 360 (Android),
// 844 × 390 (iPhone 14), 874 × 402 (iPhone 17), 932 × 430 (Pro Max), 956 × 440 (17 Pro Max); tablets
// 1024 × 768, 1366 × 1024.
const DEVICES: [number, number, boolean][] = [[568, 320, false], [667, 375, false], [740, 360, false], [844, 390, false],
  [874, 402, false], [932, 430, false], [956, 440, false], [1024, 768, true], [1366, 1024, true]];

for (const [W, H, tablet] of DEVICES) {
  const scale = Math.max(0.58, Math.min(1.45, W / 926));      // useUiScale, landscape
  const r = (n: number) => Math.round(n * scale);
  // TODAY's LandscapeBar, before this row: BAND_H = max(DRUM_H 44, SIG_H 40 / tablet 62), KEY_H half of it.
  const todayBand = Math.max(r(44), r(tablet ? 62 : 40));
  // ★★ The band this window should have: today's at LAND.growFromH (375 pt) of height, the mockup's 62
  //    from 430, a straight line between — never below today's (a tablet's today is already over 62),
  //    and on a phone never below today's + LAND.phoneLift.
  const t = Math.max(0, Math.min(1, (H - LAND.growFromH) / (LAND.growToH - LAND.growFromH)));
  const lifted = tablet ? todayBand : Math.min(62, todayBand + LAND.phoneLift);   // B8: the SE's little lift
  const grown = Math.max(todayBand, lifted, Math.round(todayBand + (62 - todayBand) * t));
  for (const [cname, plate] of CHASSIS) {
    const heights = new Set<number>();
    for (const meter of METERS) for (const shared of [false, true]) for (const singleDrum of [false, true]) {
      // ★ shared never reaches the geometry: the banner lives in the status row (§9) — so the call
      //   takes no `shared` at all, and the loop proves the deck cannot depend on it.
      void shared;
      const d = landscapeDeck({ plate, meter, tablet, W, H, scale, r, singleDrum });
      const tag = `${W}×${H} ${cname} ${meter}${shared ? '+shared' : ''}${singleDrum ? ' 1-drum' : ''}`;
      const defaultBar = !plate && meter === 'bar';
      if (!defaultBar) heights.add(d.bandH);
      ok(`${tag}: never taller than the mockup's 62 or today's (${d.bandH})`, d.bandH <= Math.max(todayBand, 62));
      ok(`${tag}: never shorter than today's (${d.bandH} ≥ ${todayBand})`, d.bandH >= todayBand);
      eq(`${tag}: the band`, d.bandH, defaultBar ? todayBand : grown);
      // ★ B8 (Stuart, SE in Display Zoom): "landscape has a little room to spare height wise" — the SE
      //   grows, but only a LITTLE: at most 8 pt over today's in Display Zoom, 14 at standard zoom.
      if (H <= 375 && !defaultBar) eq(`${tag}: ★ the SE grows a LITTLE (today's ${todayBand} + ${LAND.phoneLift})`, d.bandH, todayBand + LAND.phoneLift);
      if (defaultBar) eq(`${tag}: ★ the default chassis's bar is today's (§3.1)`, d.bandH, todayBand);
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
        // ★★★ The mode box: the longest label the code composes on ONE line, in every Display's face
        //     (Stuart, 2026-10-01: "USB:" / "RTTY" / "S9+1"). 70 is the minimum; the box grows out of
        //     the frequency window, then the spacing tightens, then — last — the type shrinks.
        let widest = 0;
        for (const face of FACES) for (const c of CANDIDATES) {
          const f = modeBoxFit({ label: c.label, stereo: c.stereo, face, fontSize: d.modeFont, letterSpacing: 1.5,
                                 readingFont: d.readingFont, minW: r(MODE_BOX.minW), padH: r(MODE_BOX.padH), windowW: d.dispW });
          const t = `${tag} ${face} "${c.label}"${c.stereo ? '+rings' : ''}`;
          const need = modeTextWidth(c.label, f.fontSize, f.letterSpacing, face) + (c.stereo ? stereoWidth(f.fontSize) : 0)
                       + 2 * r(MODE_BOX.padH);
          ok(`${t}: ONE line (need ${need.toFixed(1)} ≤ box ${f.width})`, need <= f.width + 1e-9);
          ok(`${t}: legible (${f.fontSize})`, f.fontSize >= Math.min(d.modeFont, MODE_MIN_FONT));
          ok(`${t}: never below the design 70`, f.width >= r(MODE_BOX.minW));
          ok(`${t}: the frequency keeps the larger part`,
             f.width <= Math.max(r(MODE_BOX.minW), Math.floor(d.dispW * (f.fontSize <= MODE_MIN_FONT ? MODE_BOX_LAST_SHARE : MODE_BOX_MAX_SHARE))));
          if (c.label === 'USB: RTTY') ok(`${t}: the reported case is never squeezed`, !f.squeezed);
          // ★★ Last resort means LAST: without the rings no label costs the type a point (spacing may tighten).
          if (!c.stereo) ok(`${t}: full type (${f.fontSize} = ${d.modeFont})`, f.fontSize === d.modeFont);
          widest = Math.max(widest, f.width);
        }
        // ★ Real tubes in the window: the stack fits, the pip is inside, the glass is never taller
        //   than meterLand — for every radio's fixed row, in the width the window really has (beside
        //   the WIDEST mode box any label asks for).
        const winW = d.dispW - widest - Math.round(r(10) * 2.6);
        for (const layout of ['hf', 'wide', 'fm'] as const) {
          const g = nixieGeometry(winW, d.freqH, nixieSpec(layout), TUBE_DESIGN.meterLand, { bar: false, scale });
          const t = `${tag} ${layout} tubes (window ${d.freqH})`;
          ok(`${t}: the stack fits`, stackHeight(g.glassH, scale) <= d.freqH + 1e-9);
          ok(`${t}: the pip is inside`, g.collarY - g.glassH - PIP_H * scale >= -1e-9);
          ok(`${t}: ★ the tubes stand on the line under the window`, Math.abs(g.collarY + COLLAR_H * scale - d.freqH) < 1e-9);
          ok(`${t}: glass ≤ meterLand`, g.glassH <= TUBE_DESIGN.meterLand.th * scale + 1e-9);
          const last = g.tubes[g.tubes.length - 1];
          ok(`${t}: the row fits the width`, last.x + last.w <= winW + 1e-6);
        }
      }
      // Black's gloss panel wraps the column OUTSIDE it, inside the gaps — never adding height.
      if (plate?.gloss) ok(`${tag}: gloss sits in the column gap`, d.glossOut > 0 && d.glossOut <= d.colGap);
      else eq(`${tag}: no gloss`, d.glossOut, 0);
    }
    eq(`${W}×${H} ${cname}: ONE band across meter × shared × drums${plate ? '' : ' (LED / analogue)'}`, heights.size, 1);
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
// ── ★★ Build 356: the 17 Pro Max's tubes grow; the SE is today's to the point ──
{
  const dev = (W: number, H: number, meter: MeterKind, plate: Plate = { screws: true, gloss: false }) => {
    const scale = Math.max(0.58, W / 926);
    return landscapeDeck({ plate, meter, tablet: false, W, H, scale, r: (n: number) => Math.round(n * scale) });
  };
  eq('17 Pro Max (956 × 440): the mockup\'s 62 pt band', dev(956, 440, 'vu').bandH, 62);
  eq('Pro Max (932 × 430): the mockup\'s numbers — band 62, keys 28, LED window 33.5, labels back',
     [dev(932, 430, 'vu').bandH, dev(932, 430, 'vu').keyH, +dev(932, 430, 'vu').freqH.toFixed(1), dev(932, 430, 'vu').labelH > 0],
     [62, 28, 33.5, true]);
  ok(`17 Pro Max: the frequency window is ≥ 9 pt taller than today's (${dev(956, 440, 'vu').freqH.toFixed(1)} vs ${
       landscapeDeck({ plate: { screws: true, gloss: false }, meter: 'vu', tablet: false, W: 956, scale: 956 / 926, r: (n: number) => Math.round(n * 956 / 926) }).freqH.toFixed(1)})`,
     dev(956, 440, 'vu').freqH - landscapeDeck({ plate: { screws: true, gloss: false }, meter: 'vu', tablet: false, W: 956,
       scale: 956 / 926, r: (n: number) => Math.round(n * 956 / 926) }).freqH >= 9);
  eq('17 Pro Max: default chassis + LED grows too; the default BAR does not',
     [dev(956, 440, 'vu', null).bandH, dev(956, 440, 'bar', null).bandH], [62, 45]);
  for (const meter of ['bar', 'vu', 'edge'] as const) for (const plate of [null, { screws: true, gloss: false }]) {
    const today = landscapeDeck({ plate, meter, tablet: false, W: 667, scale: 667 / 926, r: (n: number) => Math.round(n * 667 / 926) });
    // The untouched default bar stays today's to the point; every other deck grows a little on the SE.
    if (!plate && meter === 'bar') eq(`SE (667 × 375) default bar: the whole deck is today's`, dev(667, 375, meter, plate), today);
    else ok(`SE (667 × 375) ${plate ? 'silver' : 'default'} ${meter}: grows a little (${today.bandH} → ${dev(667, 375, meter, plate).bandH})`,
            dev(667, 375, meter, plate).bandH === today.bandH + LAND.phoneLift);
  }
  // Monotone: a taller window never gets a shorter band.
  let prev = 0, mono = true;
  for (let H = 300; H <= 500; H++) { const b = landscapeBand({ plate: {}, meter: 'vu', tablet: false, H, r: (n: number) => n }); if (b < prev) mono = false; prev = b; }
  ok('the band never shrinks as the window gets taller', mono);
}
// Every compact result anywhere is legible.
for (const [W, H, tablet] of DEVICES) for (const [, plate] of CHASSIS) for (const meter of ['vu', 'edge'] as const) {
  const scale = Math.max(0.58, Math.min(1.45, W / 926));
  const d = landscapeDeck({ plate, meter, tablet, W, H, scale, r: (n: number) => Math.round(n * scale) });
  if (d.meter !== 'bar') ok(`${W} ${meter}: the mode box is legible (${d.modeFont} / ${d.readingFont})`,
                            d.modeFont >= LAND.minModeFont && d.readingFont >= LAND.minReadingFont);
}

// ★ Nixie (or any non-Hyperlegible display) on the DEFAULT bar deck grows on a big phone; untouched stays today's.
{
  const r = (n: number) => n;
  const untouched = landscapeBand({ plate: null, meter: 'bar', tablet: false, H: 440, r });
  const hyper = landscapeBand({ plate: null, meter: 'bar', tablet: false, H: 440, r, display: 'hyper' });
  const nixie = landscapeBand({ plate: null, meter: 'bar', tablet: false, H: 440, r, display: 'nixie' });
  const nixieSE = landscapeBand({ plate: null, meter: 'bar', tablet: false, H: 375, r, display: 'nixie' });
  ok('default + bar + hyper keeps today on a 17 Pro Max', untouched === hyper);
  ok(`default + bar + NIXIE grows on a 17 Pro Max (${hyper} → ${nixie})`, nixie > hyper);
  const nixieSEzoom = landscapeBand({ plate: null, meter: 'bar', tablet: false, H: 320, r, display: 'nixie' });
  const todaySE = landscapeBand({ plate: null, meter: 'bar', tablet: false, H: 375, r });
  ok(`default + bar + NIXIE on the SE grows a little (${todaySE} → ${nixieSE})`, nixieSE === todaySE + LAND.phoneLift);
  ok(`default + bar + NIXIE on the SE in Display Zoom grows a little too (${nixieSEzoom})`, nixieSEzoom === todaySE + LAND.phoneLift);
}
console.log(`${fails ? 'FAIL' : 'ok'}  faceplate landscape: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
