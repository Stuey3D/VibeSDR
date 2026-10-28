/**
 * ★★ V12 TWO SERVERS AT ONCE — THE PURE RULES (GitHub discussion #57; Stuart's design, 2026-10-27).
 *
 * Stuart: "we use the server chip. We add a new option in the servers chip menu connect 2nd Server? … you then find
 * your next server and connect to that, then server 1 moves to left ear, server 2 right ear." — and a pop-out card
 * under ‹ Servers for the one in the background: its frequency, which ear it is in, its time limit, TAP to swap, HOLD
 * for the balance.
 *
 * Everything here is pure — no React Native import — so scripts/test_dual_server.ts runs it under plain node. The
 * orchestration (sockets, navigation, the native voices) is src/services/dualServer.ts; the card is
 * src/components/DualServerCard.tsx.
 *
 * ★★★ THE PAN LAW AND THE GLIDE ARE VFO A/B's, NOT NEW ONES (android/app/src/main/cpp/vibe_watch.h PanMixer / Ramp):
 *   position 0 = hard left, 0.5 = centre, 1 = hard right; L = v·√2·cos(pos·π/2), R = v·√2·sin(pos·π/2) — equal power,
 *   unity in each ear at the centre; every move a 400 ms raised cosine (zero slope at both ends, so no click), and a
 *   move re-aimed mid-glide starts FROM WHERE IT IS. "a nice transition, ~300-500 ms" (Stuart, on VFO A/B). The same
 *   Ramp is written three times — here (for the tests), VibeDualAudio.swift and VibeDualAudio.kt — ONE RULE, THREE
 *   READERS: change all three.
 *
 * ★ Each server's own audio is FOLDED TO MONO before it is placed, whatever it is (WFM stereo): a server owns ONE ear
 *   here, and its own stereo inside that ear would be half lost anyway — the other half would land in the other
 *   server's ear.
 * ★★★ TWO SERVERS OR VFO A/B — NEVER BOTH (Stuart, GitHub #57: "There will only be 2 channels at a time, either A/B on
 *     one server or Server 1 and Server 2, otherwise this gets far too complex for a little touchscreen to navigate let
 *     alone a game controller or TV remote."). So there is no VFO A/B pair to fold: starting a pairing with VFO B on
 *     closes B first (pairingStart), and while the pairing lasts A/B is not offered on either server (vfoAB.ts
 *     abOfferNow). Unpairing gives it back.
 */

/** Every pan and fade, as VFO A/B (vibe_watch.h kRampSec). */
export const DUAL_RAMP_SEC = 0.4;

/** How the two servers share the ears — the Audio menu's TWO SERVERS row. Remembered per device. */
export type DualOutput = 'stereo' | 'mono';

/** Where a voice is headed: position 0..1 (0 left, 0.5 centre, 1 right) and its level (0..1). */
export interface VoiceTarget { pos: number; gain: number }

/** A raised-cosine glide from one value to another over `dur` steps (vibe_watch.h Ramp, verbatim). */
export class Ramp {
  from = 0; to = 0; cur = 0; t = 1; dur = 1;
  constructor(v = 0) { this.set(v); }
  set(v: number) { this.from = this.to = this.cur = v; this.t = this.dur = 1; }
  aim(v: number, durSteps: number) {
    if (v === this.to) return;
    this.from = this.cur; this.to = v; this.t = 0; this.dur = Math.max(1, Math.round(durSteps));
  }
  moving() { return this.t < this.dur; }
  step(): number {
    if (this.t < this.dur) {
      this.t++;
      const u = this.t / this.dur;
      const e = 0.5 - 0.5 * Math.cos(Math.PI * u);
      this.cur = this.from + (this.to - this.from) * e;
    }
    return this.cur;
  }
}

/** The equal-power law (vibe_watch.h PanMixer::mix): what one mono sample becomes in each ear. */
export function earGains(pos: number, gain: number): { l: number; r: number } {
  const th = Math.max(0, Math.min(1, pos)) * (Math.PI / 2);
  return { l: gain * Math.SQRT2 * Math.cos(th), r: gain * Math.SQRT2 * Math.sin(th) };
}

/** Clamp a balance to −1 (all server 1) … 0 (even) … +1 (all server 2). NaN → even. */
export function clampBalance(b: number): number {
  return Number.isFinite(b) ? Math.max(-1, Math.min(1, b)) : 0;
}

