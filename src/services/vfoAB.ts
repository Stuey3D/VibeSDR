/**
 * vfoAB.ts — V12 VFO A/B on a VibeServer (GitHub discussion #57; server: vibe_watch.h, docs/v12/VFO-AB.md).
 *
 * Stuart, 2026-10-27: "dual frequencies per radio … a simple VFO A and VFO B", like VFO A/B on a rig. VFO A is the
 * listener's ordinary channel; VFO B is the server's WATCH CHANNEL id 1 — its own frequency, mode, passband and squelch,
 * mixed by the SERVER into the one audio stream the app already plays: the LOWER-frequency VFO in the LEFT ear, the
 * HIGHER in the RIGHT (the waterfall and the ears always agree), both in both ears within 5 Hz of each other, or with
 * the listener's STEREO switch off, or on a mono output.
 *
 * ★★ WHAT LIVES HERE: the wire (parse the server's `watch` state, build every message the app sends), and the L/R
 *    tags the waterfall draws beside each needle — computed with the SAME rule as the server's PanMixer (100 Hz of
 *    hysteresis before the ears swap, 5 Hz = the same frequency), so the tag never says an ear the audio is not in.
 * ★★ WHAT DOES NOT: which VFO the controls act on is the client's (VibeServerWsClient's A/B slots); the screen only
 *    draws it.
 * ★ PURE (type-only imports, no React Native), so scripts/test_vfoAB.ts runs it under plain node.
 */

export type VfoId = 'A' | 'B';
export type Ear = 'L' | 'R' | 'LR';

/** One VFO's tuning, as the screen draws it. */
export interface VfoSlot {
  frequency: number;
  mode: string;
  bandwidthLow: number;
  bandwidthHigh: number;
  /** dBFS squelch threshold, VS_SQL_OFF (-999) = off — the screen's own squelch value for this VFO. */
  squelchDb: number;
}

/** One watch channel, as the server reports it. */
export interface WatchChan {
  id: number;
  freq: number;
  mode: string;
  bw: number;
  sqlOn: boolean;
  sql: number;
  /** Its squelch is open (always true with the squelch off). */
  open: boolean;
  /** When `open` last changed — wall-clock ms (0 = never). The scanner's priority watch reads this. */
  since: number;
  /** Its level, dB on the wide row (the squelch's units). */
  lvl: number;
  /** Outside the radio's capture window — kept, but silent until it is back inside. */
  parked: boolean;
}

export interface WatchState {
  /** Watch channels this listener may hold here (0 = VFO A/B not available now). */
  max: number;
  /** The capture window VFO B must stay inside, Hz. */
  lo: number;
  hi: number;
  stereo: boolean;
  monoOut: boolean;
  chans: WatchChan[];
  /** Set when the server refused the last request — outside | full | off | mode | id | session. */
  refused?: string;
  /** The server's sentence, shown as it is. */
  note?: string;
}

/** ★ The server's PanMixer constants (vibe_watch.h kSwapHz / kSameHz) — the tags must agree with the audio. */
export const SWAP_HZ = 100;
export const SAME_HZ = 5;
/** VFO B is watch channel 1. */
export const VFO_B_ID = 1;

const num = (v: unknown, d = 0): number => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const text = (v: unknown, max: number): string | undefined =>
  typeof v === 'string' ? v.replace(/[\u0000-\u001f]/g, '').slice(0, max) : undefined;

/** The server's `watch` message → state. Null for anything that is not one. Every field sanitised: this is the
 *  network talking, and a NaN reaching a needle's x is a needle drawn nowhere. */
export function parseWatch(msg: Record<string, unknown> | null | undefined): WatchState | null {
  if (!msg || msg.type !== 'watch') return null;
  const chans: WatchChan[] = [];
  if (Array.isArray(msg.chans)) {
    for (const c of msg.chans.slice(0, 8)) {
      if (!c || typeof c !== 'object') continue;
      const o = c as Record<string, unknown>;
      const id = num(o.id, -1), freq = num(o.freq);
      if (id < 1 || !(freq > 0)) continue;
      chans.push({
        id, freq, mode: text(o.mode, 12) || 'am', bw: Math.max(0, num(o.bw)),
        sqlOn: o.sqlOn === true, sql: num(o.sql, -100), open: o.open !== false, since: num(o.since),
        lvl: num(o.lvl, -200), parked: o.parked === true,
      });
    }
  }
  const st: WatchState = {
    max: Math.max(0, Math.floor(num(msg.max))), lo: num(msg.lo), hi: num(msg.hi),
    stereo: msg.stereo !== false, monoOut: msg.monoOut === true, chans,
  };
  const r = text(msg.refused, 16); if (r) st.refused = r;
  const n = text(msg.note, 200); if (n) st.note = n;
  return st;
}

