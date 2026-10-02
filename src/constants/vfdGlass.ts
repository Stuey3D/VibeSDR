/**
 * vfdGlass.ts — the VFD glass's FILAMENT WIRES (BRIEF-lighting-and-vfd-glass §1), as pure maths so
 * scripts/test_faceplate_lighting.ts can hold the count, spacing and pixel snapping down.
 *
 * ★★ A real VFD has a few hair-thin tungsten filaments stretched horizontally across the WHOLE glass, in
 *   front of everything. They are not the GRID (the mesh on the RDS pictogram, which belongs to each lit
 *   area — that stays). Over a lit segment the wire SHADOWS the phosphor (the dark line reads); over unlit
 *   glass the faint highlight above it reads. "You should only see them when you look for them."
 */

/** The dark line: the wire's shadow on the phosphor. ★ The brief's MAXIMUM — lower it, never raise it. */
export const FILAMENT_DARK = 'rgba(0,0,0,0.50)';
/** The one-device-pixel highlight directly above it, which is what reads over unlit glass. */
export const FILAMENT_LIGHT = 'rgba(255,255,255,0.06)';

/** How many wires a window `h` pt tall gets: `max(2, round(h / 22))`. ★ By HEIGHT, never width — the Mac's
 *  long landscape strip still gets 2. */
export function filamentCount(h: number): number {
  if (!(h > 0)) return 0;
  return Math.max(2, Math.round(h / 22));
}

/**
 * The wires' y positions (pt), evenly spaced — ⅓ and ⅔ for today's windows — each SNAPPED to the device
 * pixel grid. ★★ TRAP (sub-pixel smear): ⅓ of 40 pt on a @3x panel lands between pixels and Skia
 * antialiases a 1-px line into a 2-px grey smear that reads as a scan line. `pr` = PixelRatio.get().
 */
export function filamentYs(h: number, pr: number): number[] {
  const n = filamentCount(h);
  if (!n || !(pr > 0) || !Number.isFinite(pr)) return [];
  const out: number[] = [];
  for (let i = 1; i <= n; i++) out.push(Math.round(((i * h) / (n + 1)) * pr) / pr);
  return out;
}

/** One device pixel, in pt. */
export function devicePixel(pr: number): number {
  return pr > 0 && Number.isFinite(pr) ? 1 / pr : 1;
}
