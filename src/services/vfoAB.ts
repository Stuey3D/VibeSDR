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
  /** ★ Round 3: muted in this listener's mix (absent from an older server = false). */
  mute: boolean;
}

export interface WatchState {
  /** Watch channels this listener may hold here (0 = VFO A/B not available now). */
  max: number;
  /** The capture window VFO B must stay inside, Hz. */
  lo: number;
  hi: number;
  stereo: boolean;
  monoOut: boolean;
  /** ★ Round 3: VFO A muted in this listener's mix. */
  muteA: boolean;
  /** ★★ Decoders run on either VFO here ("vfo":"B" on the decoder socket). Absent (a round-3 server) = A only. */
  decB: boolean;
  chans: WatchChan[];
  /** Set when the server refused the last request — outside | full | off | mode | band | apart | id | session. */
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
        lvl: num(o.lvl, -200), parked: o.parked === true, mute: o.mute === true,
      });
    }
  }
  const st: WatchState = {
    max: Math.max(0, Math.floor(num(msg.max))), lo: num(msg.lo), hi: num(msg.hi),
    stereo: msg.stereo !== false, monoOut: msg.monoOut === true, muteA: msg.muteA === true, decB: msg.decB === true, chans,
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

/* ★★★ DECODE ON EITHER VFO (Stuart, 2026-10-27: "That seems like a weird limitations surely it doesnt matter which VFO is
 *  listen and which is decode?"). A decoder — or digital spots, Single or Band — runs on the VFO SELECTED when it is
 *  started; what the decoder box tunes moves THAT VFO; selecting the other VFO later moves nothing. A server that cannot
 *  decode B (round 3: no `decB` in its watch state) keeps the old rule — the controls go to A first. The web's vfoAB.ts
 *  has the same two functions. */
/** Which VFO a decoder started now runs on, and whether the controls must go to A first (an older server). */
export function decStartVfo(abOn: boolean, sel: VfoId, decB: boolean): { vfo: VfoId; selectA: boolean } {
  if (!abOn || sel === 'A') return { vfo: 'A', selectA: false };
  return decB ? { vfo: 'B', selectA: false } : { vfo: 'A', selectA: true };
}
/** The 'DEC' tag on a VFO's needle label and squelch row: that VFO is decoding (a decoder or spots run on it). Only with
 *  two VFOs — with one there is nothing to tell apart. */
export function decTagFor(v: VfoId, abOn: boolean, decoderOn: VfoId | null, spotsOn: VfoId | null): string {
  return abOn && (decoderOn === v || spotsOn === v) ? 'DEC' : '';
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
/** ★★ Round 3 — MUTE A VFO in this listener's mix (Stuart, 2026-10-27: "the ability to mute a VFO that is decoding …
 *  when the 2nd VFO is muted the audio from the first one pans back to mono so both ears are in play"). The server
 *  fades it out and pans the other to the middle; the muted VFO keeps decoding and metering. */
export function msgMute(v: VfoId, on: boolean): Record<string, unknown> {
  return v === 'A' ? { type: 'watch', muteA: on } : { type: 'watch', id: VFO_B_ID, mute: on };
}
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
  /** ★★ Round 3: each VFO's own squelch (the Audio menu draws a row for each while B is on), VS_SQL_OFF = off. */
  squelchA: number;
  squelchB: number;
  /** ★★ Round 3: each VFO muted in the mix — the server's word. */
  muteA: boolean;
  muteB: boolean;
}

/** The four fields of the client's status that ARE a VFO's tuning. */
export interface TuneStatus { frequency: number; mode: string; bandwidthLow: number; bandwidthHigh: number }

/**
 * ★★★ THE AUDIO SOCKET IS VFO A'S DOOR — NEVER B'S. (Stuart, build 498: "if i switched between VFO's they would often
 * snap to the frequency the other was on rather than staying put".)
 *
 * On a VibeServer the app's audio socket (LocalAudioPlayer → Vibe.sendLocalTune) says `tune` whenever the frequency,
 * mode or passband it is handed changes — it is how the LISTENER'S CHANNEL is tuned. It was handed the screen's
 * `status`, and while B is selected `status` IS B (that is the whole design of the slots: every control works on the
 * selected VFO unchanged). So the moment you switched to B, B's frequency arrived at the audio socket as a change, was
 * not the server's dial (that is A's), and went out as a tune of VFO A: A jumped onto B. Every tune of B after that
 * dragged A along with it. Switching back looked like the snap had happened the other way round.
 * ★ So the door is handed VFO A's tuning whichever VFO is selected — the inactive slot while B is tuned.
 */
export function vfoATune(status: TuneStatus, info: Pick<VfoABInfo, 'on' | 'active' | 'other'> | null | undefined): TuneStatus {
  if (info?.on && info.active === 'B' && info.other) {
    const a = info.other;
    return { frequency: a.frequency, mode: a.mode, bandwidthLow: a.bandwidthLow, bandwidthHigh: a.bandwidthHigh };
  }
  return { frequency: status.frequency, mode: status.mode, bandwidthLow: status.bandwidthLow, bandwidthHigh: status.bandwidthHigh };
}

/**
 * ★★ …AND ITS "A PERSON ASKED" COUNTER. The audio socket treats a bump of the screen's userTuneSeq as a person asking
 * and re-asserts its tune even when nothing changed (audioTuneForward.ts). A bump made while B is tuned was a person
 * tuning B — which went out as a `watch` — so it must not reach A's door as a re-assert of A (on a shared dial that
 * is a transmit nobody asked for: the shared-dial contract). This counter advances only with bumps made while A is
 * selected. Pure; one per screen.
 */
export class DoorSeq {
  private raw: number | null = null;
  private out = 0;
  next(raw: number, bTuned: boolean): number {
    if (this.raw === null) { this.raw = raw; this.out = raw; return raw; }
    if (!bTuned) this.out += raw - this.raw;
    this.raw = raw;
    return this.out;
  }
}

/** ★ How long a fresh spectrum socket waits for the server's VFO B state before deciding the session has none — four
 *  of the server's half-second beats, and room for a slow link. */
export const RESYNC_MS = 2500;
/** After our own OFF, a state with B in it is the echo of the old one, not a B to adopt. */
export const ADOPT_AFTER_OFF_MS = 1500;

/** ★ A passband `width` Hz wide in the SHAPE of the mode's default edges (the server reports only B's width): a
 *  sideband stays on its side of the carrier, anything straddling it stays centred. Unusable width = the defaults. */
export function edgesForWidth(low: number, high: number, width: number): [number, number] {
  if (!(width > 0) || !(high > low) || Math.abs((high - low) - width) < 1) return [low, high];
  if (low >= 0) return [low, Math.round(low + width)];
  if (high <= 0) return [Math.round(high - width), high];
  return [Math.round(-width / 2), Math.round(width / 2)];
}

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
  /** The server holds a VFO B this client did not know about, and it has been taken on (A still selected). */
  adopted?: boolean;
}

