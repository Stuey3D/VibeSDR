/**
 * The status row, modelled for the faceplate tests (2026-10-06): every run's width at a window's status size, then
 * ControlsBar's own fit — row 9's statusFit in landscape, PortraitStats' drop order in portrait — at every width from
 * an SE in Display Zoom to a full-screen Mac. Shared by test_faceplate_segfield.ts (VCR), test_faceplate_dotfield.ts
 * (DOT) and test_faceplate_screenfont.ts (★ Nixie One and Atkinson — the status row takes the display's font now).
 *
 * ★ VCR / DOT widths are the cells' ARITHMETIC (n cells × the fixed pitch) — exactly what StatusRun lays out and what
 *   the measuring twin reports. ★ Nixie / Hyper widths are the TTF's own advances (lib_font_metrics, no kerning —
 *   it errs wide) with StatusText's 0.4 letter-spacing and the row's drawn pieces (the node icon, the bars, the
 *   gain arrow, the DSP pills) at ControlsBar's sizes. The window's own margins are an estimate (EDGE below).
 */
import { statusFit, statusFits, statusState, type StatusItem, type StatusRowSpec } from '../src/constants/displayText.ts';
import { segFieldCells } from '../src/constants/displayText.ts';
import { dotChar } from '../src/constants/dotField.ts';
import {
  statusDotSlots, statusParts, statusRunWidth, statusSegFs, statusSegSlots, statusTags, type StatusPart, type StatusSlots,
} from '../src/constants/statusField.ts';
import { DOTO, HYPER, NIXIE } from './lib_font_metrics.ts';

export type Face = 'seg' | 'dot';
/** ★ A TEXT face (2026-10-06): the status row in the display's font, not in cells. 'doto' is the Doto run every metal
 *  Display drew before (RC18) — kept for the comparison. */
export type TextFace = 'nixie' | 'hyper' | 'doto';
export type RowFace = Face | TextFace;
const isText = (f: RowFace): f is TextFace => f === 'nixie' || f === 'hyper' || f === 'doto';
export const segCells = segFieldCells;
export const slotsFor = (face: Face, parts: StatusPart[]): StatusSlots =>
  face === 'seg' ? statusSegSlots(parts, segCells) : statusDotSlots(parts, dotChar);

/** useUiScale's scale, and StatusWell's status size: Doto 12 × scale, 10 pt floor. */
export const scaleFor = (W: number, landscape: boolean) =>
  landscape ? Math.max(0.58, Math.min(1.45, W / 926)) : Math.max(0.75, Math.min(1.45, W / 390));
export const statusSize = (W: number, landscape: boolean, face: RowFace = 'dot', chassis: Chassis = 'metal') => {
  if (chassis === 'metal') return Math.max(10, 12 * scaleFor(W, landscape));     // 12 pt on every Display
  // ★ The default chassis (2026-10-06, statusDisplayFor): the footer's own CLOCK_FONT — portrait s.f(8), landscape
  //   max(9, s.f(7)) — in the display's font; the cells keep their 10 pt floor.
  const clock = landscape ? Math.max(9, 7 * scaleFor(W, true)) : 8 * scaleFor(W, false);
  return isText(face) ? clock : Math.max(10, clock);
};
export type Chassis = 'metal' | 'default';

/** The worst case the row can show: a shared server with listeners, recording, all three DSP badges, a fast rate,
 *  a stepping gain, the IF filter. */
export const WORST = {
  utc: '17:03 UTC', srv: '18:03 BST', srvShort: '18:03 +1', rec: '0:12:34',
  shared: 'SHARED TUNER · ASK TO TUNE · 12/20 👤', sharedShort: 'SHARED',
  dsp: ['NR', 'NB', 'AN'], rate: '123k/s 25fps', gain: { label: 'GAIN', dir: 'down' as const, value: '44.5dB' },
  if: '· IF 1400k auto',
};

/** Each item's run, as ControlsBar builds it. */
export function runs(face: Face): Record<string, StatusPart[]> {
  const t = statusTags(WORST.dsp, face);
  return {
    utc: statusParts(WORST.utc),
    localTime: [{ kind: 'node' }, ...statusParts(WORST.srv)],
    localTimeShort: [{ kind: 'node' }, ...statusParts(WORST.srvShort)],
    rec: [{ kind: 'rec' }, ...statusParts(WORST.rec)],
    shared: statusParts(WORST.shared),
    sharedShort: statusParts(WORST.sharedShort),
    dsp: [t.text],
    meter: [{ kind: 'bars', q: 3 }],
    linkIconsB: [{ kind: 'node' }],
    rate: statusParts(WORST.rate),
    gain: [...statusParts('· ' + WORST.gain.label), { kind: 'arrow', dir: WORST.gain.dir }, ...statusParts(WORST.gain.value)],
    if: statusParts(WORST.if),
  };
}

/** ★ A text face's item widths (pt) — ControlsBar's sd-text path: StatusText (letter-spacing 0.4), SectionIcon
 *  round(1.1 × size) + its 4 pt gap, LinkBars (3 × 3 + 2 × 1.5), GainArrow (11/12 × size), the DSP pills (pm.dspTag:
 *  11 pt, spacing 1, 5 + 5 padding, 1 + 1 border, 6 apart), the recording dot (5 + 3 gap). */
