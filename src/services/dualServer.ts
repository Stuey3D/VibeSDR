/**
 * ★★ V12 TWO SERVERS AT ONCE — the pairing, its two audio voices and the background server's socket (GitHub discussion
 * #57; Stuart's design, 2026-10-27). The rules are pure, in dualMix.ts (tested by scripts/test_dual_server.ts); the
 * card is DualServerCard.tsx.
 *
 * ★★★ HOW TWO SERVERS PLAY AT ONCE WITHOUT A SECOND COPY OF THE AUDIO ENGINE
 *   · Server 1 — the one you were on when you chose "Listen to a 2nd server" — stays on the ordinary NATIVE pump
 *     (VibePowerModule.startLocalAudio: its own socket, the lock-screen card, the Android foreground service). Its
 *     socket is never closed or reopened by the pairing: leaving its screen simply does not stop it. "the difference
 *     is the audio keeps playing from the connected server" (Stuart).
 *   · Server 2 rides a SECOND VOICE: a socket read here, in JavaScript (the same reader LocalAudioPlayer kept for a
 *     platform without the native pump), handing each Opus packet to VibeDualAudio — its own player on iOS/Mac (a
 *     second AVAudioEngine), its own AudioTrack on Android — so the two never share a decoder or a queue.
 *   · Each server KEEPS its voice for the life of the pairing. A swap only changes which screen is in front: no socket
 *     moves, so there is no gap and the server never sees a reconnect. Only when one server is left on the second
 *     voice is it moved to the main pump (a short gap, once), because only the main pump has the lock-screen card and,
 *     on Android, the service that keeps the app alive.
 * ★★ THE BACKGROUND SERVER IS AUDIO ONLY. Its screen — and with it the spectrum socket, the waterfall, the decoders —
 *    is gone; its audio socket is exactly what the server already serves a phone with the screen locked (the spectrum
 *    is paused there too). Nothing here keeps a session alive that the server would otherwise end: no pings, no
 *    keep-alives. A "STILL LISTENING?" from the server pops the card, and only a TAP answers it.
 * ★★★ EVERY SERVER TYPE (Stuart, 2026-10-30: "I thought we were looking to be able to do it on all servers"). Each kind
 *     keeps its own audio path and only learns which voice it rides (dualMix.audioKindOf):
 *   · VibeServer — the native pump, or the second voice's socket read here (auxOpen, proto 'vibe').
 *   · UberSDR — its native engine (startAudioEngine), or the second voice's socket read here (proto 'uber': the
 *     engine's own 21-byte header + Opus). Its screen's tunes go to whichever socket is playing (uberAuxSend).
 *   · KiwiSDR / Web-888 / OpenWebRX — the screen's own ADAPTER, which decodes in JS: kept alive while its screen is away
 *     (parkBackend) and ADOPTED by the screen coming back (takeParked) — one session, never two. Its PCM goes to the
 *     main player or the second voice (extAudioOut.ts); a move is a change of route, no reconnect.
 *   · FM-DX — MP3 decoded natively into the main player, and only there: it always takes the main player, and the
 *     other side goes to the second voice (dualMix.attachSecond / moveToAux).
 * ★★★ THE SHARED-DIAL CONTRACT HOLDS: the background server is never sent a tune. Its socket says nothing at all
 *     unless its own screen is in front and a person tunes there (LocalAudioPlayer's own rules), or the listener taps
 *     "I'm still here".
 */
import { useSyncExternalStore } from 'react';
import { NativeEventEmitter, NativeModules, Platform } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { bytesToBase64 } from './base64';
import { decodeVibeAdpcmFrame } from './imaAdpcm';
import { noteFault } from './faultLog';
import { crumb } from './crumbs';
import {
  DUAL_OFF, attachSecond, beginPicking, bgSide, clampBalance, dualEligible, dualTargets, endSide, holds,
  markEnded, sameServer, sameRadio, swap, terminalWhy, transportFor, detachSecond, pickSameServer,
  sideHasAudio, withSideMuted, DUAL_STALL_MS, audioKindOf, firstMovesToAux, normBase, uberAudioUrl, vibeAuxUrl,
  endedWhy, type UberLink,
  type AudioKind, type DualCore, type DualOutput, type DualRouteLike, type DualSideCore, type Transport,
} from './dualMix';
export { uberAudioUrl, vibeAuxUrl, endedWhy, type UberLink };
import type { BackendCallbacks, SDRBackend } from './SDRBackend';

/** The native second voice (ios/VibeSDR/VibeDualAudio.swift, android …/VibeDualAudio.kt). Absent = an older binary
 *  or a platform without it (tvOS, web): the feature is not offered — no dead row. */
type DualNative = {
  setNativeMix?: (pos: number, gain: number, active: boolean) => void;
  setAuxMix?: (pos: number, gain: number) => void;
  /** ★ Each voice's own stereo width, 0 (folded) … 1 (as sent) — dualMix STEREO WHEN ALONE. Absent in an older binary
   *  (every voice folded, as before). */
  setNativeWidth?: (w: number) => void;
  setAuxWidth?: (w: number) => void;
  startAux?: () => void;
  stopAux?: () => void;
  pushAuxOpus?: (b64: string, rate: number, channels: number) => void;
  pushAuxPcm?: (b64: string, rate: number, channels: number) => void;
  /** ★ `nativeAge` / `auxAge`: ms since that voice last had a buffer (−1 = never) — the "has its stream stopped?" test. */
  getLevels?: () => Promise<{ native?: number; aux?: number; nativeAge?: number; auxAge?: number }>;
};
const Dual = (NativeModules as { VibeDualAudio?: DualNative }).VibeDualAudio;
const Vibe = NativeModules.VibePowerModule as {
  startLocalAudio?: (host: string, port: number, initialTune: string, authSuffix: string, wsBase: string) => void;
  stopLocalAudio?: () => void;
  setInstanceName?: (name: string) => void;
  // ★ The other kinds' main-player paths (dualMix.audioKindOf): UberSDR's engine, external PCM, FM-DX's MP3.
  startAudioEngine?: (baseUrl: string, frequency: number, mode: string, uuid: string, password: string) => void;
  stopAudioEngine?: () => void;
  setAdminAuth?: (q: string) => void;
  sendAudioCommand?: (json: string) => void;
  stopExternalAudio?: () => void;
  stopFmdxAudio?: () => void;
} | undefined;

/** Can this build play two servers at all? */
export const dualSupported: boolean = Platform.OS !== 'web' && !!Dual?.pushAuxOpus && !!Dual?.setNativeMix;

/** Where a side's audio socket is — exactly the arguments LocalAudioPlayer gave the native pump. */
export interface DualLink { host: string; port: number; suffix: string; wsBase: string }

/** ★ The route a side's screen lives on: FM-DX has its own (TunerScreen); every other kind is the receiver screen. */
export function dualScreenOf(params: Record<string, unknown> | null | undefined): 'SDR' | 'Tuner' {
  return params?.serverType === 'fmdx' ? 'Tuner' : 'SDR';
}

