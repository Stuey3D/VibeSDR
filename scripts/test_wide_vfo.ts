/**
 * The drawn width of a whole-channel mode (src/utils/wideVfo.ts) and what needleGeometry does with it.
 *
 * ★★★ Stuart, 2026-11-12 (a B210 on a ~7 MHz span, DAB 7D at 194.064 MHz): the multiplex filled ~1.5 MHz and the VFO's
 *     passband was ~200 kHz — the audio filter's. So the cases below pin:
 *       • DAB's drawn passband is the ensemble, 1.536 MHz, about the BLOCK's centre (as dial offsets);
 *       • the table holds DAB, ADS-B and AIS only — WFM, DRM, ISM/LoRa, NDB are right as they are (see the header there);
 *       • the label reads "DAB 7D · 194.064M";
 *       • a block covering most of the view is drawn as edges only, and never frosted; an ordinary passband is unchanged.
 *     It is drawn only: a source check below holds main.ts to never putting the wide passband in a message.
 *
 * Run: node --no-warnings scripts/test_wide_vfo.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { WIDE_VFO, WIDE_VFO_FILL_MAX, wideVfoFill, wideVfoLabel, wideVfoPassband } from '../src/utils/wideVfo.ts';
import { needleGeometry } from '../src/utils/needleGeometry.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};

// ── the table ──
eq('the modes in the table', Object.keys(WIDE_VFO).sort(), ['adsb', 'ais', 'dab']);
eq('DAB is the 1.536 MHz ensemble', WIDE_VFO.dab.half * 2, 1_536_000);
eq('ADS-B is ~2 MHz', WIDE_VFO.adsb.half * 2, 2_000_000);
eq('AIS spans both 25 kHz channels (161.975 + 162.025)', [162e6 - WIDE_VFO.ais.half, 162e6 + WIDE_VFO.ais.half], [161_962_500, 162_037_500]);
eq('edges only past 90 % of the view', WIDE_VFO_FILL_MAX, 0.9);

// ── the passband, as dial offsets ──
eq('DAB on its block: ±768 kHz', wideVfoPassband('dab', 194_064_000, 194_064_000), [-768_000, 768_000]);
eq('DAB, no centre given: the dial', wideVfoPassband('dab', 194_064_000), [-768_000, 768_000]);
eq('DAB, a 0 centre: the dial', wideVfoPassband('dab', 194_064_000, 0), [-768_000, 768_000]);
eq('DAB, readout 1 kHz behind the block: the block is drawn where it is', wideVfoPassband('dab', 194_063_000, 194_064_000), [-767_000, 769_000]);
eq('ADS-B', wideVfoPassband('adsb', 1_090_000_000), [-1_000_000, 1_000_000]);
eq('AIS', wideVfoPassband('ais', 162_000_000), [-37_500, 37_500]);

// ── the label ──
eq('DAB label', wideVfoLabel('dab', 194_064_000, '7D'), 'DAB 7D · 194.064M');
eq('DAB label, offset block', wideVfoLabel('dab', 210_096_000, '10N'), 'DAB 10N · 210.096M');
eq('DAB label, block not known yet', wideVfoLabel('dab', 225_648_000), 'DAB · 225.648M');
eq('ADS-B label', wideVfoLabel('adsb', 1_090_000_000), 'ADS-B · 1090.000M');
eq('AIS label', wideVfoLabel('ais', 162_000_000), 'AIS · 162.000M');

// ── fill or edges ──
eq('a block at a fifth of the view: filled', wideVfoFill(400, 600, 1000), true);
eq('an RTL DAB view (1.536 of 2.048 MHz = 75 %): filled', wideVfoFill(125, 875, 1000), true);
eq('just under 90 %: filled', wideVfoFill(50, 949, 1000), true);
eq('90 %: edges only', wideVfoFill(50, 950, 1000), false);
eq('wider than the view: edges only', wideVfoFill(-300, 1300, 1000), false);
eq('half off the left: what is visible counts', wideVfoFill(-500, 500, 1000), true);
eq('no width: nothing drawn', wideVfoFill(0, 10, 0), false);

// ── needleGeometry ──
{
  // Stuart's view: 7 MHz span centred on 194.064, 1000 px
  const span = 7e6, lo0 = 194_064_000 - span / 2, W = 1000;
  const hzToX = (hz: number) => ((hz - lo0) / span) * W;
  const [fl, fh] = wideVfoPassband('dab', 194_064_000);
  const n = needleGeometry({ bwHz: span, tuneHz: 194_064_000, filterLow: fl, filterHigh: fh, width: W, pxPerHz: W / span, hzToX, wide: true });
  eq('7 MHz span: the block is 1.536/7 of the view', n && Math.round(n.hiX - n.loX), Math.round(1.536e6 / span * W));
  eq('…filled, no frost', n && [n.fill, n.frost], [true, false]);
  // an RTL's narrowest: a view narrower than the block
  const span2 = 1.2e6, lo2 = 194_064_000 - span2 / 2;
  const n2 = needleGeometry({ bwHz: span2, tuneHz: 194_064_000, filterLow: fl, filterHigh: fh, width: W, pxPerHz: W / span2,
                              hzToX: (hz: number) => ((hz - lo2) / span2) * W, wide: true });
  eq('1.2 MHz view: clipped to the view', n2 && [n2.loXc, n2.hiXc], [0, W]);
  eq('…edges only (both off screen), no frost', n2 && [n2.fill, n2.frost], [false, false]);
  // an ordinary passband: exactly as before
  const n3 = needleGeometry({ bwHz: span2, tuneHz: 194_064_000, filterLow: -100_000, filterHigh: 100_000, width: W, pxPerHz: W / span2,
                              hzToX: (hz: number) => ((hz - lo2) / span2) * W });
  eq('an audio filter: filled and frosted as ever', n3 && [n3.fill, n3.frost], [true, true]);
  const n4 = needleGeometry({ bwHz: span2, tuneHz: 194_064_000, filterLow: -1e6, filterHigh: 1e6, width: W, pxPerHz: W / span2,
                              hzToX: (hz: number) => ((hz - lo2) / span2) * W });
  eq('…even a wide one that is not `wide` (WFM at 1 MHz): unchanged', n4 && [n4.fill, n4.frost], [true, true]);
}

// ── wired in, and drawn only ──
{
  const main = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
  eq('web: renderFrame draws the wide passband', /wf\.filterLow = wide \? wide\.lo/.test(main) && /wf\.vfoWide = !!wide/.test(main), true);
  eq('web: DAB, ADS-B and AIS feed it', /dabOn \? 'dab' : adsbOn \? 'adsb' : aisOn \? 'ais'/.test(main), true);
  eq('web: never sent — no wideVfoPassband result reaches a filter / bandwidth message',
     /wideVfoPassband[^\n]*(_send|setBandwidth|setFilter)/.test(main), false);
  const wv = readFileSync(new URL('../src/components/WaterfallView.tsx', import.meta.url), 'utf8');
  eq('app: frost and panels honour the geometry', /needle && needle\.frost && needleFrost > 0/.test(wv) && (wv.match(/needle && needle\.fill && needle\./g) ?? []).length === 2, true);
  const scr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
  eq('app: SDRScreen passes the wide passband and its label', /wideVfo=\{!!wideDrawn\}/.test(scr) && /needleLabel=\{wideLabel\}/.test(scr)
     && /filterLow=\{wideDrawn \? wideDrawn\[0\]/.test(scr), true);
  const hh = readFileSync(new URL('../handheld-ui/src/app_impl.h', import.meta.url), 'utf8');
  eq('R36S: drawOverlays uses the port', /wideVfoPassband\(wm, s\.vfoHz, centre, lo, hi\)/.test(hh) && /wideVfoFill\(/.test(hh), true);
}

console.log(fails ? `\n${fails} failed, ${passes} passed` : `wide VFO: all ${passes} passed`);
process.exit(fails ? 1 : 0);
