/**
 * modeBox.ts — the mode box's LABEL and its WIDTH on the LED / analogue decks (faceplates §4.1 / §9),
 * pure so scripts/test_faceplate_modebox.ts can prove the longest label fits on ONE line on every
 * device, chassis, Display, meter and orientation.
 *
 * ★★★ THE LABEL NEVER WRAPS (Stuart, 2026-10-01). On USB with RTTY running the mode box read
 *   "USB:" / "RTTY" / "S9+1" — three lines — in a fixed 70 pt box, with the frequency window beside it
 *   holding "lots of spare width". The default deck never had this: its mode button is
 *   `minWidth` + its CONTENT (FreqModePill), so it simply grows to the label. The compact deck's box
 *   was a FIXED s.r(70), built for a bare "USB", and "USB: RTTY" wrapped at its space.
 * ★★ THE RULE IS THE DEFAULT DECK'S: the design width as a MINIMUM, grown to the label — and the
 *   width comes out of the frequency window, which flexes (the tubes narrow, §7). Only when the window
 *   cannot spare it does the label's letter-spacing tighten, and only after THAT does the type shrink
 *   — last resort, never below the legibility floor, and never onto a second line.
 * ★ Widths are ESTIMATED from the bundled fonts' own advance widths (hmtx, measured from
 *   assets/fonts), not guessed: Atkinson Hyperlegible per glyph, Doto monospaced at 0.6 em.
 */

import type { DecoderType } from '../components/DecoderPanel';
import type { SDRMode } from '../services/sdrTypes';

/** Advance widths (em) of Atkinson Hyperlegible Regular, read from assets/fonts' hmtx table — every
 *  glyph a mode label or reading can contain. The app asks for BOLD; both platforms SYNTHESISE it
 *  from this one file (there is no Bold cut bundled), so ATKINSON_BOLD_K pads for the emboldening. */
export const ATKINSON_EM: Readonly<Record<string, number>> = {
  A: 0.626, B: 0.619, C: 0.656, D: 0.673, E: 0.567, F: 0.548, G: 0.710, H: 0.690, I: 0.414, J: 0.510,
  K: 0.627, L: 0.539, M: 0.820, N: 0.689, O: 0.727, P: 0.601, Q: 0.753, R: 0.618, S: 0.596, T: 0.558,
  U: 0.690, V: 0.589, W: 0.840, X: 0.623, Y: 0.595, Z: 0.607,
  0: 0.648, 1: 0.402, 2: 0.549, 3: 0.574, 4: 0.614, 5: 0.583, 6: 0.598, 7: 0.510, 8: 0.615, 9: 0.598,
  ':': 0.205, ' ': 0.280, '+': 0.602, '-': 0.368, '.': 0.240, '/': 0.380,
};
/** The widest Atkinson glyph above (W) — what an unlisted character is assumed to take. */
export const ATKINSON_EM_MAX = 0.84;
/** Synthesised bold's extra width — generous on purpose: an over-estimate costs a point of window, an
 *  under-estimate costs the label a line. */
export const ATKINSON_BOLD_K = 1.08;
/** Doto Black (the `dot` Display's mode font) is monospaced: every glyph 0.6 em, one weight. */
export const DOTO_EM = 0.6;

export type ModeFace = 'hyper' | 'doto';

/** The width (pt) RN lays `text` out at, letter-spacing included (RN adds it after every glyph). */
export function modeTextWidth(text: string, size: number, letterSpacing: number, face: ModeFace): number {
  let em = 0;
  for (const ch of text) em += face === 'doto' ? DOTO_EM : (ATKINSON_EM[ch.toUpperCase()] ?? ATKINSON_EM_MAX);
  const k = face === 'doto' ? 1 : ATKINSON_BOLD_K;
  return em * size * k + letterSpacing * [...text].length;
}

/** The stereo rings beside the label (ControlsBar StereoIcon: size round(0.95 × font), 1.62 wide, 5 pt
 *  margin). */
export function stereoWidth(fontSize: number): number {
  return Math.round(fontSize * 0.95) * 1.62 + 5;
}

// ── The label ────────────────────────────────────────────────────────────────

/** Mode pill label: there's a single CW button (the sideband id cwu/cwl is an internal demod detail),
 *  so show it as plain "CW" to match the button. */
export function modeDisplay(mode: string): string {
  const m = mode.toLowerCase();
  return (m === 'cwu' || m === 'cwl' || m === 'cw') ? 'CW' : mode.toUpperCase();
}

/**
 * §5.1: the running decoder composed onto the demod — USB → "USB: RTTY" (wefax reads FAX); DAB is "DAB".
 * ★★ A STANDALONE digimode IS its own mode: an OpenWebRX Meshcore / Meshtastic / ADS-B profile sets
 *   mod AND secondary_mod to the same id (OwrxAdapter), and this read "MESHCORE: MESHCORE" — the
 *   longest label the app could produce, and a stutter. The decoder is shown only when it is not the
 *   mode itself.
 */
export function composeModeLabel(mode: string, decoder: string | null | undefined, dab = false): string {
  if (dab) return 'DAB';
  const dec = decoder ? (decoder === 'wefax' ? 'fax' : decoder) : '';
  return modeDisplay(mode) + (dec && dec.toLowerCase() !== String(mode).toLowerCase() ? `: ${dec.toUpperCase()}` : '');
}

/** The longest reading the box's second line shows (formatReading: "-120dB", "S9+60", "120 dBf"). */
export const READING_LONGEST = '-120 dBf';

