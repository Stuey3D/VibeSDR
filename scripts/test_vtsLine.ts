/**
 * The executable spec for src/services/vtsLine.ts — the VTS station line (app AND web) and what the
 * web pill drops to fit.
 *
 * Run: node --no-warnings scripts/test_vtsLine.ts
 *
 * What it must prove:
 *   1. The line reads "PI: C363 / BBC Radio6Music: A Tribe Called Quest - Can I Kick it?".
 *   2. Every missing part leaves no punctuation behind — no dangling "/" or ":".
 *   3. A message that only repeats the name is not shown twice.
 *   4. DAB's identity is labelled SId, never PI.
 *   5. The 14-segment form has no colons (DSEG draws ':' as '-').
 *   6. The coloured runs join to exactly the plain line, for every combination.
 *   7. The pill drops band → RDS mark → flag, in that order, only as far as it must; absent items cost
 *      nothing; bringing one back needs the hysteresis to spare.
 */
import {
  vtsHex, vtsLine, vtsLineSegments, vtsIdText, vtsStationText, vtsFit, vtsFits, vtsFitState,
  VTS_DROP_ORDER, type VtsFitSpec, type VtsLineParts,
} from '../src/services/vtsLine.ts';

let fails = 0;
const ok = (cond: boolean, what: string) => {
  if (cond) console.log('  ok   ' + what);
  else { console.log('  FAIL ' + what); fails++; }
};
const eq = (got: unknown, want: unknown, what: string) =>
  ok(got === want, `${what}${got === want ? '' : ` — got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`}`);

console.log('1. the owner\'s sentence');
eq(vtsLine({ id: 'C363', name: 'BBC Radio6Music', text: 'A Tribe Called Quest - Can I Kick it?' }),
   'PI: C363 / BBC Radio6Music: A Tribe Called Quest - Can I Kick it?', 'full line');

console.log('2. omissions leave no punctuation');
eq(vtsLine({ id: 'C363' }), 'PI: C363', 'PI only');
eq(vtsLine({ name: 'Heart' }), 'Heart', 'name only');
eq(vtsLine({ text: 'Now playing: Adele' }), 'Now playing: Adele', 'RadioText only');
eq(vtsLine({ id: 'C363', name: 'Heart' }), 'PI: C363 / Heart', 'PI + name');
eq(vtsLine({ id: 'C363', text: 'Now playing' }), 'PI: C363 / Now playing', 'PI + text, no name');
eq(vtsLine({ name: 'Heart', text: 'More music' }), 'Heart: More music', 'name + text, no PI');
eq(vtsLine({}), '', 'nothing → empty');
eq(vtsLine({ id: '  ', name: '  ', text: '' }), '', 'whitespace only → empty');
eq(vtsLine({ id: 'C363', name: ' Heart  ', text: '  Hits ' }), 'PI: C363 / Heart: Hits', 'trimmed');
for (const p of [{ id: 'C363' }, { name: 'X' }, { text: 'Y' }, { id: 'C363', text: 'Y' }, {}] as VtsLineParts[]) {
  const l = vtsLine(p);
  ok(!/^\s*[/:]|[/:]\s*$/.test(l) && !l.includes('/ :') && !l.includes(': /'), `no dangling punctuation in ${JSON.stringify(l)}`);
}

console.log('3. a message that repeats the name');
eq(vtsStationText('Heart', 'Heart'), 'Heart', 'RT == PS shown once');
eq(vtsLine({ id: 'C363', name: 'Heart', text: 'Heart' }), 'PI: C363 / Heart', '…with PI');

console.log('4. DAB is an SId');
eq(vtsLine({ id: 'C6D6', idLabel: 'SId', name: 'BBC 6Music', text: 'Live' }), 'SId: C6D6 / BBC 6Music: Live', 'SId label');
eq(vtsHex(0xC363), 'C363', 'hex');
eq(vtsHex(0x1A), '001A', 'padded');
eq(vtsHex(-1), '', 'none (-1)');
eq(vtsHex(0), '', 'none (0)');
eq(vtsHex(undefined), '', 'none (undefined)');
eq(vtsHex(NaN), '', 'none (NaN)');

console.log('5. 14-segment form');
eq(vtsLine({ id: 'C363', name: 'BBC R6', text: 'Tribe' }, { seg: true }), 'PI C363 / BBC R6 - Tribe', 'no colons');
eq(vtsIdText('C363', 'PI', true), 'PI C363', 'seg id');

console.log('6. coloured runs == plain line');
const names = [undefined, '', 'Heart'], texts = [undefined, '', 'Hits', 'Heart'], ids = [undefined, '', 'C363'];
let combos = 0, bad = 0;
for (const id of ids) for (const name of names) for (const text of texts) for (const seg of [false, true]) {
  const p = { id, name, text };
  combos++;
  const joined = vtsLineSegments(p, { seg }).map((r) => r.s).join('');
  if (joined !== vtsLine(p, { seg })) { bad++; console.log('    mismatch', JSON.stringify(p), seg, joined); }
}
ok(bad === 0, `${combos} combinations join identically`);
const segs = vtsLineSegments({ id: 'C363', name: 'N', text: 'T' }).map((r) => r.kind).join(',');
eq(segs, 'id,sep,name,sep,text', 'run kinds');

console.log('7. drop order');
eq(VTS_DROP_ORDER.join(','), 'band,rds,flag', 'band, then RDS mark, then flag');
const spec = (available: number, widths: VtsFitSpec['widths']): VtsFitSpec =>
  ({ available, fixed: 150, gap: 10, widths });
const all = { band: 140, rds: 30, flag: 20 };
// Total = 150 + (140+10) + (30+10) + (20+10) = 370
eq(vtsFit(spec(1000, all)).step, 0, 'wide: nothing dropped');
eq(vtsFit(spec(370, all)).step, 0, 'exact fit: nothing dropped');
eq(vtsFit(spec(369, all)).step, 1, 'one px short: band goes first');
ok(vtsFit(spec(369, all)).hidden.has('band') && !vtsFit(spec(369, all)).hidden.has('rds'), '…and only the band');
eq(vtsFit(spec(219, all)).step, 2, 'then the RDS mark');
eq(vtsFit(spec(179, all)).step, 3, 'then the flag');
eq(vtsFit(spec(100, all)).step, 3, 'never beyond the list');
ok(!vtsFits(spec(100, all), vtsFitState(3)), '…and says so when even that does not fit');
// No band on screen: dropping it frees nothing, so the RDS mark is the first thing that can go.
eq(vtsFit(spec(220, { rds: 30, flag: 20 })).step, 0, 'absent band: 220 fits rds+flag');
eq(vtsFit(spec(219, { rds: 30, flag: 20 })).step, 2, 'absent band costs nothing; RDS mark goes next');
// Hysteresis: back from 3 only with 12 px to spare.
eq(vtsFit(spec(186, all), { prevStep: 3, hysteresis: 12 }).step, 3, 'hysteresis holds the flag off at +6 px');
eq(vtsFit(spec(192, all), { prevStep: 3, hysteresis: 12 }).step, 2, 'flag returns with 12 px to spare');
eq(vtsFit(spec(1000, all), { prevStep: 3, hysteresis: 12 }).step, 0, 'everything returns when wide');
eq(vtsFit(spec(150, all), { prevStep: 0, hysteresis: 12 }).step, 3, 'dropping is immediate');

console.log(fails ? `\n${fails} FAILED` : '\nall passed');
process.exit(fails ? 1 : 0);
