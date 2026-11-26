/**
 * snrColour — ONE colour-coded SNR scale for every decoded message, app and web alike (beta96, Stuart 11-26: "for all
 * modes if possible a colour coded snr readout per message … it will allow me to check antenna performance quickly").
 * DISPLAY ONLY: nothing here reaches a decoder or what counts as a decode.
 *
 * ★★ THE RULE: three bands — red, amber, green — with the NUMBER always shown (never colour alone). The colours are one
 *    set everywhere (NAVTEX's, beta96). Only the THRESHOLDS differ per mode, and only where the measurement differs:
 *    each mode's figure is measured its own way (bandwidth, what "noise" is), so a number is comparable WITHIN a mode
 *    (this antenna vs that one, today vs yesterday), not across modes. Each scale says what it measures (`what`) — that
 *    line is the chip's tooltip, so nobody has to guess.
 *      red   = within a few dB of where the decoder stops: that message was nearly lost.
 *      amber = decoding, with little margin.
 *      green = comfortable.
 * ★ The per-mode table (source, units, thresholds, cost) is docs/v12/MESSAGE-SNR.md — change both together.
 * ★ Chip: the band colour FILLED, dark text on it (SNR_CHIP_TEXT) — so it reads the same on the default, silver and
 *   black faceplates and with transparency on or off (it carries its own background).
 */

export type SnrBand = 'red' | 'amber' | 'green';
/** The scale's colours — one set for web and app (readable on the default, silver and black themes). */
export const SNR_COLOURS: Record<SnrBand, string> = { red: '#e5484d', amber: '#f0a020', green: '#3dbb5f' };
/** Text drawn ON a filled chip (≥ 4.5:1 against all three colours). */
export const SNR_CHIP_TEXT = '#111111';

export interface SnrScale {
  /** Under this → red. */
  red: number;
  /** Over this → green (between: amber). */
  green: number;
  /** What the number is — the chip's tooltip / accessibility line. */
  what: string;
  /** Show a + on positive values (WSJT-X style, where negative SNRs are normal). */
  signed?: boolean;
}

/** ★ The modes' scales. Keys are what the callers pass; docs/v12/MESSAGE-SNR.md has the reasoning for each number. */
export const SNR_SCALES = {
  /** SITOR-B bit-centre SNR in the 100 Hz bit bandwidth (fsk_decoder) — NAVTEX's beta96 scale, unchanged. */
  navtex: { red: 5, green: 10, what: 'SNR in the 100 Hz bit bandwidth, measured at each bit centre' },
  /** WSJT-X convention: dB in 2500 Hz. FT8 decodes to about −21: red within 6 dB of that, green from −5. */
  ft8: { red: -15, green: -5, signed: true, what: 'SNR in 2500 Hz (the WSJT-X convention)' },
  /** FT4 stops about 3.5 dB earlier than FT8. */
  ft4: { red: -12, green: -2, signed: true, what: 'SNR in 2500 Hz (the WSJT-X convention)' },
  /** WSPR decodes to about −28 in 2500 Hz. */
  wspr: { red: -22, green: -12, signed: true, what: 'SNR in 2500 Hz (the WSJT-X convention)' },
  /** JS8 shares FT8's modulation (normal speed). */
  js8: { red: -15, green: -5, signed: true, what: 'SNR in 2500 Hz (the WSJT-X convention)' },
  /** Burst power over the channel's noise floor, in the channel bandwidth — the datalinks and packet modes. Their
   *  demodulators need ~8–10 dB (MSK/GMSK/AFSK, D8PSK a few more) — red under 10, green from 20. */
  burst: { red: 10, green: 20, what: 'burst power over the channel noise floor, in the channel bandwidth' },
  /** Mode S / ADS-B: the reply's pulse power over the 1090 MHz noise floor at the sample rate (dump1090's measure). The
   *  preamble detector needs ~8 dB; red under 10, green from 20. */
  adsb: { red: 10, green: 20, what: 'reply pulse power over the 1090 MHz noise floor' },
} satisfies Record<string, SnrScale>;
export type SnrMode = keyof typeof SNR_SCALES;

const num = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);

export function snrBandOf(scale: SnrScale, db: number): SnrBand { return db < scale.red ? 'red' : db <= scale.green ? 'amber' : 'green'; }
export function snrBandFor(mode: SnrMode, db: number): SnrBand { return snrBandOf(SNR_SCALES[mode], db); }
export function snrColourFor(mode: SnrMode, db: number): string { return SNR_COLOURS[snrBandFor(mode, db)]; }

/** "12 dB", "−7 dB", "+3 dB" (signed scales) — or '' for no number (an older server, a mode that cannot measure). */
export function snrText(mode: SnrMode, db: unknown): string {
  if (!num(db)) return '';
  const r = Math.round(db);
  const sign = r < 0 ? '−' : SNR_SCALES[mode].signed && r > 0 ? '+' : '';
  return `${sign}${Math.abs(r)} dB`;
}
/** The chip's tooltip: "SNR 12 dB (amber) — burst power over …; red under 10, green over 20". */
export function snrTitle(mode: SnrMode, db: unknown): string {
  if (!num(db)) return '';
  const s = SNR_SCALES[mode];
  const f = (v: number) => (v < 0 ? '−' + Math.abs(v) : String(v));
  return `SNR ${snrText(mode, db)} (${snrBandFor(mode, db)}) — ${s.what}; red under ${f(s.red)} dB, green over ${f(s.green)} dB`;
}
/** Everything a chip needs, or null when there is no number (draw nothing — never a fake one). */
export function snrChip(mode: SnrMode, db: unknown): { text: string; bg: string; fg: string; title: string; band: SnrBand } | null {
  if (!num(db)) return null;
  const band = snrBandFor(mode, db);
  return { text: snrText(mode, db), bg: SNR_COLOURS[band], fg: SNR_CHIP_TEXT, title: snrTitle(mode, db), band };
}
/** The web's chip as HTML (escaped by construction: only digits, signs and fixed words). '' without a number. */
export function snrChipHtml(mode: SnrMode, db: unknown): string {
  const c = snrChip(mode, db);
  if (!c) return '';
  return `<span class="snrChip ${c.band}" title="${c.title.replace(/"/g, '&quot;')}">${c.text}</span>`;
}
