/**
 * sqlAutoTrack.ts — AUTO SQUELCH for a second VFO (V12 VFO A/B round 3; docs/v12/VFO-AB.md).
 *
 * ★★ Stuart, 2026-10-27: "the 2 VFO's can they have their own squelch and auto squelch options which show in the audio
 *    menu when the 2nd VFO is active". VFO A's auto squelch is CLIENT-side (web main.ts autoSquelchTick, app SDRScreen's
 *    frame emit — briefs/BRIEF-auto-squelch.md): it tracks the channel's own quiet level from the server's `sig.chan`,
 *    opens a margin above it, holds the gate open through the gaps between words (the hang) and sends the threshold the
 *    server gates on. VFO B gets THE SAME, done the same way: the server sends B's level on the same `sig` message
 *    (`chanB`, every frame, the same measurement on the same row as `chan`), and this class runs the same steps on it
 *    and hands back the threshold to send as B's `watch` squelch.
 * ★ The algorithm is web main.ts autoSquelchTick, step for step — the constants, the seeding order, the
 *   rise-only-while-shut baseline, the neighbour ceiling, the two thresholds (`decide` judges, the applied one is OFF
 *   during the hang). Each of those has its measured reason written beside the original; they are not repeated here.
 *   A class rather than module state so it can run once per VFO. A's own implementations are left exactly as they are.
 * ★ Pure (no DOM, no React Native) — scripts/test_vfoAB.ts runs it under node, and both clients import it.
 */
import { VS_SQL_OFF } from '../services/vibeSquelch.ts';
import { SQL_NEAR_CEIL_DB, SQL_NEAR_FLOOR_DB, SQL_NEAR_SMOOTH, SQL_AUTO_SEED_RISE as SEED_RISE } from '../services/squelchNeighbours.ts';

export const SQL_AUTO_MARGIN_DEFAULT = 12;
export const SQL_AUTO_HYST_DB = 3;
export const SQL_AUTO_HANG_MS = 700;
export const SQL_AUTO_FALL = 0.20;
export const SQL_AUTO_RISE = 0.002;
/** ★★ beta92 THE SEED LOCK (Stuart 11-23: "the first time i set auto squelch it set itself below the noise and stayed open
 *  … its done that a few times"). The baseline only rises while the gate is SHUT; a seed that comes out too low opens the
 *  gate at once, so it never shuts and the baseline never rises — open for good until auto is toggled. While the gate has
 *  NEVER shut since the seed, its being open is no evidence of a signal: the baseline may rise at this rate (per sample,
 *  ~1 s at 20/s) — still under the neighbour ceiling, so a real station on the channel keeps it open. Once it has shut,
 *  the old rule. */
export const SQL_AUTO_SEED_RISE = SEED_RISE;

export class SqlAutoTrack {
  /** The channel's tracked quiet level (NaN until seeded). */
  base = NaN;
  /** The neighbour reference, smoothed (NaN = none yet). */
  near = NaN;
  /** When the signal last stood above `decide` — the hang runs from here. */
  openAt = -Infinity;
  /** The channel this baseline belongs to (frequency|mode|passband). Any change reseeds. */
  key = '';
  /** The threshold as the listener should see it (the red line) — the setting, not what the hang applies. */
  shown = NaN;
  /** The value last handed back to send (VS_SQL_OFF during a hang). */
  sent: number | null = null;
  /** ★ beta92 the gate has shut at least once since the seed (until then a rise is allowed while open — the seed lock). */
  everShut = false;

  /** Start again (auto switched on, or the VFO made afresh). */
  reset(key = '') { this.base = NaN; this.near = NaN; this.openAt = -Infinity; this.key = key; this.shown = NaN; this.sent = null; this.everShut = false; }

  /**
   * One level reading. `chan` = the channel's level (the server's figure, the units its gate compares); `key` = the
   * channel's identity; `nearExcess` = how far the channel stands above its empty neighbours on the view (NaN = cannot
   * say); `margin` = ABOVE NOISE, dB; `maxDb` = the top of the scale. Returns the threshold to SEND (VS_SQL_OFF = held
   * open by the hang), or null when it has not changed — every send is a message to the server.
   */
  tick(chan: number, key: string, nearExcess: number, margin: number, nowMs: number, maxDb = Infinity): number | null {
    if (!Number.isFinite(chan)) return null;
    if (key !== this.key) { this.key = key; this.base = NaN; this.near = NaN; this.everShut = false; }
    const nearInst = Number.isFinite(nearExcess) ? chan - nearExcess : NaN;
    if (Number.isFinite(nearInst)) {
      this.near = Number.isFinite(this.near) ? this.near + (nearInst - this.near) * SQL_NEAR_SMOOTH : nearInst;
      if (!Number.isFinite(this.base)) this.base = nearInst;   // ★ seed from it, never from a channel that may carry the signal
    }
    const openish = (nowMs - this.openAt) < SQL_AUTO_HANG_MS;
    const a = chan < this.base ? SQL_AUTO_FALL : (openish ? (this.everShut ? 0 : SQL_AUTO_SEED_RISE) : SQL_AUTO_RISE);
    this.base = Number.isFinite(this.base) ? this.base + (chan - this.base) * a : chan;
    if (Number.isFinite(this.near) && this.base > this.near + SQL_NEAR_CEIL_DB) this.base = this.near + SQL_NEAR_CEIL_DB;
    // ★ beta92 and a floor: the baseline follows the AGC both ways (squelchNeighbours.ts SQL_NEAR_FLOOR_DB)
    if (Number.isFinite(this.near) && this.base < this.near - SQL_NEAR_FLOOR_DB) this.base = this.near - SQL_NEAR_FLOOR_DB;
    const decide = this.base + margin - (openish ? SQL_AUTO_HYST_DB : 0);
    if (chan >= decide) this.openAt = nowMs;
    const gateOpen = (nowMs - this.openAt) < SQL_AUTO_HANG_MS;
    if (!gateOpen) this.everShut = true;
    this.shown = decide;
    const want = gateOpen ? VS_SQL_OFF : Math.round(Math.min(maxDb, decide));
    if (!Number.isFinite(want) || want === this.sent) return null;
    this.sent = want;
    return want;
  }
}
