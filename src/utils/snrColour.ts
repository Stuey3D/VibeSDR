/**
 * snrColour — ONE colour-coded SNR readout for every decoded message, app and web alike (beta96, Stuart 11-26: "for all
 * modes if possible a colour coded snr readout per message … it will allow me to check antenna performance quickly").
 * DISPLAY ONLY: nothing here reaches a decoder or what counts as a decode.
 *
 * ★★★ THE COLOUR IS THE MARGIN OVER THAT MODE'S DECODE THRESHOLD, NOT AN ABSOLUTE dB SCALE (Stuart 11-26: "the snr colours
 *     need to be accurate to the mode so if a mode only needs a weak signal to work great no point listing all as red").
 *     FT8 decodes to about −21 dB in 2.5 kHz, so −15 is a healthy FT8 decode; AIS needs ~9 dB over its floor. Each mode has
 *     its threshold `thr` ON THE SCALE ITS DECODER REPORTS (a published figure only where our decoder reports that figure;
 *     otherwise read off our own bench: the reported SNR of the decodes at the ~50 % point) — and then, for every mode:
 *       red   = margin under 3 dB  — at or just above the threshold: that message was nearly lost;
 *       amber = 3 to 6 dB margin   — decoding with little to spare (a fade takes it);
 *       green = 6 dB margin or more — solid.
 *     The NUMBER shown is always the real dB the decoder measured; only the colour is per mode. The number is comparable
 *     WITHIN a mode (this antenna vs that one, today vs yesterday), not across modes (each measures its own way).
 * ★ The per-mode table — source, units, threshold and where it came from, cost — is docs/v12/MESSAGE-SNR.md. Change both.
 * ★ Chip: the band colour FILLED, dark text on it (SNR_CHIP_TEXT) — it carries its own background, so it reads the same on
 *   the default, silver and black faceplates, transparency on or off. Never colour alone: the number is always there.
 */

export type SnrBand = 'red' | 'amber' | 'green';
/** The colours — one set for web and app (readable on the default, silver and black themes). */
export const SNR_COLOURS: Record<SnrBand, string> = { red: '#e5484d', amber: '#f0a020', green: '#3dbb5f' };
/** Text drawn ON a filled chip (≥ 4.5:1 against all three colours). */
export const SNR_CHIP_TEXT = '#111111';
/** Margin over the threshold: under RED_MARGIN → red; RED_MARGIN … GREEN_MARGIN → amber; ≥ GREEN_MARGIN → green. */
export const SNR_RED_MARGIN = 3;
export const SNR_GREEN_MARGIN = 6;

export interface SnrScale {
  /** The decode threshold, dB, on the scale this mode's decoder REPORTS. */
  thr: number;
  /** What the number is — the chip's tooltip / accessibility line. */
  what: string;
  /** Where `thr` came from (the doc's table says the same). */
  src: string;
  /** Show a + on positive values (WSJT-X style, where negative SNRs are normal). */
  signed?: boolean;
}

const WSJT = 'SNR in 2500 Hz (the WSJT-X convention)';
/** ★ The modes' scales. Keys are what callers (and the server's "sm" field) pass. docs/v12/MESSAGE-SNR.md has each row. */
export const SNR_SCALES = {
  /** SITOR-B bit-centre SNR in the 100 Hz bit bandwidth (fsk_decoder, beta96). */
  navtex: { thr: 6, what: 'SNR in the 100 Hz bit bandwidth, measured at each bit centre',
            src: 'tools/rtty-bench: CER 28 % at a meter reading of ~6 dB, 0.6 % at ~9 dB' },
  ft8: { thr: -21, signed: true, what: WSJT, src: 'docs/v12/FTX-SENSITIVITY.md: 50 % at −20.8 dB (jt9 −20.7)' },
  ft4: { thr: -16, signed: true, what: WSJT, src: 'docs/v12/FTX-SENSITIVITY.md: 50 % at −16.3 dB' },
  ft2: { thr: -13, signed: true, what: WSJT, src: 'docs/v12/FTX-SENSITIVITY.md: 50 % at −13.3 dB' },
  /** JS8 normal speed (the common one); its faster sub-modes stop ~2–4 dB earlier. */
  js8: { thr: -20, signed: true, what: WSJT, src: 'docs/v12/FTX-SENSITIVITY.md: Normal 8/8 at −20 dB (4/8 before the second look)' },
  /** Mode S: the reply's preamble pulse power over the 1090 MHz floor (vibe_adsb.h Frame::snr). */
  adsb: { thr: 10, what: 'reply pulse power over the 1090 MHz noise floor',
          src: 'test-adsb: the preamble gate is 9 dB; at the 50 % point (2.4 MS/s) decoded replies report ~10 dB' },
  /** AIS: the frame's mean power over the channel floor, S/N (vibe_ais.h burstSnr_). */
  ais: { thr: 9, what: 'frame power over the channel noise floor',
         src: 'test-ais: 50 % at Eb/N0 ≈ 8.5 dB, where decoded frames report ~8.6 dB' },
  /** ACARS: the AM carrier over the channel floor (vibe_acars.h). */
  acars: { thr: 2, what: 'AM carrier over the channel noise floor',
           src: 'test-acars: 27–67 % decoded at 45 dB-Hz, those blocks report 1.6–2.6 dB' },
  /** VDL2: the burst's power over the channel floor, (S+N)/N (vibe_vdl2.h). */
  vdl2: { thr: 14, what: 'burst power over the channel noise floor',
          src: 'test-vdl2: 50 % between 14 and 16 dB, decoded bursts report 12.6–14.9 dB' },
  /** HFDL: the equaliser's own SNR (vibe_hfdl.h) — it reads ~5 dB over the symbol-bandwidth SNR; per bit rate. */
  hfdl300: { thr: 2, what: 'the HFDL equaliser’s SNR (300 bit/s)', src: 'test-hfdl --sens 0 / 4: 50 % at −6 dB, reported ~2 dB' },
  hfdl600: { thr: 2.5, what: 'the HFDL equaliser’s SNR (600 bit/s)', src: 'test-hfdl --sens 1: 20 % at −6 dB (2.0), all at −4 (3.2)' },
  hfdl1200: { thr: 4, what: 'the HFDL equaliser’s SNR (1200 bit/s)', src: 'test-hfdl --sens 2: none at −4 dB, all at −2 (4.2)' },
  hfdl1800: { thr: 7, what: 'the HFDL equaliser’s SNR (1800 bit/s)', src: 'test-hfdl --sens 3: none at 0 dB, 9/10 at +2 (7.1)' },
  /** ORBCOMM: the carrier's M2M4 estimate, Es/N0 (vibe_orbcomm.h snrDb) — reads the transmitted Es/N0 to 0.1 dB. */
  orbcomm: { thr: 5.5, what: 'Es/N0 of the satellite carrier (M2M4 estimate)',
             src: 'test-orbcomm: 69 % of packets at Es/N0 6 dB (reported 6.0), 93 % at 8; ORBCOMM.md: 50 % at ~6' },
  /** Radiosondes: S/N in the scan's box (vibe_rs41_rx.h Chan::snr — 10 kHz; 20 kHz for an M10/M20), per type: the box
   *  reads a type-dependent offset from a bench's "SNR in 10 kHz", so each type's threshold is read off its own sweep. */
  rs41: { thr: 4.5, what: 'sonde power over the floor in a 10 kHz box', src: 'test-rs41: 2/12 frames at 6 dB (reported 3.9), 11/12 at 7 (4.9)' },
  dfm: { thr: 5, what: 'sonde power over the floor in a 10 kHz box', src: 'test-sondes-types: 0 at 4 dB, 12 of 15 at 6 (reported 6.0)' },
  m10: { thr: 1.5, what: 'sonde power over the floor in a 20 kHz box', src: 'test-sondes-types: 0 at 10 dB, 14 of 19 at 12 (reported 2.2)' },
  m20: { thr: 0, what: 'sonde power over the floor in a 20 kHz box', src: 'test-sondes-types: 1 at 10 dB (−1.4), 16 of 19 at 12 (0.6)' },
} satisfies Record<string, SnrScale>;
export type SnrMode = keyof typeof SNR_SCALES;

