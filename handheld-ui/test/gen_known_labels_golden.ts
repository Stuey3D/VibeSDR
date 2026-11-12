/** gen_known_labels_golden.ts — the reference for the handheld's SIGNAL LABELS port (src/known_labels.h): the app's own
 *  src/utils/knownSignals.ts run over a synthetic EiBi schedule, a synthetic receiver bookmark list and a set of views,
 *  written as text lines the C++ (test/known_labels_test.cpp) must reproduce exactly. The inputs are mirrored in
 *  known_labels_inputs.json, which both sides read — one copy of the data.
 *  Run: node --no-warnings --import ./ts_resolve.mjs gen_known_labels_golden.ts <out.txt> */
import { readFileSync, writeFileSync } from 'node:fs';
import { eibiKnownSignals, bookmarkSignals, knownSignalList, labelLayout, KNOWN_SIGNALS, type KnownSignal, type BlockedBox }
  from '../../src/utils/knownSignals.ts';

const inp = JSON.parse(readFileSync(new URL('./known_labels_inputs.json', import.meta.url), 'utf8'));
const out: string[] = [];
const n = (v: number | undefined) => (v === undefined ? '-1' : String(v));
const sig = (tag: string, s: KnownSignal) => `${tag} ${s.hz} ${n(s.lo)} ${n(s.hi)} ${s.dialHz} ${s.mode} ${s.prio} ${s.dec || '-'} ${s.src} |${s.label}`;

out.push(`T ${KNOWN_SIGNALS.length}`);
for (const s of KNOWN_SIGNALS) out.push(sig('K', s));
const eibiRows = inp.eibi.map((r: any) => ({ ...r, source: 'eibi' }));
for (const nowMin of inp.nowMins as number[]) {
  const e = eibiKnownSignals(eibiRows, nowMin);
  out.push(`E@${nowMin} ${e.length}`);
  for (const s of e) out.push(sig('E', s));
}
const bm = bookmarkSignals([], inp.bookmarks.map((b: any) => ({ ...b, source: 'server' })), false);
out.push(`B ${bm.length}`);
for (const s of bm) out.push(sig('B', s));

const list = knownSignalList(eibiKnownSignals(eibiRows, inp.layoutNowMin), bm);
out.push(`A ${list.length}`);
// ★ a deterministic width (code points × 7 + 6) — the layout's rules are under test here, not a font's metrics
const measure = (t: string) => Array.from(t).length * 7 + 6;
for (const v of inp.views) {
  const p = labelLayout(list, v.lo, v.span, v.width, measure, v.dial, v.blocked as BlockedBox[], v.pad ?? 3);
  out.push(`V ${v.name} ${p.length}`);
  for (const q of p) out.push(`P ${q.row} ${q.x.toFixed(3)} ${q.lx.toFixed(3)} ${q.w} ${q.sig.hz} |${q.sig.label}`);
}
writeFileSync(process.argv[2], out.join('\n') + '\n');
