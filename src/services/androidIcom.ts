/**
 * androidIcom.ts — the ANDROID half of an Icom server's setup facts (docs/ICOM-SUPPORT-PLAN.md Phase 4), for the
 * shared Icom setup card that ServerModeScreen draws in the main app and in VibeServer Lite.
 *
 * ★★ What the phone knows that a Linux server's hwinfo cannot: whether Android lets us record the radio's USB audio
 *    (the microphone permission), which USB input it picked, and whether capture is running. The engine's own facts
 *    — model, CI-V address, baud, transport, the preflight with every menu item, audio/audioProblem — come along as
 *    `radio`, the SAME object hwinfo carries (radioCapsJsonCore), so the card can be one component for every platform.
 * ★ Read-only: nothing here opens the radio. icomAskForAudio() repeats the explained permission ask the server start
 *   already made; it does nothing unless an Icom is being served.
 * ★ Kept in its own file on purpose: the UI work on ServerModeScreen / icomRadio.ts is another branch's, and this is
 *   the only place the native names icomSetup / icomAskForAudio are spelled.
 */
import { NativeModules, Platform } from 'react-native';

const Local: any = (NativeModules as any).VibeLocalSDR;

/** VibeIcom.facts() on the Kotlin side. `radio` is hwinfo.radio for driver "icom" (see icomRadio.ts's IcomRadio). */
export interface AndroidIcomFacts {
  radio?: Record<string, unknown>;
  /** RECORD_AUDIO granted. False = say "allow the radio's audio" and offer icomAskForAudio(). */
  micPermission: boolean;
  /** AudioRecord is recording the radio's USB input right now. */
  audioRunning: boolean;
  /** The input it records from, e.g. "USB Audio CODEC (USB)"; "" before it started. */
  audioDevice: string;
  /** Plain words, or "" — e.g. Android routed the recording off the radio, or the app must be opened once. */
  audioProblem: string;
  /** Android 7.0+: below it the radio's audio is not served at all (no way to prove the input). */
  audioSupported: boolean;
}

/** The connected radio as getConnectedRadio() reports it — the Icom fields are present only for driver "icom". */
export interface AndroidConnectedRadio {
  driver: string;
  model: string;
  /** false = a CP210x Android will not identify until the owner allows it (Android 10+ hides its serial): show
   *  "an Icom? press Start and allow it" — Start checks the serial before anything is sent. */
  icomConfirmed?: boolean;
  micPermission?: boolean;
}

export async function androidIcomFacts(): Promise<AndroidIcomFacts | null> {
  if (Platform.OS !== 'android') return null;
  try {
    const s = await Local?.icomSetup?.();
    return typeof s === 'string' ? (JSON.parse(s) as AndroidIcomFacts) : null;
  } catch {
    return null;
  }
}

export function icomAskForAudio(): void {
  if (Platform.OS !== 'android') return;
  try { Local?.icomAskForAudio?.(); } catch {}
}
