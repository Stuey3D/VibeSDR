/**
 * DAB block labels and the block swapper on a wide spectrum (src/utils/dabBlockView.ts) — shared by the app and the web.
 *
 * ★★★ Stuart, 2026-11-15, on the Almere USRP B210 at 56 MS/s in DAB on 7D: "add the spectrum labels to show the blocks
 *     and, if in the memory, show the MUX name next to it — DAB 7D – MTVNL, DAB 8A – 8A regio Alphen. And allow clicking
 *     the waterfall to tune to the specific DAB block … Maybe on radios with sample rates of 4 MS/s and above which can
 *     show 2 or more multiplexes side by side."
 *
 * Run: node --no-warnings scripts/test_dab_block_view.ts   (run-tests.sh does)
 */
import { DAB_BLOCKS, dabBlockIndex } from '../src/services/dabBlocks.ts';
import { DAB_BLOCK_HALF_HZ, DAB_WIDE_MIN_SPAN_HZ, PRIO_DAB_CURRENT, PRIO_DAB_NAMED, PRIO_DAB_BLOCK, dabWideView, dabMuxNames,
         dabBlockLabel, dabBlockSignals, isDabBlockSignal, dabBlocksInView, withDabBlocks, dabBlockHit, dabBlockHint }
  from '../src/utils/dabBlockView.ts';
import { labelLayout, KNOWN_SIGNALS, estimateLabelWidth, type KnownSignal } from '../src/utils/knownSignals.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const hz = (n: string) => DAB_BLOCKS[dabBlockIndex(n)].hz;
const i7D = dabBlockIndex('7D'), i8A = dabBlockIndex('8A');

// ── when: two whole blocks side by side ───────────────────────────────────────────────────────────────────────────
eq('two whole blocks need 1.712 + 1.536 MHz', DAB_WIDE_MIN_SPAN_HZ >= 1_712_000 + 1_536_000, true);
eq('an RTL at 2.4 MS/s is not wide', dabWideView(2_400_000), false);
eq('an RTL at 3.2 MS/s is not wide', dabWideView(3_200_000), false);
eq('an RSP at 4 MS/s is wide', dabWideView(4_000_000), true);
eq('the B210 at 56 MS/s is wide', dabWideView(56_000_000), true);
eq('no span, not wide', dabWideView(NaN), false);

// ── names: only what the receiver said, ranked ────────────────────────────────────────────────────────────────────
{
  const n = dabMuxNames({
    live: { channel: '7D', label: 'MTVNL' },
    memory: { '7D': 'old 7D name', '8A': '8A regio Alphen' },
    scan: [{ block: '8A', ensemble: 'scan 8A' }, { block: '11C', ensemble: 'NPO' }, { block: '5C', ensemble: '  ' }],
    bookmarks: [{ frequency: hz('11C'), mode: 'dab', ensemble: 'bm 11C' }, { frequency: hz('12C'), mode: 'dab', ensemble: 'Bm 12C' },
                { frequency: hz('12B'), mode: 'dab' }, { frequency: hz('9A'), mode: 'wfm', ensemble: 'not dab' }],
  });
  eq('the live ensemble outranks the memory', n.get('7D'), 'MTVNL');
  eq('the memory outranks the scan', n.get('8A'), '8A regio Alphen');
  eq('the scan outranks a bookmark', n.get('11C'), 'NPO');
  eq('a bookmark with an ensemble name counts', n.get('12C'), 'Bm 12C');
  eq('a bookmark without one is not invented', n.has('12B'), false);
  eq('a non-DAB bookmark is not a block name', n.has('9A'), false);
  eq('a blank name is no name', n.has('5C'), false);
  eq('nothing known, nothing named', dabMuxNames({}).size, 0);
}

// ── label text ────────────────────────────────────────────────────────────────────────────────────────────────────
eq('Stuart\'s example', dabBlockLabel('7D', 'MTVNL'), 'DAB 7D — MTVNL');
eq('Stuart\'s other example', dabBlockLabel('8A', '8A regio Alphen'), 'DAB 8A — 8A regio Alphen');
eq('a bare block', dabBlockLabel('5A'), 'DAB 5A');
eq('a long name is shortened', dabBlockLabel('12B', 'An extremely long ensemble name here'), 'DAB 12B — An extremely long…');

