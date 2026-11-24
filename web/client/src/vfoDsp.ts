/**
 * vfoDsp.ts — the Audio panel's NOISE REDUCTION / AUTO NOTCH / NOISE BLANKER, for VFO A and (beta93) VFO B. Pure: no DOM,
 * so scripts/test-vfo-dsp.ts runs it under plain node.
 *
 * ★★★ THE ECHO RING (Stuart, 2026-11-24, the XCover's web page). The NR slider slid by itself — 38 → 33 → 31 → … → 2 % —
 *     and the spectrum socket ran at 1 MB/s against a normal 20 KB/s. Two halves, both "say so" fixes on their own:
 *       · the page RE-SENT every strength the server reported (onDspState dispatched `input` on the slider, 2026-08-10);
 *       · the server ANSWERED every `nr` with a full hwinfo (2026-11-11, so a switch never "flipped back").
 *     Put two values in flight — a drag, or the reconnect's restore crossing the arrival hwinfo — and they circulate for
 *     ever: each report re-sent, each re-send answered. ~700 laps a second on a LAN; on a shared dial every page echoes
 *     every broadcast, so the ring doubles each lap. That flood is also what kept VFO B from switching off (its OFF sat
 *     behind it).
 *  ★★ THE RULE NOW: a report PAINTS, it never SENDS (ReportedControl has no path to the wire), and the server answers a
 *     CHANGE only. A press sends, and holds off reports for DSP_PRESS_HOLD_MS so a drag is not dragged back by its own
 *     older echoes. "The server's word wins" is kept: after the hold, whatever it reports is what is shown.
 *
 * ★★★ TWO SETS WITH VFO B (Stuart: "maybe needs two sets of nr/nb controls for each vfo"). The server runs each VFO's NR,
 *     auto notch and blanker separately (vibe_watch.h ChannelFx, local_sdr_shim.cpp WatchDsp): VFO A's are the ordinary
 *     `nr` / `notch` / `nbx` messages and hwinfo; VFO B's go as {"type":"watch","id":1,…} and come back in the watch
 *     state's chans[id 1]. The B set is drawn only when the server REPORTS B's (an older server's B has none — a B set
 *     there would be a dead control).
 */

/** How long a press holds off the server's reports of that control (ms). A drag's echoes land inside it. */
export const DSP_PRESS_HOLD_MS = 1500;

/** ★★★ The slider's percentage → the wire's strength. 0..80 % covers AudioNR's floor (0..1.0), the last fifth crosses into
 *  over-subtraction (1.0..1.4, where AudioNR::setStrength clamps) — see the long note this carried in main.ts. */
export function nrStrength(pct: number): number {
  const p = Math.max(0, Math.min(100, pct));
  return p <= 80 ? p / 80 : 1 + ((p - 80) / 20) * 0.4;
}

/** ★★★ THE EXACT INVERSE OF nrStrength(), AND IT MUST STAY EXACT (scripts/test-nr-roundtrip.mjs). It no longer feeds a
 *  loop (reports paint, never send), but a slider that reads back a different figure from the one set is still a lie. */
export function nrPercent(strength: number): number {
  const s = Math.max(0, Math.min(1.4, strength));
  return s <= 1 ? s * 80 : 80 + ((s - 1) / 0.4) * 20;
}

/** What the NR slider should show for a server report: 0 = off; on = the server's strength as a whole percent (at least 1,
 *  so ON never reads as OFF), or `fallbackPct` when the server has no strength to state. */
export function nrSliderFromReport(on: boolean, strength: number | undefined, fallbackPct: number): number {
  if (!on) return 0;
  if (typeof strength === 'number' && Number.isFinite(strength)) return Math.max(1, Math.round(nrPercent(strength)));
  return Math.max(1, Math.round(fallbackPct) || 50);
}

/** ★★★ One control the SERVER reports and the listener can press. press() is the only way a value reaches the wire (the
 *  caller sends what it returns); report() returns true when the display should change — and the caller must only PAINT. */
export class ReportedControl<T> {
  value: T;
  private pressedAt = -Infinity;
  private readonly holdMs: number;
  constructor(initial: T, holdMs = DSP_PRESS_HOLD_MS) { this.value = initial; this.holdMs = holdMs; }
  /** The listener moved it: the caller sends `v`. */
  press(v: T, nowMs: number): T { this.value = v; this.pressedAt = nowMs; return v; }
  /** The server said `v`. true = paint it (never send it). Held off just after a press; equal = nothing to do. */
  report(v: T, nowMs: number): boolean {
    if (nowMs - this.pressedAt < this.holdMs) return false;
    if (Object.is(v, this.value)) return false;
    this.value = v;
    return true;
  }
}

/** VFO B's own NR / notch / blanker as the server's watch state reports them (chans[id 1]) — null = not reported (an older
 *  server, or no VFO B): no B set is drawn. */
export interface VfoBDsp { nr: boolean; nrStrength?: number; notch: boolean; nbx?: boolean }
export function vfoBDspOf(chan: unknown): VfoBDsp | null {
  const c = chan as Record<string, unknown> | null | undefined;
  if (!c || typeof c.nr !== 'boolean') return null;
  return {
    nr: c.nr, notch: c.notch === true,
    nrStrength: typeof c.nrStrength === 'number' && Number.isFinite(c.nrStrength) ? c.nrStrength : undefined,
    nbx: typeof c.nbx === 'boolean' ? c.nbx : undefined,
  };
}

/** Which sets the Audio panel draws: A alone, or A and B (VFO B on AND its DSP reported). */
export function dspSets(abOn: boolean, bDsp: VfoBDsp | null): Array<'A' | 'B'> {
  return abOn && bDsp ? ['A', 'B'] : ['A'];
}

/** The wire for VFO B's settings — the VFO-B channel convention (watch, id 1), never the A messages. */
export function vfoBDspMsg(s: { nr?: boolean; nrStrength?: number; notch?: boolean; nbx?: boolean }): Record<string, unknown> {
  const m: Record<string, unknown> = { type: 'watch', id: 1 };
  if (s.nr !== undefined) m.nr = s.nr;
  if (s.nrStrength !== undefined) m.nrStrength = Math.round(s.nrStrength * 1e6) / 1e6;
  if (s.notch !== undefined) m.notch = s.notch;
  if (s.nbx !== undefined) m.nbx = s.nbx;
  return m;
}
