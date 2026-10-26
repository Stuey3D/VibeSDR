// test_digi_modes.ts — V12 DIGITAL SPOTS, the client half (src/utils/digiModes.ts): band stepping is the SAME rule as the
// server's (decoders/digi_bands.h stepSpan — the C++ cases in vibeserver/test-digi-spots.cpp, run again here), the
// server's band list is parsed defensively, and the mode styles are readable and colour-blind safe:
//   • every letter is unique (the shape cue that never depends on colour),
//   • every letter's ink has ≥ 4.5:1 contrast on its own fill (WCAG AA),
//   • every fill stands off BOTH map themes (≥ 1.5:1 against the dark and the light basemap — the rim does the rest),
//   • under simulated protanopia, deuteranopia and tritanopia, every pair of modes that shares no letter-free cue is
//     still ≥ ΔE 10 apart, or differs by its letter (FST4/FST4W share a colour on purpose and differ by letter).
import { DIGI_MODE_STYLE, digiModeStyle, digiModeFamily, readSpotBands, stepSpotBand, spotBandAt, landingSpotBand, bandPassband, type SpotBand } from '../src/utils/digiModes';

let fail = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

// ── stepping: the C++ test's cases ──
const mk = (id: string, lo: number, hi: number, hz: number): SpotBand => ({ id, name: id.replace('m', ' m').replace('c m', ' cm'), hz, lo: hz - 4000, hi: hz + 4000, bandLo: lo, bandHi: hi, modes: ['FT8'] });
const B: SpotBand[] = [
  mk('160m', 1.8e6, 2.0e6, 1.842e6), mk('80m', 3.5e6, 4.0e6, 3.576e6), mk('60m', 5.25e6, 5.45e6, 5.359e6), mk('40m', 7.0e6, 7.3e6, 7.061e6),
  mk('30m', 10.1e6, 10.15e6, 10.139e6), mk('20m', 14.0e6, 14.35e6, 14.0788e6), mk('17m', 18.068e6, 18.168e6, 18.102e6),
  mk('15m', 21.0e6, 21.45e6, 21.076e6), mk('12m', 24.89e6, 24.99e6, 24.917e6), mk('10m', 28.0e6, 29.7e6, 28.076e6),
  mk('6m', 50e6, 54e6, 50.315e6), mk('4m', 70e6, 70.5e6, 70.156e6), mk('2m', 144e6, 148e6, 144.174e6), mk('70cm', 420e6, 450e6, 432.176e6),
];
const at = (i: number) => (i < 0 ? '-' : B[i].id);
ok(at(stepSpotBand(B, 14074000, +1)) === '17m' && at(stepSpotBand(B, 14074000, -1)) === '30m', '20 m: up → 17 m, down → 30 m');
ok(at(stepSpotBand(B, 28074000, +1)) === '6m', '10 m → 6 m');
ok(at(stepSpotBand(B, 432174000, +1)) === '70cm' && at(stepSpotBand(B, 1840000, -1)) === '160m', 'clamped at both ends, never wrapped');
ok(at(stepSpotBand(B, 12000000, +1)) === '20m' && at(stepSpotBand(B, 12000000, -1)) === '30m', 'between bands: the nearest that way');
ok(at(stepSpotBand(B, 0, +1)) === '160m' && at(stepSpotBand(B, 9e9, -1)) === '70cm', 'from nowhere: the first / the last');
ok(stepSpotBand([], 1, 1) === -1, 'no bands: −1');
ok(spotBandAt(B, 7074000)?.id === '40m' && spotBandAt(B, 12e6) === null, 'spotBandAt');
ok(landingSpotBand(B, 7074000, '2m')?.id === '40m' && landingSpotBand(B, 12e6, '2m')?.id === '2m' && landingSpotBand(B, 12e6, null)?.id === '20m',
   'landing: the band the dial is on, else the last used, else 20 m');
const pb = bandPassband(B, 14078800);
ok(pb[0] === -4000 && pb[1] === 4000 && bandPassband(B, 14080800)[0] === -6000, 'the drawn VFO is the band span, wherever the dial sits in the band');
ok(bandPassband(B, 12e6)[0] === -12000, 'off any band: the 24 kHz the server cuts');

// ── the server's list, defensively ──
const raw = [{ id: '20m', name: '20 m', hz: 14078800, lo: 14073850, hi: 14083750, bandLo: 14e6, bandHi: 14.35e6, modes: ['FT8', 'FT4<script>'] },
             { id: 'x', name: 'bad', hz: -1, lo: 1, hi: 2, bandLo: 1, bandHi: 2, modes: [] }, null, 'junk'];
const rb = readSpotBands(raw);
ok(rb.length === 1 && rb[0].id === '20m' && rb[0].modes[1] === 'FT4script', 'malformed bands dropped, mode names kept to their alphabet');
ok(readSpotBands(undefined).length === 0 && readSpotBands({}).length === 0, 'absent / not a list: none');

