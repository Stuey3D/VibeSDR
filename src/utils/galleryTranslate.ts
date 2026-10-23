// galleryTranslate.ts — TRANSLATE in the gallery's text viewer (V12, 2026-10-23): which lines of a NAVTEX message (or an
// RTTY run) go to the translator, what language they are in, and how the translation is put back together.
// ★ One rule for the app (src/components/GalleryOverlay.tsx: Apple Translation on iOS, ML Kit on Android — the same
//   on-device layer as TRANSCRIBE's captions) and the web client (web/client/src/main.ts: the browser's own Translator
//   and LanguageDetector, where it has them). The translator itself is passed in, so this stays pure:
//   scripts/test_gallery_translate.ts.
// ★★ THE LAYOUT SURVIVES: line by line. Never sent: our own header lines (everything before the first blank line of a
//    "NAVTEX · …" / "RTTY · …" file), ZCZC / NNNN, a "[… signal lost 12 s …]" / "[start lost]" marker, a line that is
//    mostly figures (times, positions, "NR. 4039.2"), punctuation or lost characters ('_'), and a line of fewer than
//    two words. Those stay exactly as received, in place.
// ★ An FT8 summary is our own English text — nothing in it is translated.
import { navtexIsoLanguages, navtexLanguages } from './navtexStations.generated.ts';

export interface GalleryTrLine { text: string; translate: boolean }

const isUpperLetter = (c: string) => c !== c.toLowerCase() && c === c.toUpperCase();

/** One line of a message: worth sending to the translator? */
export function galleryLineTranslatable(line: string): boolean {
  const t = line.trim();
  if (!t) return false;
  if (/^(ZCZC|NNNN)\b/.test(t)) return false;
  if (/^\[[^\]]*\]$/.test(t)) return false;                          // [… signal lost 12 s …], [start lost], [end lost]
  const solid = t.replace(/\s+/g, '');
  const letters = (t.match(/\p{L}/gu) ?? []).length;
  if (letters < solid.length * 0.6) return false;                    // figures, positions, punctuation, '_' garbage
  return t.split(/[^\p{L}]+/u).filter((w) => w.length >= 2).length >= 2;
}

/** The item's lines, each marked: send it, or keep it as received. `kind` is the gallery's ("navtex", "rtty", "ft8"). */
export function galleryTranslateLines(text: string, kind: string): GalleryTrLine[] {
  const lines = text.replace(/\r\n?/g, '\n').split('\n');
  if (kind === 'ft8') return lines.map((l) => ({ text: l, translate: false }));
  // ★ Our own header ("NAVTEX · B · Oostende … / Received … / 489.5 kHz USB · background decoding") is English we wrote.
  let body = 0;
  if (/^(NAVTEX|RTTY) · /.test(lines[0] ?? '')) {
    const blank = lines.findIndex((l) => !l.trim());
    body = blank < 0 ? lines.length : blank + 1;
  }
  return lines.map((l, i) => ({ text: l, translate: i >= body && galleryLineTranslatable(l) }));
}

/** The text the translator would see — for language detection. */
export function galleryTranslatableText(lines: GalleryTrLine[]): string {
  return lines.filter((l) => l.translate).map((l) => l.text.trim()).join('\n');
}

/** "🇮🇹" anywhere in a title (the NAVTEX station's flag) → "IT"; '' when there is none. */
export function galleryTitleIso(title: string): string {
  const m = /([\u{1F1E6}-\u{1F1FF}])([\u{1F1E6}-\u{1F1FF}])/u.exec(title || '');
  if (!m) return '';
  return String.fromCharCode(65 + (m[1].codePointAt(0)! - 0x1f1e6), 65 + (m[2].codePointAt(0)! - 0x1f1e6));
}

/** The station's own language, from the title's flag ('it' for 🇮🇹) — the hint, '' when there is none. */
export function galleryHintLanguage(title: string): string {
  const iso = galleryTitleIso(title);
  return iso ? navtexIsoLanguages(iso)[0] ?? '' : '';
}

/** The text's language by the NAVTEX stopword lists (en nl de fr es pt it no da sv pl), with the station's language
 *  deciding between close ones (Norwegian/Danish) and standing in when the text is too short to say. '' = unknown.
 *  ★ A platform detector (Apple NaturalLanguage, the browser's LanguageDetector) is asked first where there is one. */
export function galleryGuessLanguage(text: string, hint: string): string {
  const langs = navtexLanguages(text);
  if (langs.length === 1) return langs[0];
  if (langs.length > 1) return hint && langs.includes(hint) ? hint : '';
  return hint;
}

const base = (id: string) => (id || '').toLowerCase().split(/[-_]/)[0];
/** Same language, any variant ('en' and 'en-GB'). */
export function gallerySameLanguage(a: string, b: string): boolean { return !!a && !!b && base(a) === base(b); }

/** Should TRANSLATE be drawn? Only with something to send, a known source, and a source that is not the target. */
export function galleryCanTranslate(lines: GalleryTrLine[], source: string, target: string): boolean {
  return lines.some((l) => l.translate) && !!source && !!target && !gallerySameLanguage(source, target);
}

/** A teleprinter's capitals read as acronyms to a translator: an all-capitals line goes in lower case and comes back
 *  in capitals, so the translation looks like the message. A mixed-case line (RTTY from a PC) goes as it is. */
export function galleryAllCaps(line: string): boolean {
  const ls = [...line].filter((c) => c.toLowerCase() !== c.toUpperCase());
  return ls.length > 0 && ls.every(isUpperLetter);
}

/** Translate the lines that are marked, one at a time (the translator is the platform's); a line the translator could
 *  not do stays as received. The indentation is kept. `failed` counts those lines; `reason` is the translator's last. */
export async function galleryTranslate(lines: GalleryTrLine[], target: string,
  tr: (text: string) => Promise<{ ok: boolean; text?: string; reason?: string }>):
  Promise<{ text: string; done: number; failed: number; reason: string }> {
  const out: string[] = [];
  let done = 0, failed = 0, reason = '';
  for (const l of lines) {
    if (!l.translate) { out.push(l.text); continue; }
    const lead = /^\s*/.exec(l.text)![0];
    const caps = galleryAllCaps(l.text);
    const src = l.text.trim();
    let r: { ok: boolean; text?: string; reason?: string };
    try { r = await tr(caps ? src.toLowerCase() : src); } catch (e: any) { r = { ok: false, reason: String(e?.message ?? e) }; }
    const t = r.ok ? (r.text ?? '').trim() : '';
    if (t) {
      done++;
      let up = t;
      try { up = caps ? t.toLocaleUpperCase(target || undefined) : t; } catch { up = caps ? t.toUpperCase() : t; }
      out.push(lead + up);
    } else { failed++; if (r.reason) reason = r.reason; out.push(l.text); }
  }
  return { text: out.join('\n'), done, failed, reason };
}
