/**
 * textModes.ts — ★ V12 (2026-10-31) THE MFSK KEYBOARD MODES: MFSK16/32, OLIVIA, CONTESTIA and THOR, decoded by a
 * VibeServer (android/app/src/main/cpp/decoders/textmodes.h). ★★ ONE KEY, "MFSK" (Stuart, 2026-10-31: "if it makes it
 * easier for users then add it" — Olivia, Contestia and THOR are multi-tone FSK modes too, so the family's own name
 * covers them): its AUTO runs every common sub-mode of all four side by side and names the one it locks
 * ("OLIVIA 8/250 · 1500 Hz"). The fallback controls under the key (autoModes.ts, RTTY's pattern): FAMILY (AUTO, MFSK,
 * OLIVIA, CONTESTIA, THOR), then MODE (the family's "ANY" and every sub-mode). The choices are the SERVER'S data
 * (vibeserver.json `mfskModes`, AUTO first, each with its group), so nothing here lists the sub-modes.
 * Shared by the app (DecoderClient, ModeSelector, SDRScreen) and the web page (web/client/src/main.ts).
 *
 * Wire: 0x01 text as it decodes ("[OLIVIA 8/250 · 1500 Hz]" on a new lock) · 0x03 | u8 (0 searching, 1 locked) ·
 *       {"type":"textmode_status","data":{"detected":"OLIVIA 8/250","hz":1500}} when it changes.
 */
import { parseGroupedModes, fullModeName, type GroupedMode } from './autoModes';

export type TextFamily = 'mfsk';
export const TEXT_FAMILIES: readonly TextFamily[] = ['mfsk'];
export type TextMode = GroupedMode;

/** The key's legend. */
export const TEXT_FAMILY_KEY: Record<TextFamily, string> = { mfsk: 'MFSK' };

export const TEXT_FAMILY_HINT: Record<TextFamily, string> = {
  mfsk: 'MFSK16/32, OLIVIA, CONTESTIA and THOR: tune USB so the whole signal sits in your passband — for OLIVIA, 1.5 kHz below the published centre (14.0715 MHz for 14.0730). AUTO tries the common sub-modes of all four side by side and names the one it finds; if it cannot lock, pick the family and then the sub-mode below. The first word of an over can go while it locks.',
};

/** `mfskModes` from vibeserver.json, or [] (short ids, labels and groups only; anything else is dropped). */
export function parseTextModes(v: unknown): TextMode[] { return parseGroupedModes(v, 48); }

export interface TextModeStatus { detected: string; hz: number }
export function parseTextModeStatus(d: any): TextModeStatus | null {
  if (!d || typeof d !== 'object') return null;
  const detected = typeof d.detected === 'string' && /^[A-Za-z0-9 /.-]{0,24}$/.test(d.detected) ? d.detected : '';
  const hz = Number.isFinite(d.hz) ? Math.max(0, Math.min(5000, Math.round(d.hz))) : 0;
  return { detected, hz };
}
/** The status line: what is locked and where, or what it is listening for. */
export function textModeStatusText(_fam: TextFamily, mode: string, s: TextModeStatus | null, list: readonly TextMode[] = []): string {
  if (s?.detected) return `${s.detected} · ${s.hz} Hz`;
  return mode === 'auto' ? 'listening — AUTO (MFSK, OLIVIA, CONTESTIA or THOR, from the signal)' : `listening — ${fullModeName(list, mode)}`;
}
export function isTextFamily(x: unknown): x is TextFamily { return x === 'mfsk'; }
