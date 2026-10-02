/**
 * plateLight.ts — ONE light angle for every lit metal surface (BRIEF-lighting-and-vfd-glass §2), as pure
 * maths so scripts/test_faceplate_lighting.ts can prove LEFT (104°) reproduces today's numbers exactly.
 *
 * ★★ The sheen direction used to be baked into each surface (ChassisPlate's cssAngle(104), the black gloss
 *   panel's cssAngle(112), the hot-spot at a fixed 28 % -10 %, the screws' highlight at 35 % 30 %). A light
 *   that moves has to move ALL of them together, "or the deck reads as parts from different photos".
 * ★ NOT derived (the brief): the bottom shade (gravity, not the lamp), the dome caps' top chamfer and cap
 *   sheen (overhead room light always catches a cap's top edge), the lips.
 * ★★ Every function here is a WORKLET with no default parameters and no module constants read in a default
 *   — they run on the UI thread from useDerivedValue (the LED VU crash: a default param reading a module
 *   constant is a ReferenceError there; scripts/test_worklet_defaults.mjs).
 */

/** Today's angle — LEFT, light from the left, slightly above (Stuart's Mac landscape bar). */
export const LIGHT_DEFAULT_DEG = 104;

/** CSS `linear-gradient(<deg>, …)` → start / end points over a w × h box (CSS convention: 0 = up,
 *  90 = right). The same formula ChassisPlate's cssAngle has always used. */
export function cssAnglePts(deg: number, w: number, h: number): { sx: number; sy: number; ex: number; ey: number } {
  'worklet';
  const a = (deg * Math.PI) / 180;
  const dx = Math.sin(a), dy = -Math.cos(a);
  const len = Math.abs(w * dx) + Math.abs(h * dy);
  const cx = w / 2, cy = h / 2;
  return { sx: cx - (dx * len) / 2, sy: cy - (dy * len) / 2, ex: cx + (dx * len) / 2, ey: cy + (dy * len) / 2 };
}

/** The black gloss panel's reflection: the plate's angle + 8°, today's offset (104 → 112). */
export function glossAngle(deg: number): number {
  'worklet';
  return deg + 8;
}

/**
 * The radial hot-spot's x, as a fraction of the width: on the LIT side — the brief's `50 % − 22 % × sin(angle)`,
 * with the 22 % scaled by 1 / sin(104°).
 * ★★ Scaled so 104° gives EXACTLY today's 28 %: the brief's formula gives 28.65 % at 104° (its "today's 28 %"
 *   is that, rounded), which would move today's hot-spot by 0.65 % of the width and break acceptance §6.2
 *   ("pixel-identical to today"). Scaling the slope (not offsetting it) keeps the mirror exact: RIGHT (256°)
 *   is 72 %, TOP (180°) is 50 % — and there is no step at 104° for tilt to cross.
 */
export function hotspotX(deg: number): number {
  'worklet';
  return 0.5 - (0.22 * Math.sin((deg * Math.PI) / 180)) / Math.sin((104 * Math.PI) / 180);
}

/** The radial hot-spot's y: −10 %, fixed (the brief: y unchanged). */
export const HOTSPOT_Y = -0.10;

/**
 * A screw's highlight centre, as fractions of its 9 pt box (today `radial-gradient(circle at 35% 30%)`).
 * ★★ It sits TOWARD the lamp: horizontally it swings with the light, today's −0.15 scaled by sin(angle) /
 *   sin(104°), so LEFT is 35 %, TOP 50 %, RIGHT the exact mirror 65 %; vertically it stays at today's 30 %
 *   because every offered light is from ABOVE (the brief: never from below). Rotating today's offset instead
 *   was tried on paper and put RIGHT's highlight on the screw's LOWER right — a lamp under the deck.
 */
export function screwHighlight(deg: number): { fx: number; fy: number } {
  'worklet';
  return { fx: 0.5 - (0.15 * Math.sin((deg * Math.PI) / 180)) / Math.sin((104 * Math.PI) / 180), fy: 0.30 };
}
