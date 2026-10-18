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

/**
 * A stick turned like a jog wheel. feed() returns whole steps since the last call: + clockwise, − anticlockwise.
 *
 * ★★ LIKE THE DRUM, SPEED DECIDES THE RATE (Stuart, 2026-10-18: "detect when a user is slowing down and make them more
 *    accurate, I did find myself overshooting signals"). Pass the time (ms) and the degrees per step follow how fast
 *    the stick is turning: slow and deliberate = more turn per step (landing on a signal), normal = degPerStep, a fast
 *    spin = fewer degrees per step (crossing a band). The STEP itself never changes — that is the user's step (9 kHz
 *    on MW, a channel grid) — only how much turning one step costs.
 *    The speed is smoothed, quickly when slowing and gently when speeding up, so easing off to land is felt at once.
 *    Without a time, every step costs degPerStep (the original behaviour).
 * ★★ THE SPRING-BACK IS NOT A TURN. Letting go, the stick springs back to the centre, and near the centre its angle is
 *    noise: those last few degrees used to count, adding a step or two after the user had stopped. Rotation now counts
 *    only while the stick is out near its rim (`count`); inside that the angle is followed but never counted.
 */
export class Jog {
  private last: number | null = null;
  private acc = 0;
  private lastT: number | null = null;
  private speed = 0;                       // °/s, smoothed
  /** @param degPerStep turn for one step at normal speed. @param engage how far out the stick must be to start
   *  (0..1). @param release where it lets go. @param count how far out it must be for turning to count. */
  constructor(private degPerStep: number, private engage = 0.6, private release = 0.35, private count = 0.7) {}
  /** Screen convention: x right, y DOWN (the Gamepad API's), so a growing atan2(y, x) is clockwise. */
  feed(x: number, y: number, t?: number): number {
    const mag = Math.hypot(x, y);
    if (mag < (this.last === null ? this.engage : this.release)) {
      this.last = null; this.acc = 0; this.lastT = null; this.speed = 0;
      return 0;
    }
    const a = Math.atan2(y, x) * 180 / Math.PI;
    if (this.last === null) { this.last = a; this.lastT = t ?? null; return 0; }
    let d = a - this.last;
    if (d > 180) d -= 360;
    if (d < -180) d += 360;
    this.last = a;
    if (t !== undefined && this.lastT !== null) {
      const dt = Math.max(1, t - this.lastT) / 1000;
      const v = Math.abs(d) / dt;
      // The first movement after taking hold sets the speed outright: a turn begun briskly is brisk from its first step.
      if (this.speed === 0) this.speed = v;
      else this.speed += (v - this.speed) * Math.min(1, dt / (v < this.speed ? JOG_SLOWING_S : JOG_SPEEDING_S));
    }
    if (t !== undefined) this.lastT = t;
    if (mag < this.count) { this.acc = 0; return 0; }   // springing back: followed, never counted; a part-turned step is dropped
    const per = t === undefined ? this.degPerStep : jogDegPerStep(this.degPerStep, this.speed);
    this.acc += d;
    const steps = this.acc > 0 ? Math.floor(this.acc / per) : Math.ceil(this.acc / per);
    this.acc -= steps * per;
    return steps;
  }
}

// The speed curve (°/s of stick rotation). A deliberate quarter turn takes about a second (~90 °/s); a spin is several
// turns a second.
const JOG_SLOW = 120, JOG_NORMAL = 300, JOG_FAST = 450;
const JOG_SLOWING_S = 0.04, JOG_SPEEDING_S = 0.12;
/** Degrees of turn per step at `speed` °/s: 1.5× degPerStep when slow, degPerStep at normal speed, down to a third of it
 *  on a fast spin. */
export function jogDegPerStep(degPerStep: number, speed: number): number {
  if (speed <= JOG_SLOW) return degPerStep * 1.5;
  if (speed < JOG_NORMAL) return degPerStep * (1.5 - 0.5 * (speed - JOG_SLOW) / (JOG_NORMAL - JOG_SLOW));
  if (speed <= JOG_FAST) return degPerStep;
  return Math.max(degPerStep / 3, degPerStep * JOG_FAST / speed);
}

/** Which mapping a pad gets: with two analogue sticks, or without (by position). An explicit choice wins. */
export function padProfile(axes: number, forced?: string | null): 'sticks' | 'buttons' {
  if (forced === 'sticks' || forced === 'buttons') return forced;
  return axes >= 4 ? 'sticks' : 'buttons';
}
