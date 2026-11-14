/* ★★ V12 THE RTTY SKIMMER in the web client (docs/v12/RTTY-SKIMMER.md, 2026-11-14) — the pieces main.ts draws with:
 *  the row's text before its settings are found, a short settings label for the row, and each signal's HISTORY (the server
 *  sends a row's last 160 characters; the card shows the whole message, joined from those windows as they arrive). */

/** What a row says until its parameters are found (Stuart's words). */
export const RTTY_WAITING = 'RTTY detected — waiting for parameter detection';

/** The row's short settings ("45/170 R") — the full words are in the text and at the top of the card. */
export function rttyShort(r: { set: boolean; shift: number; baud: number; reverse: boolean } | undefined): string {
  if (!r || !r.set) return '…';
  const b = r.baud < 47 ? '45' : r.baud < 60 ? '50' : '75';
  return `${b}/${Math.round(r.shift)}${r.reverse ? ' R' : ''}`;
}

/** ★ Join a new window of a signal's text onto what is held: the window's start overlaps the history's end (the server
 *  sends the LAST 160 characters each time). The longest overlap wins; none at all (a gap — the list skipped past more
 *  than a window, or under 8 characters in common) is marked with " … ". Capped at `cap` characters, oldest dropped. */
export function rttyJoin(hist: string, win: string, cap = 8000): string {
  if (!win) return hist;
  if (!hist) return win.slice(-cap);
  if (hist.endsWith(win)) return hist;
  const max = Math.min(hist.length, win.length), min = Math.min(8, win.length);   // ★ a short overlap is chance
  for (let k = max; k >= min; k--) {
    if (hist.endsWith(win.slice(0, k))) {
      const out = hist + win.slice(k);
      return out.length > cap ? out.slice(out.length - cap) : out;
    }
  }
  const out = hist + ' … ' + win;
  return out.length > cap ? out.slice(out.length - cap) : out;
}
