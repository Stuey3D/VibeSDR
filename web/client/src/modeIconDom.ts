/**
 * ★ V12 MODE ICONS on the web (Stuart, 2026-10-28) — the category glyph before a decoder / whole-radio mode's label:
 * an aircraft for ADS-B, a ship for AIS / DSC / NAVTEX, a Morse key for CW… The drawings are the app's
 * (src/utils/modeIcons.ts — one table for both clients); this only puts them into buttons. CSS: `.modeIco` in index.html
 * (1em, currentColor — so a lit button lights its glyph too).
 */
import { modeIconFor, modeIconSvg } from '../../../src/utils/modeIcons';

/** Put `id`'s glyph at the front of `el` (once — a second call is a no-op). Nothing for an id without one. */
export function addModeIcon(el: HTMLElement, id: string): void {
  if (el.firstElementChild?.classList.contains('modeIco')) return;
  const n = modeIconFor(id);
  if (n) el.insertAdjacentHTML('afterbegin', modeIconSvg(n));
}

/** A label with its glyph, for a button built in code: sets the text AND the icon (textContent alone would drop it). */
export function setModeLabel(el: HTMLElement, id: string, label: string): void {
  el.textContent = label;
  addModeIcon(el, id);
}