/**
 * ★★ THE A/B SLOTS. The ACTIVE VFO's tuning is the client's own `status` — so every control, readout, needle and
 * bookmark on the screen works on whichever VFO is selected without knowing there are two — and the INACTIVE one waits
 * in `stash`. A switch swaps them. While B is selected, what the server says about its dial (config: VFO A's) goes to
 * A's slot (aSlot()), and tune / mode / passband / squelch go out as `watch` messages instead of moving the radio.
 * ★★ PER SESSION — AND THE SERVER SAYS WHICH. A new session on the server has no VFO B, so a reconnect starts with
 *    one VFO. But a fresh SPECTRUM SOCKET is not a fresh session: the client reopens it on its own (the frames-stale
 *    and pong watchdogs — a rotation freezes the JS thread for a second or two —, a network path change, the app
 *    coming back to the front) under the SAME session id, and the server keeps VFO B and keeps mixing it into the
 *    audio. Resetting on every socket open is what left Stuart with one VFO on the screen and two in his ears after
 *    turning the phone (build 498: "the 2 vfo's disappeared off screen as if I had gone back to one but I still had 2
 *    sets of audio one in each ear"). So the client tells the core a socket opened (socketOpened) and the SERVER'S
 *    STATE decides: a state carrying B keeps — or, for a client that had lost it, ADOPTS — VFO B; a state without it,
 *    or silence from a server that beats its VFO B state twice a second (expireIfSilent), means one VFO.
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
  /** When the listener last turned B OFF here — a state already on its way with B in it must not bring B back. */
  private offAt = 0;
  /** A spectrum socket opened at this time and no `watch` state has been heard on it since (0 = not waiting). */
  private openAt = 0;
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
    const b = this.on ? vfoB(this.state) : undefined;
    return { offered: this.offered, on: this.on, active: this.active,
             other: this.on && this.stash ? { ...this.stash, squelchDb: this.active === 'A' ? this.sqlB : this.sqlA } : null,
             activeSquelchDb: this.active === 'A' ? this.sqlA : this.sqlB, state: this.state,
             squelchA: this.sqlA, squelchB: this.sqlB,
             muteA: !!b && this.state?.muteA === true, muteB: !!b && b.mute };
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
    this.offAt = this.now();
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
  /** VFO B as the server runs it, taken on as ours — A selected (the status stays A's), B in the stash. */
  private adoptB(b: WatchChan) {
    const slot: VfoSlot = { frequency: b.freq, mode: '', bandwidthLow: 0, bandwidthHigh: 0, squelchDb: -999 };
    this.adoptMode(b.mode, slot);   // ★ the mode's own passband shape…
    const [lo, hi] = edgesForWidth(slot.bandwidthLow, slot.bandwidthHigh, b.bw);   // ★ …at the width the server runs
    slot.bandwidthLow = lo; slot.bandwidthHigh = hi;
    this.on = true; this.active = 'A'; this.onAt = this.now();
    this.stash = slot;
    this.sqlB = b.sqlOn ? b.sql : -999;
    slot.squelchDb = this.sqlB;
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
  /** ★★ Decode on either VFO — tune VFO B while A is selected (a tap in the decoder box when its decoder runs on B):
   *  B's slot is moved where it is held and B's `watch` aim sent; the controls stay on A. */
  tuneOtherB(frequency?: number, mode?: string) {
    if (!this.on || this.active !== 'A' || !this.stash) return;
    const s = this.stash;
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
  squelch(db: number): boolean { return this.squelchFor(this.active, db); }
  /** ★★ Round 3 — a squelch change on a NAMED VFO (the Audio menu has a row for each). Returns true when it was B's
   *  (sent here as `watch`); A's goes out as the ordinary squelch by the client. */
  squelchFor(v: VfoId, db: number): boolean {
    if (v === 'B') {
      if (!this.on) return true;   // ★ no VFO B: nothing to set, and never A's squelch by mistake
      this.sqlB = db; this.localAt = this.now(); this.send(msgSquelch(db)); return true;
    }
    this.sqlA = db;
    return false;
  }
  /** ★★ Round 3 — mute VFO `v` in the mix. Only while B is on (the server ignores it otherwise). */
  setMute(v: VfoId, on: boolean) { if (this.on) this.send(msgMute(v, on)); }
  setStereo(on: boolean) { this.stereo = on; if (this.on) this.send(msgStereo(on)); }
  setMonoOut(on: boolean) { if (on === this.monoOut) return; this.monoOut = on; if (this.on) this.send(msgMonoOut(on)); }

  /** ★★ A spectrum socket has opened. Nothing is reset: the server's next `watch` state says whether this session still
   *  has VFO B (see PER SESSION above). */
  socketOpened() { this.openAt = this.now(); }
  /** ★ The server beats its VFO B state twice a second (shim watchTick) to every spectrum socket of the session, so a
   *  socket open for RESYNC_MS with B on here and not one state heard means the server has no B for us (a new session).
   *  Returns true when that turned B off — the client emits status if `active` was B (statusChanged via reset()). */
  expireIfSilent(): { expired: boolean; statusChanged: boolean } {
    if (!this.openAt || !this.on || this.now() - this.openAt < RESYNC_MS) return { expired: false, statusChanged: false };
    this.openAt = 0;
    return { expired: true, statusChanged: this.reset() };
  }

  /** The server's `watch` message. */
  onWatch(msg: Record<string, unknown>): WatchOutcome {
    const out: WatchOutcome = { statusChanged: false, notes: [], reset: false };
    const st = parseWatch(msg);
    if (!st) return out;
    this.openAt = 0;   // ★ the server has spoken on this socket — its state, not a timer, decides from here
    this.state = st;
    if (st.refused && st.note) out.notes.push(st.note);   // ★ the server's own sentence
    if (!this.on) {
      // ★★★ THE SERVER HOLDS A VFO B FOR THIS SESSION AND WE DO NOT KNOW IT — a client made afresh under the same
      //     session (the screen rebuilt it), or a state that arrived after a timer gave up. Its audio is already in
      //     the listener's ears; the screen must show it. ADOPTED, never asked for: nothing is sent (the shared-dial
      //     contract — the server sends full state, the client has no vote). A not on its way back from our own OFF.
      const b0 = vfoB(st);
      if (b0 && !st.refused && this.offered > 0 && this.now() - this.offAt > ADOPT_AFTER_OFF_MS) {
        this.adoptB(b0);
        out.adopted = true;
      }
      return out;
    }
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
