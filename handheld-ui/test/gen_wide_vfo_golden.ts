/**
 * The R36S's wide VFO (src/wide_vfo.h) against the app's and the web's (src/utils/wideVfo.ts): the TS table, passband,
 * label and fill rule over a grid of cases, one JSON line each, for wide_vfo_test.cpp to replay through the port.
 * Run by run_wide_vfo_test.sh.
 *   node --no-warnings handheld-ui/test/gen_wide_vfo_golden.ts <out.jsonl>
 */
import { writeFileSync } from 'node:fs';
import { WIDE_VFO, WIDE_VFO_FILL_MAX, wideVfoFill, wideVfoLabel, wideVfoPassband, type WideVfoMode } from '../../src/utils/wideVfo.ts';

const out: string[] = [];
out.push(JSON.stringify({ k: 'max', v: WIDE_VFO_FILL_MAX }));
for (const [m, e] of Object.entries(WIDE_VFO)) out.push(JSON.stringify({ k: 'table', m, half: e.half, name: e.name }));
const cases: Array<[WideVfoMode, number, number, string]> = [
  ['dab', 194_064_000, 194_064_000, '7D'], ['dab', 194_063_000, 194_064_000, '7D'], ['dab', 225_648_000, 0, ''],
  ['dab', 210_096_000, 210_096_000, '10N'], ['dab', 239_200_000, 239_200_000, '13F'],
  ['adsb', 1_090_000_000, 1_090_000_000, ''], ['ais', 162_000_000, 162_000_000, ''], ['ais', 161_999_500, 0, ''],
];
for (const [m, dial, centre, blk] of cases) {
  const [lo, hi] = wideVfoPassband(m, dial, centre);
  out.push(JSON.stringify({ k: 'pb', m, dial, centre, blk, lo, hi, label: wideVfoLabel(m, centre > 0 ? centre : dial, blk) }));
}
const W = [0, 320, 640, 1000];
for (const w of W) for (const lo of [-900, -1, 0, 30, 125, 320]) for (const hi of [10, 300, 500, 576, 620, 640, 875, 949, 950, 1300]) {
  if (hi <= lo) continue;
  out.push(JSON.stringify({ k: 'fill', lo, hi, w, fill: wideVfoFill(lo, hi, w) }));
}
writeFileSync(process.argv[2], out.join('\n') + '\n');
