import { useEffect, useRef } from 'react';
import { NativeModules } from 'react-native';
import { v4 as uuidv4 } from 'uuid';

// Both platforms expose the SAME native surface as "VibePowerModule"
// (iOS: VibePowerModule.swift; Android: VibeStreamModule.kt getName()).
// Recording/NR methods are iOS-only — Android stubs them.
export const VibePowerModule = NativeModules.VibePowerModule as
  | {
      startAudioEngine:  (baseUrl: string, frequency: number, mode: string, uuid: string, password: string) => void;
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
      setNowPlayingAlbum?: (album: string) => void;   // CarFM: freq -> ALBUM (§5b)
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
}

export default function AudioPlayer({ baseUrl, frequency, mode, step, instanceName, uuid: propUuid, password }: AudioPlayerProps) {
  const activeUrl  = useRef<string | null>(null);
  const activeFreq = useRef<number>(0);
  const activeMode = useRef<string>('');
  const uuid       = useRef<string>(propUuid ?? uuidv4());

  // Start/stop when baseUrl OR the session uuid changes. A new uuid means a
  // full from-scratch reconnect (e.g. the data saver resuming): the old engine
  // is torn down and a fresh native session is opened.
  useEffect(() => {
    if (baseUrl === activeUrl.current && propUuid === uuid.current) return;
    activeUrl.current = baseUrl;

    if (!VibePowerModule) {
      console.error('[AudioPlayer] VibePowerModule not found in NativeModules');
    }

    if (baseUrl) {
      uuid.current = propUuid ?? uuidv4();
      VibePowerModule?.startAudioEngine(baseUrl, frequency, mode, uuid.current, password ?? '');
      VibePowerModule?.setInstanceName(instanceName ?? '');
      activeFreq.current = frequency;
      activeMode.current = mode;
    } else {
      VibePowerModule?.stopAudioEngine();
    }

    return () => { VibePowerModule?.stopAudioEngine(); };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [baseUrl, propUuid]);

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