/** hwinfo's `watch`: how many watch channels this server offers (0 or absent = none — no toggle, no dead control). */
export function watchOffered(hwinfo: Record<string, unknown> | null | undefined): number {
  const n = hwinfo ? num(hwinfo.watch) : 0;
  return n > 0 ? Math.floor(n) : 0;
}

/** ★★ The `watch` STATE's `max` — the same offer, sent again whenever it changes without an hwinfo (the server's
 *  watchTellMax: a listener's own mode into or out of DRM, which takes VFO B away and gives it back). The web reads it
 *  (spectrum.ts `if (w.max >= 0) this.watchMax = w.max`); the app read hwinfo only, so an offer the server re-stated
 *  here never reached the toggle. Null = this message does not say (absent or junk — keep what we have). */
export function watchMaxOf(msg: Record<string, unknown> | null | undefined): number | null {
  if (!msg || msg.type !== 'watch' || typeof msg.max !== 'number' || !Number.isFinite(msg.max) || msg.max < 0) return null;
  return Math.floor(msg.max);
}

/** VFO B, from a state (undefined when it is off). */
export function vfoB(st: WatchState | null | undefined): WatchChan | undefined {
  return st?.chans.find((c) => c.id === VFO_B_ID);
}

// ── Messages ───────────────────────────────────────────────────────────────────────────────────────────────────────
/** Turn VFO B on: the server creates it at VFO A's frequency, mode and width. `stereo` / `monoOut` ride along so the
 *  very first block is mixed the way this listener wants it. */
export function msgOn(stereo: boolean, monoOut: boolean): Record<string, unknown> {
  return { type: 'watch', id: VFO_B_ID, on: true, stereo, monoOut };
}
export function msgOff(): Record<string, unknown> {
  return { type: 'watch', id: VFO_B_ID, off: true };
}
/** Re-aim VFO B (any subset). The passband travels as the edges, as a VFO A `bandwidth` does. */
export function msgAim(o: { frequency?: number; mode?: string; low?: number; high?: number }): Record<string, unknown> {
  const m: Record<string, unknown> = { type: 'watch', id: VFO_B_ID };
  if (o.frequency && o.frequency > 0) m.frequency = Math.round(o.frequency);
  if (o.mode) m.mode = o.mode;
  if (Number.isFinite(o.low) && Number.isFinite(o.high) && (o.high as number) > (o.low as number)) {
    m.bandwidthLow = Math.round(o.low as number); m.bandwidthHigh = Math.round(o.high as number);
  }
  return m;
}
/** VFO B's own squelch: `db` is the app's value (VS_SQL_OFF = -999 is off — vibeSquelch.ts's one rule). */
export function msgSquelch(db: number): Record<string, unknown> {
  const on = Number.isFinite(db) && db > -999;
  return { type: 'watch', id: VFO_B_ID, sql: on ? Math.round(db * 10) / 10 : -100, sqlOn: on };
}
export function msgStereo(on: boolean): Record<string, unknown> { return { type: 'watch', stereo: on }; }
export function msgMonoOut(on: boolean): Record<string, unknown> { return { type: 'watch', monoOut: on }; }

// ── The ears ───────────────────────────────────────────────────────────────────────────────────────────────────────
/**
 * Which ear each VFO is in — the server's rule exactly: ranked by frequency, the lower LEFT, the higher RIGHT; within
 * SAME_HZ both in both ears; the order swaps only once one is SWAP_HZ past the other (so a needle wobbling over the
 * other never flutters the tags, as it never flutters the audio). Stereo off or a mono output: no tags at all — both
 * are everywhere, and a tag would say otherwise.
 * ★ Stateful (the hysteresis remembers the last order). One per screen; reset() when VFO B goes.
 */
export class EarOrder {
  private aLower: boolean | null = null;
  reset() { this.aLower = null; }
  ears(aHz: number, bHz: number, stereo: boolean, monoOut = false): { A: Ear | null; B: Ear | null } {
    if (!(aHz > 0) || !(bHz > 0)) return { A: null, B: null };
    if (this.aLower === null) this.aLower = aHz <= bHz;
    else if (this.aLower && aHz > bHz + SWAP_HZ) this.aLower = false;
    else if (!this.aLower && aHz < bHz - SWAP_HZ) this.aLower = true;
    if (!stereo || monoOut) return { A: null, B: null };
    if (Math.abs(aHz - bHz) <= SAME_HZ) return { A: 'LR', B: 'LR' };
    return this.aLower ? { A: 'L', B: 'R' } : { A: 'R', B: 'L' };
  }
}

/** ★★ VFO B's needle colour. 'auto' (the default, and anything unreadable) = a contrast to A's: cyan, or amber when
 *  A's is already blue-ish — what B has drawn in since it shipped. Otherwise the listener's own pick (Stuart, build 498:
 *  "the ability for the user to choose a VFO B colour alongside the main VFO colour picker would be good"). */