// ── the rows ──────────────────────────────────────────────────────────────────────────────────────────────────────
{
  const names = new Map([['7D', 'MTVNL'], ['8A', '8A regio Alphen']]);
  const sigs = dabBlockSignals(names, i7D);
  eq('every Band III block, in order', sigs.length === DAB_BLOCKS.length && sigs.every((s, i) => i === 0 || s.hz >= sigs[i - 1].hz), true);
  eq('the current block outranks all', sigs[i7D].prio, PRIO_DAB_CURRENT);
  eq('the current block is flagged', [sigs[i7D].dabCurrent, sigs[i8A].dabCurrent], [true, false]);
  eq('a named block', [sigs[i8A].prio, sigs[i8A].label], [PRIO_DAB_NAMED, 'DAB 8A — 8A regio Alphen']);
  eq('a bare block', sigs[dabBlockIndex('5A')].prio, PRIO_DAB_BLOCK);
  eq('the bar is the block\'s bandwidth', [sigs[i7D].lo, sigs[i7D].hi], [hz('7D') - DAB_BLOCK_HALF_HZ, hz('7D') + DAB_BLOCK_HALF_HZ]);
  eq('the block outranks every signal label', Math.max(...KNOWN_SIGNALS.map(s => s.prio)) < PRIO_DAB_BLOCK, true);
  eq('recognised as a block row', [isDabBlockSignal(sigs[0]), isDabBlockSignal(KNOWN_SIGNALS[0])], [true, false]);

  // in view: 7D's centre ± 2.6 MHz holds 7C, 7D, 8A
  eq('blocks in view', dabBlocksInView(hz('7D') - 2_600_000, hz('7D') + 2_600_000).map(i => DAB_BLOCKS[i].name), ['7C', '7D', '8A']);

  // merged only on a wide view
  eq('narrow: the signal labels alone', withDabBlocks(null, sigs, 2_400_000), null);
  eq('narrow with signal labels: those', withDabBlocks(KNOWN_SIGNALS, sigs, 2_400_000) === KNOWN_SIGNALS, true);
  eq('wide: the blocks', withDabBlocks(null, sigs, 8_000_000) === sigs, true);
  const merged = withDabBlocks(KNOWN_SIGNALS.slice(0, 3) as KnownSignal[], sigs, 8_000_000)!;
  eq('wide with signal labels: one sorted list', merged.length === sigs.length + 3 && merged.every((s, i) => i === 0 || s.hz >= merged[i - 1].hz), true);

  // laid out on a 56 MS/s view at phone and desktop width: never overlapping, the current and named ones kept
  for (const W of [390, 1400]) {
    const span = 56_000_000, lo = hz('9A') - span / 2;
    const placed = labelLayout(sigs, lo, span, W, (t) => estimateLabelWidth(t, 9, 3, 0.62), hz('7D'), [], 4);
    const rows = [0, 1].map(r => placed.filter(p => p.row === r).sort((a, b) => a.lx - b.lx));
    let overlap = false;
    for (const r of rows) for (let k = 1; k < r.length; k++) if (r[k].lx < r[k - 1].lx + r[k - 1].w) overlap = true;
    eq(`${W} px: no two labels overlap`, overlap, false);
    eq(`${W} px: the current block is drawn first`, placed[0]?.sig.label, 'DAB 7D — MTVNL');
    eq(`${W} px: the named neighbour is kept`, placed.some(p => p.sig.label === 'DAB 8A — 8A regio Alphen'), true);
  }
}

// ── the click ─────────────────────────────────────────────────────────────────────────────────────────────────────
eq('a click on 8A\'s centre', dabBlockHit(hz('8A'), 56e6), i8A);
eq('a click 700 kHz above 8A', dabBlockHit(hz('8A') + 700_000, 56e6), i8A);
eq('a click in the guard between 7D and 8A', dabBlockHit((hz('7D') + hz('8A')) / 2, 56e6), -1);
eq('a click below Band III', dabBlockHit(170_000_000, 56e6), -1);
eq('a narrow view never swaps', dabBlockHit(hz('8A'), 2_400_000), -1);
eq('10A / 10N overlap: the nearer centre', [dabBlockHit(hz('10A') - 100_000, 56e6), dabBlockHit(hz('10N') + 50_000, 56e6)],
   [dabBlockIndex('10A'), dabBlockIndex('10N')]);
eq('no frequency', dabBlockHit(NaN, 56e6), -1);

// ── the hint ──────────────────────────────────────────────────────────────────────────────────────────────────────
{
  const names = new Map([['8A', '8A regio Alphen']]);
  eq('hint: go to a named block', dabBlockHint(i8A, names, i7D), 'go to 8A — 8A regio Alphen');
  eq('hint: go to a bare block', dabBlockHint(dabBlockIndex('5A'), names, i7D), 'go to 5A');
  eq('hint: the block already on', dabBlockHint(i7D, new Map([['7D', 'MTVNL']]), i7D), 'on 7D — MTVNL');
  eq('hint: none', dabBlockHint(-1, names, i7D), '');
}

console.log(`dab block view: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
