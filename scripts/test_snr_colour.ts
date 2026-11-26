// test_snr_colour.ts — beta96 (Stuart 11-26) the colour-coded SNR per message: src/utils/snrColour.ts, the ONE scale every
// mode's rows use (web + app). The colour is the MARGIN over that mode's decode threshold — "if a mode only needs a weak
// signal to work great no point listing all as red". docs/v12/MESSAGE-SNR.md is the table these numbers come from.
//   node --no-warnings scripts/test_snr_colour.ts
import { SNR_SCALES, SNR_COLOURS, SNR_CHIP_TEXT, snrBandFor, snrChip, snrChipHtml, snrModeOf, snrText, snrTitle, type SnrMode }
  from '../src/utils/snrColour.ts';
import { SNR_RED_DB, SNR_GREEN_DB, snrBand } from '../src/utils/navtexQuality.ts';
import fs from 'node:fs';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };

// 1. ★ Per mode: a TYPICAL GOOD decode is green, one JUST decoded (at the threshold) is red, a little margin is amber.
//    "Typical good" = what a decent station/aircraft reads on that mode (the doc's table says where each came from).
const typical: Record<SnrMode, { good: number; edge: number; some: number }> = {
  navtex: { good: 20, edge: 6, some: 10 },        // strong Niton/Cullercoats copies read 20–35 dB in 100 Hz
  ft8: { good: -12, edge: -20, some: -17 },       // ★ −15 is a healthy FT8 decode (Stuart's example) — not red
  ft4: { good: -8, edge: -15, some: -12 },
  ft2: { good: -5, edge: -12, some: -9 },
  js8: { good: -10, edge: -19, some: -16 },
  adsb: { good: 25, edge: 10, some: 14 },
  ais: { good: 20, edge: 9, some: 13 },
  acars: { good: 15, edge: 2, some: 6 },
  vdl2: { good: 25, edge: 14, some: 18 },
  hfdl300: { good: 12, edge: 2, some: 6 },
  hfdl600: { good: 12, edge: 3, some: 6 },
  hfdl1200: { good: 14, edge: 4, some: 8 },
  hfdl1800: { good: 16, edge: 7, some: 11 },
};
for (const k of Object.keys(SNR_SCALES) as SnrMode[]) {
  const t = typical[k];
  ok(!!t, `${k}: has a typical row in this test`);
  if (!t) continue;
  ok(snrBandFor(k, t.good) === 'green', `${k}: a typical good decode (${t.good} dB) is green, got ${snrBandFor(k, t.good)}`);
  ok(snrBandFor(k, t.edge) === 'red', `${k}: a just-decoded message (${t.edge} dB) is red, got ${snrBandFor(k, t.edge)}`);
  ok(snrBandFor(k, t.some) === 'amber', `${k}: a little margin (${t.some} dB) is amber, got ${snrBandFor(k, t.some)}`);
  const s = SNR_SCALES[k];
  ok(snrBandFor(k, s.thr + 2.9) === 'red' && snrBandFor(k, s.thr + 3) === 'amber' && snrBandFor(k, s.thr + 5.9) === 'amber' && snrBandFor(k, s.thr + 6) === 'green',
     `${k}: the bands are 3 and 6 dB over the threshold`);
  ok(s.what.length > 10 && s.src.length > 10, `${k}: says what it measures and where the threshold came from`);
}
ok(snrBandFor('ft8', -15) !== 'red' && snrBandFor('ft8', -15) === 'green', '★ Stuart\'s example: FT8 at −15 dB is healthy (green)');
ok(snrBandFor('ais', 8) === 'red' && snrBandFor('ft8', 8) === 'green', 'the same 8 dB is red on AIS and green on FT8 — the colour is per mode');

// 2. The number is the real dB (rounded), signed where negatives are normal; nothing for no number.
ok(snrText('ft8', -15.4) === '−15 dB' && snrText('ft8', 3) === '+3 dB' && snrText('ais', 12.6) === '13 dB' && snrText('ais', 0) === '0 dB', 'the text');
ok(snrText('ais', NaN) === '' && snrText('ais', undefined) === '' && snrText('ais', '12') === '', 'no number → nothing');
ok(snrChip('ais', null) === null && snrChip('nonsense', 12) === null && snrChip(undefined, 12) === null, 'no number or an unknown mode → no chip (never a guessed scale)');
ok(snrModeOf('hfdl1200') === 'hfdl1200' && snrModeOf('toString') === null && snrModeOf(3) === null, 'a wire mode key is checked against the table');
const c = snrChip('vdl2', 17)!;
ok(c.band === 'amber' && c.bg === SNR_COLOURS.amber && c.fg === SNR_CHIP_TEXT && c.text === '17 dB', 'the chip: colour behind, dark text, the number');
ok(/^SNR 17 dB, 3 dB over where this mode stops decoding \(amber\)/.test(c.title) && c.title.includes('red under 17, green from 20'), 'the tooltip says the margin and the bands: ' + c.title);
ok(snrTitle('ft8', -15).includes('red under −18, green from −15'), 'signed bands in the tooltip');
const h = snrChipHtml('ais', 20);
ok(h.startsWith('<span class="snrChip green"') && h.includes('>20 dB</span>') && h.includes('aria-label="SNR 20 dB'), 'the web chip: ' + h);
ok(snrChipHtml('ais', undefined) === '', 'the web chip: nothing without a number');

// 3. Contrast: the chip's dark text on each colour ≥ 4.5:1 (WCAG AA), so the number reads on every faceplate.
const lum = (hex: string) => { const v = [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16) / 255).map((x) => (x <= 0.03928 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4)); return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2]; };
for (const [b, col] of Object.entries(SNR_COLOURS)) {
  const r = (lum(col) + 0.05) / (lum(SNR_CHIP_TEXT) + 0.05);
  ok(r >= 4.5, `${b} chip contrast ${r.toFixed(2)}:1`);
}

// 4. NAVTEX reads the same scale (its strip, bars and the server's red %).
ok(SNR_RED_DB === 9 && SNR_GREEN_DB === 12 && snrBand(8) === 'red' && snrBand(9) === 'amber' && snrBand(12) === 'green', 'NAVTEX: red < 9, amber 9–12, green ≥ 12');
const gal = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_gallery.h', import.meta.url), 'utf8');
ok(/kSnrRed = 9, kSnrGreen = 12;/.test(gal), 'the server\'s NAVTEX red % uses the same 9 dB (vibe_gallery.h kSnrRed)');

// 5. The web's CSS has the chip and its three colours are the scale's.
const html = fs.readFileSync(new URL('../web/client/index.html', import.meta.url), 'utf8');
for (const [b, col] of Object.entries(SNR_COLOURS)) ok(new RegExp(`\\.snrChip\\.${b}\\s*\\{[^}]*background:\\s*${col}`, 'i').test(html), `index.html .snrChip.${b} is ${col}`);

console.log(`snr colour: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
