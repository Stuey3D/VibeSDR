/**
 * The Servers button tucks into its edge tab when a box would cover it (src/utils/serversChipCollapse.ts — Stuart,
 * 2026-11-25: "I think the servers button has always clipped, especially on phones" / "in waterfall mode it's there
 * fully" / "it will help the SE massively"). The chip's full rect is { left 12 (or the inset + 8), top = the furniture
 * line + 46, ~112 x 38 }; the boxes' rects follow SDRScreen's rules for each window (SMALL above the deck, or from the
 * server-name row on a short portrait window; BIG up to the status row; Advanced RDS BIG up to 0.82 of the height; the
 * half-open split's upper panel from the top edge).
 * Run: node --no-warnings scripts/test_servers_chip.ts   (run-tests.sh does)
 */
import { chipCollapsed, CHIP_EXPAND_DELAY_MS, type ChipRect } from '../src/utils/serversChipCollapse.ts';

let fails = 0;
function ok(cond: boolean, what: string) {
  if (cond) console.log(`  ✓ ${what}`);
  else { fails++; console.log(`  ✗ ${what}`); }
}
const chipAt = (insetTop: number, insetLeft = 0): ChipRect => ({ x: Math.max(12, insetLeft + 8), y: insetTop + 46, w: 112, h: 38 });
/** A box centred in the 8 pt wrap, from `top` to `bottom` (window y). */
const box = (W: number, maxW: number, top: number, bottom: number): ChipRect => {
  const w = Math.min(maxW, W - 16); return { x: (W - w) / 2, y: top, w, h: bottom - top };
};

// ── No overlay: the full button, everywhere ─────────────────────────────────────────────────────
ok(!chipCollapsed(chipAt(62), []), 'plain waterfall (no box): the full "‹ Servers" button');

// ── iPhone 17 Pro, portrait 402 x 874 (status row 62) ───────────────────────────────────────────
{
  const W = 402, chip = chipAt(62), deckTop = 874 - 34 - 8 - 250;
  ok(!chipCollapsed(chip, [box(W, 760, deckTop - 8 - 246, deckTop - 8)]), 'iPhone 17 Pro, SMALL box above the deck: full button');
  ok(chipCollapsed(chip, [box(W, 760, 62 + 6, deckTop - 8)]), 'iPhone 17 Pro, BIG box up to the status row: tucked');
  ok(chipCollapsed(chip, [box(W, 560, 62 + 6, deckTop - 8)]), 'iPhone 17 Pro, DAB BIG: tucked');
}

// ── iPhone SE 375 x 667 and Display Zoom 320 x 568 (status row 20): a short portrait window, so SMALL fills from the
//    server-name row (boxTopLimit = rightStackTop + 8; rightStackTop = 20 + 46 + the name's ~22 + 8) ────────────────
for (const [name, W, H] of [['iPhone SE', 375, 667], ['SE Display Zoom', 320, 568]] as const) {
  const chip = chipAt(20), deckTop = H - 8 - 230;
  const smallTop = 20 + 46 + 22 + 8 + 8;
  ok(chipCollapsed(chip, [box(W, 760, smallTop, deckTop - 8)]), `${name}, SMALL box (from the server-name row): tucked`);
  ok(chipCollapsed(chip, [box(W, 560, smallTop, deckTop - 8)]), `${name}, DAB SMALL: tucked`);
  ok(chipCollapsed(chip, [box(W, 760, 20 + 6, deckTop - 8)]), `${name}, BIG: tucked`);
  ok(!chipCollapsed(chip, []), `${name}, no box: the full button`);
}