export interface DualSide extends DualSideCore {
  name: string;
  /** The SDR route params that bring its screen back (one-shot ones stripped). */
  params: Record<string, unknown>;
  /** What it is playing, in DISPLAY Hz, as its screen last showed it. */
  freqHz: number;
  /** The mode its screen SHOWS (dualMix.cardMode — DAB, ADS-B… over the idle demodulator), and in DAB the service. */
  mode: string;
  station?: string;
  /** Its session limit as an absolute time (ms), and whether that limit is a guarantee rather than an end. */
  endsAt: number | null;
  soft: boolean;
  /** The server asked "still listening?" — the deadline it gave (ms epoch). */
  asking: number | null;
  /** ★★ WHICH RADIO on its server — `/r/<id>`, or '' when its address is the radio (dualMix.radioKeyOf). Pairing is per
   *  RADIO: two radios on one VibeServer are two sides (Gesiima, #57). Set by its screen once the radio is chosen. */
  radioKey?: string;
  /** ★ Its radio was unlocked with a PIN of its own at the door: coming back, the door asks again (a radio PIN is proved
   *  with a fresh nonce, which a return address cannot carry). Every other side comes straight back to its radio. */
  pinned?: boolean;
  /** ★★ Nothing to give (dualMix.sideHasAudio), as its screen last said: `quiet` = a spectrum-only whole-radio mode
   *  (ADS-B, AIS, ISM, NDB); `band` = DIGITAL SPOTS: BAND, which mutes the dial's audio. */
  quiet?: boolean;
  band?: boolean;
}

export interface DualState extends DualCore<DualSide> {
  output: DualOutput;
  /** −1 = all server 1 … 0 even … +1 = all server 2. */
  balance: number;
  /** ★ What starting the pairing did that the listener should hear about — "VFO B closed" (dualMix.pairingStart).
   *  Shown in the server list's banner while picking; gone with the pairing. */
  note?: string | null;
}

let state: DualState = { ...(DUAL_OFF as DualCore<DualSide>), output: 'stereo', balance: 0 };
const subs = new Set<() => void>();

export function getDual(): DualState { return state; }
/** `quiet` = called during a RENDER (roleOnMount): the state and the native mix change now, the components hear of it
 *  a tick later — a store that notifies other components mid-render is a React error, not a feature. */
function set(next: DualState, quiet = false) {
  if (next === state) return;
  const prev = state;
  state = next;
  // ★ A breadcrumb on every change of the pairing's SHAPE — phase, who is in front, each side's session, address and
  //   voice — so Export logs shows how a pairing got where it did (Stuart, 2026-10-30: after dropping the second server
  //   the audio surged, and leaving the server kept it playing).
  if (prev.phase !== next.phase || prev.fg !== next.fg || prev.sides.length !== next.sides.length
      || prev.sides.some((s, i) => s.id !== next.sides[i]?.id || s.transport !== next.sides[i]?.transport)) {
    const where = (sd: DualSide) => {
      const p = sd.params as { localHost?: string; localPort?: number; baseUrl?: string };
      return `${sd.id.slice(0, 8)}@${p.localHost ? `${p.localHost}:${p.localPort ?? ''}` : (p.baseUrl ?? '?')}${sd.radioKey ?? ''}`;
    };
    crumb(`dual ${prev.phase}→${next.phase} fg ${next.fg?.slice(0, 8) ?? '-'} `
      + next.sides.map((sd, i) => `s${i + 1} ${where(sd)} ${sd.transport}${sd.ended ? ' ended' : ''}`).join(' · '));
  }
  onChanged(prev);
  const tell = () => subs.forEach((f) => { try { f(); } catch { /* a listener must not stop the rest */ } });
  if (quiet) setTimeout(tell, 0); else tell();
}
export function subscribeDual(f: () => void): () => void { subs.add(f); return () => { subs.delete(f); }; }
/** The whole pairing, for the card (small): it changes on the front server's frequency too. */
export function useDual(): DualState { return useSyncExternalStore(subscribeDual, getDual, getDual); }

/** The pairing's SHAPE only — phase, who is where, which voice, the output — for the big screens. ★ SDRScreen must not
 *  re-render because the front server's own frequency was copied into the store (noteStatus); this snapshot is the
 *  same object until the shape changes. */
export interface DualShape {
  phase: DualState['phase']; fg: string | null; output: DualOutput;
  sides: { id: string; transport: Transport; ended: boolean; name: string; muted: boolean }[];
}
let shapeKey = '';
let shape: DualShape = { phase: 'off', fg: null, output: 'stereo', sides: [] };
function getShape(): DualShape {
  const k = `${state.phase}|${state.fg}|${state.output}|`
          + state.sides.map((s) => `${s.id}:${s.transport}:${s.ended ? 1 : 0}:${s.muted ? 1 : 0}:${s.name}`).join(',');
  if (k !== shapeKey) {
    shapeKey = k;
    shape = { phase: state.phase, fg: state.fg, output: state.output,
              sides: state.sides.map((s) => ({ id: s.id, transport: s.transport, ended: !!s.ended, name: s.name,
                                                       muted: !!s.muted })) };
  }
  return shape;
}
export function useDualShape(): DualShape { return useSyncExternalStore(subscribeDual, getShape, getShape); }

/* ─────────────────────────────── settings (per device) ─────────────────────────────── */

const K_OUTPUT = 'vibe.dual.output';
const K_BALANCE = 'vibe.dual.balance';
let loaded = false;
function loadSettings() {
  if (loaded) return;
  loaded = true;
  AsyncStorage.multiGet([K_OUTPUT, K_BALANCE]).then((kv) => {
    const out = kv[0]?.[1] === 'mono' ? 'mono' : 'stereo';
    const bal = clampBalance(Number(kv[1]?.[1] ?? 0));
    if (out !== state.output || bal !== state.balance) set({ ...state, output: out, balance: bal });
  }).catch(() => {});
}
loadSettings();

/** ★ The Audio menu's TWO SERVERS row. Remembered per device: it is about this listener's ears and headphones. */
export function setDualOutput(o: DualOutput) {
  set({ ...state, output: o });
  AsyncStorage.setItem(K_OUTPUT, o).catch(() => {});
}
/** ★ HOLD the card. ONE balance for every pair, not per pair: it says which ear the listener favours ("my left
 *  ear is better"), and a per-pair memory would make the same gesture mean different things on different nights. */
export function setDualBalance(b: number, persist = true) {
  set({ ...state, balance: clampBalance(b) });
  if (persist) AsyncStorage.setItem(K_BALANCE, String(clampBalance(b))).catch(() => {});
}

/* ─────────────────────────────── the main pump's owner ─────────────────────────────── */

/**
 * ★★★ THE NATIVE PUMP IS A SINGLETON, SO "STOP" MUST SAY WHOSE. With two servers a screen can go away AFTER another
 *     has started the pump for its own server (a swap, or the survivor moved across), and an unconditional stop from
 *     the stale screen would silence the server that is now playing — the localGen lesson (localSession.ts) again.
 *  ★ Claimed with the SESSION ID on every start. Outside a pairing the claimant is always the screen in front, so this
 *    is the old behaviour exactly; it only ever refuses a stop from a screen whose server is no longer the one playing.
 */
let nativeOwner: string | null = null;
export function claimNative(id: string) { nativeOwner = id; }
/** May `id` stop the pump? Clears the claim when it may. */
export function releaseNative(id: string): boolean {
  if (nativeOwner !== null && nativeOwner !== id) return false;
  nativeOwner = null;
  return true;
}
export function nativeOwnedBy(id: string | null): boolean { return !!id && nativeOwner === id; }

/* ─────────────────────────────── the second voice's socket ─────────────────────────────── */

