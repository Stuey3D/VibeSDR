/**
 * acTags.ts — ★★★ beta96 THE AIRCRAFT TAGS (Stuart 11-27: "mil needs to be a little box next to the flight number, same
 * for emergency squawk too, colour coded") — one source for the web lists (the live ADS-B list, the AIR & SEA list) and
 * the app's port: MIL in the military tone; an emergency squawk in the alert red — 7700 EMERG, 7600 RADIO, 7500 HIJACK.
 * Calm: no blinking. The box is the live ADS-B list's own .tag (index.html #adsbList, tracks.ts .trkRow .tag).
 */
export type AcTag = 'mil' | 'emerg' | 'radio' | 'hijack';
export const AC_TAG_WORDS: Record<AcTag, string> = { mil: 'MIL', emerg: 'EMERG', radio: 'RADIO', hijack: 'HIJACK' };
/** The tags an aircraft wears: its emergency first (a squawk, or the ADS-B emergency status), then MIL. */
export function acTagsFor(o: { mil?: boolean; squawk?: string | number | null; emergency?: string | null }): AcTag[] {
  const sq = String(o.squawk ?? '').replace(/[^0-7]/g, '');
  const e = String(o.emergency ?? '').toLowerCase();
  const out: AcTag[] = [];
  if (sq === '7500' || e === 'unlawful') out.push('hijack');
  else if (sq === '7600' || e === 'nordo') out.push('radio');
  else if (sq === '7700' || (e && e !== 'none')) out.push('emerg');
  if (o.mil) out.push('mil');
  return out;
}
export function acTagHtml(t: AcTag): string {
  return `<span class="tag ${t === 'mil' ? 'mil' : 'em'}" data-tag="${t}">${AC_TAG_WORDS[t]}</span>`;
}