const num = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);
/** A mode key from the wire, or null (an unknown key draws nothing — never a guessed scale). */
export function snrModeOf(v: unknown): SnrMode | null {
  return typeof v === 'string' && Object.prototype.hasOwnProperty.call(SNR_SCALES, v) ? (v as SnrMode) : null;
}

export function snrBandOf(scale: SnrScale, db: number): SnrBand {
  const m = db - scale.thr;
  return m < SNR_RED_MARGIN ? 'red' : m < SNR_GREEN_MARGIN ? 'amber' : 'green';
}
export function snrBandFor(mode: SnrMode, db: number): SnrBand { return snrBandOf(SNR_SCALES[mode], db); }
export function snrColourFor(mode: SnrMode, db: number): string { return SNR_COLOURS[snrBandFor(mode, db)]; }

const signedDb = (v: number) => (v < 0 ? '−' + Math.abs(v) : String(v));
/** "12 dB", "−7 dB", "+3 dB" (signed scales) — or '' for no number (an older server, a mode that cannot measure). */
export function snrText(mode: SnrMode, db: unknown): string {
  if (!num(db)) return '';
  const r = Math.round(db);
  return `${r < 0 ? '−' : SNR_SCALES[mode].signed && r > 0 ? '+' : ''}${Math.abs(r)} dB`;
}
/** The chip's tooltip: "SNR 12 dB, 3 dB over this mode's threshold (amber) — <what>. Decodes from about 9 dB: red under 12,
 *  green from 15." */
export function snrTitle(mode: SnrMode, db: unknown): string {
  if (!num(db)) return '';
  const s = SNR_SCALES[mode];
  const m = Math.round(db - s.thr);
  const rel = m >= 0 ? `${m} dB over` : `${-m} dB under`;
  return `SNR ${snrText(mode, db)}, ${rel} where this mode stops decoding (${snrBandFor(mode, db)}) — ${s.what}. `
    + `Decodes from about ${signedDb(s.thr)} dB: red under ${signedDb(s.thr + SNR_RED_MARGIN)}, green from ${signedDb(s.thr + SNR_GREEN_MARGIN)}.`;
}
/** Everything a chip needs, or null when there is no number or no known mode (draw nothing — never a fake one). */
export function snrChip(mode: SnrMode | string | null | undefined, db: unknown):
    { text: string; bg: string; fg: string; title: string; band: SnrBand } | null {
  const k = snrModeOf(mode);
  if (!k || !num(db)) return null;
  const band = snrBandFor(k, db);
  return { text: snrText(k, db), bg: SNR_COLOURS[band], fg: SNR_CHIP_TEXT, title: snrTitle(k, db), band };
}
const escAttr = (s: string) => s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');
/** The web's chip as HTML: <span class="snrChip red|amber|green" title="…">12 dB</span>. '' without a number. The CSS
 *  (web/client/index.html .snrChip) sets the colours from the class — the same three as SNR_COLOURS. */
export function snrChipHtml(mode: SnrMode | string | null | undefined, db: unknown): string {
  const c = snrChip(mode, db);
  if (!c) return '';
  return `<span class="snrChip ${c.band}" role="img" aria-label="${escAttr(c.title)}" title="${escAttr(c.title)}">${escAttr(c.text)}</span>`;
}
