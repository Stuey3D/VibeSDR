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
  /** The phone's region ('GB') — which variant 'English' means. */
  deviceRegion?: string;
  /** iOS: how many speech languages Apple lets an app keep at once (AssetInventory.maximumReservedLocales). */
  maxLanguages?: number;
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
  cs: 'Czech', sk: 'Slovak', hu: 'Hungarian', ro: 'Romanian', el: 'Greek', sv: 'Swedish', da: 'Danish',
  nb: 'Norwegian', no: 'Norwegian', fi: 'Finnish', he: 'Hebrew', hr: 'Croatian', sr: 'Serbian', bg: 'Bulgarian',
  ca: 'Catalan', fa: 'Persian', ms: 'Malay', ur: 'Urdu', bn: 'Bengali', ta: 'Tamil', sw: 'Swahili',
  lt: 'Lithuanian', lv: 'Latvian', et: 'Estonian', sq: 'Albanian', be: 'Belarusian', kk: 'Kazakh', az: 'Azerbaijani',
  ka: 'Georgian', is: 'Icelandic', mt: 'Maltese',
};
/** A locale id or code ('zh-CN', 'yue', 'RU') → the name the box shows; anything unknown passes through. */
export function langName(idOrCode: string | undefined): string {
  if (!idOrCode) return '';
  return LANG_NAMES[idOrCode.toLowerCase().split(/[-_]/)[0]] ?? idOrCode;
}
/** ★★ WHICH VARIANT A LANGUAGE MEANS (Stuart, 2026-10-15: French came out as French (Belgium), English as English
 *  (Australia) — the native lists are alphabetical and the old fallback took the first). In order: the phone's own
 *  region (en-GB on a British phone), then the language's home variant, then one already on the phone. */
const HOME_REGION: Record<string, string> = {
  en: 'us', fr: 'fr', de: 'de', es: 'es', it: 'it', pt: 'br', nl: 'nl', ru: 'ru', uk: 'ua', pl: 'pl', tr: 'tr',
  ar: 'sa', zh: 'cn', cmn: 'cn', yue: 'cn', ja: 'jp', ko: 'kr', hi: 'in', vi: 'vn', th: 'th', id: 'id', sv: 'se',
  da: 'dk', nb: 'no', fi: 'fi', el: 'gr', cs: 'cz', ro: 'ro', hu: 'hu', he: 'il', ms: 'my',
};
const baseOf = (id: string) => id.toLowerCase().split(/[-_]/)[0];

/** The native locale for a language code ('en' → 'en-GB' on a British phone). An id with a region passes through. */
export function pickLocale(caps: TranscribeCaps | null, code: string): string | null {
  const ls = caps?.languages ?? [];
  const base = baseOf(code);
  const matches = ls.filter(l => baseOf(l.id) === base);
  if (!matches.length) return null;
  if (code.includes('-')) { const exact = matches.find(l => l.id.toLowerCase() === code.toLowerCase()); if (exact) return exact.id; }
  const region = (caps?.deviceRegion ?? '').toLowerCase();
  const home = HOME_REGION[base];
  const tail = (r: string) => (l: TranscribeLanguage) => l.id.toLowerCase().split(/[-_]/).pop() === r;
  return (matches.find(l => !!region && tail(region)(l))
       ?? matches.find(l => !!home && tail(home)(l))
       ?? matches.find(l => l.installed)
       ?? matches[0]).id;
}

/** ★ The LANGUAGE dropdown: EVERY language the device can transcribe, one row per language, named and sorted.
 *  ★★ A ROW IS A LANGUAGE, NOT A VARIANT: `id` is the code ('fr') and that is what is chosen and stored. The variant
 *     (`locale`) is worked out when it is used — keying rows on the variant id lost the tick whenever the variant
 *     pickLocale preferred changed (after a download), which read as "the language will not select". */
export function transcribeLanguageList(caps: TranscribeCaps | null): { code: string; id: string; locale: string; name: string; installed: boolean }[] {
  const bases = new Map<string, string>();   // base code → native name (region stripped)
  for (const l of caps?.languages ?? []) {
    const base = baseOf(l.id);
    if (base === 'mul') continue;            // Apple's "multiple languages of India" model — not a language to choose
    if (!bases.has(base)) bases.set(base, (l.name || base).replace(/\s*\(.*\)\s*$/, ''));
  }
  const out: { code: string; id: string; locale: string; name: string; installed: boolean }[] = [];
  for (const [code, native] of bases) {
    const locale = pickLocale(caps, code);
    if (!locale) continue;
    // ON PHONE: any variant of the language — the speech model is the language's.
    const installed = !!caps?.languages?.some(l => baseOf(l.id) === code && l.installed);
    out.push({ code, id: code, locale, name: LANG_NAMES[code] ?? native, installed });
  }
  return out.sort((a, b) => a.name.localeCompare(b.name));
}

/** A stored choice → the row it belongs to: 'auto', or the language code (older builds stored 'fr-BE'). */
export function transcribeChoiceCode(stored: string): string { return stored === 'auto' ? 'auto' : baseOf(stored); }
