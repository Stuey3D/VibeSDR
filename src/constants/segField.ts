/**
 * segField.ts — what the VCR (14-segment) mode box SHOWS, cell by cell (2026-10-06, Stuart's screenshot of the
 * Mac app on VCR: "The demodulator button is the only one not in the correct font for the skin").
 *
 * Two FIXED fields, drawn by components/SegField.tsx in DSEG14 — the VTS strip's own segment face:
 *
 *   THE MODE FIELD   SEG_MODE_CELLS cells. A plain mode is ONE TIGHT WORD from the first cell ("AM", "WFM",
 *                    "MESHTASTIC") with no colon electrode anywhere; a running decoder SPLITS the field: the demod
 *                    right-aligned in the first three, a COLON ELECTRODE in the gap after cell 3, then the decoder
 *                    ("USB:RTTY", " AM:RTTY"). ★★ In WFM the last three cells'
 *                    room is the STEREO RINGS' slot; in every other mode that room is more cells (Stuart:
 *                    "we should have room for the full USB:RTTY or MESHCORE if when not in FM mode we cheat a
 *                    little and replace the stereo rings with more digits"). The field's width never changes.
 *   THE READOUT      the template  -88+88 dB F S  — every electrode always there as a ghost, the value lit:
 *                      S-meter   " S9+27"  dB lit only when there is a "+N" over S9 ("S7" lights no unit)
 *                      dBFS      "-73"     dB F S lit
 *                      SNR       " 24"     dB lit
 *                      dBf       "120"     dB F lit (FM-DX's unit; S stays dark)
 *                    The main number always lives in the "-88" group and only the S-meter's over-S9 dB uses
 *                    the "+88" group, so a reading never hops between groups as it changes.
 *   THE UNIT         SEG_UNIT_CELLS cells beside the frequency digits: "KHZ" / "MHZ" / " HZ", right-aligned, every
 *                    electrode a ghost — capitals, because a 14-segment display has no lower case.
 *
 * ★ Pure: no React, no runtime imports — scripts/test_faceplate_segfield.ts runs it under plain Node. The
 *   caller hands in the cell converter (displayText's toSegCells), so the folding rules stay in ONE place.
 */

/** Cells in the mode field — sized to the longest label the app composes (see the test): "MESHTASTIC" and
 *  "USB:WHISPER" (the colon rides in a gap, not a cell) are ten. */
export const SEG_MODE_CELLS = 10;
/** The demod's cells; the colon electrode sits in the gap after the last of them. */
export const SEG_DEMOD_CELLS = 3;
/** The stereo rings' slot, in cells, at the field's right-hand end (WFM only). */
export const SEG_STEREO_CELLS = 3;
/** The readout's cells: "-88+88". */
export const SEG_READ_CELLS = 6;
/** ★ The readout's ghost — what each cell's electrodes are. Cell 1 is the classic meter's "±1" half-digit:
 *  the minus AND a "1", so −100 … −120 dBFS and 100+ dBf fit without a seventh cell (it reads "-88+88"
 *  with a "1" tucked in front, the way every 3½-digit meter's lead digit does). */
export const SEG_READ_GHOST: readonly string[] = ['-1', '8', '8', '+', '8', '8'];
/** The frequency unit's cells: "KHZ", "MHZ" — the longest unit the readout shows. */
export const SEG_UNIT_CELLS = 3;

/** The frequency unit ("kHz", "MHz", "Hz") → its cells, upper case and RIGHT-aligned in a fixed three, so swapping
 *  units never moves a cell ('' = dark). */
export function segUnitCells(unit: string): string[] {
  const u = [...unit.trim().toUpperCase()].slice(-SEG_UNIT_CELLS);
  return [...Array<string>(SEG_UNIT_CELLS - u.length).fill(''), ...u];
}

/** The 14-segment all-on glyph — the mode field's ghost (DSEG14's `~`, displayText SEG_GHOST). */
export const SEG_ALL = '~';

/** Turns text into display cells, one string per cell ('' = blank, a trailing '.' = that cell's point). */
export type ToCells = (text: string) => string[];

export interface SegModeCells {
  /** What each cell shows, from cell 0. Longer than the field only when `marquee`. */
  cells: string[];
  /** The colon electrode (between the demod and the decoder) is lit. */
  colon: boolean;
  /** ★ The field is SPLIT demod | colon | decoder, so the colon electrode exists (ghosted) after the demod's three
   *  cells, and on DOT the colon column sits there. False for a plain mode: one tight word, no colon anywhere — the
   *  ghost colon after "WFM" and DOT's colon column inside "MESHTASTIC" read as a gap in the word (Stuart, 2026-10-06,
   *  of "WF M"). Also false while marquee: the colon then rides in a cell of its own. */
  split: boolean;
  /** The field's right-hand end is the stereo rings' slot (WFM with no decoder). */
  stereoSlot: boolean;
  /** How many cells the text has: SEG_MODE_CELLS, less the rings' slot. */
  textCells: number;
  /** ★ Too long for the field (an OpenWebRX decoder name nobody could list): it steps through the window like
   *  the VTS strip — never cut short. The colon then takes a cell of its own (':') so it moves with the text. */
  marquee: boolean;
}