/** ★ `proto` — whose framing the socket speaks: 'vibe' (a VibeServer's /ws/audio: formats 0–4) or 'uber' (UberSDR's
 *  /ws?…&format=opus: a 21-byte header — rate at 8, channels at 12 — then one Opus packet, the native engine's own
 *  handlePacket). Absent = 'vibe'. */
type AuxProto = 'vibe' | 'uber';
type AuxOpen = { id: string; url: string; hello: string | null; proto?: AuxProto };
let aux: {
  id: string; url: string; ws: WebSocket | null; wanted: boolean; terminal: boolean; retries: number;
  lastFrameAt: number; openedAt: number; started: boolean; proto: AuxProto;
  retryTimer: ReturnType<typeof setTimeout> | null; dog: ReturnType<typeof setInterval> | null;
} | null = null;
let auxOnBytes: ((n: number) => void) | null = null;

/** The second voice is open for this side at this URL — a screen coming back to it ADOPTS it, no reopen. */
export function auxIsOpenFor(id: string | null, url?: string): boolean {
  return !!id && !!aux && aux.wanted && aux.id === id && (url === undefined || aux.url === url);
}
/** The screen in front reads the link meter from here while its server is on the second voice. */
export function setAuxOnBytes(f: ((n: number) => void) | null) { auxOnBytes = f; }

/** Open (or adopt) the second voice for side `o.id`. `force` = a reconnect asked for a NEW socket. */
export function auxOpen(o: AuxOpen, force = false) {
  if (!force && auxIsOpenFor(o.id, o.url)) return;
  auxClose();
  aux = { id: o.id, url: o.url, ws: null, wanted: true, terminal: false, retries: 0, lastFrameAt: 0,
          openedAt: Date.now(), started: false, proto: o.proto ?? 'vibe', retryTimer: null, dog: null };
  auxConnect(o.hello);
  // ★ A half-open socket never says so: no frame for 8 s on an open socket = dead, reopen (as the native pump's
  //   revive does). Checked every 3 s, and only while the voice exists.
  aux.dog = setInterval(() => {
    const a = aux;
    if (!a || !a.wanted || a.terminal) return;
    const now = Date.now();
    if (a.ws && a.ws.readyState === WebSocket.OPEN && a.lastFrameAt && now - a.lastFrameAt > 8000) {
      try { a.ws.close(); } catch { /* reconnect below */ }
    }
    // ★ Ninety seconds without a single frame, through every retry, is a server that has gone — say so and let the
    //   pairing end that side, rather than leave a card for a server that is not coming back. Only in the BACKGROUND:
    //   in front, its own screen owns the connection and says what is wrong; this keeps retrying under it.
    const since = a.lastFrameAt || a.openedAt;
    if (state.fg !== a.id && now - since > 90_000) {
      const id = a.id;
      auxClose();
      serverEnded(id, 'the connection was lost');
    }
  }, 3000);
}

function auxConnect(hello: string | null) {
  const a = aux;
  if (!a || !a.wanted) return;
  let ws: WebSocket;
  try { ws = new WebSocket(a.url); } catch (e) { noteFault('dual-aux', 'open', e as Error, a.url.slice(0, 40)); auxRetry(); return; }
  ws.binaryType = 'arraybuffer';
  a.ws = ws;
  ws.onopen = () => {
    if (aux !== a || a.ws !== ws) return;
    // ★ The hello is the screen's own (assertTune rules, LocalAudioPlayer) — and only ever on the FIRST open. A
    //   reopen says nothing: the server kept the session's dial (THE SHARED-DIAL CONTRACT — recovery is not a user
    //   action).
    if (hello) { try { ws.send(hello); } catch { /* the socket died between open and send */ } }
    hello = null;
  };
  ws.onmessage = (e) => {
    if (aux !== a || a.ws !== ws) return;
    if (typeof e.data === 'string') { onAuxText(a.id, e.data, a); return; }
    if (!(e.data instanceof ArrayBuffer)) return;
    try { onAuxFrame(e.data as ArrayBuffer, a); }
    catch (err) { noteFault('dual-aux', 'frame', err as Error, `len=${(e.data as ArrayBuffer).byteLength}`); }
  };
  ws.onerror = () => { /* onclose follows */ };
  ws.onclose = () => {
    if (aux !== a || a.ws !== ws) return;
    a.ws = null;
    if (a.terminal || !a.wanted) return;
    auxRetry();
  };
}

function auxRetry() {
  const a = aux;
  if (!a || !a.wanted || a.terminal) return;
  if (a.retryTimer) return;
  const delay = Math.min(15_000, 2000 * 2 ** Math.min(a.retries, 3));   // 2, 4, 8, 15, 15… (the native pump's)
  a.retries++;
  a.retryTimer = setTimeout(() => {
    if (aux !== a) return;
    a.retryTimer = null;
    auxConnect(null);
  }, delay);
}

/** ★ A reopen must reach the dial the listener is on now: UberSDR tunes its own channel from the URL (its native engine
 *  puts `frequency` and `mode` on every reopen — VibePowerModule.audioWsURL), so the URL follows each tune. Nothing is
 *  sent: this only changes where the NEXT open goes. */
export function auxSetUrl(id: string, url: string) {
  if (aux && aux.id === id) aux.url = url;
}

/** UberSDR's audio frame (VibePowerModule.handlePacket, verbatim): 21-byte header, then Opus. */
const UBER_HEADER = 21;
function onUberFrame(buf: ArrayBuffer, a: NonNullable<typeof aux>) {
  if (buf.byteLength <= UBER_HEADER + 2) return;
  const dv = new DataView(buf);
  const sr = dv.getUint32(8, true), ch = dv.getUint8(12);
  if (sr < 8000 || sr > 96000 || (ch !== 1 && ch !== 2)) return;
  a.lastFrameAt = Date.now();
  a.retries = 0;
  if (!a.started) { Dual?.startAux?.(); a.started = true; }
  Dual?.pushAuxOpus?.(bytesToBase64(new Uint8Array(buf, UBER_HEADER)), sr, ch);
}

function onAuxFrame(buf: ArrayBuffer, a: NonNullable<typeof aux>) {
  auxOnBytes?.(buf.byteLength);
  if (a.proto === 'uber') { onUberFrame(buf, a); return; }
  if (buf.byteLength <= 6) return;
  a.lastFrameAt = Date.now();
  a.retries = 0;
  const dv = new DataView(buf);
  const format = dv.getUint8(1);
  if (!a.started) { Dual?.startAux?.(); a.started = true; }
  if (format === 3) {
    const rate = dv.getUint32(2, true);
    const ch = dv.getUint8(0) === 2 ? 2 : 1;
    Dual?.pushAuxOpus?.(bytesToBase64(new Uint8Array(buf, 6)), rate, ch);
    return;
  }
  // ★ Format 4 is DAB+ AAC the server could not decode — never ours to play (LocalAudioPlayer's note).
  if (format === 4) return;
  let rate: number, ch: number, pcm: Uint8Array;
  if (format === 1 || format === 2) {
    const d = decodeVibeAdpcmFrame(buf);
    rate = d.rate; ch = d.channels; pcm = new Uint8Array(d.pcm.buffer, d.pcm.byteOffset, d.pcm.byteLength);
  } else if (format === 0) {
    rate = dv.getUint32(2, true); ch = dv.getUint8(0) === 2 ? 2 : 1; pcm = new Uint8Array(buf, 6);
  } else return;   // a format this build does not know: drop it (ELSE MEANS DONGLE)
  Dual?.pushAuxPcm?.(bytesToBase64(pcm), rate, ch === 2 ? 2 : 1);
}

