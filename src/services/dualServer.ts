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
import {
  DUAL_OFF, attachSecond, beginPicking, bgSide, clampBalance, dualEligible, dualTargets, endSide, holds,
  markEnded, sameServer, swap, terminalWhy, transportFor,
  type DualCore, type DualOutput, type DualRouteLike, type DualSideCore, type Transport,
} from './dualMix';

/** The native second voice (ios/VibeSDR/VibeDualAudio.swift, android …/VibeDualAudio.kt). Absent = an older binary
 *  or a platform without it (tvOS, web): the feature is not offered — no dead row. */
type DualNative = {
  setNativeMix?: (pos: number, gain: number, active: boolean) => void;
  setAuxMix?: (pos: number, gain: number) => void;
  startAux?: () => void;
  stopAux?: () => void;
  pushAuxOpus?: (b64: string, rate: number, channels: number) => void;
  pushAuxPcm?: (b64: string, rate: number, channels: number) => void;
  getLevels?: () => Promise<{ native?: number; aux?: number }>;
};
const Dual = (NativeModules as { VibeDualAudio?: DualNative }).VibeDualAudio;
const Vibe = NativeModules.VibePowerModule as {
  startLocalAudio?: (host: string, port: number, initialTune: string, authSuffix: string, wsBase: string) => void;
  stopLocalAudio?: () => void;
  setInstanceName?: (name: string) => void;
} | undefined;

/** Can this build play two servers at all? */
export const dualSupported: boolean = Platform.OS !== 'web' && !!Dual?.pushAuxOpus && !!Dual?.setNativeMix;

/** Where a side's audio socket is — exactly the arguments LocalAudioPlayer gave the native pump. */
export interface DualLink { host: string; port: number; suffix: string; wsBase: string }

export interface DualSide extends DualSideCore {
  name: string;
  /** The SDR route params that bring its screen back (one-shot ones stripped). */
  params: Record<string, unknown>;
  /** What it is playing, in DISPLAY Hz, as its screen last showed it. */
  freqHz: number;
  mode: string;
  /** Its session limit as an absolute time (ms), and whether that limit is a guarantee rather than an end. */
  endsAt: number | null;
  soft: boolean;
  /** The server asked "still listening?" — the deadline it gave (ms epoch). */
  asking: number | null;
}

