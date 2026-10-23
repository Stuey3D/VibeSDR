/**
 * ★★★ ON-DEVICE TRANSCRIPTION + TRANSLATION (V12, experimental) — the JS face of the native VibeTranscriber
 * module (ios/VibeSDR/VibeTranscriber.swift, android …/VibeTranscriberModule.kt).
 *
 * It listens to the audio WE PLAY, so it is client-side and backend-agnostic: VibeServer, UberSDR, Kiwi, OWRX,
 * FM-DX, a local dongle, DAB — anything. Not to be confused with UberSDR's STT key (`whisper`), which is that
 * server's own engine and stays exactly as it is.
 *
 * ★★ Nothing leaves the phone: Apple SpeechAnalyzer + Translation (iOS 26+), Android's on-device SpeechRecognizer
 *    (12+; language list, downloads and AUTO need 13; detection on 14+) + ML Kit on-device translation. Where the device cannot do it, capabilities()
 *    says so and the TRANSCRIBE key is not drawn (AGENTS.md: no dead controls).
 */
import { NativeEventEmitter, NativeModules, Platform } from 'react-native';

export interface TranscribeLanguage { id: string; name: string; installed?: boolean }
export interface TranscribeCaps {
  available: boolean;
  reason?: string;
  /** Two-letter code of the phone's language — the translation target. */
  deviceLanguage?: string;
  deviceLanguageName?: string;
  /** iOS: the phone's language as a translation target, with its variant where Apple's Translation has several
   *  ('pt-BR', 'zh-Hant', 'en-GB'); else the bare code. Android (ML Kit) translates to bare codes only. */
  deviceTranslateTo?: string;
  /** TRANSLATE TO's list: every language the device can translate into (iOS: Apple's Translation, ids like 'pt',
   *  'pt-PT', 'zh-TW'; Android: ML Kit's bare codes). `installed`: the pack is already here (iOS only). */
  translateLanguages?: { id: string; name: string; installed?: boolean }[];
  /** The phone's region ('GB') — which variant 'English' means. */
  deviceRegion?: string;
  languages?: TranscribeLanguage[];
  /** Android: the recogniser can identify the spoken language itself (14+). iOS AUTO scores every model on the device. */
  nativeLanguageDetection?: boolean;
  /** Android 12 (API 31-32): the phone cannot be asked which speech languages it has, nor fetch one — the user chooses
   *  the language (no AUTO, no GET), and `installed` is never claimed. */
  chooseOnly?: boolean;
}
export type TranscribeEvent =
  | { type: 'status'; text: string }
  | { type: 'error'; text: string }
  | { type: 'partial'; text: string }
  /** AUTO's search, for the box: the languages it listens for, then what it heard (or that nothing was recognised). */
  | { type: 'trying'; text: string }
  | { type: 'tried'; text: string }
  | { type: 'lang'; text: string; name: string; detected?: boolean }
  | { type: 'final'; text: string; lang?: string; translated?: string }
  /** SSB auto-tune moved the RECOGNISER's copy of the audio (never what is heard): the correction as a dial change,
   *  Hz (a number on iOS, a string on Android); 0 = none. */
  | { type: 'tune'; text?: string; hz: number | string };

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

/** GET — fetch a language's speech model (+ translation pack) without listening; { ok, text } for the row.
 *  `keep` (iOS): a ★ language or the phone's own — it KEEPS one of Apple's 5 places; any other GET gives its place
 *  straight back (the system may tidy that language away when unused — GET again). Android has no places. */
export function downloadTranscribeLanguage(language: string, translateTo: string, keep = false): Promise<{ ok: boolean; text: string }> {
  if (!Native?.download) return Promise.resolve({ ok: false, text: 'Not in this build' });
  const p = Platform.OS === 'ios' ? Native.download(language, translateTo || '', !!keep) : Native.download(language, translateTo || '');
  return p.catch((e: any) => ({ ok: false, text: String(e?.message ?? e) }));
}

/** ★★ THE 5 KEPT PLACES (iOS; Stuart, 2026-10-17 night): the phone's language + the ★ list (locale ids, ≤ 4). Native
 *  reserves the ★ ones on the device, keeps the phone's language if the app holds it, and gives back every other
 *  place. Call at launch once capabilities and the ★ list are known, and after every ★ change. No-op elsewhere. */
