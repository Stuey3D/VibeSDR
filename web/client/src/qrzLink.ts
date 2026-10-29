/* qrzLink.ts — ★★ V12 (2026-10-29) A CALLSIGN OPENS ITS QRZ.COM PAGE, in a new tab (the web page's way of opening another
 * site, as the SIGNAL ID WIKI key does). Every spotted or decoded callsign — the CW / PSK list and spots, JTTY, the
 * digital spots — is drawn as a link by `callHtml`; `wireQrz` stops its click reaching the row, whose own click TUNES.
 * The app's CwView CallLink is the same rule. ★ A link only: nothing is fetched, scraped or stored from QRZ. */
import { qrzCallUrl } from '../../../src/utils/cwFormat';

const escAttr = (s: string) => s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

/** The callsign as a QRZ link (its text escaped by `esc`), or just the escaped text when it is not a callsign. */
export function callHtml(call: string, esc: (s: string) => string, text?: string): string {
  const url = qrzCallUrl(call);
  const shown = esc(text ?? call);
  if (!url) return shown;
  return `<a class="qrz" href="${escAttr(url)}" target="_blank" rel="noopener noreferrer" title="${escAttr(call)} on QRZ.com">${shown}</a>`;
}

/** ★ A link's click is the link's: it must not also reach the row (a tap on the row tunes to it). */
export function wireQrz(root: HTMLElement): void {
  for (const a of root.querySelectorAll<HTMLAnchorElement>('a.qrz')) a.addEventListener('click', (e) => e.stopPropagation());
}