/**
 * The mode label (constants/modeBox composeModeLabel: "USB: RTTY", "WFM", "MESHCORE") → the field's cells.
 * @param stereo the label has the rings' slot (ControlsBar stereoSlot: WFM). ★ When a decoder runs on WFM the
 *        rings give their slot to the decoder's name, so "WFM:WHISPER" is still shown whole.
 */
export function segModeCells(label: string, stereo: boolean, toCells: ToCells, total = SEG_MODE_CELLS): SegModeCells {
  const m = /^([^:]+):\s*(.+)$/.exec(label.trim());
  if (m) {
    const demod = toCells(m[1].trim()), dec = toCells(m[2].trim());
    if (demod.length <= SEG_DEMOD_CELLS) {
      const lead = [...Array(SEG_DEMOD_CELLS - demod.length).fill(''), ...demod];
      const fits = SEG_DEMOD_CELLS + dec.length <= total;
      return fits
        ? { cells: [...lead, ...dec], colon: true, split: true, stereoSlot: false, textCells: total, marquee: false }
        : { cells: [...demod, ':', ...dec], colon: false, split: false, stereoSlot: false, textCells: total, marquee: true };
    }
  }
  const cells = toCells(label.trim());
  const textCells = stereo ? total - SEG_STEREO_CELLS : total;
  /* ★★ A PLAIN MODE IS ONE TIGHT WORD (2026-10-06, Stuart's VCR screenshot read "WF M"). It used to sit right-aligned
   *  in the demod's cells with the colon electrode ghosted after them, so that starting a decoder never moved "AM"
   *  sideways — but " AM" then opened on a dark cell, and the split layout's furniture showed in a mode that has no
   *  split. Now it starts at the first cell, and the split (and its colon) exist only while a decoder runs. */
  return { cells, colon: false, split: false, stereoSlot: stereo, textCells, marquee: cells.length > textCells };
}

export interface SegReadingCells {
  /** Six cells over SEG_READ_GHOST; '' = dark. Each cell's lit glyphs (two for the "-1" half-digit). */
  cells: string[];
  dB: boolean;
  F: boolean;
  S: boolean;
}

const DARK: SegReadingCells = { cells: ['', '', '', '', '', ''], dB: false, F: false, S: false };

/**
 * The reading text (meters.ts formatReading: "S9+27", "S7", "-73dB", "24db", "120 dBf") → the readout's cells.
 * ★ 'S' is drawn as a 5 — the "8" electrode has no other S. `unit` is the readout setting; a text that says
 *   dBf (FM-DX's static label) is dBf whatever the setting.
 */
export function segReadingCells(text: string, unit: 'snr' | 'smeter' | 'dbfs' | 'dbf'): SegReadingCells {
  const t = text.trim();
  const s = /^S(\d)(?:\+(\d+))?$/i.exec(t);
  if (s) {
    const over = s[2] ? Math.min(99, parseInt(s[2], 10)) : 0;
    const cells = ['', '5', s[1], '', '', ''];
    if (over > 0) {
      cells[3] = '+';
      if (over >= 10) cells[4] = String(Math.floor(over / 10));
      cells[5] = String(over % 10);
    }
    return { cells, dB: over > 0, F: false, S: false };
  }
  const n = /^(-?\d+)\s*(dBf|dB)?$/i.exec(t);
  if (!n) return DARK;
  const u = /^dBf$/.test(n[2] ?? '') ? 'dbf' : unit === 'smeter' ? 'dbfs' : unit;
  const v = parseInt(n[1], 10);
  const neg = v < 0;
  // ★ 199 is the field's reach (the "±1" half-digit); nothing the meters produce comes near it.
  const a = Math.min(199, Math.abs(v));
  const cells = ['', '', String(a % 10), '', '', ''];
  if (a >= 10) cells[1] = String(Math.floor(a / 10) % 10);
  if (a >= 100) cells[0] = neg ? '-1' : '1';
  else if (neg) { if (a >= 10) cells[0] = '-'; else cells[1] = '-'; }
  return { cells, dB: true, F: u === 'dbfs' || u === 'dbf', S: u === 'dbfs' };
}