/**
 * ★ THE BALANCE NEVER RAISES ANYONE — it only lowers the server you are leaning AWAY from (cos taper, so the middle of
 *   the travel is gentle and the ends are decisive). Even = both at their own level, exactly as with no balance at all;
 *   a full lean silences the other server without disconnecting it.
 * ★ Small dead zone round the middle: a finger lifting off a slider always moves it a hair, and "nearly even" should be
 *   EVEN, not 0.999.
 */
export function balanceGains(balance: number): { s1: number; s2: number } {
  let b = clampBalance(balance);
  if (Math.abs(b) < 0.04) b = 0;
  const taper = (x: number) => Math.cos(x * (Math.PI / 2));
  return { s1: b > 0 ? taper(b) : 1, s2: b < 0 ? taper(-b) : 1 };
}

/**
 * Where each of the two servers should be. `alive` false = that server has gone (fades out where it is — it keeps
 * its place so nothing swings across the head on its way out); the one left alone goes to the CENTRE at unity, both
 * ears, as Stuart asked ("leaving one server should leave the other playing centred").
 * ★★ EARS BELONG TO SERVERS, NOT TO THE SCREEN: server 1 is left and server 2 right whichever is in front — a swap
 *    changes the picture, never the sound.
 */
export function dualTargets(o: { output: DualOutput; balance: number; s1Alive: boolean; s2Alive: boolean }):
    { s1: VoiceTarget; s2: VoiceTarget } {
  const bg = balanceGains(o.balance);
  const both = o.s1Alive && o.s2Alive;
  const stereo = o.output === 'stereo';
  const s1: VoiceTarget = !o.s1Alive ? { pos: stereo && o.s2Alive ? 0 : 0.5, gain: 0 }
                        : both ? { pos: stereo ? 0 : 0.5, gain: bg.s1 } : { pos: 0.5, gain: 1 };
  const s2: VoiceTarget = !o.s2Alive ? { pos: stereo && o.s1Alive ? 1 : 0.5, gain: 0 }
                        : both ? { pos: stereo ? 1 : 0.5, gain: bg.s2 } : { pos: 0.5, gain: 1 };
  return { s1, s2 };
}

/** The card's ear icon: which speaker is lit — 'L' / 'R', or 'mono' (one speaker that follows the audio level). */
export function earIcon(output: DualOutput, index: 0 | 1): 'L' | 'R' | 'mono' {
  if (output === 'mono') return 'mono';
  return index === 0 ? 'L' : 'R';
}

/**
 * ★★ THE CARD'S MODE IS THE MODE THE SCREEN SHOWS, NOT THE DEMODULATOR UNDER IT (Stuart, 2026-10-28: the card read
 *   "194.064 MHz WFM" — "i am in DAB not WFM mode on the Pi2"). A whole-radio mode (DAB, ADS-B, ISM, AIS, NDB) leaves the
 *   ordinary demodulator sitting idle underneath, so `status.mode` is not what is playing. The same order as the
 *   receiver's own label (SDRScreen `mode:` and the web client's): DAB, ADS-B, ISM, AIS, NDB, else the demodulator.
 *   Every flag is one the SERVER's reports set — nothing here is invented.
 */
export function cardMode(o: { mode: string; dab?: boolean; adsb?: boolean; ism?: boolean; ais?: boolean;
                              ndb?: boolean }): string {
  if (o.dab) return 'DAB';
  if (o.adsb) return 'ADS-B';
  if (o.ism) return 'ISM';
  if (o.ais) return 'AIS';
  if (o.ndb) return 'NDB';
  return String(o.mode ?? '').toUpperCase();
}

/**
 * ★ …and in DAB, WHAT is playing: the service the screen's station strip names ("Grt Hits Nrthnts"), else the
 *   multiplex — its ensemble and block ("BBC National DAB · 12B") — while the service list is still arriving. Nothing outside DAB: an RDS name is the
 *   frequency's, and the frequency is already on the card.
 */
export function cardStation(o: { dab: boolean; service?: string | null; ensemble?: string | null;
                                 channel?: string | null }): string {
  if (!o.dab) return '';
  const svc = (o.service ?? '').trim();
  if (svc) return svc;
  const ens = (o.ensemble ?? '').trim();
  const ch = (o.channel ?? '').trim();
  return ens && ch ? `${ens} · ${ch}` : ens || ch;
}

/* ─────────────────────────────── the background server's clock ─────────────────────────────── */