export const VFO_B_AUTO = 'auto';
export function autoVfoBColour(a: string): string {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})/i.exec(a || '');
  if (!m) return '#38c8ff';
  const r = parseInt(m[1], 16), b = parseInt(m[3], 16);
  return b > 140 && b > r + 30 ? '#ffb020' : '#38c8ff';
}
export function vfoBNeedle(aColour: string, chosen: string | null | undefined): string {
  return typeof chosen === 'string' && /^#[0-9a-f]{6}$/i.test(chosen) ? chosen : autoVfoBColour(aColour);
}

/** The other one. */
export const otherVfo = (v: VfoId): VfoId => (v === 'A' ? 'B' : 'A');

/** ★ The inactive VFO's brightness — "dims only SLIGHTLY (still clearly visible and readable)" (Stuart). */
export const INACTIVE_INTENSITY = 0.75;
/** The A↔B brightness cross-fade, ms. */
export const SWITCH_FADE_MS = 200;

/** What the client tells the screen about VFO A/B. */
export interface VfoABInfo {
  /** Watch channels the server offers (0 = no VFO A/B here — no toggle). */
  offered: number;
  on: boolean;
  /** Which VFO the tune controls act on. */
  active: VfoId;
  /** The INACTIVE VFO's tuning and squelch (the active one is the screen's ordinary status). Null when off. */
  other: VfoSlot | null;
  /** The ACTIVE VFO's squelch — the slider follows it on a switch. */
  activeSquelchDb: number;
  /** The server's last state (null before the first). */
  state: WatchState | null;
}

/** The four fields of the client's status that ARE a VFO's tuning. */
export interface TuneStatus { frequency: number; mode: string; bandwidthLow: number; bandwidthHigh: number }

/** What one `watch` message did, for the client to act on. */
export interface WatchOutcome {
  /** The ACTIVE VFO's tuning changed (B adopted from the server while B is selected) — emit onStatus. */
  statusChanged: boolean;
  /** Sentences to show through the notice path (a refusal, or why B went). */
  notes: string[];
  /** VFO B is gone and the client is back to one VFO (A selected) — emit onStatus too if `statusChanged`. */
  reset: boolean;
  /** B's level, when B is selected — the meter's reading. */
  meterDb?: number;
}

/**
 * ★★ THE A/B SLOTS. The ACTIVE VFO's tuning is the client's own `status` — so every control, readout, needle and
 * bookmark on the screen works on whichever VFO is selected without knowing there are two — and the INACTIVE one waits
 * in `stash`. A switch swaps them. While B is selected, what the server says about its dial (config: VFO A's) goes to
 * A's slot (aSlot()), and tune / mode / passband / squelch go out as `watch` messages instead of moving the radio.
 * ★★ PER SESSION: a reconnect is a new session on the server, which forgets VFO B, so the client calls reset() on a
 *    fresh socket — "a reconnect starts with one VFO".
 * PURE: the client hands in its status, a sender and its mode-adopt rule; scripts/test_vfoAB.ts drives it in node.
 */
export class VfoABCore {
  offered = 0;
  on = false;
  active: VfoId = 'A';
  stereo = true;
  monoOut = false;
  state: WatchState | null = null;
  private stash: VfoSlot | null = null;
  private sqlA = -999;
  private sqlB = -999;
  private onAt = 0;
  private localAt = 0;
  private status: () => TuneStatus;
  private send: (m: Record<string, unknown>) => void;
  private adoptMode: (mode: string, t: TuneStatus) => void;
  private now: () => number;
  // ★ Fields, not parameter properties: node's type stripping (the test runs this file as it is) has no parameter properties.
  constructor(status: () => TuneStatus, send: (m: Record<string, unknown>) => void,
              adoptMode: (mode: string, t: TuneStatus) => void, now: () => number = () => Date.now()) {
    this.status = status; this.send = send; this.adoptMode = adoptMode; this.now = now;
  }

