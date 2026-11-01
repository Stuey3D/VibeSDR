// galleryTranslatePlan.ts — the app's gallery TRANSLATE, in one place (V12, 2026-11-01): what GalleryOverlay's key does,
// so the NAVTEX map's cards (MapOverlay kind 'navtex') use the SAME translator, the same rule and the same words — never a
// second implementation. The phone's own on-device translator (Apple Translation / ML Kit, services/transcriber), the
// line rule of utils/galleryTranslate (ZCZC, figures, positions and garbage stay as received).
import { galleryCanTranslate, galleryGuessLanguage, galleryHintLanguage, galleryTranslatableText, galleryTranslate,
         galleryTranslateLines, type GalleryTrLine } from '../utils/galleryTranslate';
import { detectTextLanguage, langName, textTranslateInfo, translateText } from './transcriber';

export interface GalleryTrPlan { lines: GalleryTrLine[]; source: string; target: string }
/** Whether this text can be translated here, and how — null when the key must not be drawn (AGENTS.md: no dead key). */
export async function planGalleryTranslate(text: string, kind: string, title: string, translateTo: string): Promise<GalleryTrPlan | null> {
  const info = await textTranslateInfo();
  if (!info.available) return null;
  const target = translateTo || info.deviceLanguage || '';
  const lines = galleryTranslateLines(text, kind);
  const body = galleryTranslatableText(lines);
  if (!body || !target) return null;
  const hint = galleryHintLanguage(title);
  const guess = galleryGuessLanguage(body, hint);
  // ★ iOS: NaturalLanguage reads it, with the station's language and the stopword guess as its priors.
  const source = (info.detect ? await detectTextLanguage(body, [hint, guess].filter(Boolean)) : '') || guess;
  return galleryCanTranslate(lines, source, target) ? { lines, source, target } : null;
}
/** Run a plan: the translated text, and the note to show under it. */
export async function runGalleryTranslate(plan: GalleryTrPlan): Promise<{ ok: boolean; text: string; note: string }> {
  const r = await galleryTranslate(plan.lines, plan.target, (t) => translateText(t, plan.source, plan.target));
  if (!r.done) return { ok: false, text: '', note: r.reason || 'The translation did not work on this phone.' };
  return { ok: true, text: r.text, note: r.failed ? `${r.failed} line${r.failed === 1 ? '' : 's'} left as received${r.reason ? ` (${r.reason})` : ''}` : '' };
}
/** "MACHINE TRANSLATION · French → English · on this device" — the gallery viewer's own words. */
export function galleryTranslateBanner(plan: GalleryTrPlan): string {
  return `MACHINE TRANSLATION · ${langName(plan.source)} → ${langName(plan.target)} · on this device`;
}
