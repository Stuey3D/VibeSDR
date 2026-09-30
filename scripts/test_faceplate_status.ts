/**
 * The landscape status row's fit (src/constants/displayText.ts statusFit) — faceplates brief §8.2.
 *
 * ★★★ WHAT DOESN'T FIT IS DROPPED, NOT SQUEEZED, strictly in STATUS_DROP_ORDER (IF first … the
 * recording timer last); SHARED TUNER shortens to `SHARED` before it goes; the CONNECTION METER is
 * never dropped; PORTRAIT never drops anything; the row packs before it drops (so nothing goes where
 * it fits today); and bringing an item back needs the hysteresis to spare, so a 1 pt wobble cannot flap.
 *
 * Run: node --no-warnings scripts/test_faceplate_status.ts   (run-tests.sh does)
 */
import {
  statusFit, statusFits, statusState, statusStepCount, STATUS_DROP_ORDER,
  type StatusRowSpec, type StatusItem,
} from '../src/constants/displayText.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);
const hid = (h: Set<StatusItem>) => STATUS_DROP_ORDER.filter((i) => h.has(i));

// A full shared-server row with recording and DSP on — every item present, round numbers.
const W: Record<StatusItem, number> = {
  utc: 50, localTime: 60, rec: 40, shared: 120, dsp: 70, linkIcons: 30, rate: 55, gain: 80, if: 60,
};
const METER = 16;
function spec(w: Partial<Record<StatusItem, number>> = W): StatusRowSpec {
  const g = (i: StatusItem) => w[i] ?? 0;
  return {
    sectionGap: 8, sharedShort: 50,
    left:   [{ item: 'utc', width: g('utc'), lead: 8 }, { item: 'localTime', width: g('localTime'), lead: 4 },
             { item: 'rec', width: g('rec'), lead: 8 }],
    centre: [{ item: 'shared', width: g('shared'), lead: 8 }, { item: 'dsp', width: g('dsp'), lead: 8 }],
    right:  [{ item: 'linkIcons', width: g('linkIcons'), lead: 4 }, { item: null, width: METER, lead: 4 },
             { item: 'rate', width: g('rate'), lead: 4 }, { item: 'gain', width: g('gain'), lead: 4 },
             { item: 'if', width: g('if'), lead: 4 }],
  };
}
const S = spec();
const N = statusStepCount();

// ── The step table IS the order ──────────────────────────────────────────────
eq('steps: centred, packed, one per item, +1 for SHARED shortening', N, 2 + STATUS_DROP_ORDER.length + 1);
eq('step 0 hides nothing, not packed', [hid(statusState(0).hidden), statusState(0).packed], [[], false]);
eq('step 1 packs, hides nothing', [hid(statusState(1).hidden), statusState(1).packed], [[], true]);
{
  // Every prefix of the order: after dropping the first i items, exactly those are hidden.
  const sharedAt = STATUS_DROP_ORDER.indexOf('shared');
  for (let i = 0; i <= STATUS_DROP_ORDER.length; i++) {
    const step = 1 + i + (i > sharedAt ? 1 : 0);
    eq(`prefix ${i}: hides exactly the first ${i}`, hid(statusState(step).hidden), STATUS_DROP_ORDER.slice(0, i));
  }
  const shortStep = 2 + sharedAt;
  const st = statusState(shortStep);
  eq('SHARED shortens BEFORE it drops', [st.sharedShort, st.hidden.has('shared')], [true, false]);
  eq('…and every earlier item is already gone', hid(st.hidden), STATUS_DROP_ORDER.slice(0, sharedAt));
  eq('dropped SHARED is not "short"', statusState(shortStep + 1).sharedShort, false);
  ok('never shortened before its turn', !statusState(shortStep - 1).sharedShort);
}
eq('the drop order is the brief\'s', [...STATUS_DROP_ORDER],
   ['if', 'gain', 'linkIcons', 'localTime', 'dsp', 'rate', 'shared', 'utc', 'rec']);

