/**
 * textModes.ts — ★ V12 (2026-10-31) THE MFSK KEYBOARD MODES: MFSK16/32, OLIVIA (with CONTESTIA) and THOR, decoded by a
 * VibeServer (android/app/src/main/cpp/decoders/textmodes.h). One key a family; each family's choices are the SERVER'S
 * data (vibeserver.json `mfskModes` / `oliviaModes` / `thorModes`, AUTO first), so nothing here lists the sub-modes.
 * Shared by the app (DecoderClient, ModeSelector, SDRScreen) and the web page (web/client/src/main.ts).
 *
 * Wire: 0x01 text as it decodes ("[OLIVIA 8/250 · 1500 Hz]" on a new lock) · 0x03 | u8 (0 searching, 1 locked) ·
 *       {"type":"textmode_status","data":{"detected":"OLIVIA 8/250","hz":1500}} when it changes.
 */
export type TextFamily = 'mfsk' | 'olivia' | 'thor';
export const TEXT_FAMILIES: readonly TextFamily[] = ['mfsk', 'olivia', 'thor'];
export interface TextMode { id: string; label: string }

/** The key's legend. */
export const TEXT_FAMILY_KEY: Record<TextFamily, string> = { mfsk: 'MFSK', olivia: 'OLIVIA', thor: 'THOR' };

export const TEXT_FAMILY_HINT: Record<TextFamily, string> = {
  mfsk: 'MFSK16 and MFSK32: tune so the signal sits in your USB passband (any audio offset). AUTO tries both speeds and names the one it finds; pick one only if AUTO cannot lock.',
  olivia: 'OLIVIA and CONTESTIA: tune the dial 1.5 kHz below the published centre (e.g. 14.0715 MHz USB for 14.0730), or anywhere the whole signal fits your passband. AUTO tries the common tones/bandwidths of both and names the one it finds.',
  thor: 'THOR: tune so the signal sits in your USB passband. AUTO tries THOR 4, 5, 8, 11, 16 and 22 and names the one it finds; the first word of an over can go while it locks.',
};

/** `<family>Modes` from vibeserver.json, or [] (short ids and labels only; anything else is dropped). */
export function parseTextModes(v: unknown): TextMode[] {
  if (!Array.isArray(v)) return [];
  const out: TextMode[] = [];
  for (const m of v) {
    const id = typeof m?.id === 'string' ? m.id : '';
    const label = typeof m?.label === 'string' ? m.label : '';
    if (/^[a-z0-9-]{1,24}$/.test(id) && /^[A-Za-z0-9 /.-]{1,24}$/.test(label)) out.push({ id, label });
    if (out.length >= 32) break;
  }
  return out;
}

export interface TextModeStatus { detected: string; hz: number }
export function parseTextModeStatus(d: any): TextModeStatus | null {
  if (!d || typeof d !== 'object') return null;
  const detected = typeof d.detected === 'string' && /^[A-Za-z0-9 /.-]{0,24}$/.test(d.detected) ? d.detected : '';
  const hz = Number.isFinite(d.hz) ? Math.max(0, Math.min(5000, Math.round(d.hz))) : 0;
  return { detected, hz };
}
/** The status line: what is locked and where, or what it is listening for. */
export function textModeStatusText(fam: TextFamily, mode: string, s: TextModeStatus | null): string {
  if (s?.detected) return `${s.detected} · ${s.hz} Hz`;
  return mode === 'auto' ? `listening — AUTO (${TEXT_FAMILY_KEY[fam]} sub-mode from the signal)` : 'listening';
}
export function isTextFamily(x: unknown): x is TextFamily { return x === 'mfsk' || x === 'olivia' || x === 'thor'; }