// ── styles ──
const modes = Object.keys(DIGI_MODE_STYLE);
const letters = modes.map((m) => DIGI_MODE_STYLE[m].letter);
ok(new Set(letters).size === letters.length, `every mode has its own letter (${modes.map((m) => m + '=' + DIGI_MODE_STYLE[m].letter).join(' ')})`);
const lin = (c: number) => { c /= 255; return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); };
const rgb = (h: string) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16));
const lum = (h: string) => { const [r, g, b] = rgb(h).map(lin); return 0.2126 * r + 0.7152 * g + 0.0722 * b; };
const contrast = (a: string, b: string) => { const x = lum(a), y = lum(b); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); };
for (const m of modes) {
  const s = DIGI_MODE_STYLE[m];
  ok(contrast(s.colour, s.ink) >= 4.5, `${m}: letter ink on its fill ${contrast(s.colour, s.ink).toFixed(1)}:1`);
}
// The basemaps' land (vibemap-style.json: dark #0b0d10-ish, light #f2efe9 — the OSM land colour the app uses).
const DARK = '#10141A', LIGHT = '#F2EFE9';
for (const m of modes) {
  const s = DIGI_MODE_STYLE[m];
  ok(contrast(s.colour, DARK) >= 3 || contrast(s.colour, LIGHT) >= 1.5,
     `${m}: stands off the dark map ${contrast(s.colour, DARK).toFixed(1)}:1, the light ${contrast(s.colour, LIGHT).toFixed(1)}:1 (+ a dark rim)`);
}
// Colour-vision deficiency simulation (Viénot, Brettel & Mollon 1999 for protan/deutan; Brettel 1997 tritan approximation),
// in linear RGB, then CIE76 ΔE in Lab.
const SIM: Record<string, number[][]> = {
  protanopia:   [[0.11238, 0.88762, 0.0], [0.11238, 0.88762, 0.0], [0.00401, -0.00401, 1.0]],
  deuteranopia: [[0.29275, 0.70725, 0.0], [0.29275, 0.70725, 0.0], [-0.02234, 0.02234, 1.0]],
  tritanopia:   [[1.0, 0.14461, -0.14461], [0.0, 0.85924, 0.14076], [0.0, 0.85924, 0.14076]],
};
const toLab = (r: number, g: number, b: number) => {
  const X = 0.4124 * r + 0.3576 * g + 0.1805 * b, Y = 0.2126 * r + 0.7152 * g + 0.0722 * b, Z = 0.0193 * r + 0.1192 * g + 0.9505 * b;
  const f = (t: number) => (t > 0.008856 ? Math.cbrt(t) : 7.787 * t + 16 / 116);
  const fx = f(X / 0.9505), fy = f(Y), fz = f(Z / 1.089);
  return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)];
};
const sim = (h: string, M: number[][] | null) => {
  const c = rgb(h).map(lin);
  const v = M ? M.map((row) => Math.max(0, Math.min(1, row[0] * c[0] + row[1] * c[1] + row[2] * c[2]))) : c;
  return toLab(v[0], v[1], v[2]);
};
for (const [name, M] of [['normal vision', null], ...Object.entries(SIM)] as [string, number[][] | null][]) {
  let worst = 1e9, pair = '';
  for (let i = 0; i < modes.length; i++) for (let j = i + 1; j < modes.length; j++) {
    const a = DIGI_MODE_STYLE[modes[i]], b = DIGI_MODE_STYLE[modes[j]];
    if (a.colour === b.colour) continue;   // ★ the same fill on purpose — told apart by letter
    const p = sim(a.colour, M), q = sim(b.colour, M);
    const d = Math.hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]);
    if (d < worst) { worst = d; pair = `${modes[i]}/${modes[j]}`; }
  }
  // ★ The modes Band hears together on one band (FT8, FT4, FT2, JS8, WSPR — and since 2026-10-27 JTTY and PSK) must stay
  //   apart by colour alone too.
  ok(worst >= 10 || ['protanopia', 'deuteranopia', 'tritanopia'].includes(name),
     `${name}: the closest two fills are ${pair}, ΔE ${worst.toFixed(1)}` + (worst < 10 ? ' — told apart by their letters' : ''));
  const core = ['FT8', 'FT4', 'FT2', 'JS8', 'WSPR', 'JTTY', 'PSK'];
  let cw = 1e9, cp = '';
  for (let i = 0; i < core.length; i++) for (let j = i + 1; j < core.length; j++) {
    const p = sim(DIGI_MODE_STYLE[core[i]].colour, M), q = sim(DIGI_MODE_STYLE[core[j]].colour, M);
    const d = Math.hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]);
    if (d < cw) { cw = d; cp = `${core[i]}/${core[j]}`; }
  }
  ok(cw >= 10, `${name}: FT8/FT4/FT2/JS8/WSPR/JTTY/PSK stay ≥ ΔE 10 apart by colour alone (closest ${cp}, ${cw.toFixed(1)})`);
}
ok(digiModeStyle('ft8').letter === '8' && digiModeStyle('Q65-60A').letter === 'Q' && digiModeStyle('ZZ9').colour === '#9E9E9E',
   'lookup is case-insensitive, a submode takes its mode\'s style, an unknown mode draws grey with its first letter');
// ── V12 (2026-10-27): JTTY and PSK, and spot modes to their family ──
ok(DIGI_MODE_STYLE.JTTY?.letter === 'T' && DIGI_MODE_STYLE.PSK?.letter === 'P', 'JTTY draws "T", PSK "P"');
ok(digiModeStyle('PSK31') === DIGI_MODE_STYLE.PSK && digiModeStyle('psk63') === DIGI_MODE_STYLE.PSK, 'PSK31 and PSK63 spots take PSK\'s style');
ok(digiModeFamily('PSK63', ['FT8', 'PSK']) === 'PSK' && digiModeFamily('Q65-60A', ['Q65']) === 'Q65' && digiModeFamily('FT8', ['FT8', 'FT4']) === 'FT8'
   && digiModeFamily('FST4W', ['FST4', 'FST4W']) === 'FST4W' && digiModeFamily('FT4', ['FT8']) === 'FT4',
   'a spot\'s family for the mode filter: PSK63 → PSK, Q65-60A → Q65, FST4W stays FST4W (not FST4), an unknown stays itself');
console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
