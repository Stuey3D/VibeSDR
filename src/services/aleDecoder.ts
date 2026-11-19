/**
 * aleDecoder.ts — ★ V12 2026-11-19 2G HF ALE (MIL-STD-188-141 Appendix A): the words both clients use (the app's
 * ModeSelector / DecoderClient / SDRScreen, the web's main.ts). One copy, so the hint and the status line read the same
 * everywhere. The decoder is a VibeServer's own (android/app/src/main/cpp/decoders/ale_decoder.h, docs/v12/HF-DATA.md).
 *
 * Wire: 0x01 text — one line a transmission ("[ALE] 21:04:12  call  K1ABC → W1XYZ") · 0x03 | u8 (0 listening, 1 a
 *       transmission being read) · {"type":"ale_status","data":{"words":n,"offset":hz,"last":"TIS G4X"}} on change.
 * ⚖ What a line may say is the SERVER's decision (addresses always; an AMD message's text on the amateur bands, or to
 *   the owner alone with their opt-in) — nothing here filters or asks for more.
 */

/** ★ The key's hint: what it reads, where, and that ALE nets hop. */
export const ALE_HINT = 'ALE (2G, MIL-STD-188-141): the link-setting bursts of HF nets — soundings, calls and replies, one line each (“call  K1ABC → W1XYZ”). Tune an ALE channel in USB with the dial on the channel (HFLink amateur channels: 3.596, 7.102, 10.145, 14.109, 18.106 MHz). Nets scan many channels, so a station is heard only while it is on yours.';

export interface AleStatus { words: number; offset: number; last: string }
export function parseAleStatus(d: any): AleStatus | null {
  if (!d || typeof d !== 'object') return null;
  const words = Number.isFinite(d.words) ? Math.max(0, Math.min(999, Math.round(d.words))) : 0;
  const offset = Number.isFinite(d.offset) ? Math.max(-200, Math.min(200, Math.round(d.offset))) : 0;
  const last = typeof d.last === 'string' && /^[A-Z]{2,4} [ -~]{3}$/.test(d.last) ? d.last : '';
  return { words, offset, last };
}
/** The status line: listening, or the transmission being read. */
export function aleStateText(st: number, s: AleStatus | null = null): string {
  if (!st) return 'listening for ALE';
  return s && s.words ? `reading ALE… ${s.words} word${s.words === 1 ? '' : 's'}${s.last ? ` (${s.last})` : ''}` : 'reading ALE…';
}