/** ★ The screen in front reads its own audio socket's text from here while it is on the second voice (UberSDR's
 *  dsp_filters / dsp_status — what VibeWsText carries from the native engine). */
let auxTextSink: { id: string; f: (text: string) => void } | null = null;
export function setAuxTextSink(id: string | null, f: ((text: string) => void) | null) {
  auxTextSink = id && f ? { id, f } : (auxTextSink && (!id || auxTextSink.id === id) ? null : auxTextSink);
}

function onAuxText(id: string, text: string, a: NonNullable<typeof aux>) {
  if (auxTextSink && auxTextSink.id === id) { try { auxTextSink.f(text); } catch { /* the screen's problem */ } }
  let msg: { type?: string; secs?: number } | null = null;
  try { msg = JSON.parse(text); } catch { return; }
  const type = String(msg?.type ?? '');
  const why = terminalWhy(type);
  if (why) { a.terminal = true; serverEnded(id, why); return; }
  serverSaid(id, type, Number(msg?.secs) || 0);
}

/** Send on the second voice (a tune from its screen in front). */
export function auxSend(id: string | null, text: string): boolean {
  const a = aux;
  if (!a || !id || a.id !== id || !a.ws || a.ws.readyState !== WebSocket.OPEN) return false;
  try { a.ws.send(text); return true; } catch { return false; }
}

/** Close the second voice (any side, or only if it is `id`'s). */
export function auxClose(id?: string) {
  const a = aux;
  if (!a || (id && a.id !== id)) return;
  a.wanted = false;
  if (a.retryTimer) clearTimeout(a.retryTimer);
  if (a.dog) clearInterval(a.dog);
  try { a.ws?.close(); } catch { /* already closed */ }
  aux = null;
  if (a.started) Dual?.stopAux?.();
}

/* ──────────────────── KIWI / WEB-888 / OPENWEBRX: the adapter outlives its screen ──────────────────── */

/**
 * ★★★ THE ADAPTER IS THE SESSION. Kiwi and OpenWebRX decode their audio in the same JavaScript adapter that draws the
 *     waterfall, so a background Kiwi is simply its adapter KEPT: the screen leaving hands it here instead of destroying
 *     it (parkBackend), its waterfall is paused (Kiwi `wf_speed=0`; OpenWebRX skips the FFT), its PCM keeps going to its
 *     voice, and the screen coming back ADOPTS it (takeParked) — the same sockets, the same session, no second slot.
 *     Kiwi counts listeners per connection; a reopen for the same person would be a second one.
 *  ★ Every adapter a side has — in front or parked — is noted here by its side id, so the pairing can move its audio
 *    between voices (moveToNative / moveToAux) whichever screen is up.
 */
const backends = new Map<string, SDRBackend>();
const parked = new Set<string>();

/** A screen created (or adopted) side `id`'s adapter: its audio rides the voice the pairing gives that side. */
export function noteBackend(id: string | null, b: SDRBackend | null) {
  if (!id || !b) return;
  backends.set(id, b);
  b.audioOut?.setRoute(transportFor(state, id));
}

/** The screen of side `id` is going and the pairing keeps it playing (dualHolds): keep its adapter. Returns true when
 *  it was kept — the caller must then NOT destroy it. Only an adapter that can be adopted (rebindCallbacks) is kept. */
export function parkBackend(id: string | null, b: SDRBackend | null): boolean {
  if (!id || !b || !b.rebindCallbacks || !holds(state, id)) return false;
  backends.set(id, b);
  parked.add(id);
  try { b.pauseSpectrum(); } catch { /* the waterfall is not ours to save */ }
  if (b.audioOut) b.audioOut.background = true;
  // ★ From here on it reports to the pairing: an ending or a lost connection is the card's to say, and NOTHING here
  //   reconnects it — a receiver that ended the session (a time limit, a kick, inactivity) stays ended.
  b.rebindCallbacks(backgroundCallbacks(id));
  crumb(`dual park ${id.slice(0, 8)} (${b.kind})`);
  return true;
}

/** The screen coming back to side `id` takes its running adapter (and must rebind it), or null to build its own. */
export function takeParked(id: string | null): SDRBackend | null {
  if (!id || !parked.has(id)) return null;
  parked.delete(id);
  const b = backends.get(id) ?? null;
  if (b) {
    if (b.audioOut) b.audioOut.background = false;
    try { b.resumeSpectrum(); } catch { /* it will resume on its own next frame */ }
  }
  return b;
}

/** Side `id` is over: a parked adapter is destroyed (its own stop closes whichever voice it was on). */
function dropBackend(id: string) {
  const b = backends.get(id);
  backends.delete(id);
  if (b && parked.delete(id)) { try { b.destroy(); } catch { /* already gone */ } }
}

/** What a background adapter's server says, in the card's words — never reconnecting, never answering for anybody. */
function backgroundCallbacks(id: string): BackendCallbacks {
  const lost = () => serverEnded(id, 'the connection was lost');
  const fmdx = kindOf(id) === 'fmdx';
  return {
    onSpectrum: () => {}, onStatus: () => {}, onConnect: () => {},
    // ★ FM-DX: its /text socket drops and comes back on its own (FmdxAdapter's silent reconnect) while the MP3 audio
    //   plays on natively — a blip is not an ending. Kiwi / OpenWebRX never reconnect themselves: a drop IS the end.
    onDisconnect: fmdx ? () => {} : lost,
    // ★ FM-DX is ONE shared tuner: somebody else may retune it while it is in the background — the card follows.
    onFmdxState: (st: { freqHz?: number; ps?: string }) => {
      const s = state.sides.find((x) => x.id === id);
      const f = Number(st?.freqHz) || 0;
      const ps = String(st?.ps ?? '').trim();
      if (s && f > 0 && (s.freqHz !== f || (s.station ?? '') !== ps)) patchSide(id, { freqHz: f, station: ps });
    },
    onServerLost: lost,
    onServerBusy: () => serverEnded(id, 'the radio is busy'),
    onError: (m: string) => serverEnded(id, endedWhy(m)),
  } as unknown as BackendCallbacks;
}

/* ─────────────────────────────── UBERSDR: its audio socket on either voice ─────────────────────────────── */

const uberLinks = new Map<string, UberLink>();
export function noteUberLink(id: string, l: UberLink) {
  const prev = uberLinks.get(id);
  uberLinks.set(id, { ...l, bwLow: l.bwLow ?? prev?.bwLow, bwHigh: l.bwHigh ?? prev?.bwHigh });
  if (uberLinks.size > 8) uberLinks.delete(uberLinks.keys().next().value as string);
}

let uberClient = 'VibeSDR';
/** The User-Agent the app names itself with (constants/version USER_AGENT), handed in by the player. */
export function setUberClientName(c: string) { if (c) uberClient = c; }

/**
 * ★★★ AN UBERSDR SCREEN'S AUDIO COMMANDS GO TO THE SOCKET THAT IS PLAYING IT. On the main player that is the native
 *     engine (UberSDRWsClient._routeTune, untouched); on the second voice it is the socket read here. Returns true when it
 *     went to the second voice (the caller then sends nothing natively). A tune or a filter also moves the URL a reopen
 *     uses — the engine's own rule (the reopen tunes the channel from the URL).
 */
