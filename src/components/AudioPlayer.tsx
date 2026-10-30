import { useEffect, useRef } from 'react';
import { NativeModules, NativeEventEmitter } from 'react-native';
import { noteAudioEvent } from '../services/audioPathLog';
import { v4 as uuidv4 } from 'uuid';
import { USER_AGENT } from '../constants/version';
import { auxClose, auxIsOpenFor, auxOpen, claimNative, dualHolds, nativeOwnedBy, noteUberLink, releaseNative,
         setUberClientName, uberAudioUrl, type UberLink } from '../services/dualServer';

// Both platforms expose the SAME native surface as "VibePowerModule"
// (iOS: VibePowerModule.swift; Android: VibeStreamModule.kt getName()).
// Recording/NR methods are iOS-only — Android stubs them.
export const VibePowerModule = NativeModules.VibePowerModule as
  | {
      startAudioEngine:  (baseUrl: string, frequency: number, mode: string, uuid: string, password: string) => void;
      /** ★★★ THE OWNER'S ADMIN CREDENTIAL FOR THE AUDIO SOCKET. This engine opens its own socket,
       *  so the credential the JS client puts on the spectrum URL never reached it — and on a busy
       *  receiver it was refused for having none while the spectrum socket evicted the occupant
       *  and took the slot. The owner held their own radio in silence. Set BEFORE starting: it is
       *  read when the socket is built. */
      setAdminAuth:      (q: string) => void;
      stopAudioEngine:   () => void;
      // v7 FM-DX Webserver spike: native MP3-over-WS audio. baseUrl = server root.
      startFmdxAudio?:   (baseUrl: string) => void;
      stopFmdxAudio?:    () => void;
      /** Android: hold the window's drawing through a rotation until the SDR screen has re-rendered (MainActivity). */
      armRotationHold?:     (on: boolean) => void;
      releaseRotationHold?: () => void;
      /** Android: arm/disarm the thread sweep (VibeAffinity); resolves the plan. */
      armThreads?:          (on: boolean) => Promise<string>;
      lastThreadSweep?:     () => Promise<string>;
      sendTuneCommand:   (frequency: number, mode: string) => void;
      /** ★ Adopt the server's dial into native's cache. NEVER transmits — see SdrWsClient's config
       *  handler and VibeStreamService.noteServerFreq. Optional: older native builds lack it. */
      noteServerFreq?:   (frequency: number, mode: string) => void;
      sendBandwidth:     (low: number, high: number) => void;
      setStep:           (hz: number) => void;
      setInstanceName:   (name: string) => void;
      setMuted:          (muted: boolean) => void;
      setVolume:         (v: number) => void;
      startRecording:    () => Promise<string>;
      stopRecording:     () => Promise<string | null>;
      shareRecording:    (path: string) => void;
      setNrMode:         (mode: 'off' | 'nr' | 'nr2') => void;
      setNoiseBlanker:   (on: boolean) => void;
      setNotch?:         (on: boolean) => void;
      sendAudioCommand:  (json: string) => void;
      setNowPlaying:     (title: string, artist: string) => void;
      setArtwork:        (serverType: string) => void;
      setStationLogo?:   (url: string) => void;   // FM-DX: inlay station favicon on the art
      setMediaSkipMode:  (mode: 'step' | 'bookmark') => void;
      /** ★★ ⏮⏭ on/off (greyed on the lock screen, refused if a stale one fires) — a SHARED DIAL with
       *  others listening switches them off, as FM-DX always has (see services/blindTuneGate).
       *  Optional: native builds before it lack it, and JS refuses the skip anyway. */
      setMediaSkipEnabled?: (enabled: boolean) => void;
      setBrowseItems?:   (json: string) => void;
      setReconnectFailed?: (failed: boolean) => void;
      setDefaultInstance?: (name: string) => void;   // '' = none (Siri "set a default")
      setVoiceConnected?: (connected: boolean) => void;   // Siri: emit now vs stash
      getPendingVoiceQuery?: () => Promise<string | null>;   // cold-launch Siri query
      /** ★★★ Hold the NATIVE self-heal across a legitimate pause (a DAB transition) — see
       *  holdNativeHealing below. Optional: native builds before it lack it. */
      holdHealing?:      (ms: number) => void;
      /** ★★ V12: a mode with nothing to hear (ADS-B, ISM) lets go of the output — see setNativeQuiet. iOS only; optional
       *  (older builds and Android lack it). */
      setQuiet?:         (on: boolean) => void;
      getDebugInfoSync:  () => string;
      addListener:       (name: string) => void;
      removeListeners:   (count: number) => void;
    }
  | undefined;