export interface DualState extends DualCore<DualSide> {
  output: DualOutput;
  /** −1 = all server 1 … 0 even … +1 = all server 2. */
  balance: number;
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
  sides: { id: string; transport: Transport; ended: boolean; name: string }[];
}
let shapeKey = '';
let shape: DualShape = { phase: 'off', fg: null, output: 'stereo', sides: [] };
function getShape(): DualShape {
  const k = `${state.phase}|${state.fg}|${state.output}|`
          + state.sides.map((s) => `${s.id}:${s.transport}:${s.ended ? 1 : 0}:${s.name}`).join(',');
  if (k !== shapeKey) {
    shapeKey = k;
    shape = { phase: state.phase, fg: state.fg, output: state.output,
              sides: state.sides.map((s) => ({ id: s.id, transport: s.transport, ended: !!s.ended, name: s.name })) };
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

type AuxOpen = { id: string; url: string; hello: string | null };
let aux: {
  id: string; url: string; ws: WebSocket | null; wanted: boolean; terminal: boolean; retries: number;
  lastFrameAt: number; openedAt: number; started: boolean;
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
          openedAt: Date.now(), started: false, retryTimer: null, dog: null };
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

function onAuxFrame(buf: ArrayBuffer, a: NonNullable<typeof aux>) {
  auxOnBytes?.(buf.byteLength);
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

function onAuxText(id: string, text: string, a: NonNullable<typeof aux>) {
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

/** Tell the two native voices where each server is now. Only on a pairing change — the native side glides. */
function pushMix() {
  if (!Dual?.setNativeMix) return;
  if (state.phase !== 'paired') {
    // ★ Centre, unity, and STAND DOWN once there: the main pump goes back to exactly its single-server path.
    Dual.setNativeMix(0.5, 1, false);
    return;
  }
  const [s1, s2] = state.sides;
  const t = dualTargets({ output: state.output, balance: state.balance,
                          s1Alive: !!s1 && !s1.ended, s2Alive: !!s2 && !s2.ended });
  const tgt = [t.s1, t.s2];
  state.sides.forEach((s, i) => {
    if (s.transport === 'native') Dual.setNativeMix?.(tgt[i].pos, tgt[i].gain, true);
    else Dual.setAuxMix?.(tgt[i].pos, tgt[i].gain);
  });
}

function onChanged(prev: DualState) {
  if (prev.phase !== state.phase || prev.sides !== state.sides || prev.output !== state.output
      || prev.balance !== state.balance) pushMix();
  watchNativeText(state.phase !== 'off');
  // ★ The lock-screen card names the server in FRONT (Stuart: "AirPods/remote controls, lock screen Now Playing:
  //   the foreground server"). Remote ▶/⏸ act on the whole app — both servers pause and resume together.
  if (prev.fg !== state.fg && state.fg) {
    const s = state.sides.find((x) => x.id === state.fg);
    if (s) Vibe?.setInstanceName?.(s.name);
  }
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

/** May this screen offer "Listen to a 2nd server"? Only on a VibeServer, only with no pairing yet. */
export function canListenSecond(params: DualRouteLike): boolean {
  return dualSupported && state.phase === 'off' && dualEligible(params);
}

/**
 * "Listen to a 2nd server" — the screen in front goes to the background and keeps playing. The caller then goes back
 * to the server list exactly as ‹ Back to server list does.
 */
export function listenSecond(side: Omit<DualSide, 'transport' | 'ended' | 'asking'>) {
  if (!links.has(side.id)) return;   // ★ its audio never started — nothing to keep playing
  set({ ...state, ...beginPicking(state, { ...side, transport: 'native', ended: null, asking: null }) });
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
      || state.sides.some((s) => sameServer(s.params as DualRouteLike, params))) {
    /* ★★★ A RECEIVER SCREEN THAT IS NOT PART OF THE PAIRING ENDS IT — before its own audio starts on the same engine.
     *  Somewhere the second voice cannot go (an UberSDR, a Kiwi, local hardware), the same server again (the picker
     *  should have caught that), or a screen that arrived from outside the pairing altogether (the watch, a link, the
     *  crash guard). Leaving either server playing under it would put two streams into one player. */
    abandon(true);
    return null;
  }
  const id = newId();
  set({ ...state, ...attachSecond(state, {
    id, transport: 'aux', ended: null, name: String(params.instanceName ?? ''), params: returnParams(params),
    freqHz: 0, mode: '', endsAt: null, soft: false, asking: null,
  }) }, true);
  return id;
}

/** The screen in front keeps its side's card facts current (cheap: only on change). */
export function noteStatus(id: string, p: Partial<Pick<DualSide, 'freqHz' | 'mode' | 'endsAt' | 'soft' | 'name'>>) {
  const s = state.sides.find((x) => x.id === id);
  if (!s) return;
  if ((Object.keys(p) as (keyof typeof p)[]).every((k) => s[k] === p[k])) return;
  patchSide(id, p);
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

/** End a side now: stop its voice; the other plays on centred (dualMix.endSide). */
function endSideNow(id: string) {
  const { st, fx } = endSide(state, id);
  if (fx.stop) {
    if (fx.stop.transport === 'aux') auxClose(fx.stop.id);
    else if (releaseNative(fx.stop.id)) Vibe?.stopLocalAudio?.();
  }
  if (fx.moveToNative) moveToNative(fx.moveToNative);
  set({ ...state, ...st });
}

/**
 * ★ The lone survivor was on the second voice with no screen in front of it: reopen it on the main pump — same URL,
 *   same session (the server re-affirms the slot), and NO hello: nothing a person asked for. A short gap, once.
 */
function moveToNative(id: string) {
  const s = state.sides.find((x) => x.id === id);
  const l = links.get(id);
  auxClose(id);
  if (!s || !l) return;
  claimNative(id);
  Vibe?.startLocalAudio?.(l.host, l.port, '', l.suffix, l.wsBase);
  Vibe?.setInstanceName?.(s.name);
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
  set({ ...(DUAL_OFF as DualCore<DualSide>), output: state.output, balance: state.balance });
  // ★ dualSide carries the SESSION: the screen comes back as the same listener and its player ADOPTS the pump that is
  //   still playing (LocalAudioPlayer: nativeOwnedBy) instead of reopening it.
  return { ...s.params, dualSide: s.id };
}

/** Stop everything the pairing holds (the banner's Stop, or somewhere a second server cannot go). */
export function abandon(quiet = false) {
  if (state.phase === 'off') return;
  for (const s of state.sides) {
    if (s.transport === 'aux') auxClose(s.id);
    else if (releaseNative(s.id)) Vibe?.stopLocalAudio?.();
  }
  set({ ...(DUAL_OFF as DualCore<DualSide>), output: state.output, balance: state.balance }, quiet);
}

/** Is the picked server the one already playing? Then the picker goes BACK to it rather than open a second session. */
export function isSolo(params: DualRouteLike & { baseUrl?: string }): boolean {
  const s = soloSide();
  return !!s && sameServer(s.params as DualRouteLike, params);
}

/** The card's answer to STILL LISTENING? — a TAP, a person's own action. `ping` is the smallest message there is:
 *  the server counts any message as presence and this one changes nothing else (the web client sends the same). */
export function answerStillHere(id: string) {
  const s = state.sides.find((x) => x.id === id);
  if (!s) return;
  const ping = JSON.stringify({ type: 'ping' });
  if (s.transport === 'aux') auxSend(id, ping);
  // ★ sendLocalText, NOT sendLocalTune: the pump REMEMBERS the last tune to say again on a reopen, and a ping must not
  //   take its place (VibePowerModule.swift / VibeStreamService.kt).
  else (NativeModules.VibePowerModule as { sendLocalText?: (j: string) => void })?.sendLocalText?.(ping);
  patchSide(id, { asking: null });
}
