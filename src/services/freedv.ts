/**
 * ★★★ V12 FreeDV DIGITAL VOICE — ONE RULE, SEVERAL READERS (the app and the VibeServer web client import this file; the R36S
 * UI mirrors it in handheld-ui/src/freedv.h). docs/v12/FREEDV.md.
 *
 * FreeDV is a MODE beside USB and LSB: the server demodulates SSB, runs the FreeDV modem and the Codec 2 vocoder (the system's
 * libcodec2, loaded at run time) and plays the decoded speech instead of the SSB audio. It is SILENT until there is speech;
 * the VTS line says why ("FreeDV · searching") and, once in sync, what is on air ("FreeDV 700D · 6 dB · G4ABC").
 *
 * ★★ OFFERED ONLY WHERE THE SERVER LISTS IT (vibeserver.json `modes` has "freedv", `freedvSubs` the sub-modes its library
 *    runs). A server without libcodec2 never lists it, and no other backend has it — a FreeDV key there would be dead.
 * ★★ THE SIDEBAND IS THE SERVER'S: FreeDV keeps the SSB convention (LSB below 10 MHz, USB from 10 MHz up) and the server
 *    applies it; its report says which ("sb"). freedvPassband draws it; a client never decides it.
 * ★ AUTO finds which of 700D / 700E / 1600 is on air; a manual choice underneath is the fallback when AUTO cannot settle
 *   ({"type":"freedv_sub","sub":"700D"}; "auto" goes back). The report says which is chosen ("sub") and which is heard ("mode").
 */

export const FREEDV_SUBS = ['700D', '700E', '1600'] as const;
export type FreeDvSub = typeof FREEDV_SUBS[number];
export type FreeDvChoice = 'auto' | FreeDvSub;

/** Key legend, spoken name and hint. */
export const FREEDV_LABEL = 'FreeDV';
export const FREEDV_NAME = 'FreeDV digital voice';
export const FREEDV_HINT = 'Digital voice on the HF amateur bands. Silent until a FreeDV signal is decoded; AUTO finds 700D, 700E or 1600.';

/** The server's {"type":"freedv"} report (vibe_freedv.h publish). */
export interface FreeDvState {
  sub: FreeDvChoice;            // what the listener (or the room) chose
  sb: 'usb' | 'lsb';            // the sideband the server demodulates
  sync: boolean;                // decoding speech now
  mode: FreeDvSub | '';         // the sub-mode in sync ('' while searching)
  snr?: number;                 // dB, while in sync
  text?: string;                // the station's text channel (callsign etc.), once received twice the same
}

const isSub = (s: unknown): s is FreeDvSub => typeof s === 'string' && (FREEDV_SUBS as readonly string[]).includes(s);

/** Parse a report; null when it is not one. Unknown fields are ignored. Text is clipped and kept printable. */
export function parseFreeDv(msg: unknown): FreeDvState | null {
  if (!msg || typeof msg !== 'object') return null;
  const m = msg as Record<string, unknown>;
  if (m.type !== 'freedv') return null;
  const sub: FreeDvChoice = isSub(m.sub) ? m.sub : 'auto';
  const sync = m.state === 'sync';
  const st: FreeDvState = { sub, sb: m.sb === 'lsb' ? 'lsb' : 'usb', sync, mode: sync && isSub(m.mode) ? m.mode : '' };
  if (sync && typeof m.snr === 'number' && Number.isFinite(m.snr)) st.snr = Math.round(m.snr);
  if (typeof m.text === 'string') {
    const t = m.text.replace(/[^\x20-\x7e]/g, '').trim().slice(0, 40);
    if (t) st.text = t;
  }
  return st;
}

/** The sub-modes THIS server offers (vibeserver.json `freedvSubs`), in key order; [] = FreeDV not offered. */
export function freedvOffered(j: { modes?: unknown; freedvSubs?: unknown } | null | undefined): FreeDvSub[] {
  if (!j || !Array.isArray(j.modes) || !j.modes.includes('freedv')) return [];
  const subs = Array.isArray(j.freedvSubs) ? j.freedvSubs : [];
  return FREEDV_SUBS.filter((s) => subs.includes(s));
}

/** ★ The VTS line — short, the R36S screen is small. Searching names the choice when it is not AUTO, so a listener who
 *  forced 1600 sees why nothing comes. */
export function freedvVts(s: FreeDvState | null): string {
  if (!s) return 'FreeDV · searching';
  if (!s.sync) return s.sub === 'auto' ? 'FreeDV · searching' : `FreeDV ${s.sub} · searching`;
  const parts = [`FreeDV ${s.mode}`];
  if (s.snr !== undefined) parts.push(`${s.snr} dB`);
  if (s.text) parts.push(s.text);
  return parts.join(' · ');
}

/** The passband to draw (Hz from the dial): the SSB side the server demodulates, 2.7 kHz wide. */
export function freedvPassband(sb: 'usb' | 'lsb'): [number, number] {
  return sb === 'lsb' ? [-2700, -50] : [50, 2700];
}
/** ★ The server's rule, for the moment before its first report (a fresh tune): LSB below 10 MHz. The report wins. */
export function freedvSidebandAt(hz: number): 'usb' | 'lsb' {
  return hz > 0 && hz < 10e6 ? 'lsb' : 'usb';
}

/** The manual-choice message. */
export function freedvSubMessage(sub: FreeDvChoice | null): { type: 'freedv_sub'; sub?: string } {
  return sub ? { type: 'freedv_sub', sub } : { type: 'freedv_sub' };
}
