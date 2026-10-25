// galleryOrigin.ts — WHO a gallery item came from, in words: ONE rule for the web client and the app (V12, 2026-10-25).
// ★ Until ISM, an item was either background decoding's or a listener's. An ISM day's log and summary card are written
//   by BOTH ("one file per UTC day per band" — vibe_ism_gallery.h): the server marks such an item "both", and it is
//   said as both, never filed under one of them.
export interface GalleryOriginItem { o: string; p?: string; wl?: boolean }

/** The list's tag: "BACKGROUND", "BACKGROUND · LISTENER ON", "LISTENER", "BACKGROUND + LISTENER". */
export function galleryOriginTag(it: GalleryOriginItem): string {
  if (it.o === 'both') return 'BACKGROUND + LISTENER';
  return it.o === 'background' ? (it.wl ? 'BACKGROUND · LISTENER ON' : 'BACKGROUND') : 'LISTENER';
}
/** The open item's line: "background decoding (Profile 1)", "a listener's decoder", "background decoding and listeners". */
export function galleryOriginWords(it: GalleryOriginItem): string {
  if (it.o === 'both') return 'background decoding and listeners';
  return it.o === 'background' ? `background decoding${it.p ? ` (${it.p})` : ''}` : "a listener's decoder";
}
