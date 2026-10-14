/**
 * ★★★ ON-DEVICE TRANSCRIPTION + TRANSLATION (V12, experimental) — the JS face of the native VibeTranscriber
 * module (ios/VibeSDR/VibeTranscriber.swift, android …/VibeTranscriberModule.kt).
 *
 * It listens to the audio WE PLAY, so it is client-side and backend-agnostic: VibeServer, UberSDR, Kiwi, OWRX,
 * FM-DX, a local dongle, DAB — anything. Not to be confused with UberSDR's STT key (`whisper`), which is that
 * server's own engine and stays exactly as it is.
 *
 * ★★ Nothing leaves the phone: Apple SpeechAnalyzer + Translation (iOS 26+), Android's on-device SpeechRecognizer
 *    (13+, language detection on 14+) + ML Kit on-device translation. Where the device cannot do it, capabilities()
 *    says so and the TRANSCRIBE key is not drawn (AGENTS.md: no dead controls).
 */
import { NativeEventEmitter, NativeModules } from 'react-native';

export interface TranscribeLanguage { id: string; name: string; installed?: boolean }
export interface TranscribeCaps {
  available: boolean;
  reason?: string;
  /** Two-letter code of the phone's language — the translation target. */
  deviceLanguage?: string;
  deviceLanguageName?: string;
  languages?: TranscribeLanguage[];
  /** Android: the recogniser can identify the spoken language itself (14+). iOS AUTO is done by comparing models. */
  nativeLanguageDetection?: boolean;
}
export type TranscribeEvent =
  | { type: 'status'; text: string }
  | { type: 'error'; text: string }
  | { type: 'partial'; text: string }
  | { type: 'lang'; text: string; name: string }
  | { type: 'final'; text: string; lang?: string; translated?: string };

const Native: any = NativeModules.VibeTranscriber;

let capsPromise: Promise<TranscribeCaps> | null = null;
/** Asked once per launch — the answer is the device's, not the server's. */
export function transcribeCaps(): Promise<TranscribeCaps> {
  if (!Native?.capabilities) return Promise.resolve({ available: false, reason: 'Not in this build' });
  if (!capsPromise) {
    capsPromise = Native.capabilities().catch((e: any) => {
      capsPromise = null;
      return { available: false, reason: String(e?.message ?? e) };
    });
  }
  return capsPromise as Promise<TranscribeCaps>;
}

export function startTranscribe(language: string, translateTo: string) { Native?.start?.(language || 'auto', translateTo || ''); }
export function stopTranscribe() { Native?.stop?.(); }

export function onTranscribe(fn: (e: TranscribeEvent) => void): () => void {
  if (!Native) return () => {};
  const sub = new NativeEventEmitter(Native).addListener('VibeTranscribe', fn as any);
  return () => sub.remove();
}

/** Short labels for the language row — the native list is long (dozens of locales); these are offered when present. */
export const COMMON_LANGS = ['en', 'es', 'fr', 'de', 'it', 'pt', 'nl', 'ru', 'ar', 'zh', 'ja', 'ko', 'hi', 'tr', 'pl'];

/** The first native locale for a two-letter code ('en' → 'en-GB' on a British phone, if the list carries it). */
export function pickLocale(caps: TranscribeCaps | null, code: string): string | null {
  const ls = caps?.languages ?? [];
  const matches = ls.filter(l => l.id.toLowerCase().split(/[-_]/)[0] === code);
  if (!matches.length) return null;
  // Prefer an installed one, then the phone's own region.
  const region = (Intl as any)?.DateTimeFormat?.().resolvedOptions?.().locale?.split('-')[1];
  return (matches.find(l => l.installed)
       ?? matches.find(l => region && l.id.endsWith(region))
       ?? matches[0]).id;
}
