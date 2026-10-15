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
  /** AUTO's search, for the box: the languages being tried, then what each one made of it. */
  | { type: 'trying'; text: string }
  | { type: 'tried'; text: string }
  | { type: 'lang'; text: string; name: string; detected?: boolean }
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

/** Ask again (after a download) — the installed ticks change. */
export function refreshTranscribeCaps(): Promise<TranscribeCaps> { capsPromise = null; return transcribeCaps(); }

/** DOWNLOAD… — fetch a language's speech model (+ translation pack) without listening; { ok, text } for the row. */
export function downloadTranscribeLanguage(language: string, translateTo: string): Promise<{ ok: boolean; text: string }> {
  if (!Native?.download) return Promise.resolve({ ok: false, text: 'Not in this build' });
  return Native.download(language, translateTo || '').catch((e: any) => ({ ok: false, text: String(e?.message ?? e) }));
}

export function startTranscribe(language: string, translateTo: string) { Native?.start?.(language || 'auto', translateTo || ''); }
export function stopTranscribe() { Native?.stop?.(); }

export function onTranscribe(fn: (e: TranscribeEvent) => void): () => void {
  if (!Native) return () => {};
  const sub = new NativeEventEmitter(Native).addListener('VibeTranscribe', fn as any);
  return () => sub.remove();
}

/** Names for the language list and the [tags] — ★ named, not ISO codes: Stuart looked for "CN" for China Radio and
 *  never saw "ZH" (2026-10-15). Mandarin and Cantonese are separate because shortwave carries both (CRI, RTHK, the
 *  PBS regionals); Android may call Mandarin 'cmn'. Anything not here takes the platform's own name. */
export const LANG_NAMES: Record<string, string> = {
  en: 'English', es: 'Spanish', fr: 'French', de: 'German', it: 'Italian', pt: 'Portuguese', nl: 'Dutch',
  ru: 'Russian', uk: 'Ukrainian', pl: 'Polish', tr: 'Turkish', ar: 'Arabic', zh: 'Mandarin', yue: 'Cantonese',
  ja: 'Japanese', ko: 'Korean', hi: 'Hindi', vi: 'Vietnamese', th: 'Thai', id: 'Indonesian', cmn: 'Mandarin',
};
/** A locale id or code ('zh-CN', 'yue', 'RU') → the name the box shows; anything unknown passes through. */
export function langName(idOrCode: string | undefined): string {
  if (!idOrCode) return '';
  return LANG_NAMES[idOrCode.toLowerCase().split(/[-_]/)[0]] ?? idOrCode;
}
/** Which region a bare code means when the device lists several (zh-HK / zh-TW are other variants). */
const PREFERRED_REGION: Record<string, string> = { zh: 'cn', yue: 'cn' };

/** The first native locale for a two-letter code ('en' → 'en-GB' on a British phone, if the list carries it). */
export function pickLocale(caps: TranscribeCaps | null, code: string): string | null {
  const ls = caps?.languages ?? [];
  const matches = ls.filter(l => l.id.toLowerCase().split(/[-_]/)[0] === code);
  if (!matches.length) return null;
  // Prefer an installed one, then the phone's own region.
  const region = (Intl as any)?.DateTimeFormat?.().resolvedOptions?.().locale?.split('-')[1];
  const pref = PREFERRED_REGION[code];
  return (matches.find(l => pref && l.id.toLowerCase().endsWith(`-${pref}`))
       ?? matches.find(l => l.installed)
       ?? matches.find(l => region && l.id.endsWith(region))
       ?? matches[0]).id;
}

/** ★ The LANGUAGE dropdown: EVERY language the device can transcribe, one row per language (the native lists carry
 *  regional variants — en-AU, en-GB … — and the variant is picked by pickLocale), named and sorted by name. */
export function transcribeLanguageList(caps: TranscribeCaps | null): { code: string; id: string; name: string; installed: boolean }[] {
  const bases = new Map<string, string>();   // base code → native name (region stripped)
  for (const l of caps?.languages ?? []) {
    const base = l.id.toLowerCase().split(/[-_]/)[0];
    if (base === 'mul') continue;            // Apple's "multiple languages of India" model — not a language to choose
    if (!bases.has(base)) bases.set(base, (l.name || base).replace(/\s*\(.*\)\s*$/, ''));
  }
  const out: { code: string; id: string; name: string; installed: boolean }[] = [];
  for (const [code, native] of bases) {
    const id = pickLocale(caps, code);
    if (!id) continue;
    // ON PHONE describes the variant we would actually use (pickLocale's choice), not any variant.
    out.push({ code, id, name: LANG_NAMES[code] ?? native, installed: !!caps?.languages?.find(l => l.id === id)?.installed });
  }
  return out.sort((a, b) => a.name.localeCompare(b.name));
}