export function uberAuxSend(id: string | null, obj: Record<string, unknown>): boolean {
  if (!id || !aux || aux.id !== id || aux.proto !== 'uber') return false;
  const l = uberLinks.get(id);
  if (l && obj.type === 'tune') {
    if (typeof obj.frequency === 'number') l.freq = obj.frequency;
    if (typeof obj.mode === 'string') l.mode = obj.mode;
    if (typeof obj.bandwidthLow === 'number' && typeof obj.bandwidthHigh === 'number') {
      l.bwLow = obj.bandwidthLow; l.bwHigh = obj.bandwidthHigh;
    }
    auxSetUrl(id, uberAudioUrl(l, uberClient));
  }
  auxSend(id, JSON.stringify(obj));
  return true;
}

/* ─────────────────────────────── the background server talking ─────────────────────────────── */

/** A non-terminal message from a background server: STILL LISTENING?, or a warning of its limit. */
function serverSaid(id: string, type: string, secs: number) {
  if (state.fg === id) return;   // its screen is in front and hears it on the spectrum socket
  if (type === 'idle_check') patchSide(id, { asking: Date.now() + Math.max(5, secs) * 1000 });
  else if (type === 'session_warning' && secs > 0) patchSide(id, { endsAt: Date.now() + secs * 1000 });
}

/** The server ended a side. In the background: the card says why for a few seconds, then it goes. */
function serverEnded(id: string, why: string) {
  const s = state.sides.find((x) => x.id === id);
  if (!s || s.ended) return;
  if (state.fg === id) return;   // its own screen shows the server's refusal, and ends the side from there
  set({ ...state, ...markEnded(state, id, why) });
  setTimeout(() => endSideNow(id), 6000);
}

function patchSide(id: string, p: Partial<DualSide>) {
  if (state.phase === 'off' || !state.sides.some((s) => s.id === id)) return;
  set({ ...state, sides: state.sides.map((s) => (s.id === id ? { ...s, ...p } : s)) });
}

let textSub: { remove: () => void } | null = null;
/** The native pump's text frames (VibeLocalAudioText) — the background server on the main pump speaking. */
function watchNativeText(on: boolean) {
  if (on && !textSub) {
    try {
      const em = new NativeEventEmitter(NativeModules.VibePowerModule);
      textSub = em.addListener('VibeLocalAudioText', (e: { text?: string }) => {
        const native = state.sides.find((s) => s.transport === 'native');
        if (!native || typeof e?.text !== 'string') return;
        let msg: { type?: string; secs?: number } | null = null;
        try { msg = JSON.parse(e.text); } catch { return; }
        const type = String(msg?.type ?? '');
        const why = terminalWhy(type);
        if (why) serverEnded(native.id, why);
        else serverSaid(native.id, type, Number(msg?.secs) || 0);
      });
    } catch { textSub = null; }
  } else if (!on && textSub) {
    try { textSub.remove(); } catch { /* gone */ }
    textSub = null;
  }
}

/* ─────────────────────────────── the mix ─────────────────────────────── */

/**
 * ★★ IS EACH SERVER'S STREAM STILL COMING? Judged twice a second while two are paired: the second voice by its own
 *    socket's last frame (here), the main pump by the native voice's last buffer (getLevels' nativeAge). A server in
 *    ADS-B / AIS / ISM / NDB sends nothing at all, so this is also how a BACKGROUND server's spectrum-only mode is known
 *    (its screen, and with it the flag, is gone). A closed squelch is not a stop — the server sends gated silence.
 *    Kept out of the state: it changes nothing anyone draws, only where the voices sit.
 */
const live = new Map<string, boolean>();
let liveTimer: ReturnType<typeof setInterval> | null = null;
function liveOf(id: string): boolean { return live.get(id) !== false; }
function watchLive(on: boolean) {
  if (on && !liveTimer) {
    liveTimer = setInterval(() => {
      if (state.phase !== 'paired') return;
      const now = Date.now();
      (Dual?.getLevels?.() ?? Promise.resolve(undefined)).then((lv) => {
        let changed = false;
        for (const sd of state.sides) {
          let ok: boolean;
          if (sd.transport === 'aux') {
            // ★ The second voice's feed: its socket's last frame, or a Kiwi / OpenWebRX adapter's last buffer.
            const last = auxLastFrame(sd.id);
            ok = last > 0 && now - last < DUAL_STALL_MS;
          } else {
            // ★ An older binary without the age: never judged stopped (as before).
            const age = Number(lv?.nativeAge);
            ok = !Number.isFinite(age) || (age >= 0 && age < DUAL_STALL_MS);
          }
          if (live.get(sd.id) !== ok) { live.set(sd.id, ok); changed = true; }
        }
        if (changed) pushMix();
      }).catch(() => {});
    }, 500);
  } else if (!on && liveTimer) {
    clearInterval(liveTimer);
    liveTimer = null;
    live.clear();
  }
}

/** When side `id`'s second-voice feed last delivered (ms epoch; 0 = never / not on it). */
function auxLastFrame(id: string): number {
  const a = aux;
  if (a && a.id === id) return a.lastFrameAt;
  const out = backends.get(id)?.audioOut;
  return out && out.current === 'aux' ? out.lastPushAt : 0;
}

/** Tell the two native voices where each server is now. On a pairing change, a mute, or a stream stopping or coming
 *  back — the native side glides. */
/** Where a voice is heard: one ear, both ears (centred), or not at all (zero gain). */
export type DualEar = 'L' | 'R' | 'both' | 'off';
function earOf(v: { pos: number; gain: number }): DualEar {
  if (!(v.gain > 0)) return 'off';
  return Math.abs(v.pos - 0.5) < 0.05 ? 'both' : v.pos < 0.5 ? 'L' : 'R';
}
let lastEars: [DualEar, DualEar] = ['L', 'R'];
let lastMixSig = '';
/** The ear the voice of side `index` (0 = server 1) is in now — the card draws this. */
export function dualEar(index: 0 | 1): DualEar { return lastEars[index]; }
function pushMix() {
  if (!Dual?.setNativeMix) return;
  if (state.phase === 'picking') {
    // ★ The one playing while the server list is up: centred — at zero when the listener muted it or it is in Band
    //   (Band's mute is this side's now, not the speaker's: the pairing took it over from the screen).
    const solo = state.sides[0];
    Dual.setNativeWidth?.(1);   // alone in both ears: its own stereo
    if (solo && (solo.muted || solo.band)) { Dual.setNativeMix(0.5, 0, true); return; }
  }
  if (state.phase !== 'paired') {
    // ★ Centre, unity, and STAND DOWN once there: the main pump goes back to exactly its single-server path.
    Dual.setNativeWidth?.(1);
    Dual.setNativeMix(0.5, 1, false);
    return;
  }
  const [s1, s2] = state.sides;
  const audio = (sd: DualSide | undefined) => !!sd && sideHasAudio({ quiet: sd.quiet, band: sd.band, live: liveOf(sd.id),
                                                                       front: state.fg === sd.id });
  const t = dualTargets({ output: state.output, balance: state.balance,
                          s1Alive: !!s1 && !s1.ended, s2Alive: !!s2 && !s2.ended,
                          s1Muted: !!s1?.muted, s2Muted: !!s2?.muted, s1Audio: audio(s1), s2Audio: audio(s2) });
  const tgt = [t.s1, t.s2];
  // ★ A breadcrumb whenever where the voices sit changes, with every input that decided it — Export logs then shows why
  //   a server went to both ears (Stuart, 2026-10-30: unmuting server 2 took both ears instead of server 1 going left).
  const sig = state.sides.map((sd, i) => `s${i + 1}[${sd.transport}${state.fg === sd.id ? ' front' : ''}${sd.muted ? ' muted' : ''}`
    + `${sd.quiet ? ' quiet' : ''}${sd.band ? ' band' : ''}${liveOf(sd.id) ? '' : ' stopped'}${sd.ended ? ' ended' : ''}`
    + ` → ${tgt[i].pos.toFixed(2)}/${tgt[i].gain.toFixed(2)}${tgt[i].width ? ' stereo' : ''}]`).join(' ') + ` ${state.output} bal ${state.balance.toFixed(2)}`;
  if (sig !== lastMixSig) { lastMixSig = sig; crumb(`dual mix ${sig}`); }
  // ★ What each card's ear icon shows (Stuart, 2026-10-29: "show both when it is in both ears") — from the SAME targets
  //   the voices get, so the icon can never disagree with what is heard.
  const ears = tgt.map((v) => earOf(v)) as [DualEar, DualEar];
  if (ears[0] !== lastEars[0] || ears[1] !== lastEars[1]) {
    lastEars = ears;
    setTimeout(() => subs.forEach((f) => { try { f(); } catch { /* a listener must not stop the rest */ } }), 0);
  }
  state.sides.forEach((s, i) => {
    // ★ The width before the place: both glide together on the voice's next buffer.
    if (s.transport === 'native') { Dual.setNativeWidth?.(tgt[i].width); Dual.setNativeMix?.(tgt[i].pos, tgt[i].gain, true); }
    else { Dual.setAuxWidth?.(tgt[i].width); Dual.setAuxMix?.(tgt[i].pos, tgt[i].gain); }
  });
}