/** ★★★ A PAUSE THE CALLER KNOWS ABOUT IS NOT A STALL. The self-heal (VibePowerModule.swift /
 *  VibeStreamService.kt `heal`) repairs "frames arriving, nothing playing for 3 s" — and entering,
 *  leaving or changing a DAB multiplex, or picking a service, is exactly that for several seconds
 *  while the server acquires and primes. Judged as a fault, the heal rebuilt the pipeline and then
 *  reopened the socket in the middle of the acquisition: "DAB worked previously to the new audio
 *  watchdog" (Stuart, 2026-09-29, Sony VibeServer Lite). The web client has held since the heal
 *  was written (audio.holdHealing); the native heal runs below JS and never heard about DAB.
 *  ★ Extends, never shortens. A native build without the method logs ONCE and carries on — the
 *    heal it would hold does not exist in that build either. */
/** ★★★ V12 QUIET (Stuart, 2026-10-25: "I switch to the mac and VibeSDR pulls my Airpods over for silence"). ADS-B and
 *  ISM send no audio, but the native engine kept the output running and Now Playing said "playing" — which is what Apple's
 *  automatic switching follows. The web's AudioPlayer.setQuiet (1f0b204a): while on, the native player and engine are
 *  paused, the session released and Now Playing paused; the audio SOCKET stays open (the server counts the listener by it).
 *  ★ Android has no such method: its Bluetooth headsets do not move between devices on their own, so there is nothing
 *    to release there; a build without it logs once. */
let setQuietMissingLogged = false;
export function setNativeQuiet(on: boolean, why: string): void {
  const fn = VibePowerModule?.setQuiet;
  if (typeof fn !== 'function') {
    if (!setQuietMissingLogged) { setQuietMissingLogged = true; noteAudioEvent(`quiet ${on ? 'on' : 'off'} (${why}) — not in this native build`); }
    return;
  }
  noteAudioEvent(`quiet ${on ? 'on' : 'off'} (${why})`);
  try { fn(on); } catch { /* the engine is gone */ }
}

let holdHealingMissingLogged = false;
/* ★ `quiet`: a REPEATED hold (ADS-B keeps one up for as long as the mode runs) is not news each time — the
 *  first is logged, the renewals are not, or the audio log becomes a column of the same line. */
export function holdNativeHealing(ms: number, why: string, quiet = false): void {
  const fn = VibePowerModule?.holdHealing;
  if (typeof fn !== 'function') {
    if (!holdHealingMissingLogged) {
      holdHealingMissingLogged = true;
      console.warn(`[AudioPlayer] native holdHealing unavailable in this build — self-heal not held (${why})`);
    }
    return;
  }
  try {
    fn(ms);
    if (!quiet) noteAudioEvent(`self-heal held ${Math.round(ms / 1000)} s — ${why}`);
  } catch (e) {
    console.warn('[AudioPlayer] holdHealing failed:', e);
  }
}

export interface AudioPlayerProps {
  baseUrl:       string | null;
  frequency:     number;
  mode:          string;
  step?:         number;
  instanceName?: string;
  uuid?:         string;
  /** Bypass password — appended to the audio WS URL (rate-limit bypass). */
  password?:     string;
  /** The owner's admin credential ("&vs_admin_ticket=…"), for taking a busy receiver back. It has
   *  to be on BOTH sockets: the spectrum one evicts the occupant, and without it here the audio
   *  socket is refused in the same breath. */
  adminAuth?:    string;
  /** ★★★ BUMP TO REBUILD THE NATIVE AUDIO SOCKET WITHOUT CHANGING WHO WE ARE. The engine carries
   *  no admin credential — it cannot, the native side takes only a PIN — so on a BUSY receiver its
   *  socket is refused ("audio WS refused — server busy … no credential") while the spectrum
   *  socket, which does carry one, evicts the occupant and takes the slot. The listener then owns
   *  the radio and hears nothing, because the refused audio socket never tries again.
   *  ★★ Reconnecting AFTER the takeover needs no credential at all: by then we ARE the occupant,
   *     and the server admits our own session freely. The credential was only ever needed to
   *     evict, and the spectrum socket has already done that.
   *  ★ Deliberately NOT the uuid. Changing that changes our identity, which is what made the app
   *    collide with its own session earlier tonight — the engine must come back as the SAME
   *    listener. */
  restartKey?:   number;
  /** ★★★ THE NATIVE WATCHDOG HAS REOPENED THE AUDIO SOCKET SEVERAL TIMES AND NO PACKET HAS COME
   *  BACK. Native cannot cure that on its own: an UberSDR server drops a socket whose session it
   *  never registered, and only a fresh POST /connection — which lives in JS — makes it keep one.
   *  Without this the watchdog retried every four seconds indefinitely and the user simply had no
   *  sound (issue #20). */
  onStuck?:      () => void;
  /** ★★ V12 TWO SERVERS AT ONCE (src/services/dualServer.ts). Absent (null) = the ordinary single-server path, exactly
   *  as before. `id` is this screen's side (= `uuid`, its session); `transport` which voice its audio rides: 'native' —
   *  this engine (server 1, or any UberSDR playing alone); 'aux' — the second voice, whose socket dualServer reads in JS
   *  and which outlives this component. */
  dual?:         { id: string; transport: 'native' | 'aux' } | null;
  /** The filter edges, for a reopen of the second voice's socket (the engine keeps its own copy natively). */
  bandwidthLow?:  number;
  bandwidthHigh?: number;
}

