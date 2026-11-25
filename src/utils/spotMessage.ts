/**
 * ★★ V12 beta94 A SPOT'S MESSAGE (Stuart 11-25, comparing UberSDR's Digital Spots "Message" column — "CQ IW2FAC JN45",
 * "F5MXH N2XSG RR73", "ZL1RKO W9DHI -16"): the decoded text a station put on the air, read off a digital_spot frame and
 * drawn on the EXPANDED row's own line, under the detail line. Pure, so scripts/test_spot_message_app.ts can run it.
 *
 *  - A VibeServer (beta94+) sends it as `msg` for the slotted modes (FT8, FT4, FT2, JS8, WSPR "K1ABC FN42 37", FST4W,
 *    Q65 …); never for a skimmer's spot (CW / RTTY read theirs from band_skim). An older VibeServer sends none: no line.
 *  - UberSDR sends a message too; its key was never confirmed, so the plausible spellings are all read.
 *  - OFF THE AIR = UNTRUSTED: cleaned as every server string is (utils/safeText) and bounded — a VibeServer cuts at 48
 *    characters, this caps at SPOT_MSG_MAX whatever the server did.
 * Display only — nothing here is stored or reported.
 */
import { cleanText } from './safeText';

export const SPOT_MSG_MAX = 80;

/** The message a spot frame's `data` carries, cleaned and bounded; undefined when there is none. */
export function spotMsgOf(d: Record<string, unknown> | null | undefined): string | undefined {
  if (!d || typeof d !== 'object') return undefined;
  for (const k of ['msg', 'message', 'text']) {
    const v = d[k];
    if (typeof v === 'string') { const s = cleanText(v, SPOT_MSG_MAX); if (s) return s; }
  }
  return undefined;
}

/** The expanded row's DETAIL line: the parts present, joined with · — a missing piece simply drops out (a report or a 73
 *  carries no locator, so no gap where the grid would have been). The message is NOT here: it has its own line. */
export function spotDetailLine(parts: (string | undefined | null | false)[]): string {
  return parts.filter(Boolean).join(' · ');
}

/** The expanded row's MESSAGE line: '' (no line at all) when the spot has none, or when a band skimmer's own text for
 *  the signal is shown instead (CW / RTTY — the skim text wins, as on the web). */
export function spotMsgLine(msg: string | undefined, skimText?: string): string {
  if (skimText) return '';
  return typeof msg === 'string' ? msg.trim() : '';
}