export function textWidths(face: TextFace, size: number): Record<string, number> {
  const f = face === 'nixie' ? NIXIE() : face === 'doto' ? DOTO() : HYPER();
  const t = (s: string) => f.width(s, size, 0.4);
  const icon = Math.round(size * 1.1) + 4;
  const tag = (s: string) => f.width(s, 11, 1) + 12;
  return {
    utc: t(WORST.utc), localTime: icon + t(WORST.srv), localTimeShort: icon + t(WORST.srvShort),
    rec: 5 + 3 + t(WORST.rec), shared: t(WORST.shared), sharedShort: t(WORST.sharedShort),
    dsp: WORST.dsp.reduce((a, d, i) => a + tag(d) + (i ? 6 : 0), 0),
    meter: 3 * 3 + 2 * 1.5, linkIconsB: Math.round(size * 1.1),
    rate: t(WORST.rate), gain: t('· ' + WORST.gain.label) + (11 / 12) * size + t(WORST.gain.value), if: t(WORST.if),
  };
}

/** Item widths (pt) at a status size. The DSP run on VCR carries its frame padding (StatusField framePad + 0.5). */
export function widths(face: RowFace, size: number): Record<string, number> {
  if (isText(face)) return textWidths(face, size);
  const out: Record<string, number> = {};
  for (const [k, parts] of Object.entries(runs(face))) {
    out[k] = statusRunWidth(face, slotsFor(face, parts).cells.length, size);
    if (k === 'dsp' && face === 'seg') out[k] += 2 * (Math.min(2.5, statusSegFs(size) * 0.25) + 0.5);
  }
  return out;
}

/** ★ ESTIMATE: what the bar and the window take outside the row (the bar's side padding, the recessed window's lip,
 *  the row's own 4 pt padding) — and a notched phone's landscape safe area, which the screen applies. */
const EDGE = (W: number, landscape: boolean) => 2 * 12 + 2 * 4 + (landscape && W >= 800 && W < 1000 ? 2 * 47 : 0);

export interface LandscapeFit { W: number; size: number; avail: number; hidden: StatusItem[]; packed: boolean; fits: boolean;
  sharedShort: boolean }

/** LandscapeStatus' fit at window width W (shared server, recording, DSP on, a reading in every item). */
export function landscapeFit(face: RowFace, W: number, chassis: Chassis = 'metal'): LandscapeFit {
  const size = statusSize(W, true, face, chassis);
  const w = widths(face, size);
  const avail = W - EDGE(W, true);
  const specFor = (noUtc: boolean): StatusRowSpec => ({
    sectionGap: 8, sharedShort: w.sharedShort,
    left: [{ item: 'utc', width: noUtc ? 0 : w.utc, lead: 0 },
           { item: 'localTime', width: noUtc ? w.localTimeShort : w.localTime, lead: 4 },
           { item: 'rec', width: w.rec, lead: 8 }],
    centre: [{ item: 'shared', width: w.shared, lead: 8 }, { item: 'dsp', width: w.dsp, lead: 8 }],
    // The plate's row: no phone ⇄ icons; on a shared server SHARED TUNER replaces the node icon.
    right: [{ item: null, width: w.meter, lead: 4 }, { item: 'rate', width: w.rate, lead: 4 },
            { item: 'gain', width: w.gain, lead: 4 }, { item: 'if', width: w.if, lead: 4 }],
  });
  const wide = statusFits(avail - 8, specFor(false), statusState(0));
  const spec = specFor(!wide);
  const fit = statusFit(avail, spec, { hysteresis: 8, centreFirst: 3 });
  const hidden = [...fit.hidden];
  if (!wide) hidden.unshift('utc');
  return { W, size, avail, hidden: [...new Set(hidden)], packed: fit.packed, fits: statusFits(avail, spec, fit),
           sharedShort: fit.sharedShort };
}

/** PortraitStats' drop order and its fit at window width W (the stats line: bars, rate, gain, IF). */
const PORTRAIT_ORDER = ['if', 'gain', 'linkIcons', 'rate'] as const;
export function portraitStatsFit(face: RowFace, W: number, chassis: Chassis = 'metal'): { W: number; size: number; hidden: string[]; fits: boolean } {
  const size = statusSize(W, false, face, chassis);
  const w = widths(face, size);
  const avail = W - EDGE(W, false);
  const width = (k: number) => {
    const drop = new Set(PORTRAIT_ORDER.slice(0, k));
    const parts = [w.meter, drop.has('linkIcons') ? 0 : w.linkIconsB, drop.has('rate') ? 0 : w.rate,
                   drop.has('gain') ? 0 : w.gain, drop.has('if') ? 0 : w.if].filter(v => v > 0);
    return parts.reduce((a, b) => a + b, 0) + 4 * (parts.length - 1);
  };
  let k = 0;
  while (k < PORTRAIT_ORDER.length && width(k) > avail) k++;
  return { W, size, hidden: PORTRAIT_ORDER.slice(0, k) as unknown as string[], fits: width(k) <= avail };
}

/** Portrait row 4 (it never drops — it shrinks): both clocks, the recording timer and the DSP badges. */
export function portraitClockRow(face: RowFace, W: number, chassis: Chassis = 'metal'): { W: number; need: number; avail: number } {
  const size = statusSize(W, false, face, chassis);
  const w = widths(face, size);
  const need = w.utc + 4 + w.localTime + 8 + w.rec + 8 + w.dsp;
  return { W, need, avail: W - EDGE(W, false) };
}

export const LANDSCAPE_WIDTHS = [568, 667, 844, 926, 1180, 1366, 1920];
export const PORTRAIT_WIDTHS = [320, 375, 390, 430, 768, 1024];
