/**
 * feldHell.ts — ★ V12 (2026-10-31) FELD HELL, drawn by a VibeServer (android/app/src/main/cpp/decoders/feld_hell.h). Hell is
 * a facsimile of text: the server paints what it hears into strips and the eye reads them — no character recognition.
 * The words and the strip arithmetic both clients use (the app's DecoderClient / DecoderImageCanvas / ModeSelector, the
 * web page's main.ts), so the two draw the same page and say the same thing.
 *
 * Wire (the WEFAX image path's frames, read against the attached decoder — never sniffed):
 *   0x02 once at the start (a new picture) · 0x01 | u32 line | u32 width | grey bytes (255 paper, 0 ink) — a strip is
 *   HELL_STRIP_ROWS lines (line = strip · 28 + row, the column twice: the upper copy, then the lower), re-sent as it fills
 *   (every character), so a line is drawn at its own place and simply replaces what was there ·
 *   {"type":"hell_status","data":{"hz":980,"signal":1}} when it changes.
 * Not kept by the server's gallery (the MFSK keys' rule: a keyboard mode's over, not a chart); SAVE writes the page shown.
 */

/** Lines in one strip: 14 half-elements a column, printed twice (FeldHellDecoder::kRows × 2). */
export const HELL_STRIP_ROWS = 28;
/** ★ Strips on one page. A page is 96 s of keyed text (8 × 12 s): at 210 × 224 it still fits a phone's box whole and
 *  readable. The next strip starts a fresh page and this one goes to PREV — a roll that grew without end would be shrunk
 *  to fit until the letters were a thread (the picture view fits the whole picture by default). */
export const HELL_PAGE_STRIPS = 8;

export const HELL_HINT = 'FELD HELL (Hellschreiber): drawn, not read — the text is painted as it is heard, a strip at a time, and you read it. Every column is printed twice, one copy above the other, so a slant or a fade that cuts one copy leaves the other whole. Tune USB so the signal is in your passband (the tone is found by itself); each new page puts the last one under PREV.';

export interface HellStatus { hz: number; signal: boolean }
export function parseHellStatus(d: any): HellStatus | null {
  if (!d || typeof d !== 'object') return null;
  const hz = Number.isFinite(d.hz) ? Math.max(0, Math.min(5000, Math.round(d.hz))) : 0;
  return { hz, signal: d.signal === 1 || d.signal === true };
}
/** The status line. */
export function hellStatusText(s: HellStatus | null): string {
  if (s?.signal) return `drawing · ${s.hz} Hz`;
  return s && s.hz ? `listening — last tone ${s.hz} Hz` : 'listening for a Hell tone';
}

/**
 * Where a line lands on the page being drawn. The server numbers lines from 0 for the whole session; the page starts at
 * the strip that was arriving when it was opened (or cleared), and a strip past HELL_PAGE_STRIPS starts the next page.
 * A re-sent line keeps its place (the strip filling in).
 */
export class HellPager {
  private base = -1;
  /** Forget the page: the next line starts a new one (0x02, CLR, a fresh box). */
  reset() { this.base = -1; }
  /** `y` = the row on the current page; `newPage` = this line begins a page (bank the old one first). */
  place(line: number): { y: number; newPage: boolean } {
    const stripStart = line - (line % HELL_STRIP_ROWS);
    let newPage = false;
    if (this.base < 0 || line < this.base || line - this.base >= HELL_PAGE_STRIPS * HELL_STRIP_ROWS) {
      this.base = stripStart; newPage = true;
    }
    return { y: line - this.base, newPage };
  }
}
