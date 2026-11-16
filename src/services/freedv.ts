/**
 * ★★★ V12 FreeDV DIGITAL VOICE — ONE RULE, SEVERAL READERS (the app and the VibeServer web client import this file; the R36S
 * UI mirrors it in handheld-ui/src/freedv.h). docs/v12/FREEDV.md.
 *
 * FreeDV is a MODE beside USB and LSB: the server demodulates SSB, runs the FreeDV modem and the vocoder — RADE V1 (FreeDV's
 * own receiver, compiled into the server) or 700D / 700E / 1600 (the system's libcodec2, loaded at run time) — and plays the
 * decoded speech instead of the SSB audio. It is SILENT until there is speech; the VTS line says why ("FreeDV · searching")
 * and, once in sync, what is on air ("FreeDV 700D · 6 dB · G4ABC", "FreeDV RADE · 4 dB").
 * ★★ RADE's callsign comes in the END-OF-OVER frame, so it names the station that has JUST STOPPED: the report carries it as
 *    "last" and the VTS shows it only while searching ("FreeDV · last G4ABC", in place of "searching") — never beside a station that is
 *    talking now, which may be the other side of the QSO.
 *
 * ★★ OFFERED ONLY WHERE THE SERVER LISTS IT (vibeserver.json `modes` has "freedv", `freedvSubs` the sub-modes it runs —
 *    RADE where it is built in and the owner has not switched it off, the others where libcodec2 loaded). A server with
 *    neither never lists it, and no other backend has it — a FreeDV key there would be dead.
 * ★★ THE SIDEBAND IS THE SERVER'S: FreeDV keeps the SSB convention (LSB below 10 MHz, USB from 10 MHz up) and the server
 *    applies it; its report says which ("sb"). freedvPassband draws it; a client never decides it.
 *    ★ A listener may OVERRIDE it (Stuart 2026-11-16): SIDEBAND AUTO / USB / LSB under the FreeDV controls, beside the
 *    sub-mode choice ({"type":"freedv_sub","sb":"lsb"}; "auto" goes back to the convention). The server still applies it;
 *    the report says what was chosen ("sbSel") and what is demodulated ("sb").
 * ★ AUTO finds which of RADE / 700D / 700E / 1600 is on air; a manual choice underneath is the fallback when AUTO cannot settle
 *   ({"type":"freedv_sub","sub":"700D"}; "auto" goes back). The report says which is chosen ("sub") and which is heard ("mode").
 */

export const FREEDV_SUBS = ['RADE', '700D', '700E', '1600'] as const;   // ★ key order: RADE first, it is what is on air
export type FreeDvSub = typeof FREEDV_SUBS[number];
export type FreeDvChoice = 'auto' | FreeDvSub;
export const FREEDV_SIDEBANDS = ['auto', 'usb', 'lsb'] as const;
export type FreeDvSideband = typeof FREEDV_SIDEBANDS[number];

/** Key legend, spoken name and hint. */
export const FREEDV_LABEL = 'FreeDV';
export const FREEDV_NAME = 'FreeDV digital voice';
export const FREEDV_HINT = 'Digital voice on the HF amateur bands. Silent until a FreeDV signal is decoded; AUTO finds RADE (what most stations send), 700D, 700E or 1600 \u2014 whichever this server runs.';

/** The server's {"type":"freedv"} report (vibe_freedv.h publish). */
export interface FreeDvState {
  sub: FreeDvChoice;            // what the listener (or the room) chose
  sb: 'usb' | 'lsb';            // the sideband the server demodulates
  sbSel: FreeDvSideband;        // the sideband the listener (or the room) chose — 'auto' = FreeDV's convention
  sync: boolean;                // decoding speech now
  mode: FreeDvSub | '';         // the sub-mode in sync ('' while searching)
  snr?: number;                 // dB, while in sync
  text?: string;                // the station's text channel (callsign etc.), once received twice the same
  last?: string;                // ★ RADE: the callsign of the over that has just ENDED (its end-of-over frame, CRC-checked)
}