// ── iPad mini, portrait 744 x 1133 (status row 24) ──────────────────────────────────────────────
{
  const W = 744, H = 1133, chip = chipAt(24), bottomOff = 220;
  // Decoder BIG on a tall window is capped at 0.62 of the height (DecoderPanel bigH), so it stops well below the chip.
  const bigBody = Math.min(H - bottomOff - 30 - 46, H * 0.62);
  ok(!chipCollapsed(chip, [box(W, 760, H - bottomOff - 46 - bigBody, H - bottomOff)]),
     `iPad mini, decoder BIG (0.62 cap, top y ${Math.round(H - bottomOff - 46 - bigBody)}): full button`);
  // Advanced RDS BIG reaches 0.82 of the height (AdvRdsPanel maxH) — up over the chip.
  const adv = Math.min(H - bottomOff - 24 - 16, H * 0.82);
  ok(chipCollapsed(chip, [box(W, 600, H - bottomOff - adv, H - bottomOff)]), 'iPad mini, Advanced RDS BIG: tucked');
  ok(!chipCollapsed(chip, [box(W, 760, H - bottomOff - 246, H - bottomOff)]), 'iPad mini, SMALL: full button');
}

// ── iPhone Duo ──────────────────────────────────────────────────────────────────────────────────
{
  // Folded front screen, portrait 466 x 678, full bleed (chromeTop 0): the status column top-right clears to 174.
  const chip = chipAt(0);
  ok(chipCollapsed(chip, [box(466, 760, 6, 678 - 300)]), 'Duo folded, BIG (up the side of the status column): tucked');
  ok(!chipCollapsed(chip, [box(466, 760, 174 + 8 + 8, 678 - 300)]), 'Duo folded, SMALL (from below the status column): full button');
  // Open inner screen, portrait 669 x 951.
  ok(chipCollapsed(chip, [box(669, 760, 6, 951 - 260)]), 'Duo open portrait, BIG: tucked');
  ok(!chipCollapsed(chip, [box(669, 760, 951 - 260 - 246, 951 - 260)]), 'Duo open portrait, SMALL: full button');
  // Open landscape 951 x 669: Advanced RDS BIG up to the top (Stuart's 16.31.05 screenshot).
  // At 951 pt it is the two-column box, 800 wide (wideCols).
  ok(chipCollapsed(chip, [box(951, 800, 16, 669 - 110)]), 'Duo open landscape, Advanced RDS BIG (two columns, 800 pt): tucked');
  ok(!chipCollapsed(chip, [box(951, 600, 669 - 110 - 230, 669 - 110)]), 'Duo open landscape, a SMALL box over the deck: full button');
  // Half-open: SMALL is lower-left below the fold; BIG's upper panel runs from the top edge.
  ok(!chipCollapsed(chip, [{ x: 8, y: 504, w: 322, h: 418 }]), 'Duo half-open, SMALL (below the fold): full button');
  ok(chipCollapsed(chip, [{ x: 8, y: 8, w: 322, h: 439 }, { x: 8, y: 504, w: 322, h: 418 }]), 'Duo half-open, BIG (two panels): tucked');
}

// ── The rule's edges ─────────────────────────────────────────────────────────────────────────────
{
  const chip = { x: 12, y: 46, w: 112, h: 38 };
  ok(chipCollapsed(chip, [{ x: 200, y: 0, w: 10, h: 10 }, { x: 0, y: 90, w: 400, h: 100 }]), 'one of several overlays is enough');
  ok(!chipCollapsed(chip, [{ x: 0, y: 46 + 38 + 13, w: 400, h: 100 }]), 'a box 13 pt below the chip: full button');
  ok(chipCollapsed(chip, [{ x: 0, y: 46 + 38 + 11, w: 400, h: 100 }]), 'a box 11 pt below (inside the chip\'s scrim): tucked');
  ok(!chipCollapsed(chip, [{ x: 140, y: 40, w: 200, h: 60 }]), 'a box beside the chip, 16 pt to its right: full button');
  ok(!chipCollapsed({ x: 12, y: 46, w: 0, h: 0 }, [{ x: 0, y: 0, w: 400, h: 400 }]), 'an unmeasured chip never tucks');
  ok(CHIP_EXPAND_DELAY_MS >= 250 && CHIP_EXPAND_DELAY_MS <= 800, `comes back after ${CHIP_EXPAND_DELAY_MS} ms clear (no flicker as a box resizes)`);
}

if (fails) { console.log(`\nservers chip: ${fails} FAILED`); process.exit(1); }
console.log('\nservers chip: all passed');