/** Every client-side decoder the label can carry — DecoderPanel's DecoderType, held to it by the type
 *  check below (a decoder added there and not here fails `tsc`), so the test's "longest label" is the
 *  code's, not a guess. */
export const CLIENT_DECODERS = (['rtty', 'navtex', 'wefax', 'sstv', 'morse', 'whisper', 'ft8', 'time'] as const) satisfies
  readonly NonNullable<DecoderType>[];
type MissingDecoder = Exclude<NonNullable<DecoderType>, typeof CLIENT_DECODERS[number]>;
// eslint-disable-next-line @typescript-eslint/no-unused-vars
const everyDecoderListed: [MissingDecoder] extends [never] ? true : MissingDecoder = true;

/** Every demod the app can be in (sdrTypes' SDRMode, held to it exactly by `satisfies`). */
export const DEMODS = { usb: 1, lsb: 1, am: 1, sam: 1, fm: 1, nfm: 1, cwu: 1, cwl: 1, wfm: 1 } as const satisfies
  Record<SDRMode, 1>;

/**
 * Every label the box can be asked to show that the CODE can enumerate: each demod bare and with each
 * client decoder (WFM also with its stereo rings), each standalone digimode (pass dataModes'
 * WHOLE_PROFILE_MODES), and DAB with its rings. ★ An OpenWebRX server names its own carried decoders
 * on the wire, so those cannot be listed here — they take the same rule, and the one-line Text shrinks
 * one that is longer still rather than wrap it.
 */
export function modeLabelCandidates(standalone: readonly string[]): { label: string; stereo: boolean }[] {
  const out: { label: string; stereo: boolean }[] = [];
  for (const m of Object.keys(DEMODS)) for (const d of [null, ...CLIENT_DECODERS]) {
    const label = composeModeLabel(m, d);
    out.push({ label, stereo: false });
    if (m === 'wfm') out.push({ label, stereo: true });
  }
  for (const m of standalone) out.push({ label: composeModeLabel(m, m, m === 'dab'), stereo: false });
  out.push({ label: composeModeLabel('wfm', null, true), stereo: true });
  return out;
}

// ── The width ────────────────────────────────────────────────────────────────

/** The box at scale 1: the mockup's 70 pt (portrait and landscape, LAND.modeBox) as the MINIMUM, and
 *  its side padding. */
export const MODE_BOX = { minW: 70, padH: 5 } as const;

/** At most this share of the whole window (frequency + mode box) goes to the mode box: the frequency
 *  keeps the larger part, so its tubes / digits stay the thing the window is for. */
export const MODE_BOX_MAX_SHARE = 0.45;
/** ★ The very last step, only at the type's floor: the box may take up to HALF the window — never more;
 *  the frequency is the thing the window is for. (Reached only by WFM + Whisper + the stereo rings on
 *  the SE in Display Zoom, landscape, where the band's height has already set the type at 9 pt.) */
export const MODE_BOX_LAST_SHARE = 0.5;
/** What letter-spacing tightens to before the type is allowed to shrink. */
export const MODE_LS_TIGHT = 0.5;
/** The type's floor — LAND.minModeFont; a floor does not scale down. */
export const MODE_MIN_FONT = 9;

export interface ModeBoxFit {
  /** The box's width (pt): at least the design width, grown to the label. */
  width:         number;
  fontSize:      number;
  letterSpacing: number;
  /** true when the label had to give way (tightened, or shrunk) — the test's finding, not the app's. */
  squeezed:      boolean;
}

/**
 * The mode box for a label: the design width (`minW`) as a minimum, grown to the label (+ the stereo
 * rings + the box's side padding), out of the window — capped at MODE_BOX_MAX_SHARE of it. If the
 * label still does not fit: tighten the letter-spacing, then shrink the type (floor MODE_MIN_FONT), and
 * at the floor take up to MODE_BOX_LAST_SHARE. Past that the one-line Text shrinks itself (ModeReadout
 * `oneLine`) — for an OpenWebRX decoder name nobody could list; never a second line.
 * @param windowW the whole window's width (frequency + mode box); 0 = not measured yet (no cap).
 */
export function modeBoxFit(o: { label: string; stereo: boolean; face: ModeFace; fontSize: number;
                                letterSpacing: number; readingFont: number; minW: number; padH: number;
                                windowW: number }): ModeBoxFit {
  const need = (fs: number, ls: number) => Math.max(
    modeTextWidth(o.label, fs, ls, o.face) + (o.stereo ? stereoWidth(fs) : 0),
    modeTextWidth(READING_LONGEST, o.readingFont, 0, o.face),
  ) + 2 * o.padH;
  const maxW = o.windowW > 0 ? Math.max(o.minW, Math.floor(o.windowW * MODE_BOX_MAX_SHARE)) : Infinity;
  const fs0 = o.fontSize, ls0 = o.letterSpacing;
  if (need(fs0, ls0) <= maxW) {
    return { width: Math.max(o.minW, Math.ceil(need(fs0, ls0))), fontSize: fs0, letterSpacing: ls0, squeezed: false };
  }
  const ls = Math.min(ls0, MODE_LS_TIGHT);
  let fs = fs0;
  while (fs > MODE_MIN_FONT && need(fs, ls) > maxW) fs--;
  const lastW = Math.max(maxW, Math.floor(o.windowW * MODE_BOX_LAST_SHARE));
  return { width: Math.min(lastW, Math.max(maxW, Math.ceil(need(fs, ls)))), fontSize: fs, letterSpacing: ls, squeezed: true };
}