const isSub = (s: unknown): s is FreeDvSub => typeof s === 'string' && (FREEDV_SUBS as readonly string[]).includes(s);

/** Parse a report; null when it is not one. Unknown fields are ignored. Text is clipped and kept printable. */
export function parseFreeDv(msg: unknown): FreeDvState | null {
  if (!msg || typeof msg !== 'object') return null;
  const m = msg as Record<string, unknown>;
  if (m.type !== 'freedv') return null;
  const sub: FreeDvChoice = isSub(m.sub) ? m.sub : 'auto';
  const sync = m.state === 'sync';
  const sbSel: FreeDvSideband = m.sbSel === 'usb' || m.sbSel === 'lsb' ? m.sbSel : 'auto';   // ★ older servers: no override
  const st: FreeDvState = { sub, sb: m.sb === 'lsb' ? 'lsb' : 'usb', sbSel, sync, mode: sync && isSub(m.mode) ? m.mode : '' };
  if (sync && typeof m.snr === 'number' && Number.isFinite(m.snr)) st.snr = Math.round(m.snr);
  if (typeof m.text === 'string') {
    const t = m.text.replace(/[^\x20-\x7e]/g, '').trim().slice(0, 40);
    if (t) st.text = t;
  }
  if (typeof m.last === 'string') {
    const t = m.last.replace(/[^\x20-\x7e]/g, '').trim().slice(0, 12);   // ★ FreeDV's reliable text: 8 characters
    if (t) st.last = t;
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
  if (!s.sync) {
    // ★ a hand-picked sideband is named too: a listener who forced the wrong side sees why nothing comes
    const what = [s.sub === 'auto' ? '' : s.sub, s.sbSel && s.sbSel !== 'auto' ? s.sbSel.toUpperCase() : ''].filter(Boolean).join(' ');
    /* ★ RADE: who has just stopped (their end-of-over callsign) takes the place of "searching" — silence with "last G4ABC"
     *  says the over ended, and "FreeDV RADE USB · searching · last DG4SDX" was 41 characters, past the R36S's 40. */
    if (s.last) return what ? `FreeDV ${what} · last ${s.last}` : `FreeDV · last ${s.last}`;
    return what ? `FreeDV ${what} · searching` : 'FreeDV · searching';
  }
  const parts = [`FreeDV ${s.mode}`];
  if (s.snr !== undefined) parts.push(`${s.snr} dB`);
  if (s.text) parts.push(s.text);
  return parts.join(' · ');
}

/** The passband to draw (Hz from the dial): the SSB side the server demodulates, 2.7 kHz wide. */
export function freedvPassband(sb: 'usb' | 'lsb'): [number, number] {
  return sb === 'lsb' ? [-2700, -50] : [50, 2700];
}
/** ★ The server's rule, for the moment before its first report (a fresh tune): LSB below 10 MHz, unless the listener
 *  overrode it. The report wins. */
export function freedvSidebandAt(hz: number, sel: FreeDvSideband = 'auto'): 'usb' | 'lsb' {
  if (sel === 'usb' || sel === 'lsb') return sel;
  if (hz >= 5.25e6 && hz <= 5.45e6) return 'usb';   // ★ 60 m is USB (the FreeDV application's own rule)
  return hz > 0 && hz < 10e6 ? 'lsb' : 'usb';
}

/** Key legend for a sideband choice. */
export function freedvSidebandLabel(sb: FreeDvSideband): string {
  return sb === 'auto' ? 'AUTO' : sb.toUpperCase();
}

/** The manual-choice message: a sub-mode, a sideband, both, or neither (only asks for the report). */
export function freedvSubMessage(sub: FreeDvChoice | null, sb?: FreeDvSideband | null): { type: 'freedv_sub'; sub?: string; sb?: string } {
  const m: { type: 'freedv_sub'; sub?: string; sb?: string } = { type: 'freedv_sub' };
  if (sub) m.sub = sub;
  if (sb) m.sb = sb;
  return m;
}
