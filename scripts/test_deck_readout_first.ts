/**
 * ★★★ THE READOUT COMES FIRST, AND THE TUNING ROW IS NEVER SMALLER THAN THE MENU ROW (2026-10-16).
 *
 * ChicopeeRadio2009's iPhone 12 (390 pt, no Display Zoom, default text): on the bar meter the frequency window
 * was 30 pt under 44 pt menu keys, and the ‹ › / − + tuning keys (TUNE = keys) were no bigger than the menu keys
 * above them. Stuart: "the most important controls should not be smaller than the sub controls for the menus etc."
 *
 * This walks the portrait deck as ControlsBar.PortraitBar builds it — portraitDeck (with the tuning keys' height as
 * `maxKey`) and, on a phone's bar meter, phoneBarStack — at every phone width from the SE in Display Zoom (320 pt)
 * to a Pro Max (430) and a tablet, for both chassis families, all three meters, shared or not, drums / keys / keys
 * with DAB's MUX label, and proves:
 *   1. the tuning row's key is never smaller than the menu row's key (height) — whenever the tuning row is keys;
 *   2. the frequency window is never smaller than the menu keys (phones);
 *   3. the deck is never taller than it was (the waterfall loses nothing), and a shared server costs the default
 *      bar deck no more than it did;
 *   4. landscape: the menu keys (two rows in the band) are never taller than the tuning keys (the band's well).
 * and prints the before / after budget per width.
 *
 * Run: node --no-warnings scripts/test_deck_readout_first.ts   (run-tests.sh does)
 */
import { portraitDeck, phoneBarStack, landscapeDeck, compactKeyHitSlop, type MeterKind } from '../src/constants/meters.ts';
import { tunerKeysLayout } from '../src/constants/drumWell.ts';

let fails = 0, passes = 0;
const ok = (what: string, cond: boolean) => { if (cond) passes++; else { fails++; console.error(`FAIL ${what}`); } };

const sharedBannerFont = (f: number) => Math.max(9, Math.min(13, Math.round(f * 0.34)));   // ControlsBar's
type Tune = 'drums' | 'keys' | 'keys+mux';

/** The deck exactly as PortraitBar built it BEFORE this change (bar stack: today's 40 + 44, banner by the 26 pt rule). */
function before(W: number, cap: boolean, meter: MeterKind, shared: boolean, dabOn: boolean) {
  const scale = Math.max(0.75, Math.min(1.45, W / 390)), r = (n: number) => Math.round(n * scale);
  const tablet = W >= 600, rowGap = r(7);
  const dl = portraitDeck({ cap, meter, shared, tablet, rowGap, r });
  const thin = Math.max(6, r(7)), gap = Math.max(2, r(3));
  const bannerH = Math.round(sharedBannerFont(r(W <= 415 ? 19 : 22)) * 1.25) + 9;
  if (dl.compact || tablet) return { menu: dl.keySlot, window: dl.compact ? dl.freqH : dl.displayH, deckH: dl.blockH, bannerOut: 0 };
  const bannerIn = shared && dl.displayH - thin - gap - bannerH >= 26;
  const line = dabOn ? Math.max(thin, Math.min(r(13), dl.displayH - gap - 26 - (bannerIn ? bannerH : 0))) : thin;
  const pill = dl.displayH - line - gap - (bannerIn ? bannerH : 0);
  const out = shared && !bannerIn ? bannerH : 0;
  return { menu: dl.keySlot, window: pill, deckH: dl.blockH + out, bannerOut: out };
}

/** …and as it builds it now. */
function after(W: number, cap: boolean, meter: MeterKind, shared: boolean, tune: Tune, dabOn: boolean) {
  const scale = Math.max(0.75, Math.min(1.45, W / 390)), r = (n: number) => Math.round(n * scale);
  const tablet = W >= 600, rowGap = r(7), drumH = r(60);
  const tuneKey = tune === 'drums' ? undefined : tunerKeysLayout(0, drumH, false, r, tune === 'keys+mux').keyH;
  const base = portraitDeck({ cap, meter, shared, tablet, rowGap, r, maxKey: tuneKey });
  const thin = Math.max(6, r(7)), gap = Math.max(2, r(3));
  const bannerH = Math.round(sharedBannerFont(r(W <= 415 ? 19 : 22)) * 1.25) + 9;
  if (base.compact || tablet) {
    const kc = base.compact || !!base.keysCompact;
    return { menu: base.keySlot, window: base.compact ? base.freqH : base.displayH, deckH: base.displayH + rowGap + base.keySlot,
             bannerOut: 0, tuneKey, reach: kc ? compactKeyHitSlop(base.keySlot, rowGap, r(8)).reach : base.keySlot,
             legend: base.legendScale };
  }
  const st = phoneBarStack({ base: portraitDeck({ cap, meter, shared, tablet, rowGap, r }), shared, bannerH, thinH: thin,
                            dabLineH: dabOn ? r(13) : null, gap, r, maxKey: tuneKey });
  const out = st.bannerOut ? bannerH : 0;
  const kc = st.keysCompact;
  return { menu: st.keySlot, window: st.pillH, deckH: st.displayH + rowGap + st.keySlot + out, bannerOut: out, tuneKey,
           reach: kc ? compactKeyHitSlop(st.keySlot, rowGap, r(8)).reach : st.keySlot, legend: st.legendScale,
           sum: st.displayH === (st.bannerIn ? bannerH : 0) + st.pillH + gap + st.lineH };
}

