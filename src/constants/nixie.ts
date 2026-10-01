/**
 * nixie.ts — the Nixie frequency window's RULES and GEOMETRY (faceplates brief §7, Deck.mockup
 * `nixie` / `tb` / `narrow()`), pure so scripts/test_faceplate_nixie.ts can hold every rule down.
 *
 * ★★★ FIXED TUBE LAYOUT — real hardware never adds or removes tubes:
 *     [MHz tubes] (bulb A) [3 kHz tubes] (bulb B) [3 Hz tubes]
 *       kHz → 14 230•000   bulb B lit        MHz → 14•230 000   bulb A lit
 *       Hz  → 14 230 000   both bulbs OFF (the gap is intentional — Stuart)
 *   Units only change WHICH BULB IS LIT; no digit ever moves. The MHz-tube count belongs to the
 *   connected radio (network HF 2 → 8 tubes; a local radio to 2 GHz 4 → 10 tubes at 8/10 width), never
 *   to the current frequency. The FM tuner screen is its own fixed layout: 3 tubes + bulb + 3, MHz.
 * ★★★ THE TUBE SHRINKS, THE WINDOW DOESN'T. pip + glass + collar + 4 pt of clearance fill the window;
 *   the glass is capped at its design height and shrinks when the window does. The same in width: the
 *   group narrows to fit, it never pushes the window wider.
 * ★★★ THE TUBES STAND ON THE LINE BELOW THEM (Stuart, 2026-10-01: "the tubes need to be anchored to
 *   the line underneath them so they look attached to the radio"). The collar's foot IS the window's
 *   floor — the lip line drawn under the window. The brief's 2 pt of clearance under the collar read as
 *   a gap on a 17 Pro Max (the dark collar over a dark floor made it look like more), so the whole
 *   clearance budget now sits ABOVE the pip. Bottom-aligned, not resized: the glass is the size it was.
 * ★ The decimal point is its OWN tube (an INS-1 bulb), never a dot inside a digit tube.
 */

/** IN-14-style cathode stack, FRONT to back. A lit digit sits at its own depth. */
export const CATHODE_STACK = ['1', '6', '2', '7', '5', '0', '4', '9', '8', '3'] as const;

export interface CathodeDepth { scale: number; dy: number; opacity: number }

/** Stack index i → `scale(1 − 0.028·i) translateY(0.25·i) opacity(1 − 0.03·i)` (brief + mockup `depthT`). */
export function cathodeDepth(d: string): CathodeDepth {
  const i = Math.max(0, CATHODE_STACK.indexOf(d as any));
  return { scale: 1 - 0.028 * i, dy: 0.25 * i, opacity: 1 - 0.03 * i };
}

/** The cathodes drawn around a lit digit: up to 3 unlit BEHIND it (faint wire) and the 2 nearest IN
 *  FRONT (darker wire, occluding the glow). A switched-off tube shows the first four, all faint. */
export function cathodeNeighbours(d: string | null): { back: string[]; front: string[] } {
  if (d === null) return { back: CATHODE_STACK.slice(0, 4) as string[], front: [] };
  const i = CATHODE_STACK.indexOf(d as any);
  return {
    back:  CATHODE_STACK.slice(i + 1, i + 4) as string[],
    front: CATHODE_STACK.slice(Math.max(0, i - 2), i) as string[],
  };
}

// ── Layouts ──────────────────────────────────────────────────────────────────

/**
 * `hf`   network radios (MAX_HZ 30 MHz): 2 MHz tubes → 8 digit tubes + 2 bulbs.
 * `wide` a local radio to 2 GHz: 4 MHz tubes → 10 digit tubes (narrowed to 8/10, digits 0.9×).
 * `fm`   the FM tuner screen: 3 tubes + bulb + 3, in MHz (` 96•600`), the bulb always lit.
 */
export type NixieLayout = 'hf' | 'wide' | 'fm';