  /** Where VFO A's tuning lives right now. */
  aSlot(): TuneStatus { return this.active === 'B' && this.stash ? this.stash : this.status(); }
  get bTuned(): boolean { return this.active === 'B'; }
  private slotOfStatus(sql: number): VfoSlot {
    const s = this.status();
    return { frequency: Number(s.frequency), mode: String(s.mode), bandwidthLow: Number(s.bandwidthLow),
             bandwidthHigh: Number(s.bandwidthHigh), squelchDb: sql };
  }
  info(): VfoABInfo {
    return { offered: this.offered, on: this.on, active: this.active,
             other: this.on && this.stash ? { ...this.stash, squelchDb: this.active === 'A' ? this.sqlB : this.sqlA } : null,
             activeSquelchDb: this.active === 'A' ? this.sqlA : this.sqlB, state: this.state };
  }
  /** hwinfo's `watch`. Returns true when the offer CHANGED. A withdrawn offer resets (no dead VFO B). */
  setOffered(n: number): boolean {
    if (n === this.offered) return false;
    this.offered = n;
    if (n <= 0 && this.on) this.reset();
    return true;
  }
  /** The frequency card's toggle. Returns false when VFO A/B is not offered here. Status may change (off with B tuned). */
  setOn(on: boolean): boolean {
    if (on === this.on) return true;
    if (on) {
      if (this.offered <= 0) return false;
      this.on = true; this.onAt = this.now(); this.active = 'A';
      this.stash = this.slotOfStatus(-999);   // ★ B starts where A is (the server does the same), squelch off
      this.sqlB = -999;
      this.send(msgOn(this.stereo, this.monoOut));
      return true;
    }
    this.send(msgOff());
    this.reset();
    return true;
  }
  /** Which VFO the controls tune — a swap of the two slots; nothing is sent (both already are where they are). Returns
   *  true when the status changed. */
  select(v: VfoId): boolean {
    if (!this.on || v === this.active || !this.stash) return false;
    const cur = this.slotOfStatus(this.active === 'A' ? this.sqlA : this.sqlB);
    const s = this.status(), n = this.stash;
    s.frequency = n.frequency; s.mode = n.mode; s.bandwidthLow = n.bandwidthLow; s.bandwidthHigh = n.bandwidthHigh;
    this.stash = cur;
    this.active = v;
    return true;
  }
  /** Back to one VFO, A selected, without a word to the server. Returns true when the status changed (B was tuned). */
  reset(): boolean {
    let changed = false;
    if (this.active === 'B' && this.stash) {
      const s = this.status(), a = this.stash;
      s.frequency = a.frequency; s.mode = a.mode; s.bandwidthLow = a.bandwidthLow; s.bandwidthHigh = a.bandwidthHigh;
      changed = true;
    }
    this.on = false; this.stash = null; this.state = null; this.active = 'A';
    return changed;
  }
  // ── The controls, while B is tuned (the client asks bTuned first; status is already B's) ──
  tuneB(frequency?: number, mode?: string) {
    const s = this.status();
    if (frequency) s.frequency = frequency;
    if (mode) this.adoptMode(mode, s);
    this.localAt = this.now();
    this.send(msgAim({ frequency, mode }));
  }
  modeB(mode: string) {
    const s = this.status();
    this.adoptMode(mode, s);
    this.localAt = this.now();
    this.send(msgAim({ mode, low: s.bandwidthLow, high: s.bandwidthHigh }));
  }
  passbandB(low: number, high: number) {
    const s = this.status();
    s.bandwidthLow = low; s.bandwidthHigh = high;
    this.localAt = this.now();
    this.send(msgAim({ low, high }));
  }
  /** A squelch change, on whichever VFO is tuned. Returns true when it was B's (sent as `watch`). */
  squelch(db: number): boolean {
    if (this.active === 'B') { this.sqlB = db; this.send(msgSquelch(db)); return true; }
    this.sqlA = db;
    return false;
  }
  setStereo(on: boolean) { this.stereo = on; if (this.on) this.send(msgStereo(on)); }
  setMonoOut(on: boolean) { if (on === this.monoOut) return; this.monoOut = on; if (this.on) this.send(msgMonoOut(on)); }

  /** The server's `watch` message. */
  onWatch(msg: Record<string, unknown>): WatchOutcome {
    const out: WatchOutcome = { statusChanged: false, notes: [], reset: false };
    const st = parseWatch(msg);
    if (!st) return out;
    this.state = st;
    if (st.refused && st.note) out.notes.push(st.note);   // ★ the server's own sentence
    if (!this.on) return out;
    const b = vfoB(st);
    if (!b) {
      // ★ VFO B has gone without us (the radio filled and B yielded to a listener; a mode with no room for it; the
      //   owner switched it off; refused at creation) — after a grace for our own `on` to be answered.
      if (st.refused || this.now() - this.onAt > 1500) {
        if (st.note && !st.refused) out.notes.push(st.note);
        out.statusChanged = this.reset();
        out.reset = true;
      }
      return out;
    }
    // ★ B's tuning as the server runs it: adopted unless we have just moved it ourselves (the echo of an older request
    //   must not drag a drum backwards), and ALWAYS after a refusal (the server kept B where it was).
    if (st.refused || this.now() - this.localAt > 1500) {
      const t = this.active === 'B' ? this.status() : this.stash;
      if (t && (t.frequency !== b.freq || t.mode !== b.mode)) {
        t.frequency = b.freq;
        if (t.mode !== b.mode) this.adoptMode(b.mode, t);
        if (this.active === 'B') out.statusChanged = true;
      }
    }
    if (this.active === 'B') out.meterDb = b.lvl;
    return out;
  }
}