const WIDTHS = [320, 375, 390, 393, 402, 430, 440, 768, 1024];
const METERS: MeterKind[] = ['bar', 'vu', 'edge'];
const TUNES: Tune[] = ['drums', 'keys', 'keys+mux'];
for (const W of WIDTHS) for (const cap of [false, true]) for (const meter of METERS) for (const shared of [false, true])
for (const tune of TUNES) for (const dabOn of tune === 'keys+mux' ? [true] : [false, true]) {
  const tag = `${W} pt ${cap ? 'metal' : 'default'} ${meter}${shared ? '+shared' : ''} ${tune}${dabOn ? ' DAB' : ''}`;
  const b = before(W, cap, meter, shared, dabOn), a = after(W, cap, meter, shared, tune, dabOn);
  if (a.tuneKey != null) ok(`${tag}: ★ tuning key ${a.tuneKey} ≥ menu key ${a.menu}`, a.tuneKey >= a.menu);
  if (W < 600) ok(`${tag}: ★ frequency window ${a.window} ≥ menu key ${a.menu}`, a.window >= a.menu);
  ok(`${tag}: the deck is no taller (${b.deckH} → ${a.deckH})`, a.deckH <= b.deckH);
  ok(`${tag}: the readout never got smaller (${b.window} → ${a.window})`, a.window >= b.window);
  ok(`${tag}: a menu key keeps a 44 pt reach or more (${a.reach.toFixed(1)})`, a.reach >= 44 * Math.min(1, W / 390) - 1);
  if ('sum' in a) ok(`${tag}: the bar column adds up`, !!a.sum);
}

// ★ Silver / black keep ONE deck height across shared on the bar deck, whatever TUNE is (the room capped keys free
//   goes to the waterfall for both, not to one of them).
for (const W of WIDTHS.filter(w => w < 600)) for (const tune of TUNES) for (const dabOn of [false, true]) {
  const a = after(W, true, 'bar', false, tune, dabOn), b = after(W, true, 'bar', true, tune, dabOn);
  ok(`${W} pt metal bar ${tune}${dabOn ? ' DAB' : ''}: one deck height shared or not (${a.deckH} / ${b.deckH})`, a.deckH === b.deckH);
}

// ★ Landscape: the two key rows share the band; the tuning keys are the band's well less its padding.
for (const [W, H] of [[568, 320], [667, 375], [844, 390], [852, 393], [932, 430], [1194, 834], [1366, 1024]] as const) {
  const scale = Math.max(0.58, Math.min(1.45, W / 926)), r = (n: number) => Math.round(n * scale);
  for (const plate of [null, { screws: true, gloss: false }]) for (const meter of METERS) {
    const d = landscapeDeck({ plate, meter, tablet: Math.min(W, H) >= 600, W, H, scale, r, display: 'nixie' });
    const tune = tunerKeysLayout(d.drumW, d.bandH, true, r, false).keyH;
    ok(`landscape ${W}×${H} ${plate ? 'metal' : 'default'} ${meter}: tuning key ${tune} ≥ menu key ${d.keyH}`, tune >= d.keyH);
  }
}

// ── The budget, before → after (iPhone default chassis, TUNE = keys, no DAB) ──
console.log('  portrait budget, default chassis, TUNE = keys — menu key / tuning key / frequency window / deck (before → after)');
for (const W of [320, 375, 390, 393, 430]) for (const meter of METERS) for (const shared of [false, true]) {
  const b = before(W, false, meter, shared, false), a = after(W, false, meter, shared, 'keys', false);
  console.log(`    ${String(W).padStart(4)} ${meter.padEnd(4)} ${shared ? 'shared' : '      '}  menu ${b.menu}→${a.menu}  tune ${a.tuneKey}  `
            + `window ${b.window}→${a.window}  deck ${b.deckH}→${a.deckH}${a.bannerOut ? ` (incl. ${a.bannerOut} banner strip)` : ''}`);
}
console.log('  …metal chassis (silver / black), TUNE = keys');
for (const W of [320, 390, 430]) for (const meter of METERS) for (const shared of [false, true]) {
  const b = before(W, true, meter, shared, false), a = after(W, true, meter, shared, 'keys', false);
  console.log(`    ${String(W).padStart(4)} ${meter.padEnd(4)} ${shared ? 'shared' : '      '}  menu ${b.menu}→${a.menu}  tune ${a.tuneKey}  `
            + `window ${b.window}→${a.window}  deck ${b.deckH}→${a.deckH}`);
}

console.log(`${fails ? '✗' : '✓'} deck readout-first: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