function onChanged(prev: DualState) {
  if (prev.phase !== state.phase || prev.sides !== state.sides || prev.output !== state.output
      || prev.balance !== state.balance) pushMix();
  watchNativeText(state.phase !== 'off');
  watchLive(state.phase === 'paired');
  // ★ The lock-screen card names the server in FRONT (Stuart: "AirPods/remote controls, lock screen Now Playing:
  //   the foreground server"). Remote ▶/⏸ act on the whole app — both servers pause and resume together.
  if (prev.fg !== state.fg && state.fg) {
    const s = state.sides.find((x) => x.id === state.fg);
    if (s) Vibe?.setInstanceName?.(s.name);
  }
}

/**
 * ★★★ MUTE ONE SERVER (its card's mute; its own Audio menu's while it is in front; the Mac's MUTE key while paired).
 *     Only that server's voice goes to zero — the other keeps playing, and centres (dualMix.dualTargets). NOT the
 *     native pause (VibePowerModule.setMuted): that is the whole app's ⏸ — the lock screen, AirPods, interruptions —
 *     and both servers follow it by design.
 */
export function setSideMuted(id: string | null, on: boolean) {
  if (!id) return;
  const next = withSideMuted(state, id, on);
  if (next !== state) set({ ...state, ...next });
}
export function sideMuted(id: string | null): boolean {
  return !!id && state.phase !== 'off' && !!state.sides.find((s) => s.id === id)?.muted;
}

/** ★ A mute the pairing held on the server that is LEFT ALONE IN FRONT outlives the pairing as the ordinary mute — the
 *  screen's own muted banner — rather than the server springing back to full volume the moment the other one goes. */
function carryMute(sur: DualSide | undefined) {
  if (sur?.muted) (NativeModules.VibePowerModule as { setMuted?: (m: boolean) => void })?.setMuted?.(true);
}

/** Levels for the card's MONO speaker (0..1, smoothed natively). */
export async function dualLevels(): Promise<{ native: number; aux: number }> {
  try {
    const r = await Dual?.getLevels?.();
    return { native: Number(r?.native) || 0, aux: Number(r?.aux) || 0 };
  } catch { return { native: 0, aux: 0 }; }
}

/* ─────────────────────────────── the actions ─────────────────────────────── */

/** One-shot params that must not ride a screen's return (they were for its first arrival). */
const ONE_SHOT = ['autoTakeover', 'deepLink', 'initialFreq', 'initialMode', 'initialZoom', 'doorReturn',
                  'dualSide'];
export function returnParams(p: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = { ...p };
  for (const k of ONE_SHOT) delete out[k];
  return out;
}

/** May this screen offer "Listen to a 2nd server"? On every network receiver (dualMix.audioKindOf), only with no
 *  pairing yet. ★ VFO B on does NOT
 *  take the row away — it says it will close B (dualMix.secondServerHint), and the screen closes it first. */
export function canListenSecond(params: DualRouteLike): boolean {
  return dualSupported && state.phase === 'off' && dualEligible(params);
}

/**
 * "Listen to a 2nd server" — the screen in front goes to the background and keeps playing. The caller then goes back
 * to the server list exactly as ‹ Back to server list does.
 */
export function listenSecond(side: Omit<DualSide, 'transport' | 'ended' | 'asking'>, note: string | null = null) {
  if (!audioStarted(side.id, side.kind ?? 'vibe')) return;   // ★ its audio never started — nothing to keep playing
  set({ ...state, ...beginPicking(state, { ...side, transport: 'native', ended: null, asking: null }), note });
}

/** Has side `id` any audio running to keep? Per kind: a VibeServer's socket was opened (links), an UberSDR's engine
 *  was started (uberLinks), a Kiwi / OpenWebRX / FM-DX adapter exists (noteBackend). */
function audioStarted(id: string, kind: AudioKind): boolean {
  if (kind === 'vibe') return links.has(id);
  if (kind === 'uber') return uberLinks.has(id);
  return backends.has(id);   // Kiwi / OpenWebRX / FM-DX: the adapter that plays it
}

/**
 * ★★★ DECIDED ON A SCREEN'S FIRST RENDER, BEFORE ANY CHILD'S EFFECT: a second server's audio player must open the
 *     SECOND voice from its very first run. Opened on the main pump even once, it would replace server 1's socket —
 *     exactly the cut the feature exists to avoid. So the role is read in a useState initialiser, not an effect.
 *  Returns this screen's side id (= its session id), or null for an ordinary screen.
 */
export function roleOnMount(params: Record<string, unknown> & DualRouteLike & { baseUrl?: string },
                            newId: () => string): string | null {
  const back = typeof params.dualSide === 'string' ? params.dualSide : null;
  if (back && state.phase !== 'off' && state.sides.some((s) => s.id === back)) {
    if (state.fg !== back) set({ ...state, fg: back }, true);
    return back;
  }
  if (state.phase === 'off') return null;
  if (state.phase !== 'picking' || !dualEligible(params)
      || state.sides.some((s) => sameServer(s.params as DualRouteLike, params) && !s.radioKey)) {
    /* ★★★ A RECEIVER SCREEN THAT IS NOT PART OF THE PAIRING ENDS IT — before its own audio starts on the same engine.
     *  Something that cannot take part (this device's own radio, RTL-TCP / SpyServer — dualMix.audioKindOf), the same
     *  SERVER again (the picker should have caught that: a second session would be a second slot for one person), or a
     *  screen that arrived from outside the pairing altogether (the watch, a link, the crash guard). Leaving either
     *  server playing under it would put two streams into one player.
     *  ★★ The same SERVER is not the same radio: a server whose playing side is one radio behind its front door
     *     (`radioKey`) is opened again so its radio list can offer the OTHER radio (Gesiima, #57). Choosing the radio
     *     already playing there goes back to it (isFirstRadio). */
    abandon(true);
    return null;
  }
  const id = newId();
  const kind = audioKindOf(params) ?? 'vibe';
  const next = attachSecond(state, {
    id, transport: 'aux', ended: null, name: String(params.instanceName ?? ''), params: returnParams(params),
    freqHz: 0, mode: '', endsAt: null, soft: false, asking: null, kind,
  });
  // ★ Two that both need the main player (two FM-DX servers — dualMix.canPair): the pairing cannot take this one.
  if (next === state) { abandon(true); return null; }
  // ★★ This server needs the main player (FM-DX): server 1 moves to the second voice FIRST — before this screen's own
  //    audio starts on the main player and would cut it.
  const moved = firstMovesToAux(state, next);
  set({ ...state, ...next }, true);
  if (moved) moveToAux(moved);
  return id;
}