/**
 * ★ The SAME stages as the app's own countdown card (SDRScreen useCountdownStage): 0 normal, 1 YELLOW under five
 *   minutes, 2 RED under two — hard limits only; a soft limit is a guarantee, never a warning. One rule for both
 *   clocks, so the background server's card turns colour exactly when the front one would.
 */
export function countdownStage(leftMs: number | null, soft: boolean): 0 | 1 | 2 {
  if (leftMs == null || soft) return 0;
  return leftMs < 120_000 ? 2 : leftMs < 300_000 ? 1 : 0;
}

/**
 * ★★ "if the backgrounded servers time limit is approaching the card pops out to show you" (Stuart). It pops out when
 *   the stage RISES — at five minutes and again at two, the moments the front card changes colour — and on anything
 *   the server asks of the listener (STILL LISTENING?) or an ending. Once per rise: tucking it again is respected
 *   until the next one.
 */
export function shouldPopOut(prev: { stage: number; asking: boolean; ended: boolean },
                             now: { stage: number; asking: boolean; ended: boolean }): boolean {
  return now.stage > prev.stage || (now.asking && !prev.asking) || (now.ended && !prev.ended);
}

/** m:ss, never negative. */
export function fmtLeft(ms: number): string {
  const s = Math.max(0, Math.floor(ms / 1000));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
}

/* ─────────────────────────────── which servers can take part ─────────────────────────────── */

/** The route params the eligibility test reads (a subset of RootStackParamList['SDR']). */
export interface DualRouteLike {
  serverType?: string; isLocal?: boolean; isTcp?: boolean; localHost?: string; localPort?: number | null;
  compatOnly?: boolean; instanceName?: string;
}

/**
 * ★★★ VIBESERVER ONLY — AND THAT IS NOT A PREFERENCE, IT IS WHAT THE OTHERS' CLIENTS CAN DO (AGENTS.md: "a control
 *     that only works in one scenario should not be there"). The row is not drawn anywhere else.
 *   · VibeServer — its audio is ONE socket (/ws/audio) the server keeps feeding with the spectrum socket closed (that is
 *     how the app already plays with the screen locked), and tuning rides that same socket. An audio-only background
 *     session is exactly what the server already serves every locked phone.
 *   · UberSDR — its native audio engine and the VibeServer pump feed ONE player; a second stream would interleave
 *     with the first. Not without a second engine of its own.
 *   · KiwiSDR / Web-888 / OpenWebRX — their audio is decoded by the same JavaScript adapter that draws the waterfall;
 *     it does not outlive the screen.
 *   · Local hardware and RTL-TCP — the radio is in THIS device and is closed when its screen goes.
 *   · FM-DX — its own screen and its own native MP3 path into the same player.
 */
export function dualEligible(p: DualRouteLike | null | undefined): boolean {
  if (!p || p.compatOnly) return false;
  if (p.serverType !== 'vibeserver' || !p.isLocal || p.isTcp) return false;
  // ★ connectVibeServer always sets localHost; the on-device radio (connectLocal) never does.
  return !!p.localHost && p.localPort != null;
}

/** The note the server list shows when "Listen to a 2nd server" closed VFO B. */
export const VFO_B_CLOSED_NOTE = 'VFO B closed — two servers use both ears.';

/** "Listen to a 2nd server", said where it is chosen (also its spoken hint) — and, with VFO B on, that B will close. */
export function secondServerHint(abOn: boolean): string {
  const base = 'This server keeps playing while you pick another — or another radio on this one: this one in your left '
             + 'ear, the next in your right (Audio menu, TWO SERVERS, for both in both ears).';
  return abOn ? `VFO B will close — two servers use both ears. ${base}` : base;
}

/** ★★★ Starting a pairing: VFO B on ⇒ close it FIRST (the ordinary B-off, sent on the person's own tap of "Listen to a
 *  2nd server" — THE SHARED-DIAL CONTRACT: a user action) and say so. Two channels at a time, never three. */
export function pairingStart(abOn: boolean): { closeB: boolean; note: string | null } {
  return abOn ? { closeB: true, note: VFO_B_CLOSED_NOTE } : { closeB: false, note: null };
}

/** Two listings are the SAME server when host and port agree (the picker can list one server twice). */
export function sameServer(a: DualRouteLike & { baseUrl?: string }, b: DualRouteLike & { baseUrl?: string }): boolean {
  if (a.localHost && b.localHost) return a.localHost === b.localHost && a.localPort === b.localPort;
  return !!a.baseUrl && a.baseUrl === b.baseUrl;
}

/* ───────────────────────── two RADIOS on one server (Gesiima, #57) ───────────────────────── */

