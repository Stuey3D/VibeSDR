/**
 * ★★★ beta92 A LIST THE PAD WALKS, AND IS NEVER TRAPPED IN (Stuart, 2026-11-23: "make the spots navigatable by controller
 * but make it so that you dont have to scroll a list of hundreds to get to the header buttons or to the control deck").
 * The decoder box's #spotList (main.ts renderSpotsRows); the app's DecoderPanel does the same (src/utils/spotsNav.ts).
 *
 *  • every row is ONE stop (role=button, tabindex): enter = its click (tune); the callsign's QRZ link stays a click
 *  • the list carries [data-navlist="<its header>"] (index.html): BACK from any row → the header's first key in one press,
 *    BACK again lets go of the box; ↑ on the first row → the header; ↓ from the header → the TOP row (gamepad.ts)
 *  • the list is rebuilt on every spot: padListRebuild keeps the highlight on the SAME ROW BY KEY (data-ra) — a burst
 *    that prepends rows never moves it; a row that has gone pins it to the same place.
 */
import { padRehighlight } from './gamepad';

/** A row as one pad / keyboard stop. Full width, so straight below whatever is above it ([data-navwide]). */
export function padListRow(row: HTMLElement): void {
  row.setAttribute('role', 'button');
  row.tabIndex = 0;
  row.setAttribute('data-navwide', '');
  // ★ The QRZ link is not a second stop per row ([data-gpskip]); a click or Tab still reaches it.
  for (const a of row.querySelectorAll('a.qrz')) a.setAttribute('data-gpskip', '');
  // ★ A row holding real focus (clicked, or reached with Tab) keeps the browser's Enter (gamepad.ts onKey) — a div does
  //   not press itself on Enter, so it is told to (the NDB list's rule). Never from inside the QRZ link.
  row.onkeydown = (e) => {
    if ((e.key === 'Enter' || e.key === ' ') && e.target === row) { e.preventDefault(); row.click(); }
  };
}

/** Rebuild `host`'s rows (keyed by data-ra) with the pad's highlight kept on its row. */
export function padListRebuild(host: HTMLElement, rebuild: () => void): void {
  const lit = host.querySelector<HTMLElement>(':scope > .gpFocus');
  const key = lit?.dataset.ra, at = lit ? [...host.children].indexOf(lit) : -1;
  rebuild();
  if (!key) return;
  const rows = [...host.children] as HTMLElement[];
  const again = rows.find((r) => r.dataset.ra === key) ?? rows[Math.min(at, rows.length - 1)];
  if (again) padRehighlight(again);
}