/** The screen in front keeps its side's card facts current (cheap: only on change). */
export function noteStatus(id: string, p: Partial<Pick<DualSide, 'freqHz' | 'mode' | 'station' | 'endsAt' | 'soft' | 'name' | 'radioKey' | 'pinned' | 'quiet' | 'band'>>,
                           radioBase?: string | null) {
  const s = state.sides.find((x) => x.id === id);
  if (!s) return;
  /* ★★ A side on one radio behind a front door comes BACK to that radio (a swap, the server list's ‹ Back): its return
   *  address names it (`<base>/r/<id>`), which the screen adopts straight through — never the door's radio list, where
   *  a returning screen would sit waiting for a choice already made. */
  const wantBase = p.radioKey && radioBase ? radioBase : null;
  const baseOk = !wantBase || s.params.baseUrl === wantBase;
  if (baseOk && (Object.keys(p) as (keyof typeof p)[]).every((k) => s[k] === p[k])) return;
  patchSide(id, baseOk ? p : { ...p, params: { ...s.params, baseUrl: wantBase } });
}

/**
 * ★★★ A SCREEN COMING BACK TO A SIDE THAT IS STILL PLAYING (a swap, the server list's ‹ Back) — the side, else null.
 *  Stuart, 2026-10-28 (screen recording, iPhone, both servers connected): tapping the card took him to the Pi 2's
 *  "Choose a receiver" — its landing card, because the radio has an aerial written up — and he had to choose the radio
 *  he was already listening to. The radio was chosen when the side began; its audio never stopped. So the screen does
 *  not open the door again: it adopts its return address as the radio, exactly as an address naming a radio (`/r/<id>`)
 *  already does. ★ Except a radio unlocked with its own PIN (`pinned`): that proof cannot ride along, and the door asks.
 */
export function returningSide(myId: string | null, params: { dualSide?: unknown }): DualSide | null {
  if (!myId || params.dualSide !== myId) return null;
  // ★★ The server list's "return to …" banner (returnToSolo) ends the pairing BEFORE the screen comes up, so the side is
  //   no longer in the state — it is handed over in `returning` (Stuart, 2026-10-30: back to the Pi 2 from the
  //   directory's banner went through its front door again, "like it did before we fixed it" for the card's swap).
  if (returning?.id === myId) { const r = returning; returning = null; return r; }
  if (state.phase === 'off') return null;
  return state.sides.find((x) => x.id === myId && !x.ended) ?? null;
}
/** The side returnToSolo just brought back — read once by its screen (returningSide). */
let returning: DualSide | null = null;

/** ★★ The joining screen's radio is the one the FIRST side is already playing (same server, same radio)? */
export function isFirstRadio(myId: string | null, params: DualRouteLike & { baseUrl?: string }, radioKey: string): boolean {
  if (!myId || state.phase !== 'paired' || state.sides[1]?.id !== myId) return false;
  const first = state.sides[0];
  return !!first && sameRadio({ params: first.params as DualRouteLike, radioKey: first.radioKey ?? '' }, { params, radioKey });
}

/** …then it is not a second side: drop it and go back to the first (its screen adopts the audio still playing). Returns
 *  the params that bring the first side's screen up — the caller navigates, as for returnToSolo. */
export function backToFirstRadio(myId: string): Record<string, unknown> | null {
  if (state.phase !== 'paired' || state.sides[1]?.id !== myId) return null;
  auxClose(myId);   // ★ belt and braces: the radio list held the connect, so nothing should be open
  set({ ...state, ...detachSecond(state, myId) });
  return returnToSolo();
}

/** Where each listener's audio socket goes, by session — so the pairing can reopen a survivor on the main pump.
 *  ★ Kept for every screen that MIGHT join a pairing, because the link is known before the pairing is. */
const links = new Map<string, DualLink>();
export function linkFor(id: string | null): DualLink | null { return (id && links.get(id)) || null; }
export function sameLink(a: DualLink | null, b: DualLink | null): boolean {
  return !!a && !!b && a.host === b.host && a.port === b.port && a.suffix === b.suffix && a.wsBase === b.wsBase;
}
export function noteLink(id: string, link: DualLink) {
  links.set(id, link);
  if (links.size > 8) links.delete(links.keys().next().value as string);   // a handful of recent screens is plenty
}

/** The answer to "may this screen's audio keep playing as it goes?" — see dualMix.holds. */
export function dualHolds(id: string | null): boolean { return !!id && holds(state, id); }
export function dualTransport(id: string | null): Transport { return transportFor(state, id); }

/** TAP the card. Returns the route params to bring the background server's screen up (caller navigates). */
export function swapToBackground(): Record<string, unknown> | null {
  const bg = bgSide(state);
  if (!bg || bg.ended) return null;
  set({ ...state, ...swap(state), sides: state.sides.map((s) => (s.id === bg.id ? { ...s, asking: null } : s)) });
  return { ...bg.params, dualSide: bg.id };
}

function kindOf(id: string): AudioKind {
  return state.sides.find((x) => x.id === id)?.kind ?? 'vibe';
}

/** Stop the main player for a side of this kind — each kind's own stop (only the claimant may: releaseNative). */
function stopNativeFor(kind: AudioKind) {
  if (kind === 'uber') Vibe?.stopAudioEngine?.();
  else if (kind === 'ext') Vibe?.stopExternalAudio?.();
  else if (kind === 'fmdx') Vibe?.stopFmdxAudio?.();
  else Vibe?.stopLocalAudio?.();
}

/** Stop side `id`'s audio on `transport` — and a parked Kiwi / OpenWebRX / FM-DX adapter with it (its session ends;
 *  its own destroy stops whichever player it was on). */
function stopSideAudio(id: string, transport: Transport, kind: AudioKind) {
  // ★★ An adapter whose screen is still UP (it is leaving, or showing its refusal) is stopped and DETACHED: its screen's
  //    own destroy comes after the survivor has been moved to the main player, and must not stop it then.
  const b = backends.get(id);
  if (b && !parked.has(id) && b.detachAudio) {
    if (transport === 'native') releaseNative(id);
    else auxClose(id);
    b.detachAudio();
    backends.delete(id);
    return;
  }
  if (transport === 'aux') auxClose(id);
  else if (releaseNative(id) && !parked.has(id)) stopNativeFor(kind);
  dropBackend(id);
}