export interface NixieSpec {
  mhzTubes:  number;
  fracTubes: number;
  /** Bulb positions as the index of the digit AFTER each bulb. */
  bulbAt:    number[];
}

export function nixieSpec(layout: NixieLayout, mhzTubes?: number): NixieSpec {
  if (layout === 'fm') return { mhzTubes: 3, fracTubes: 3, bulbAt: [3] };
  const m = mhzTubes ?? (layout === 'wide' ? 4 : 2);
  return { mhzTubes: m, fracTubes: 6, bulbAt: [m, m + 3] };
}

/** How many MHz tubes a frequency needs (≥ 1). */
export function mhzDigitsFor(hz: number): number {
  return Math.max(1, String(Math.floor(Math.max(0, hz) / 1e6)).length);
}

export type NixieUnit = 'hz' | 'khz' | 'mhz';

export interface NixieReadout {
  /** One entry per digit tube: the digit it lights, or null for a tube that is switched OFF. */
  tubes: (string | null)[];
  /** Lit state per bulb, in bulbAt order. */
  bulbs: boolean[];
}

/**
 * The readout for `hz` on a fixed layout. Leading zeros are SWITCHED OFF (bare cathodes, no glow),
 * not zero-filled — except the digit just left of a lit point, which always lights (0•648 000 MHz).
 * ★ A frequency the layout cannot hold keeps its LOW digits (the caller widens the layout instead;
 *   see NixieTubes' latch) — it never shifts the digits sideways.
 */
export function nixieReadout(hz: number, spec: NixieSpec, unit: NixieUnit): NixieReadout {
  const n = spec.mhzTubes + spec.fracTubes;
  // Resolution of the last tube: 1 Hz for the 6-fraction layouts, 1 kHz for the FM screen's 3.
  const res = spec.fracTubes === 6 ? 1 : 1000;
  const v = Math.round(Math.max(0, hz) / res);
  const digits = String(v).padStart(n, '0').slice(-n);
  let litAt = -1;
  if (spec.bulbAt.length === 1) litAt = spec.bulbAt[0];              // fm: always MHz
  else if (unit === 'mhz') litAt = spec.bulbAt[0];
  else if (unit === 'khz') litAt = spec.bulbAt[1];
  const firstSig = digits.search(/[1-9]/);
  const keepFrom = Math.min(firstSig < 0 ? n - 1 : firstSig, litAt > 0 ? litAt - 1 : n - 1);
  const tubes = [...digits].map((d, k) => (k < keepFrom ? null : d));
  return { tubes, bulbs: spec.bulbAt.map((at) => at === litAt) };
}

// ── Geometry ─────────────────────────────────────────────────────────────────

/** A tube's DESIGN size at scale 1 (Deck.mockup fb / fv tables): tube width, glass height, digit px. */
export interface TubeDesign { tw: number; th: number; nf: number }

/** Deck.mockup: bar `fb` (shared: th 34, nf 28), VU/analogue `fv0` / shared, landscape `fbL` / `fvL`. */
export const TUBE_DESIGN = {
  bar:           { tw: 21, th: 40, nf: 32 },
  barShared:     { tw: 21, th: 34, nf: 28 },
  barLand:       { tw: 19, th: 32, nf: 26 },
  meter:         { tw: 22, th: 36, nf: 30 },
  meterShared:   { tw: 18, th: 29, nf: 26 },
  meterLand:     { tw: 15, th: 27, nf: 23 },
} as const;

/** Stack pieces (pt at scale 1): tip-off pip, socket collar, and the clearance — 2 × CLEAR, all of it
 *  above the pip (the collar stands on the floor, see the ★★★ above). */
export const PIP_H = 2;
export const COLLAR_H = 4;
export const CLEAR = 2;
/** Floor for the glass: below this the dome cannot be drawn at all. The stack then CLIPS rather than
 *  lie — only reachable in a window under 18 pt, which no layout has. */
export const MIN_GLASS = 8;

