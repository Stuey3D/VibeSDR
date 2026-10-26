// spotTime.ts — the time a SPOT is shown with, in the web client and the app (V12, 2026-10-26).
//
// ★★ HH:MM:SS UTC, THE SLOT'S START (M7EIW via Stuart: "Showing the seconds lets me know more precisely which
//    transmission block a decode was received like in WSJTX"). A slotted mode's spot is stamped by the server with the
//    START of the slot it was heard in — WSJT-X's time column — not the moment its decode finished: UberSDR does so
//    (its spots are on :00/:15/:30/:45 for FT8), and a VibeServer does since V12 (digi::Spot::slotUtc → `timestamp`).
//    So the time shown is the stamp, to the second: FT8 16:46:30, FT4 16:46:37 (7.5 s slots: :37.5 shows as :37),
//    FT2 16:46:33 (3.75 s), JS8 by its submode's slot, WSPR on the even minute. CW and PSK spots have no slots: their
//    stamp is when the call was heard, shown the same way. An older VibeServer (before V12) stamps the decode's end —
//    a second or so after the slot's last symbol — and shows that.
// ★ One helper for every list, card and map (web/client/src/main.ts, src/components/DecoderPanel.tsx, CwView.tsx,
//   MapOverlay.tsx), so they can never disagree.

const p2 = (n: number) => String(n).padStart(2, '0');

/** "HH:MM:SS" (UTC) of an epoch-ms stamp; "" when it is not a time. */
export function fmtSpotTime(t: number): string {
  if (!Number.isFinite(t)) return '';
  const d = new Date(t);
  if (isNaN(d.getTime())) return '';
  return `${p2(d.getUTCHours())}:${p2(d.getUTCMinutes())}:${p2(d.getUTCSeconds())}`;
}

/** The same, as a JS expression's source — for the maps' WebView/popup scripts, which cannot import. */
export const FMT_SPOT_TIME_JS =
  "function fmtSpotTime(t){var d=new Date(t);if(isNaN(d.getTime()))return'';var p=function(n){return String(n).padStart(2,'0');};" +
  "return p(d.getUTCHours())+':'+p(d.getUTCMinutes())+':'+p(d.getUTCSeconds());}";