/**
 * ★★★ PAIRING IS PER RADIO, NOT PER SERVER. Gesiima runs ONE VibeServer with TWO RTL dongles and asked whether "Listen to
 *     a 2nd server" works for two radios on it; Stuart: "Yup they are treated as separate instances" — "that is how I
 *     expected it to be". A radio behind a front door is addressed as `<base>/r/<id>`; this is the `/r/<id>` part, or ''
 *     for an address that IS the radio (a single-radio server, or a per-radio port). Two sides are the SAME RADIO only
 *     when the server AND this key agree — so the second radio on the same server is a second side like any other.
 */
export function radioKeyOf(base: string | null | undefined): string {
  if (!base) return '';
  const i = base.lastIndexOf('/r/');
  return i >= 0 ? base.slice(i).replace(/\/+$/, '') : '';
}

/** One side as far as "is it the same radio?" is concerned: its route (host and port) and its radio key. */
export interface RadioRef { params: DualRouteLike & { baseUrl?: string }; radioKey: string }

/** The same radio: the same server and the same radio behind it. */
export function sameRadio(a: RadioRef, b: RadioRef): boolean {
  return sameServer(a.params, b.params) && a.radioKey === b.radioKey;
}

/**
 * The server list, picking the server that is ALREADY playing (the picking phase's one side):
 *  · 'back'   — its address is the radio itself (no `/r/<id>`): there is nothing else on it to choose, so go back to it
 *               (a second session on the same radio would take a second slot for the same person);
 *  · 'radios' — it is one radio behind a front door: open the server, whose radio list is where a DIFFERENT radio is
 *               chosen. Choosing the one already playing there goes back to it (isFirstRadio).
 *  · 'other'  — a different server altogether.
 */
export function pickSameServer(solo: RadioRef | null, picked: DualRouteLike & { baseUrl?: string }): 'back' | 'radios' | 'other' {
  if (!solo || !sameServer(solo.params, picked)) return 'other';
  return solo.radioKey ? 'radios' : 'back';
}

/** The joining side chose the radio the FIRST side is already on: it is not a second side after all. Back to picking with
 *  the first side alone, nothing of the joiner's left (its audio never started — the radio list holds the connect). */
export function detachSecond<S extends DualSideCore>(st: DualCore<S>, id: string): DualCore<S> {
  if (st.phase !== 'paired' || st.sides.length !== 2 || st.sides[1].id !== id) return st;
  return { phase: 'picking', sides: [{ ...st.sides[0], transport: 'native' }], fg: null };
}

/** The card's name for a side: "server · radio" when the server has more than one radio to tell apart. */
export function sideName(server: string, radio: string | null | undefined): string {
  const r = (radio ?? '').trim();
  return r && r !== server ? `${server} · ${r}` : server;
}

/* ─────────────────────────────── the pairing's state machine ─────────────────────────────── */

export type DualPhase = 'off' | 'picking' | 'paired';
export type Transport = 'native' | 'aux';

/** One server in the pairing. `id` IS its session id — the server's own name for this listener. */
export interface DualSideCore {
  id: string;
  transport: Transport;
  /** null = playing; a sentence = the server ended it (shown on the card a moment, then the side goes). */
  ended: string | null;
}

export interface DualCore<S extends DualSideCore = DualSideCore> {
  phase: DualPhase;
  /** Index 0 = server 1 (LEFT), 1 = server 2 (RIGHT). picking: exactly one. */
  sides: S[];
  /** The side whose screen is in front; null while picking (the server list is in front). */
  fg: string | null;
}

export const DUAL_OFF: DualCore = { phase: 'off', sides: [], fg: null };

/** The side in the background — what the card shows. */
export function bgSide<S extends DualSideCore>(st: DualCore<S>): S | null {
  if (st.phase !== 'paired') return null;
  return st.sides.find((s) => s.id !== st.fg) ?? null;
}

/**
 * ★★★ MAY A SCREEN THAT IS GOING AWAY LEAVE ITS AUDIO PLAYING? Only when the pairing still wants it AND it is not the
 *     one in front — a swap or "Listen to a 2nd server" moves `fg` BEFORE navigating, so the screen that leaves is
 *     already in the background when its cleanup asks. A screen that leaves while still in front (Back, a crash reset,
 *     an alert's "Back to Servers") is LEAVING, and its audio stops as it always has.
 */
export function holds(st: DualCore, id: string): boolean {
  if (st.phase === 'off') return false;
  const s = st.sides.find((x) => x.id === id);
  return !!s && !s.ended && st.fg !== id;
}

