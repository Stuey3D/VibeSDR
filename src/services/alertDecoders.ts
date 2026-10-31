/**
 * alertDecoders.ts — ★ V12 EAS / SAME and SELCAL: the words both clients use (the app's ModeSelector and DecoderClient,
 * the web's main.ts). One copy, so the hint and the state line say the same thing on every screen.
 *
 * Both decoders are a VibeServer's own (decoders/same_decoder.h, decoders/selcal_decoder.h) and send the same frames:
 *   0x01 text — one line per alert / call · 0x03 | u8 state (0 listening, 1 a burst / a tone pair being read).
 */

/** ★ EAS / SAME: where it is heard. */
export const SAME_HINT = 'US alert headers (EAS / SAME): tune a NOAA Weather Radio channel, 162.400–162.550 MHz in NFM, or an EAS broadcast station. Each alert is shown once, with its event, areas and times, after two of its three header bursts agree.';

/** ★ SELCAL: where it is heard, and the one thing that matters (the dial). */
export const SELCAL_HINT = 'SELCAL: the two-tone calls a ground station sends to wake one aircraft’s radio, shown as its code (AB-CD), with all 32 ICAO tones. Tune an HF aeronautical channel in USB (the dial within about 10 Hz) or a VHF one in AM.';

/** The status line for the server's 0x03 state. */
export function sameStateText(st: number): string { return st ? 'reading an alert header…' : 'listening for alert headers'; }
export function selcalStateText(st: number): string { return st ? 'tones…' : 'listening for SELCAL'; }
