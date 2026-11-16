/**
 * tones.ts — V12 DTMF + 5-tone selective calling, the PURE half shared by the app (ModeSelector, DecoderClient) and the web
 * client (web/client/src/main.ts): the hint, the status line, the server's {"type":"tones"} status cut to its fields, and the
 * attach parameters for AUTO and the manual controls. The decoding — and the PRIVACY RULE — are the server's
 * (android/app/src/main/cpp/vibe_tones_run.h, decoders/tones_rx.h; docs/v12/TONES.md): a client is never sent digits it
 * must not show, so nothing here filters text.
 *
 * Wire (decoder socket):
 *   0x01 text — one line per DTMF string / 5-tone call heard · 0x03 | u8 state (0 listening, 1 tones sounding)
 *   {"type":"tones","data":{"amateur":bool,"band":"2 m amateur"|"","dtmf":bool,"five":bool,"pin":"auto"|id,"digits5":bool}}
 * ★ AUTO reads every 5-tone table at once and names the one that fits; the MANUAL control under it pins one table (Stuart's
 *   rule for automatic decoders) — the only way to read DZVEI right, whose digits are every ZVEI digit one higher.
 */

/** ★ The 5-tone tables, in the server's order (decoders/tones_fivetone.h toneTables) — ids as the attach message sends them. */
export const FIVE_TONE_STANDARDS: ReadonlyArray<{ id: string; label: string }> = [
  { id: 'auto', label: 'AUTO' },
  { id: 'zvei1', label: 'ZVEI-1' }, { id: 'zvei2', label: 'ZVEI-2' }, { id: 'zvei3', label: 'ZVEI-3' },
  { id: 'dzvei', label: 'DZVEI' }, { id: 'pzvei', label: 'PZVEI' },
  { id: 'ccir', label: 'CCIR' }, { id: 'eea', label: 'EEA' }, { id: 'eia', label: 'EIA' },
];

export interface TonesOptions { dtmf: boolean; five: boolean; standard: string }
export const TONES_DEFAULTS: TonesOptions = { dtmf: true, five: true, standard: 'auto' };

/** The attach message's parameters: AUTO and both decoders on send nothing; each change from that is one field. */
export function tonesParams(o: Partial<TonesOptions> | null | undefined): Record<string, unknown> {
  const p: Record<string, unknown> = {};
  if (o?.dtmf === false) p.dtmf = false;
  if (o?.five === false) p.five = false;
  const std = typeof o?.standard === 'string' ? o.standard : 'auto';
  if (std !== 'auto' && FIVE_TONE_STANDARDS.some((s) => s.id === std)) p.standard = std;
  return p;
}

export interface TonesStatus { amateur: boolean; band: string; dtmf: boolean; five: boolean; pin: string; digits5: boolean }
/** ★ The server's status, cut to its fields (a broken server must not put an object where a word goes). */
export function tonesSanitise(d: any): TonesStatus | null {
  if (!d || typeof d !== 'object') return null;
  const pin = typeof d.pin === 'string' && FIVE_TONE_STANDARDS.some((s) => s.id === d.pin) ? d.pin : 'auto';
  return {
    amateur: d.amateur === true, band: typeof d.band === 'string' ? d.band.slice(0, 40) : '',
    dtmf: d.dtmf !== false, five: d.five !== false, pin, digits5: d.digits5 === true,
  };
}

/** ★ Where it is heard, and what is shown where — the hint under the key. */
export const TONES_HINT = 'DTMF and 5-tone selective calling (CCIR, EEA, EIA, ZVEI-1/2/3, DZVEI, PZVEI) on NFM or AM. '
  + 'On the amateur bands the digits are shown — repeater, EchoLink and AllStar control codes, and amateur 5-tone. '
  + 'Anywhere else only what was heard is shown (“DTMF heard”, “ZVEI-1 5-tone heard”): a dialled or called number is private.';

/** The status line: the state, and whether digits can be shown on this dial. */
export function tonesStateText(st: number, s: TonesStatus | null): string {
  const where = s ? (s.amateur ? `${s.band}: digits shown` : 'not an amateur frequency: what was heard, no digits') : '';
  const head = st ? 'tones…' : 'listening for DTMF and 5-tone';
  return where ? `${head} · ${where}` : head;
}