/** The transport a screen on side `id` must use. Outside a pairing: the ordinary native pump, always. */
export function transportFor(st: DualCore, id: string | null): Transport {
  if (!id || st.phase === 'off') return 'native';
  return st.sides.find((x) => x.id === id)?.transport ?? 'native';
}

/** Begin: the server in front goes to the background and keeps playing; the server list comes up. */
export function beginPicking<S extends DualSideCore>(st: DualCore<S>, side: S): DualCore<S> {
  if (st.phase !== 'off') return st;
  return { phase: 'picking', sides: [{ ...side, transport: 'native', ended: null }], fg: null };
}

/** A second server's screen arrives while picking: it becomes server 2 (RIGHT), on the second voice, in front. */
export function attachSecond<S extends DualSideCore>(st: DualCore<S>, side: S): DualCore<S> {
  if (st.phase !== 'picking' || st.sides.length !== 1) return st;
  return { phase: 'paired', sides: [st.sides[0], { ...side, transport: 'aux', ended: null }], fg: side.id };
}

/** TAP the card: the background server comes to the front. Ears and transports do not move. */
export function swap<S extends DualSideCore>(st: DualCore<S>): DualCore<S> {
  const bg = bgSide(st);
  if (!bg || bg.ended) return st;
  return { ...st, fg: bg.id };
}

/** What `endSide` asks of the caller, beyond the new state. */
export interface EndEffects {
  /** Stop this side's transport. */
  stop: { id: string; transport: Transport } | null;
  /** The lone survivor is on the second voice with no screen in front of it: move it to the main pump now. */
  moveToNative: string | null;
}

/**
 * A side ends — the listener stopped it, left its screen, or the server ended it. The other plays on, CENTRED.
 *  · survivor in front (its screen is up) → pairing OFF; that screen's player moves itself to the main pump if it was
 *    on the second voice (transportFor now says 'native').
 *  · survivor in the background (the server list is up, or the ended side's own screen with its refusal) → back to
 *    PICKING with the survivor as the one playing, so a new second server can be chosen; moved to the main pump first
 *    if it was on the second voice, because only the main pump has the lock-screen card and the Android service.
 */
export function endSide<S extends DualSideCore>(st: DualCore<S>, id: string): { st: DualCore<S>; fx: EndEffects } {
  const gone = st.sides.find((s) => s.id === id);
  if (st.phase === 'off' || !gone) return { st, fx: { stop: null, moveToNative: null } };
  const stop = { id, transport: gone.transport };
  const rest = st.sides.filter((s) => s.id !== id && !s.ended);
  if (!rest.length) return { st: DUAL_OFF as DualCore<S>, fx: { stop, moveToNative: null } };
  const sur = rest[0];
  if (st.phase === 'paired' && st.fg === sur.id) {
    return { st: DUAL_OFF as DualCore<S>, fx: { stop, moveToNative: null } };
  }
  const moved = sur.transport === 'aux';
  return { st: { phase: 'picking', sides: [{ ...sur, transport: 'native' }], fg: null },
           fx: { stop, moveToNative: moved ? sur.id : null } };
}

/** Mark a side ENDED by the server (the card says why for a moment before endSide removes it). */
export function markEnded<S extends DualSideCore>(st: DualCore<S>, id: string, why: string): DualCore<S> {
  if (st.phase === 'off') return st;
  return { ...st, sides: st.sides.map((s) => (s.id === id ? { ...s, ended: why } : s)) };
}

/**
 * ★ WHY THE SERVER ENDED A BACKGROUND SERVER, in the card's words. The types are the shim's terminal set on the audio
 *   socket (VibePowerModule laTerminalTypes / VibeStreamService LA_TERMINAL_TYPES) — one list, three readers.
 */
export const TERMINAL_WHY: Record<string, string> = {
  session_expired: 'its time limit was reached',
  cooldown: 'its time limit was reached',
  idle_closed: 'nobody answered "still listening?"',
  kicked: 'the owner ended the session',
  banned: 'the owner ended the session',
  evicted: 'somebody else took the radio',
  busy: 'the radio is busy',
  elsewhere: 'the radio is in use elsewhere',
  needs_codec: 'this server needs a newer app',
};

/** null = not an ending. */
export function terminalWhy(type: string): string | null {
  return Object.prototype.hasOwnProperty.call(TERMINAL_WHY, type) ? TERMINAL_WHY[type] : null;
}
