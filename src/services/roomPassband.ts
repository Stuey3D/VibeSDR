/* ★★★ THE ROOM'S PASSBAND AND MODE, FROM A VibeServer `config` (2026-10-17) — one rule, read by the app
 *  (VibeServerWsClient) and the web client (spectrum.ts) alike, so the two cannot drift. Jr has the same
 *  rule in Swift (UberClient.adoptRoomPassband).
 *
 *  Stuart: "on shared VFO, if a user changes the bandwidth sliders I am hearing them but they aren't
 *  synchronising with mine." The server applied the passband to the one shared demodulator — so the
 *  audio changed for everybody — and `config` never said what the passband was. It does now
 *  (local_sdr_shim.cpp sendConfig, vibe_room_passband.h): `bandwidthLow`/`bandwidthHigh`, present only
 *  while the edges belong to the mode the radio is in.
 *
 *  ★★★ THE SHARED-DIAL CONTRACT, BOTH HALVES. The client ADOPTS what the server states, and adopting is
 *      NEVER a send: the caller writes its own state and redraws, and must not go through its
 *      setBandwidth()/setMode(), which transmit. An adopted value echoed back as a "change" is the loop
 *      that has bitten this project before (the NR ratchet, the hwinfo re-assert).
 *  ★★ NOT INSIDE OUR OWN ECHO. While this listener is dragging a slider the server answers each send
 *     with a config, and every config sent for anything else in that window (a zoom, somebody's tune)
 *     carries the edges from BEFORE the drag reached the server. Adopting those would yank the thumb
 *     back under the finger. Same 1.5 s settle the frequency adopt uses.
 *  ★★ AND THE MODE, NOT ONLY WHEN THE DIAL MOVES. Both clients adopted the room's mode only as part of a
 *     FREQUENCY move (> 100 Hz), so somebody switching USB to LSB on the same frequency was heard and not
 *     shown — and the passband, which is keyed on the mode, could then never match either. */

export interface RoomCtx {
  /** The dial is shared (config.shared, or the dial message said so). On a radio of one's own there is
   *  no room, and the client's own remembered state is the answer. */
  shared: boolean;
  /** The mode this client is showing now. */
  mode: string;
  /** The passband this client is showing now. */
  low: number;
  high: number;
  /** Milliseconds since this client itself last tuned, changed mode or moved the passband. */
  sinceLocalMs: number;
}

/** Below this, a config may be the echo of our own change — see the header. */
export const ROOM_SETTLE_MS = 1500;

const num = (v: unknown): number | null => (typeof v === 'number' && Number.isFinite(v) ? v : null);

/** The room's mode when it differs from ours and we may adopt it, else null. */
export function roomMode(msg: Record<string, unknown>, ctx: RoomCtx): string | null {
  if (!ctx.shared || ctx.sinceLocalMs <= ROOM_SETTLE_MS) return null;
  const m = typeof msg.mode === 'string' ? msg.mode.toLowerCase() : '';
  if (!m || m === String(ctx.mode).toLowerCase()) return null;
  return m;
}

/** The room's passband edges when they differ from ours and we may adopt them, else null.
 *  ★ `ctx.mode` must be the mode AFTER any adopt from this same message — the server only reports edges
 *    for the mode it is in, and edges from one mode drawn on another are nonsense (SSB's 50..2700 on AM). */
export function roomPassband(msg: Record<string, unknown>, ctx: RoomCtx): [number, number] | null {
  if (!ctx.shared || ctx.sinceLocalMs <= ROOM_SETTLE_MS) return null;
  const lo = num(msg.bandwidthLow), hi = num(msg.bandwidthHigh);
  if (lo === null || hi === null || !(hi > lo)) return null;
  const m = typeof msg.mode === 'string' ? msg.mode.toLowerCase() : '';
  if (!m || m !== String(ctx.mode).toLowerCase()) return null;
  if (Math.abs(lo - ctx.low) < 1 && Math.abs(hi - ctx.high) < 1) return null;   // already showing it
  return [lo, hi];
}
