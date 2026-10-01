/**
 * macAudio.ts — the Mac-only VOLUME fader and MUTE key: the store, its persistence and the native gain.
 *
 * ★★★ THE iPAD APP ON A MAC HAS NO VOLUME OF ITS OWN (no per-app volume in macOS; the system volume is
 *     everybody's), so the AUDIO popup draws a VOLUME fader and a MUTE key there — ONLY there. The rules
 *     (when it shows, the taper, what MUTE and the fader do to each other) are pure, in
 *     src/constants/macAudio.ts, and tested by scripts/test_mac_audio.ts.
 *
 * ★ DETECTION is the existing synchronous deviceClass() on the VibeLocalSDR module (ProcessInfo
 *   .isiOSAppOnMac || .isMacCatalystApp — see services/deviceClass.ts), read once per process, so the
 *   very first render already knows. Android, web, an iPhone, an iPad, an old binary: not a Mac.
 * ★ THE GAIN is VibePowerModule.setMacOutputGain — the native engine's main mixer, which every backend's
 *   audio plays through (see the note on it in VibePowerModule.swift). Native forces unity off a Mac too.
 *   Recordings are taken BEFORE it, so the fader never changes what is saved. Playing a saved recording
 *   (expo-audio, RecordingsOverlay) follows the same gain.
 * ★ PERSISTED PER DEVICE in AsyncStorage (never cloud-synced: one Mac's speakers say nothing about
 *   another's). Writes are debounced — the fader moves sixty times a second.
 */
import { useSyncExternalStore } from 'react';
import { NativeModules, Platform } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { readNativeDeviceClass } from './deviceClass';
import { parseDeviceClass } from '../constants/transparency';
import {
  MAC_AUDIO_DEFAULT, MAC_AUDIO_STORAGE_KEY, macOutputGain, macSilenced, parseMacAudio, setMacVolume,
  showMacAudio, toggleMacMute, type MacAudioState,
} from '../constants/macAudio';

let onMacCached: boolean | null = null;
/** Is this the iOS app running on a Mac? Read once per process. */
export function isMacHost(): boolean {
  if (onMacCached === null) {
    onMacCached = showMacAudio(Platform.OS, parseDeviceClass(readNativeDeviceClass()).isMac);
  }
  return onMacCached;
}

let state: MacAudioState = MAC_AUDIO_DEFAULT;
const listeners = new Set<() => void>();
let saveTimer: ReturnType<typeof setTimeout> | null = null;
let started = false;

function pushGain(): void {
  if (!isMacHost()) return;   // never touch the engine's gain anywhere else
  const fn = (NativeModules as { VibePowerModule?: { setMacOutputGain?: (g: number) => void } })
    .VibePowerModule?.setMacOutputGain;
  if (typeof fn !== 'function') return;   // an older binary: the popup is still honest (see showMacAudio)
  try { fn(macOutputGain(state, true)); } catch { /* a bridge refusing = no method */ }
}

function commit(next: MacAudioState): void {
  if (next.volume === state.volume && next.muted === state.muted) return;
  state = next;
  pushGain();
  listeners.forEach((l) => l());
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    saveTimer = null;
    AsyncStorage.setItem(MAC_AUDIO_STORAGE_KEY, JSON.stringify(state)).catch(() => {});
  }, 300);
}

/** Load the saved volume / mute and hand the gain to native. Called once at launch (App.tsx); a no-op
 *  anywhere but a Mac. Safe to call again. */
export function initMacAudio(): void {
  if (started || !isMacHost()) return;
  started = true;
  pushGain();   // unity until the store answers — today's behaviour
  AsyncStorage.getItem(MAC_AUDIO_STORAGE_KEY)
    .then((raw) => {
      if (raw == null) return;
      // ★ A touch that landed before the read wins — it is newer than what was saved.
      if (state !== MAC_AUDIO_DEFAULT) return;
      const s = parseMacAudio(raw);
      state = s;
      pushGain();
      listeners.forEach((l) => l());
    })
    .catch(() => {});
}

export function setMacAudioVolume(v: number): void { commit(setMacVolume(state, v)); }
export function toggleMacAudioMute(): void { commit(toggleMacMute(state)); }

function subscribe(l: () => void): () => void {
  listeners.add(l);
  return () => { listeners.delete(l); };
}
const snapshot = () => state;

/** The popup's view: whether to draw the controls at all, and their state. */
export function useMacAudio(): { onMac: boolean; state: MacAudioState; gain: number } {
  const s = useSyncExternalStore(subscribe, snapshot, snapshot);
  const onMac = isMacHost();
  return { onMac, state: s, gain: macOutputGain(s, onMac) };
}

/** The deck's speaker key: draw the muted legend? False on anything but a Mac. */
export function useMacSilenced(): boolean {
  const s = useSyncExternalStore(subscribe, snapshot, snapshot);
  return macSilenced(s, isMacHost());
}