export function keepTranscribeLanguages(localeIds: string[]): Promise<string[]> {
  if (Platform.OS !== 'ios' || !Native?.keep) return Promise.resolve([]);
  return Native.keep(localeIds).then((r: any) => (r?.kept ?? []) as string[]).catch(() => []);
}

/** language: 'auto', 'auto:<prior order>' or a locale id; translateTo: '' = off. */
export function startTranscribe(language: string, translateTo: string) {
  Native?.start?.(language || 'auto', translateTo || '');
}
export function stopTranscribe() { Native?.stop?.(); }
/** ★ The dial, for SSB AUTO-TUNE of the recogniser's feed (V12): in USB/LSB the native side estimates how far the
 *  voice is mistuned from its own harmonics and shifts ITS copy back (vibedsp/ssb_offset — shared C++, both
 *  platforms). Any change of mode or frequency resets the estimate. Cheap — call it whenever either changes. */
export function setTranscribeTuning(mode: string, freqHz: number) { Native?.tuning?.(String(mode || ''), Number(freqHz) || 0); }

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
/** The TRANSLATE TO button for a target: 'en' → English, 'pt-BR' → Portuguese (BR), 'zh-Hant' → Chinese (Trad.).
 *  Kept short — it is a segment button. Written Chinese is "Chinese", not the spoken "Mandarin" of LANG_NAMES. */
export function translateTargetLabel(id: string): string {
  const [code, sub] = id.split(/[-_]/);
  const base = code.toLowerCase();
  if (base === 'zh') return sub === 'Hant' || sub === 'TW' || sub === 'HK' ? 'Chinese (Trad.)' : sub ? 'Chinese (Simp.)' : 'Chinese';
  const name = LANG_NAMES[base] ?? id;
  return sub ? `${name} (${sub.toUpperCase()})` : name;
}
/** Same language, any variant ('pt' and 'pt-BR'). */
export function sameLanguage(a: string, b: string): boolean { return !!a && !!b && baseOf(a) === baseOf(b); }
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

/* ★★ V12 GALLERY TRANSLATE (2026-10-23) — a TEXT through the same on-device translator as the captions: Apple's
 *   Translation on iOS 26+ (VibeTranscriber.swift translateText, its language packs and download sheet), ML Kit on
 *   Android (TextTranslate.kt — packs on Wi-Fi only, as the captions). Nothing leaves the phone. An older native build
 *   without these methods → not available, and the gallery draws no TRANSLATE (AGENTS.md: no dead controls). */
export interface TextTranslateInfo {
  available: boolean;
  reason?: string;
  /** The phone's own translation target ('en-GB', 'pt-BR') — used when TRANSLATE TO is off. */
  deviceLanguage?: string;
  /** The platform can identify a text's language itself (iOS NaturalLanguage). */
  detect?: boolean;
}
let textInfo: Promise<TextTranslateInfo> | null = null;
export function textTranslateInfo(): Promise<TextTranslateInfo> {
  if (!Native?.textTranslateInfo || !Native?.translateText) return Promise.resolve({ available: false, reason: 'Not in this build' });
  if (!textInfo) {
    textInfo = Native.textTranslateInfo().catch((e: any) => { textInfo = null; return { available: false, reason: String(e?.message ?? e) }; });
  }
  return textInfo as Promise<TextTranslateInfo>;
}
/** One line or paragraph, source → target (language ids). { ok, text } or { ok: false, reason }. */
export function translateText(text: string, source: string, target: string): Promise<{ ok: boolean; text?: string; reason?: string }> {
  if (!Native?.translateText) return Promise.resolve({ ok: false, reason: 'Not in this build' });
  return Native.translateText(text, source, target).catch((e: any) => ({ ok: false, reason: String(e?.message ?? e) }));
}
/** The text's language by the platform's own detector (iOS), with `hints` as its priors; '' when it cannot say. */
export function detectTextLanguage(text: string, hints: string[]): Promise<string> {
  if (!Native?.detectTextLanguage) return Promise.resolve('');
  return Native.detectTextLanguage(text, hints).then((r: any) => (Number(r?.p) >= 0.5 ? String(r?.lang || '') : '')).catch(() => '');
}
