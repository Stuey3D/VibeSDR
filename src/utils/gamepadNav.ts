/**
 * ★★★ GAMEPAD — the pure parts (Stuart's mapping, 2026-10-10; memory: gamepad_mapping). Tested by
 * scripts/test_gamepad_nav.ts. The web page's gamepad layer (web/client/src/gamepad.ts) drives these.
 *
 *  • pickNext — where the highlight goes for a D-pad press: the nearest control in that direction, judged on the
 *    controls' boxes on screen (so it follows the layout, whatever it is).
 *  • Jog — a stick ROTATED like a jog wheel: clockwise = forwards. Only while the stick is pushed well out, so a
 *    resting stick never drifts the dial; the angle is unwrapped, so passing through "west" is not a 360° jump.
 */
export interface Box { x: number; y: number; w: number; h: number }
export type Dir = 'up' | 'down' | 'left' | 'right';

const cx = (b: Box) => b.x + b.w / 2;
const cy = (b: Box) => b.y + b.h / 2;

/** Index of the box to move to from `cur` in `dir`, or -1 if nothing lies that way. With no current box, the one
 *  nearest the top-left. */
export function pickNext(cur: Box | null, boxes: Box[], dir: Dir): number {
  if (!boxes.length) return -1;
  if (!cur) {
    let best = 0;
    for (let i = 1; i < boxes.length; i++)
      if (boxes[i].y + boxes[i].x * 0.5 < boxes[best].y + boxes[best].x * 0.5) best = i;
    return best;
  }
  let best = -1, bestScore = Infinity;
  for (let i = 0; i < boxes.length; i++) {
    const b = boxes[i];
    if (b.x === cur.x && b.y === cur.y && b.w === cur.w && b.h === cur.h) continue;
    const dx = cx(b) - cx(cur), dy = cy(b) - cy(cur);
    // Along the direction (must be positive) and across it (penalised, so a control straight ahead wins over a
    // nearer one off to the side).
    const along = dir === 'right' ? dx : dir === 'left' ? -dx : dir === 'down' ? dy : -dy;
    const across = dir === 'left' || dir === 'right' ? Math.abs(dy) : Math.abs(dx);
    if (along <= 1) continue;
    const score = along + across * 2.5;
    if (score < bestScore) { bestScore = score; best = i; }
  }
  return best;
}

/** A stick turned like a jog wheel. feed() returns whole steps since the last call: + clockwise, − anticlockwise. */
export class Jog {
  private last: number | null = null;
  private acc = 0;
  /** @param degPerStep how far to turn for one step. @param engage how far out the stick must be (0..1). */
  constructor(private degPerStep: number, private engage = 0.6, private release = 0.35) {}
  /** Screen convention: x right, y DOWN (the Gamepad API's), so a growing atan2(y, x) is clockwise. */
  feed(x: number, y: number): number {
    const mag = Math.hypot(x, y);
    if (mag < (this.last === null ? this.engage : this.release)) { this.last = null; this.acc = 0; return 0; }
    const a = Math.atan2(y, x) * 180 / Math.PI;
    if (this.last === null) { this.last = a; return 0; }
    let d = a - this.last;
    if (d > 180) d -= 360;
    if (d < -180) d += 360;
    this.last = a;
    this.acc += d;
    const steps = this.acc > 0 ? Math.floor(this.acc / this.degPerStep) : Math.ceil(this.acc / this.degPerStep);
    this.acc -= steps * this.degPerStep;
    return steps;
  }
}

/** Which mapping a pad gets: with two analogue sticks, or without (by position). An explicit choice wins. */
export function padProfile(axes: number, forced?: string | null): 'sticks' | 'buttons' {
  if (forced === 'sticks' || forced === 'buttons') return forced;
  return axes >= 4 ? 'sticks' : 'buttons';
}