export interface TubeBox { x: number; w: number }
export interface NixieGeometry {
  tubes:     TubeBox[];
  /** The INS-1 bulbs: `w` is the glass; the collar is w + 2 and the slot 1 pt more each side. */
  bulbs:     TubeBox[];
  /** Glass height of a digit tube and of a bulb. */
  glassH:    number;
  bulbH:     number;
  /** Digit size (px) for the cathodes. */
  nf:        number;
  gap:       number;
  /** y of the collar's top edge (the socket floor line) — everything stands on it. The collar's foot
   *  (collarY + COLLAR_H) is the window's bottom edge: the tubes stand on the line under the window. */
  collarY:   number;
  /** The natural (unshrunk) group width, for the window to ASK for. */
  naturalW:  number;
  /** 0..1 — how much the group was narrowed to fit. */
  fit:       number;
}

/**
 * Lay the fixed row out in a w × h window.
 * @param bar   the bar-meter window (§7 TRAP: narrower, 16 pt tubes, 1 pt gaps).
 * @param scale the UI scale (s.r) applied to every design number.
 * ★ The group is CENTRED IN THE TUBE WINDOW, which excludes the unit label — so kHz / MHz / Hz can
 *   never move a tube (§7 TRAP: anchor the group, do not centre it with the label).
 */
export function nixieGeometry(w: number, h: number, spec: NixieSpec, design: TubeDesign,
                              opts: { bar: boolean; scale?: number }): NixieGeometry {
  const sc = opts.scale ?? 1;
  const n = spec.mhzTubes + spec.fracTubes;
  const nb = spec.bulbAt.length;
  const k10 = Math.min(1, 8 / n);
  const barK = opts.bar ? Math.min(1, 16 / design.tw) : 1;
  const k = barK * k10;
  let tw = design.tw * k * sc;
  let nf = (k < 1 ? design.nf * (0.5 + k / 2) : design.nf) * sc;
  const gap = (k < 1 ? 1 : 2) * sc;
  const bw0 = Math.round(tw * 0.45);
  const slot = (bw: number) => bw + 2 * sc + 2 * sc;          // collar (w + 2) + 1 pt margin each side
  const naturalW = n * tw + nb * slot(bw0) + (n + nb - 1) * gap;
  // ★ The bulbs' collar overhang and margins are fixed points, not proportional — fit the rest.
  const fixed = nb * 4 * sc;
  const fit = naturalW > fixed && w > fixed ? Math.min(1, (w - fixed) / (naturalW - fixed)) : 1;
  tw *= fit;
  const bw = Math.max(2, bw0 * fit);
  const glassMax = design.th * sc;
  const glassH = Math.max(MIN_GLASS * sc, Math.min(glassMax, h - (PIP_H + COLLAR_H + 2 * CLEAR) * sc));
  nf = Math.min(nf * fit, glassH * 0.8);
  const bulbH = glassH * 0.62;
  const gapF = gap * fit;
  const groupW = n * tw + nb * (bw + 4 * sc) + (n + nb - 1) * gapF;
  let x = Math.max(0, (w - groupW) / 2);
  const tubes: TubeBox[] = [];
  const bulbs: TubeBox[] = [];
  for (let d = 0; d < n; d++) {
    const b = spec.bulbAt.indexOf(d);
    if (b >= 0) {
      bulbs[b] = { x: x + 2 * sc, w: bw };                     // 1 pt margin + the collar's 1 pt overhang
      x += bw + 4 * sc + gapF;
    }
    tubes.push({ x, w: tw });
    x += tw + gapF;
  }
  // ★★★ Anchored: the collar's foot on the window floor, no clearance under it (see the header).
  const collarY = h - COLLAR_H * sc;
  return { tubes, bulbs, glassH, bulbH, nf, gap: gapF, collarY, naturalW, fit };
}

/** The whole stack's height for a glass height — what must fit in the window (its clearance included,
 *  all of it above the pip). */
export function stackHeight(glassH: number, scale = 1): number {
  return glassH + (PIP_H + COLLAR_H + 2 * CLEAR) * scale;
}