/** End a side now: stop its voice; the other plays on centred (dualMix.endSide). */
function endSideNow(id: string) {
  const kind = kindOf(id);
  const { st, fx } = endSide(state, id);
  if (fx.stop) stopSideAudio(fx.stop.id, fx.stop.transport, kind);
  if (fx.moveToNative) moveToNative(fx.moveToNative);
  // ★ The survivor IN FRONT on the second voice: a VibeServer's player moves itself (it reads transportFor); every
  //   other kind's audio is not its screen's to move, so it is moved here.
  if (fx.frontToNative && kindOf(fx.frontToNative) !== 'vibe') moveToNative(fx.frontToNative);
  const wasOn = state.phase !== 'off';
  const sur = state.sides.find((x) => x.id !== id && !x.ended);
  set({ ...state, ...st });
  if (wasOn && state.phase === 'off') carryMute(sur);
}

/**
 * ★ The lone survivor was on the second voice with no screen in front of it: reopen it on the main pump — same URL,
 *   same session (the server re-affirms the slot), and NO hello: nothing a person asked for. A short gap, once.
 */
function moveToNative(id: string) {
  const s = state.sides.find((x) => x.id === id);
  const kind = s?.kind ?? 'vibe';
  if (kind === 'fmdx') return;   // never anywhere else (dualMix.needsMain)
  if (kind === 'ext') {
    // ★ A Kiwi / OpenWebRX adapter: the same socket and session — only where its PCM goes changes. No gap to speak of.
    const b = backends.get(id);
    if (!s || !b?.audioOut) return;
    claimNative(id);
    b.audioOut.setRoute('native');
    Vibe?.setInstanceName?.(s.name);
    return;
  }
  auxClose(id);
  if (!s) return;
  if (kind === 'uber') {
    // ★ The same session on the native engine, on the dial it was left on (the URL carries it — no `tune` is sent).
    const u = uberLinks.get(id);
    if (!u) return;
    claimNative(id);
    Vibe?.setAdminAuth?.(u.adminAuth);
    Vibe?.startAudioEngine?.(u.baseUrl, u.freq, u.mode, u.uuid, u.password);
    Vibe?.setInstanceName?.(s.name);
    return;
  }
  const l = links.get(id);
  if (!l) return;
  claimNative(id);
  Vibe?.startLocalAudio?.(l.host, l.port, '', l.suffix, l.wsBase);
  Vibe?.setInstanceName?.(s.name);
}

/**
 * ★★ The other way: server 1 leaves the main player for the second voice, because server 2 can only play there
 *    (FM-DX — dualMix.attachSecond). Called BEFORE server 2's own audio starts. The same session every time:
 *  · Kiwi / OpenWebRX — a change of route, nothing reconnects;
 *  · VibeServer — the second voice's socket to the same /ws/audio with the same session (no hello: the server kept the
 *    dial — THE SHARED-DIAL CONTRACT), then the pump stops; a short gap, once;
 *  · UberSDR — the second voice's socket to the same session on the dial it is on, then the engine stops.
 */
export function moveToAux(id: string) {
  const s = state.sides.find((x) => x.id === id);
  if (!s) return;
  const kind = s.kind ?? 'vibe';
  if (kind === 'ext') {
    const b = backends.get(id);
    releaseNative(id);
    b?.audioOut?.setRoute('aux');   // stops the main player's feed, the next buffer starts the second voice
    return;
  }
  if (kind === 'uber') {
    const u = uberLinks.get(id);
    if (releaseNative(id)) Vibe?.stopAudioEngine?.();
    if (u) auxOpen({ id, url: uberAudioUrl(u, uberClient), hello: null, proto: 'uber' }, true);
    return;
  }
  if (kind === 'vibe') {
    const l = links.get(id);
    if (releaseNative(id)) Vibe?.stopLocalAudio?.();
    if (l) auxOpen({ id, url: vibeAuxUrl(l), hello: null, proto: 'vibe' }, true);
  }
}

/** "Stop listening to <the background server>" in the server chip's menu. */
export function stopBackground() {
  const bg = bgSide(state);
  if (bg) endSideNow(bg.id);
}

/** The screen in front is LEAVING (Back to server list, or its refusal): the other plays on. */
export function leaveForeground(id: string) {
  if (state.phase === 'paired' && state.fg === id) endSideNow(id);
}

/** A screen unmounted. Unannounced while still in front = it left (a crash reset, an alert's Back): end its side. */
export function screenGone(id: string) {
  if (state.phase !== 'off' && state.fg === id) endSideNow(id);
}

/** The server ended the side in FRONT (its refusal is on screen): the background one keeps playing. */
export function foregroundEnded(id: string) {
  if (state.phase === 'paired' && state.fg === id) endSideNow(id);
}

/** Picking: the one playing in the background, for the server list's banner. */
export function soloSide(): DualSide | null {
  return state.phase === 'picking' ? state.sides[0] ?? null : null;
}

/** Picking → back to the server already playing (its screen adopts the running audio). */
export function returnToSolo(): Record<string, unknown> | null {
  const s = soloSide();
  if (!s) return null;
  returning = s;
  set({ ...(DUAL_OFF as DualCore<DualSide>), output: state.output, balance: state.balance });
  carryMute(s);
  // ★ dualSide carries the SESSION: the screen comes back as the same listener and its player ADOPTS the pump that is
  //   still playing (LocalAudioPlayer: nativeOwnedBy) instead of reopening it.
  return { ...s.params, dualSide: s.id };
}

/** Stop everything the pairing holds (the banner's Stop, or somewhere a second server cannot go). */
export function abandon(quiet = false) {
  if (state.phase === 'off') return;
  for (const s of state.sides) stopSideAudio(s.id, s.transport, s.kind ?? 'vibe');
  set({ ...(DUAL_OFF as DualCore<DualSide>), output: state.output, balance: state.balance }, quiet);
}

/** Is the picked server the RADIO already playing? Then the picker goes BACK to it rather than open a second session.
 *  ★★ Not when that side is one radio behind the server's front door: the server opens again and its radio list offers
 *     the other radio (dualMix.pickSameServer — Gesiima's two dongles on one VibeServer, #57). */
export function isSolo(params: DualRouteLike & { baseUrl?: string }): boolean {
  const s = soloSide();
  if (!s) return false;
  crumb(`dual pick ${params.localHost ?? (params.baseUrl ? normBase(params.baseUrl) : '?')}:${params.localPort ?? ''} vs solo ${
    (s.params as { localHost?: string }).localHost ?? (s.params as { baseUrl?: string }).baseUrl ?? '?'}:${
    (s.params as { localPort?: number }).localPort ?? ''}${s.radioKey ?? ''}`);
  return pickSameServer({ params: s.params as DualRouteLike, radioKey: s.radioKey ?? '' }, params) === 'back';
}

/** The card's answer to STILL LISTENING? — a TAP, a person's own action. `ping` is the smallest message there is:
 *  the server counts any message as presence and this one changes nothing else (the web client sends the same). */
export function answerStillHere(id: string) {
  const s = state.sides.find((x) => x.id === id);
  if (!s) return;
  const ping = JSON.stringify({ type: 'ping' });
  if (s.transport === 'aux') auxSend(id, ping);
  // ★ UberSDR: the same `ping` resets its session_timeout (their idle-detector.js) — sent here only on a person's tap.
  else if (s.kind === 'uber') Vibe?.sendAudioCommand?.(ping);
  // ★ sendLocalText, NOT sendLocalTune: the pump REMEMBERS the last tune to say again on a reopen, and a ping must not
  //   take its place (VibePowerModule.swift / VibeStreamService.kt).
  else (NativeModules.VibePowerModule as { sendLocalText?: (j: string) => void })?.sendLocalText?.(ping);
  patchSide(id, { asking: null });
}
