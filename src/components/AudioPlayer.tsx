import { useEffect, useRef } from 'react';
import { NativeModules, NativeEventEmitter } from 'react-native';
import { noteAudioEvent } from '../services/audioPathLog';
import { v4 as uuidv4 } from 'uuid';

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
let holdHealingMissingLogged = false;
export function holdNativeHealing(ms: number, why: string): void {
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
    noteAudioEvent(`self-heal held ${Math.round(ms / 1000)} s — ${why}`);
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
}

export default function AudioPlayer({ baseUrl, frequency, mode, step, instanceName, uuid: propUuid, password, adminAuth, restartKey, onStuck }: AudioPlayerProps) {
  const activeUrl  = useRef<string | null>(null);
  const activeFreq = useRef<number>(0);
  const activeMode = useRef<string>('');
  const uuid       = useRef<string>(propUuid ?? uuidv4());

  // Start/stop when baseUrl OR the session uuid changes. A new uuid means a
  // full from-scratch reconnect (e.g. the data saver resuming): the old engine
  // is torn down and a fresh native session is opened.
  const lastRestart = useRef<number | undefined>(restartKey);
  const lastAdmin   = useRef<string | undefined>(adminAuth);
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

  useEffect(() => {
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
    const forced = restartKey !== lastRestart.current || adminAuth !== lastAdmin.current;
    lastRestart.current = restartKey;
    lastAdmin.current   = adminAuth;
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
      VibePowerModule?.setAdminAuth?.(adminAuth ?? '');
      noteAudioEvent(`startAudioEngine → ${baseUrl} ${frequency} ${mode}`
                   + ` session=${uuid.current.slice(0, 8)}${adminAuth ? ' [admin]' : ''}`);
      VibePowerModule?.startAudioEngine(baseUrl, frequency, mode, uuid.current, password ?? '');
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

    return () => { noteAudioEvent('unmount/deps changed — stopping engine'); VibePowerModule?.stopAudioEngine(); };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [baseUrl, propUuid, restartKey, adminAuth]);

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
