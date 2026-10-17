/**
 * ★★★ WHICH LANGUAGES TRANSCRIBE'S AUTO TRIES, IN WHAT ORDER (V12, 2026-10-17) — pure, so it is tested
 * (scripts/test_transcribeOrder.ts). No React Native imports: the test runs under plain node.
 *
 * Apple has no spoken-language identifier, so iOS AUTO scores every speech model on the device on the same 10 s of
 * speech (VibeTranscriber + LanguageID.swift, from the 2026-10-17 bake-off). This order no longer decides what is
 * tried first in rounds — it ORDERS THE SHORTLIST on a device too slow to score everything each window, and breaks
 * exact ties. On Android it is the detection allow-list (14) or the one LIKELY language (13). One entry per LANGUAGE:
 *
 *  1. EiBi's languages for this frequency, when the schedule names the broadcast (up to 3) — what is on air.
 *  2. The receiver's MAIN language — ALWAYS in the first places when it is on the device (Stuart, 2026-10-17: "reserve
 *     a slot for the SDR's home language if installed. So I tune into Kiko's and Brazilian becomes one of the first
 *     languages it tries"), in the variant spoken there (pt-BR, en-GB) when that one is installed. Not on the device
 *     → it gets no reserved place (AUTO never downloads) and drops to step 4.
 *  3. The user's PREFERRED languages, in their order (Stuart, 2026-10-17: "have a preferred languages it tries first
 *     if no eibi … I can say Auto mode try English, French, Dutch, German, Spanish first") — the ★ in the list.
 *  4. The receiver area's other languages (regionLanguages.ts).
 *  — then the native side adds the phone's own languages and every other one on the device, and drops whatever is
 *    not installed. So an entry here never costs anything unless its model is really there.
 *
 * Entries are codes ('fr') or, for the main language only, a locale id with the receiver's region ('pt-BR') — the
 * native side runs that exact variant. Mandarin may be 'cmn' (Android) or 'zh' (EiBi): one language.
 */

const base = (id: string) => {
  const b = id.toLowerCase().split(/[-_]/)[0];
  return b === 'cmn' ? 'zh' : b;
};

export interface AutoOrderInput {
  /** EiBi's languages for the frequency, already limited to ones the device can transcribe. */
  eibi: string[];
  /** The receiver area's languages, nearest first (languagesNear) — [0] is the receiver's main language. */
  area: string[];
  /** The user's ★ list, in their order (language codes). */
  preferred: string[];
  /** Is this language's speech model on the device (any variant)? */
  installed: (code: string) => boolean;
  /** The receiver-country variant of a language when THAT variant is installed ('pt' → 'pt-BR'), else null. */
  mainVariant?: (code: string) => string | null;
}

export function autoOrder(o: AutoOrderInput): string[] {
  const out: string[] = [];
  const seen = new Set<string>();
  const add = (id: string) => {
    const b = id ? base(id) : '';
    if (!b || seen.has(b)) return;
    seen.add(b); out.push(id);
  };
  o.eibi.slice(0, 3).forEach(add);
  const main = o.area[0];
  if (main && o.installed(main)) add(o.mainVariant?.(main) || main);
  o.preferred.forEach(add);
  o.area.forEach(add);                 // the area's others (and an uninstalled main language, harmlessly — skipped)
  return out;
}

/** ★★ AT MOST 4 ★ (Stuart, 2026-10-17 night): Apple lets an app KEEP 5 speech languages on the device, and the 5 are the
 *  phone's own language + up to 4 ★ — a ★ language keeps its place, every other download may be tidied away by the
 *  system when unused. The same cap on Android (shared list), where ★ only orders AUTO's allow-list. */
export const PREFERRED_MAX = 4;
export function preferredFull(list: string[]): boolean { return list.length >= PREFERRED_MAX; }

/** ★ list: tapping a language adds it to the END of the preferred list; tapping it again takes it out. A fifth is
 *  refused (the list comes back unchanged — the caller says why). */
export function togglePreferred(list: string[], code: string): string[] {
  const b = base(code);
  if (list.some(c => base(c) === b)) return list.filter(c => base(c) !== b);
  return preferredFull(list) ? list : [...list, code];
}

/** Stored form: 'en,fr,nl' — junk and repeats dropped; a list stored before the cap keeps its first 4. */
export function parsePreferred(s: string | null | undefined): string[] {
  const out: string[] = [];
  for (const c of (s ?? '').split(',').map(x => x.trim().toLowerCase())) {
    if (/^[a-z]{2,3}$/.test(c) && !out.some(x => base(x) === base(c))) out.push(c);
  }
  return out.slice(0, PREFERRED_MAX);
}

/** ★★ THE RECEIVER'S LANGUAGE IS NOT ON THIS DEVICE (Stuart, 2026-10-17: "if I enable translate and transcribe on a
 *  foreign server we detect its location and put a warning triangle next to the AUTO mode and highlight the language
 *  row if the language is not installed"). The receiver's MAIN language — the same one AUTO puts first
 *  — when its model is missing, else null. Only for a KNOWN receiver position (`located`): with none, the phone's own
 *  country stands in for it and a warning about the phone's own language would be noise. A UK receiver with English
 *  on the device → null. */
export function receiverLanguageMissing(mainLang: string | undefined, located: boolean,
                                        installed: (code: string) => boolean): string | null {
  if (!located || !mainLang) return null;
  return installed(mainLang) ? null : mainLang;
}

export type LikelyWhy = 'eibi' | 'receiver' | 'preferred' | 'area' | 'phone';
/** ★★ ANDROID 12/13: ONE LANGUAGE, NOT A DETECTION (Stuart, 2026-10-17: "if we can give some form of transcription to
 *  12/13 then do it"). Android 13 has no language detection and 12 cannot even list what is installed, so AUTO there
 *  means the single most likely language — the first of autoOrder's list that is installed, else the phone's own —
 *  and the list says so honestly ("LIKELY · Portuguese (the SDR's local language)"). On 12 the same pick (no EiBi,
 *  `installed` unknown → everything counts) PRESELECTS the language. `why` names where it came from. */
export function autoLikely(o: AutoOrderInput & { phone: string }): { code: string; why: LikelyWhy } {
  const order = autoOrder(o);
  const b = (id: string) => base(id);
  for (const id of order) {
    if (!o.installed(b(id))) continue;
    const why: LikelyWhy = o.eibi.slice(0, 3).some(c => base(c) === b(id)) ? 'eibi'
      : o.area[0] && base(o.area[0]) === b(id) ? 'receiver'
      : o.preferred.some(c => base(c) === b(id)) ? 'preferred' : 'area';
    return { code: id, why };
  }
  return { code: o.phone, why: 'phone' };
}
