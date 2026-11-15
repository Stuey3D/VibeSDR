/** gen_tuned_label_golden.ts — the reference for the handheld's TUNED STATION LABEL port (src/tuned_label.h): the app's
 *  own src/utils/tunedStation.ts (and wfStamps.ts stampColour) over tuned_label_inputs.json, written as text lines the
 *  C++ (test/tuned_label_test.cpp) must reproduce exactly. One copy of the data, read by both sides.
 *  Run: node --no-warnings --import ./ts_resolve.mjs gen_tuned_label_golden.ts <out.txt> */
import { readFileSync, writeFileSync } from 'node:fs';
import { tunedStation, tunedPick, tunedLabelX, tunedPassband } from '../../src/utils/tunedStation.ts';
import { stampColour } from '../../src/utils/wfStamps.ts';

const inp = JSON.parse(readFileSync(new URL('./tuned_label_inputs.json', import.meta.url), 'utf8'));
const out: string[] = [];
for (const q of inp.queries) {
  // ★ no "mode" = no passband (the dial's ±99 Hz); a mode with no bw = its default
  const pb = q.mode !== undefined ? tunedPassband(q.mode, q.bwLow, q.bwHigh) : undefined;
  const st = tunedStation(inp.rows, q.dial, q.now, pb);
  const pk = tunedPick(inp.rows, q.dial, q.now, { liveName: q.live, vtsShowing: q.vts, settled: q.settled, passband: pb, shown: q.shown });
  out.push(`Q ${q.dial} ${q.now} ${st ? `${st.source} ${st.hz}` : '-'} H${pk.highlight} |${pk.label ?? ''}`);
}
for (const p of inp.passbands) { const b = tunedPassband(p.mode, p.bwLow, p.bwHigh); out.push(`P ${p.mode || '-'} ${b.lo} ${b.hi}`); }
for (const p of inp.place) out.push(`X ${tunedLabelX(p.nx, p.w, p.width).toFixed(4)}`);
for (const c of inp.colours) {
  let lut: number[] | null = null;
  if (c.lut) {
    lut = new Array(1024).fill(0);
    [96, 160, 224].forEach((i, k) => { const n = parseInt(c.lut[k].slice(1), 16); lut![i * 4] = (n >> 16) & 255; lut![i * 4 + 1] = (n >> 8) & 255; lut![i * 4 + 2] = n & 255; lut![i * 4 + 3] = 255; });
  }
  out.push(`C ${c.needle} ${stampColour(c.needle, lut)}`);
}
writeFileSync(process.argv[2], out.join('\n') + '\n');