export default function AudioPlayer({ baseUrl, frequency, mode, step, instanceName, uuid: propUuid, password, adminAuth, restartKey, onStuck,
                                      dual = null, bandwidthLow, bandwidthHigh }: AudioPlayerProps) {
  const activeUrl  = useRef<string | null>(null);
  const activeFreq = useRef<number>(0);
  const activeMode = useRef<string>('');
  const uuid       = useRef<string>(propUuid ?? uuidv4());

  // Start/stop when baseUrl OR the session uuid changes. A new uuid means a
  // full from-scratch reconnect (e.g. the data saver resuming): the old engine
  // is torn down and a fresh native session is opened.
  const lastRestart = useRef<number | undefined>(restartKey);
  /* ★★★ A RENEWED TICKET IS NOT A NEW CREDENTIAL — IT MUST NOT RESTART THE ENGINE. The screen renews the admin ticket
   *     every four minutes, each time as a new string; with that string in the dependency lists below, the effect's
   *     CLEANUP stopped the engine on every renewal (the early return for "nothing changed" runs after the cleanup has
   *     already happened), so a signed-in owner lost the sound every 3m59s. The server decides admin once, when it
   *     accepts the socket — so only a change of KIND (none → challenge pair → ticket, or the takeover answer) needs a
   *     new socket. A fresh value of the same kind is handed to the native side for its NEXT handshake, nothing more. */
  const adminKind = !adminAuth ? ''
    : (/vs_admin_ticket=/.test(adminAuth) ? 'ticket' : 'pair') + (/vs_takeover=1/.test(adminAuth) ? '+takeover' : '');
  const adminAuthRef = useRef(adminAuth);
  adminAuthRef.current = adminAuth;
  useEffect(() => { VibePowerModule?.setAdminAuth?.(adminAuth ?? ''); }, [adminAuth]);
  const lastAdmin   = useRef<string>(adminKind);
  /** ★★★ NATIVE IS THE ONLY LAYER THAT KNOWS WHAT THE SOCKET DID. It opens its own connection, so
   *  every fact worth having — engine started, WS ready, WS aborted, first packet, a feed refused
   *  — was reachable only through a Mac, a cable and Console.app. The person who can reproduce the
   *  fault is holding a phone; put it in the report they can send. */
  useEffect(() => {
    // ★ Both platforms since the self-heal (2026-09-28): VibeStreamService now emits VibeAudioPath
    //   for every repair it makes, through the same RCTDeviceEventEmitter its VibeMuted already
    //   reaches JS by (its addListener/removeListeners stubs do not stop delivery). VibeAudioStuck
    //   is still iOS's alone — Android never emits it, so that listener simply never fires there.
    if (!VibePowerModule) return;
    const em = new NativeEventEmitter(VibePowerModule as never);
    const path  = em.addListener('VibeAudioPath',  (e: { what?: string }) => {
      if (e?.what) noteAudioEvent('native: ' + e.what);
    });
    const stuck = em.addListener('VibeAudioStuck', (e: { reopens?: number }) => {
      noteAudioEvent(`native: STUCK after ${e?.reopens ?? '?'} reopens — re-registering session`);
      onStuck?.();
    });
    return () => { path.remove(); stuck.remove(); };
  }, [onStuck]);

  /* ★★ V12 TWO SERVERS: which voice the engine effect last opened, and whether it has run on this mount — the FIRST run
   *  may ADOPT audio already playing for this listener (a swap back, a return from the server list). */
  const transport = dual?.transport ?? 'native';
  const dualId = dual?.id ?? null;
  const ranOnce = useRef(false);
  /** Where this listener's audio is, for the pairing (reopen on either voice). Kept current with the dial. */
  const link = (): UberLink | null => (baseUrl && propUuid ? {
    baseUrl, uuid: propUuid, password: password ?? '', adminAuth: adminAuth ?? '',
    freq: frequency, mode, bwLow: bandwidthLow, bwHigh: bandwidthHigh,
  } : null);
  useEffect(() => {
    const l = link();
    if (dualId && l) noteUberLink(dualId, l);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [dualId, baseUrl, propUuid, password, adminAuth, frequency, mode, bandwidthLow, bandwidthHigh]);

  /* ★★★ THE SECOND VOICE (dualServer.ts): an UberSDR that is server 2 — or that moved there when an FM-DX joined — plays
   *  through its OWN socket read in JS, never this engine: the engine is the main player, and two servers never share
   *  one player. The socket belongs to dualServer and outlives this screen when it goes to the background. */
  const auxRestart = useRef(restartKey);
  const auxAdmin = useRef(adminKind);
  useEffect(() => {
    if (transport !== 'aux' || !dualId) return;
    if (!baseUrl) { noteAudioEvent('second voice: no baseUrl — gate closed'); return; }
    const l = link();
    if (!l) return;
    setUberClientName(USER_AGENT);
    // ★ A reconnect (restartKey) or a new credential asks for a NEW socket; anything else adopts the one playing.
    const forced = restartKey !== auxRestart.current || adminKind !== auxAdmin.current;
    auxRestart.current = restartKey; auxAdmin.current = adminKind;
    if (!forced && auxIsOpenFor(dualId)) noteAudioEvent('second voice adopted (UberSDR)');
    else {
      noteAudioEvent(`second voice opening (UberSDR) → ${baseUrl}`);
      auxOpen({ id: dualId, url: uberAudioUrl(l, USER_AGENT), hello: null, proto: 'uber' }, forced);
    }
    ranOnce.current = true;
    return () => {
      // ★ Going to the background (a swap): dualServer still holds it — keep playing. Otherwise it is ours to close.
      setTimeout(() => { if (!dualHolds(dualId)) auxClose(dualId); }, 0);
    };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [transport, dualId, baseUrl, propUuid, restartKey, adminKind]);

  const prevTransport = useRef<'native' | 'aux' | null>(null);
  useEffect(() => {
    const fromAux = prevTransport.current === 'aux';
    prevTransport.current = transport;
    if (transport === 'aux' && dualId) { activeUrl.current = null; return; }
    // ★★ V12 TWO SERVERS: the engine is ALREADY playing this listener (it never stopped while its screen was away, or the
    //   pairing just moved it here — dualServer.moveToNative) — adopt it. Restarting it would cut the audio and meet the
    //   server with a reconnect.
    if ((!ranOnce.current || fromAux) && dualId && baseUrl && nativeOwnedBy(propUuid ?? null)) {
      ranOnce.current = true;
      activeUrl.current = baseUrl; uuid.current = propUuid ?? uuid.current;
      lastRestart.current = restartKey; lastAdmin.current = adminKind;
      activeFreq.current = frequency; activeMode.current = mode;
      noteAudioEvent('native engine adopted — already playing this listener');
      return () => {
        setTimeout(() => {
          if (!dualHolds(dualId) && releaseNative(propUuid ?? '')) { noteAudioEvent('unmount — stopping engine'); VibePowerModule?.stopAudioEngine(); }
        }, 0);
      };
    }
    ranOnce.current = true;
    // ★★★ A CREDENTIAL THAT ARRIVES LATER IS STILL A REASON TO REBUILD. adminAuth was in the
    //     dependency list — so this effect re-ran when the owner typed the password — and then
    //     returned early, because baseUrl and uuid had not changed. setAdminAuth() was never
    //     called, so the native engine kept the EMPTY credential it started with, and its socket
    //     was refused on a busy receiver while the spectrum socket evicted the occupant.
    // ★★★ THE ORDER IS ALWAYS THIS WAY ROUND. You connect first and prove yourself second: the
    //     password is typed on a receiver you are already looking at. So the interesting case is
    //     precisely the one this guard excluded — the credential changing while everything else
    //     stays put. Giving the native side the ABILITY to carry it and never handing it over is
    //     the same fault as the ability not existing (Stuart, 2026-08-16: "I thought you wrote the
    //     native code to do that?" — I did, and then did not use it).
    const forced = restartKey !== lastRestart.current || adminKind !== lastAdmin.current;
    lastRestart.current = restartKey;
    lastAdmin.current   = adminKind;
    if (!forced && baseUrl === activeUrl.current && propUuid === uuid.current) return;
    // ★ A forced restart tears the engine down first: startAudioEngine on a live engine is not a
    //   reconnect, and the socket we are trying to replace is the one that was refused.
    if (forced && baseUrl) { noteAudioEvent('forced restart — stopping engine'); VibePowerModule?.stopAudioEngine(); }
    activeUrl.current = baseUrl;

    if (!VibePowerModule) {
      console.error('[AudioPlayer] VibePowerModule not found in NativeModules');
    }

    if (baseUrl) {
      uuid.current = propUuid ?? uuidv4();
      // ★ BEFORE the engine starts, never after: the credential is read when the socket URL is
      //   built, and a busy receiver decides whether to refuse us at that handshake.
      VibePowerModule?.setAdminAuth?.(adminAuthRef.current ?? '');
      noteAudioEvent(`startAudioEngine → ${baseUrl} ${frequency} ${mode}`
                   + ` session=${uuid.current.slice(0, 8)}${adminAuthRef.current ? ' [admin]' : ''}`);
      VibePowerModule?.startAudioEngine(baseUrl, frequency, mode, uuid.current, password ?? '');
      // ★ V12: the engine is the main player — say whose it is, so a screen that goes away later cannot stop another's.
      claimNative(uuid.current);
      VibePowerModule?.setInstanceName(instanceName ?? '');
      activeFreq.current = frequency;
      activeMode.current = mode;
    } else {
      // ★ baseUrl null is a GATE, not an absence — a refusal, a tune not yet loaded, or (since
      //   issue #20) a session the server has not registered yet. Say so: this is the state that
      //   produces no socket at all, which no server-side log can ever show.
      noteAudioEvent('no baseUrl — gate closed (refusal, tune, or session not registered)');
      VibePowerModule?.stopAudioEngine();
    }

    /* ★★ V12 TWO SERVERS: stop only what is still OURS and not wanted by the pairing — dualHolds (this server is going to
     *  the background and keeps playing) and releaseNative (another server has the main player now). Outside a pairing
     *  both answers are "stop", exactly as before. Decided a tick late: a re-run must not stop what the next run adopts. */
    const sid = uuid.current;
    return () => {
      if (!dualId) { noteAudioEvent('unmount/deps changed — stopping engine'); VibePowerModule?.stopAudioEngine(); return; }
      if (dualHolds(dualId) || !releaseNative(sid)) { noteAudioEvent('leaving — the pairing keeps the engine'); return; }
      noteAudioEvent('unmount/deps changed — stopping engine'); VibePowerModule?.stopAudioEngine();
    };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [baseUrl, propUuid, restartKey, adminKind, transport, dualId]);

  /* ★★★ THIS EFFECT NO LONGER TUNES, AND THAT IS THE FIX.
   *
   *  It fired `sendTuneCommand` whenever the `frequency` PROP changed — and the prop changes for two
   *  completely different reasons that a prop cannot tell apart:
   *    1. the user moved the dial, in which case SdrWsClient.tune() has ALREADY sent it (_routeTune),
   *       so this was a duplicate; and
   *    2. the SERVER said the dial moved (someone else tuned a shared receiver), in which case this
   *       ECHOED the server's own value straight back at it — a control action from a client nobody
   *       touched, and a second owner of the dial.
   *  ★★ Stuart, 2026-09-20: "The app should have always taken the servers word as gospel and never
   *     tried to force itself." ONE writer now: SdrWsClient._routeTune, reached only from a user
   *     action. Adoption goes the other way, through noteServerFreq, and is silent.
   *  ✗ DO NOT restore a send here "so the native side stays in step". That is what this was for, and
   *    keeping native's copy in step by TRANSMITTING is the entire fault — see noteServerFreq. */
  useEffect(() => {
    if (!activeUrl.current) return;
    if (frequency === activeFreq.current && mode === activeMode.current) return;
    activeFreq.current = frequency;
    activeMode.current = mode;
  }, [frequency, mode]);

  // Sync step to native for lock-screen / notification skip buttons
  useEffect(() => {
    if (step == null) return;
    VibePowerModule?.setStep(step);
  }, [step]);

  // Sync instance name
  useEffect(() => {
    VibePowerModule?.setInstanceName(instanceName ?? '');
  }, [instanceName]);

  return null;
}
