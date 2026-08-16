import { useEffect, useRef } from 'react';
import { NativeModules } from 'react-native';
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
      setBrowseItems?:   (json: string) => void;
      setReconnectFailed?: (failed: boolean) => void;
      setDefaultInstance?: (name: string) => void;   // '' = none (Siri "set a default")
      setVoiceConnected?: (connected: boolean) => void;   // Siri: emit now vs stash
      getPendingVoiceQuery?: () => Promise<string | null>;   // cold-launch Siri query
      getDebugInfoSync:  () => string;
      addListener:       (name: string) => void;
      removeListeners:   (count: number) => void;
    }
  | undefined;

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
}

export default function AudioPlayer({ baseUrl, frequency, mode, step, instanceName, uuid: propUuid, password, adminAuth, restartKey }: AudioPlayerProps) {
  const activeUrl  = useRef<string | null>(null);
  const activeFreq = useRef<number>(0);
  const activeMode = useRef<string>('');
  const uuid       = useRef<string>(propUuid ?? uuidv4());

  // Start/stop when baseUrl OR the session uuid changes. A new uuid means a
  // full from-scratch reconnect (e.g. the data saver resuming): the old engine
  // is torn down and a fresh native session is opened.
  const lastRestart = useRef<number | undefined>(restartKey);
  const lastAdmin   = useRef<string | undefined>(adminAuth);
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
    if (forced && baseUrl) VibePowerModule?.stopAudioEngine();
    activeUrl.current = baseUrl;

    if (!VibePowerModule) {
      console.error('[AudioPlayer] VibePowerModule not found in NativeModules');
    }

    if (baseUrl) {
      uuid.current = propUuid ?? uuidv4();
      // ★ BEFORE the engine starts, never after: the credential is read when the socket URL is
      //   built, and a busy receiver decides whether to refuse us at that handshake.
      VibePowerModule?.setAdminAuth?.(adminAuth ?? '');
      VibePowerModule?.startAudioEngine(baseUrl, frequency, mode, uuid.current, password ?? '');
      VibePowerModule?.setInstanceName(instanceName ?? '');
      activeFreq.current = frequency;
      activeMode.current = mode;
    } else {
      VibePowerModule?.stopAudioEngine();
    }

    return () => { VibePowerModule?.stopAudioEngine(); };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [baseUrl, propUuid, restartKey, adminAuth]);

  // Sync tune when frequency or mode changes (native owns now-playing metadata)
  useEffect(() => {
    if (!activeUrl.current) return;
    if (frequency === activeFreq.current && mode === activeMode.current) return;
    activeFreq.current = frequency;
    activeMode.current = mode;
    VibePowerModule?.sendTuneCommand(frequency, mode);
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