// ── Widths → the fewest drops that fit ───────────────────────────────────────
const L = 50 + 4 + 60 + 8 + 40;                       // 162
const C = 120 + 8 + 70;                               // 198
const R = 30 + 4 + 16 + 4 + 55 + 4 + 80 + 4 + 60;     // 257
const centred = 2 * Math.max(L, R) + C + 16;
const packed = L + C + R + 16;
eq('wide: nothing dropped, centred as today', hid(statusFit(2000, S).hidden), []);
eq('exactly centred width: centred', statusFit(centred, S).step, 0);
eq('1 pt short of centred: PACKS, drops nothing', [statusFit(centred - 1, S).step, hid(statusFit(centred - 1, S).hidden)], [1, []]);
eq('exactly packed width: nothing dropped', hid(statusFit(packed, S).hidden), []);
eq('1 pt short of packed: IF goes first', hid(statusFit(packed - 1, S).hidden), ['if']);
eq('IF + its lead freed: fits after IF', hid(statusFit(packed - 64, S).hidden), ['if']);
eq('one more pt: GAIN too', hid(statusFit(packed - 65, S).hidden), ['if', 'gain']);
{
  // Walk every width down to zero: the hidden set only ever GROWS, always as a prefix of the order,
  // and the meter never goes (it is not droppable at all — it has no item).
  let prev: StatusItem[] = [];
  let monotone = true, prefix = true;
  for (let a = 1200; a >= 0; a--) {
    const h = hid(statusFit(a, S).hidden);
    if (h.length < prev.length) monotone = false;
    if (JSON.stringify(h) !== JSON.stringify(STATUS_DROP_ORDER.slice(0, h.length))) prefix = false;
    prev = h;
  }
  ok('narrowing only ever drops more', monotone);
  ok('what is dropped is always a prefix of the order', prefix);
  eq('at 0 pt everything droppable is gone', hid(statusFit(0, S).hidden), [...STATUS_DROP_ORDER]);
  ok('the meter alone still "does not fit" at 0 — it is shown anyway (never dropped)',
     !statusFits(0, S, statusFit(0, S)));
  ok('…but it fits once there is room for it (+ the section gap)', statusFits(METER + 8, S, statusFit(METER + 8, S)));
}
{
  // SHARED shortening: find the width where only SHARED's full form is too wide.
  const sharedAt = STATUS_DROP_ORDER.indexOf('shared');
  const before = statusState(1 + sharedAt);                  // IF … rate gone, SHARED full
  eq('(setup) before shortening', hid(before.hidden), STATUS_DROP_ORDER.slice(0, sharedAt));
  // Left: utc + rec = 50+8+40 = 98; right: meter 16; centre: shared 120; gaps 16.
  const full = 98 + 120 + 16 + 16;
  eq('fits with SHARED TUNER in full', statusFit(full, S).sharedShort, false);
  const f = statusFit(full - 1, S);
  eq('1 pt short: SHARED TUNER → SHARED, nothing else goes', [f.sharedShort, hid(f.hidden)],
     [true, STATUS_DROP_ORDER.slice(0, sharedAt)]);
  const g = statusFit(98 + 50 + 16 + 16 - 1, S);
  eq('SHARED still too wide: dropped', [g.sharedShort, g.hidden.has('shared')], [false, true]);
}

// ── Absent items cost nothing and do not trigger drops ───────────────────────
{
  const plain = spec({ ...W, shared: 0, dsp: 0, rec: 40 });
  const need = (50 + 4 + 60 + 8 + 40) + (30 + 4 + 16 + 4 + 55 + 4 + 80 + 4 + 60) + 8;
  eq('no centre content: one section gap, nothing dropped', hid(statusFit(need, plain).hidden), []);
  eq('…and 1 pt short, IF goes', hid(statusFit(need - 1, plain).hidden), ['if']);
  // Nothing droppable present at all: the meter is never dropped, the result is just "everything"
  const bare = spec({});
  eq('only the meter: centred once both halves hold it', statusFit(2 * METER + 8, bare).step, 0);
  eq('only the meter: packs below that, drops nothing', [statusFit(METER + 8, bare).step, hid(statusFit(METER + 8, bare).hidden)], [1, []]);
}

// ── PORTRAIT NEVER DROPS ─────────────────────────────────────────────────────
for (const a of [0, 100, 300, 568, 2000]) {
  const f = statusFit(a, S, { portrait: true });
  eq(`portrait at ${a} pt: nothing dropped, not shortened`, [hid(f.hidden), f.sharedShort, f.packed], [[], false, false]);
}

// ── Hysteresis: a 1 pt wobble cannot flap ────────────────────────────────────
{
  const H = 8;
  const edge = packed;                                 // fits exactly; 1 pt less drops IF
  let st = statusFit(edge - 1, S);
  eq('(setup) at the edge − 1, IF is dropped', hid(st.hidden), ['if']);
  st = statusFit(edge, S, { prevStep: st.step, hysteresis: H });
  eq('+1 pt: IF stays dropped (inside the hysteresis)', hid(st.hidden), ['if']);
  st = statusFit(edge + H - 1, S, { prevStep: st.step, hysteresis: H });
  eq('+H−1 pt: still dropped', hid(st.hidden), ['if']);
  st = statusFit(edge + H, S, { prevStep: st.step, hysteresis: H });
  eq('+H pt: IF comes back', hid(st.hidden), []);
  st = statusFit(edge - 1, S, { prevStep: st.step, hysteresis: H });
  eq('narrowing drops AT ONCE, no hysteresis (nothing may overflow)', hid(st.hidden), ['if']);
  // Oscillating ±1 pt around the edge for 50 frames: at most the first change, then steady.
  let changes = 0, last = st.step;
  for (let i = 0; i < 50; i++) {
    st = statusFit(edge + (i % 2 ? 1 : -1), S, { prevStep: st.step, hysteresis: H });
    if (st.step !== last) { changes++; last = st.step; }
  }
  eq('±1 pt for 50 frames: no flapping', changes, 0);
  // A content change (a longer rate readout) that no longer fits drops at once, whatever the prevStep.
  const wider = spec({ ...W, rate: W.rate + 10 });
  eq('content grows past the edge: drops at once', hid(statusFit(edge, wider, { prevStep: 0, hysteresis: H }).hidden), ['if']);
  // Hysteresis never makes the result drop MORE than it needs from a fresh start…
  const fresh = statusFit(edge - 200, S);
  const held  = statusFit(edge - 200, S, { prevStep: N - 1, hysteresis: H });
  ok('held state is never worse than prevStep and never better than fresh', held.step <= N - 1 && held.step >= fresh.step);
  // …and it is idempotent (React may render twice).
  const once = statusFit(edge + 3, S, { prevStep: 5, hysteresis: H });
  const twice = statusFit(edge + 3, S, { prevStep: once.step, hysteresis: H });
  eq('idempotent', twice.step, once.step);
}

console.log(`${fails ? 'FAIL' : 'ok'}  faceplate status row: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
