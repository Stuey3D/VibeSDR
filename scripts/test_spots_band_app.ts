/**
 * The app's digital spots — the server's mode list, BAND mode and the governor's chips (src/services/spotsBand.ts) — against
 * the web client's (main.ts spotsReadCap / spotsTitle / filteredSpots / SPOT_STATE_WORDS, decoders.ts digital_spots_status)
 * — ONE RULE, TWO READERS (V12 app parity, 2026-10-26). The web's code is cut out of its source and run on the same inputs.
 *
 * Run: npx tsx scripts/test_spots_band_app.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import {
  parseSpotModes, parseSpotsBandCap, spotFilterModes, spotModeMatches, spotsTitleText, spotsBandHint, parseSpotsStatus,
  SPOT_STATE_WORDS, spotChipText,
} from '../src/services/spotsBand';
import { digiModeFamily, readSpotBands, spotBandAt } from '../src/utils/digiModes';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const main = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
const dec = readFileSync(new URL('../web/client/src/decoders.ts', import.meta.url), 'utf8');
const cut = (re: RegExp, what: string): string | null => {
  const m = main.match(re) ?? dec.match(re);
  if (!m) { fails++; console.error(`FAIL ${what} was not found in the web source`); return null; }
  return m[1];
};

const BANDS = [
  { id: '40m', name: '40 m', hz: 7_076_000, lo: 7_038_000, hi: 7_080_000, bandLo: 7_000_000, bandHi: 7_200_000, modes: ['FT8', 'FT4', 'JS8', 'PSK'] },
  { id: '20m', name: '20 m', hz: 14_080_000, lo: 14_070_000, hi: 14_095_600, bandLo: 14_000_000, bandHi: 14_350_000, modes: ['FT8', 'FT4', 'WSPR', 'JTTY', 'PSK'] },
];
const J = (o: object) => JSON.parse(JSON.stringify(o));

// ── 1. The web's spotsReadCap, run on the same vibeserver.json bodies ──
{
  const body = cut(/function spotsReadCap\(j: any\) \{\n([\s\S]*?)\n  if \(!spotBandCap && bandSpots\)/, 'spotsReadCap');
  if (body) {
    const webRead = new Function('readSpotBands', `return function (j) { let spotModes = ['FT8', 'FT4']; let SF_MODES; let spotBandCap = null;\n${body
      .replace(/: string\[\]/g, '').replace(/\(x: unknown\)/g, '(x)').replace(/\(x: string\)/g, '(x)')}\n return { spotModes, SF_MODES, spotBandCap }; };`)(readSpotBands) as
      (j: any) => { spotModes: string[]; SF_MODES: string[]; spotBandCap: any };
    const bodies = [
      {}, { spotModes: [] }, { spotModes: ['FT8', 'FT4', 'FT2', 'JS8', 'WSPR', 'JTTY', 'PSK'] }, { spotModes: ['FT8', 7, 'Q65<x>', ''] },
      { spotModes: ['FT8'], spotsBand: true, spotBands: BANDS }, { spotsBand: false, spotsBandWhy: '  the owner switched Band off ', spotBands: BANDS },
      { spotsBand: false, spotBands: BANDS }, { spotsBand: true, spotBands: [] }, { spotsBand: 'yes', spotBands: BANDS },
    ];
    for (const j of bodies) {
      const w = webRead(J(j));
      eq(`mode filter for ${JSON.stringify(j).slice(0, 60)}`, spotFilterModes(parseSpotModes(J(j).spotModes)), w.SF_MODES);
      eq(`band cap for ${JSON.stringify(j).slice(0, 60)}`, parseSpotsBandCap(J(j)) ?? null, w.spotBandCap);
    }
  }
}

// ── 2. The MODE filter by family (the web's filteredSpots) ──
{
  const modes = ['FT8', 'FT4', 'PSK', 'JTTY'];
  for (const [spot, filter] of [['PSK31', 'PSK'], ['PSK63', 'PSK'], ['FT8', 'FT4'], ['FT8', 'ALL'], ['JTTY', 'JTTY'], ['Q65-60A', 'FT8']] as const) {
    const web = filter === 'ALL' || digiModeFamily(spot, modes) === filter.toUpperCase();
    eq(`filter ${spot} by ${filter}`, spotModeMatches(spot, filter, modes), web);
  }
  const re = cut(/\(SF_MODES\[sfMode\] === 'ALL' \|\| (digiModeFamily\(s\.mode, spotModes\) === SF_MODES\[sfMode\]\.toUpperCase\(\))\)/, 'filteredSpots');
  eq('the web filters by family', re, 'digiModeFamily(s.mode, spotModes) === SF_MODES[sfMode].toUpperCase()');
}

// ── 3. The title (the web's spotsTitle) ──
{
  const body = cut(/function spotsTitle\(\): string \{\n([\s\S]*?)\n\}/, 'spotsTitle');
  if (body) {
    const web = new Function('spotBandAt', `return function (bandSpots, spotBandCap, spec, spotModes) {\n${body}\n};`)(spotBandAt) as
      (b: boolean, cap: any, spec: any, modes: string[]) => string;
    const cap = parseSpotsBandCap({ spotsBand: true, spotBands: BANDS })!;
    for (const [band, hz, modes] of [[true, 14_074_000, ['FT8']], [true, 10_136_000, ['FT8']], [false, 14_074_000, ['FT8', 'FT4']],
                                     [false, 14_074_000, ['FT8', 'FT4', 'JS8']], [false, 0, ['FT8']]] as const) {
      eq(`title band=${band} ${hz} ${modes.join('+')}`, spotsTitleText(band, cap, hz, [...modes]), web(band, cap, { frequency: hz }, [...modes]));
    }
    eq('title of an older server (no list)', spotsTitleText(false, undefined, 0, undefined), 'FT8 / FT4 SPOTS');
  }
  const cap = parseSpotsBandCap({ spotsBand: true, spotBands: BANDS })!;
  eq('band hint (what it covers)', spotsBandHint(cap), 'A whole band\'s digital frequencies at once — every mode on its own dial (40 m, 20 m)');
  eq('band hint (why not)', spotsBandHint(parseSpotsBandCap({ spotsBand: false, spotsBandWhy: 'too heavy for this box', spotBands: BANDS })!), 'too heavy for this box');
}

// ── 4. The governor's status frame and its words ──
{
  const body = cut(/(const ok = \['active', 'light', 'idle', 'shed', 'off', 'out'\];[^\n]*\n[\s\S]*?shed: n\(m\.shed\), live: n\(m\.live\) \}\)\);)/, 'digital_spots_status parse');
  if (body) {
    const web = new Function('msg', `${body.replace(/: unknown/g, '').replace(/\(m: any\)/g, '(m)')}\n return modes;`) as (msg: any) => unknown;
    const data = { scope: 'band', budget: 20, modes: [
      { mode: 'FT8', state: 'active', dials: 1, idle: 0, shed: 0, live: 3 }, { mode: 'WSPR', state: 'idle', dials: 1, idle: 4.7, shed: -1 },
      { mode: 'JT<9>', state: 'shed', shed: 3 }, { mode: 'PSK', state: 'asleep' }, null, { mode: 9, state: 'off' }, { mode: 'JTTY', state: 'off' },
      { mode: 'FT4', state: 'out', dials: 0 },   // ★ V12 2026-10-27: its slice beyond the capture
    ] };
    eq('status frame parsed as the web', parseSpotsStatus(J(data)), web({ data: J(data) }));
    eq('not a status frame', parseSpotsStatus({}), null);
  }
  const words = cut(/const SPOT_STATE_WORDS: Record<string, string> = \{\n([\s\S]*?)\n\};/, 'SPOT_STATE_WORDS');
  if (words) eq('state words are the web\'s', SPOT_STATE_WORDS, new Function(`return {${words}};`)());
  eq('chip text', spotChipText({ mode: 'FT4', state: 'shed', dials: 1, idle: 0, shed: 2, live: 0 }),
     "FT4: signals there, but over this server's CPU budget — not decoded (2 periods shed)");
  eq('chip text, one period', spotChipText({ mode: 'FT4', state: 'shed', dials: 1, idle: 0, shed: 1, live: 0 }),
     "FT4: signals there, but over this server's CPU budget — not decoded (1 period shed)");
  // The web's chip text, cut out of renderSpotModeChips.
  const ct = cut(/chip\.title = (`\$\{m\.mode\}: [\s\S]*?: ''\));/, 'the chip title');
  if (ct) for (const m of [{ mode: 'FT8', state: 'active', shed: 0 }, { mode: 'FT4', state: 'shed', shed: 2 }, { mode: 'JS8', state: 'off', shed: 1 }, { mode: 'JTTY', state: 'out', shed: 0 }])
    eq(`chip text ${m.mode}`, spotChipText({ ...m, dials: 0, idle: 0, live: 0 } as any),
       new Function('m', 'SPOT_STATE_WORDS', `return ${ct};`)(m, SPOT_STATE_WORDS));
}

console.log(`test_spots_band_app: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
