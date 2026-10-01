/* ★★★ THE STATION LINE — ONE SENTENCE, ONE MARQUEE, BOTH CLIENTS (2026-10-01).
 *
 *  Stuart: "both VTS should scroll PI: C363 / BBC Radio6Music: A Tribe Called Quest - Can I Kick
 *  it?" The web VTS was five separate sections (PI chip, name, RadioText, band…) that each fought
 *  for width, and on a phone the one carrying the actual message got about one character. The app
 *  had it right — marks static on the left, "Name: RadioText" scrolling as a whole — except that it
 *  never showed the PI at all, which is the station's unambiguous identity (the key a database
 *  lookup, a learned station and the FM-DX dial are all keyed on).
 *
 *  So the SCROLLING part is now one line on both platforms, composed HERE so the two cannot drift:
 *      PI: C363 / BBC Radio6Music: A Tribe Called Quest - Can I Kick it?
 *  ★ Every part is optional and a missing one leaves NO PUNCTUATION behind: no dangling "/", no
 *    trailing ":". PI only → "PI: C363"; no PI → "Name: RadioText"; RadioText only → the text.
 *  ★ DAB has no PI. Its equivalent is the SERVICE ID, labelled "SId" — it is a different code from a
 *    different system, and calling it a PI would invite someone to look it up as one.
 *
 *  Pure and dependency-free: the web client imports it straight from src/, and
 *  scripts/test_vtsLine.ts is its executable spec. */

/** What identifies the station in the line: an RDS PI, or a DAB service ID. */
export type VtsIdLabel = 'PI' | 'SId';

export interface VtsLineParts {
  /** The code, already as the four hex digits people read ("C363"). Empty/undefined = not known. */
  id?: string;
  idLabel?: VtsIdLabel;
  /** The station's name — RDS PS, or the DAB service label. */
  name?: string;
  /** The live message — RDS RadioText, or DAB DLS. */
  text?: string;
}

/** Between the identity and the station. */
export const VTS_ID_SEP = ' / ';
/** Between the name and its message. */
export const VTS_TEXT_SEP = ': ';
/** ★ The 14-segment strip has no colon — DSEG14 maps `:` to `-` (displayText SEG_MAP) — so on that
 *  display the label is followed by a SPACE ("PI C363", as a real segment radio prints it) and the
 *  name/message joint is a spaced dash rather than a glued "NAME- TEXT". */
export const VTS_TEXT_SEP_SEG = ' - ';

/** Four upper-case hex digits for a 16-bit code, '' for none (≤ 0, NaN, out of range). */
export function vtsHex(code: number | undefined | null): string {
  if (typeof code !== 'number' || !Number.isFinite(code) || code <= 0 || code > 0xFFFFFFFF) return '';
  return Math.round(code).toString(16).toUpperCase().padStart(4, '0');
}

/** "PI: C363" (or "PI C363" on a segment display), '' when there is no code. */
export function vtsIdText(id: string | undefined, label: VtsIdLabel = 'PI', seg = false): string {
  const code = (id ?? '').trim();
  if (!code) return '';
  return seg ? `${label} ${code}` : `${label}: ${code}`;
}

/** "Name: RadioText" — either half alone when the other is missing; the message is dropped when it
 *  merely REPEATS the name (some stations send their name as their only RadioText). */
export function vtsStationText(name: string | undefined, text: string | undefined, seg = false): string {
  const n = (name ?? '').trim();
  const t = (text ?? '').trim();
  if (!t || t === n) return n;
  if (!n) return t;
  return n + (seg ? VTS_TEXT_SEP_SEG : VTS_TEXT_SEP) + t;
}

/** Identity and station joined — either alone when the other is missing. */
export function vtsJoin(idText: string, station: string): string {
  const a = idText.trim(), b = station.trim();
  return a && b ? a + VTS_ID_SEP + b : a || b;
}

/** One run of the line, tagged with what it IS so each client can colour it (the identity dim, the
 *  name in the on-tune colour, the message in the text colour) without re-deciding the punctuation. */
export interface VtsSegment { kind: 'id' | 'sep' | 'name' | 'text'; s: string }

/**
 * The line as tagged runs. ★ Built FROM vtsIdText / vtsStationText rather than beside them, so the
 * coloured rendering and the plain string are the same sentence by construction — joining the runs
 * gives exactly vtsLine(), which the test asserts.
 */
export function vtsLineSegments(p: VtsLineParts, opts: { seg?: boolean } = {}): VtsSegment[] {
  const seg = !!opts.seg;
  const out: VtsSegment[] = [];
  const idText = vtsIdText(p.id, p.idLabel ?? 'PI', seg);
  const station = vtsStationText(p.name, p.text, seg);
  if (idText) out.push({ kind: 'id', s: idText });
  if (idText && station) out.push({ kind: 'sep', s: VTS_ID_SEP });
  if (station) {
    const n = (p.name ?? '').trim();
    const t = (p.text ?? '').trim();
    if (station === n) out.push({ kind: 'name', s: n });
    else if (!n) out.push({ kind: 'text', s: t });
    else {
      out.push({ kind: 'name', s: n });
      out.push({ kind: 'sep', s: seg ? VTS_TEXT_SEP_SEG : VTS_TEXT_SEP });
      out.push({ kind: 'text', s: t });
    }
  }
  return out;
}

/** The whole scrolling line as plain text. */
export function vtsLine(p: VtsLineParts, opts: { seg?: boolean } = {}): string {
  return vtsJoin(vtsIdText(p.id, p.idLabel ?? 'PI', !!opts.seg), vtsStationText(p.name, p.text, !!opts.seg));
}

// ── What the web bar drops to fit ────────────────────────────────────────────

/**
 * ★★★ THE STATIC MARKS GIVE WAY TO THE MESSAGE, IN THIS ORDER (Stuart, 2026-10-01): the band label
 *   ("DAB / DAB+ (Band III)") first — it is the one thing on the pill also written across the top of
 *   the screen at all times — then the RDS mark, then the flag. The station logo and the scrolling
 *   line are never dropped: the logo is the station, the line is why you are looking.
 *   ★ The PI is no longer a chip that could be dropped: it rides IN the line, which always scrolls.
 */
export const VTS_DROP_ORDER = ['band', 'rds', 'flag'] as const;
export type VtsDroppable = typeof VTS_DROP_ORDER[number];

export interface VtsFitSpec {
  /** The pill's content width, px (its width minus its padding). */
  available: number;
  /** Everything that never drops, INCLUDING the line's minimum readable width and the gaps between
   *  those items. */
  fixed: number;
  /** The flex gap the pill puts before each further item. */
  gap: number;
  /** Measured natural width of each droppable item that is PRESENT; 0 / missing = not showing. */
  widths: Partial<Record<VtsDroppable, number>>;
}

export interface VtsFit { step: number; hidden: Set<VtsDroppable> }

export function vtsFitState(step: number): VtsFit {
  return { step, hidden: new Set(VTS_DROP_ORDER.slice(0, Math.max(0, step))) };
}

/** Does the pill fit in state `st`? (Half a pixel of rounding slack, as statusFits.) */
export function vtsFits(spec: VtsFitSpec, st: VtsFit): boolean {
  let need = spec.fixed;
  for (const k of VTS_DROP_ORDER) {
    const w = spec.widths[k] ?? 0;
    if (w > 0 && !st.hidden.has(k)) need += w + spec.gap;
  }
  return need <= spec.available + 0.5;
}

function vtsMinStep(spec: VtsFitSpec): number {
  for (let k = 0; k < VTS_DROP_ORDER.length; k++) if (vtsFits(spec, vtsFitState(k))) return k;
  return VTS_DROP_ORDER.length;
}

/**
 * ★★ MEASURED, NEVER SQUEEZED — the landscape status row's rule (displayText statusFit) applied to
 *   the station pill: the fewest drops, strictly in VTS_DROP_ORDER, at which the measured items plus
 *   the line's minimum fit. No breakpoint: a narrow pill with no flag and no band never drops the
 *   RDS mark it has room for, and a wide one with a long band label still does what it must.
 * ★ HYSTERESIS as statusFit: dropping more happens at once (nothing may overflow); bringing an item
 *   back needs `hysteresis` px to spare, so a flag arriving or a resize by a pixel cannot make the
 *   RDS mark flap. Pass the previous result's step as `prevStep`.
 */
export function vtsFit(spec: VtsFitSpec, opts: { prevStep?: number; hysteresis?: number } = {}): VtsFit {
  const k = vtsMinStep(spec);
  const prev = opts.prevStep;
  if (prev === undefined || k >= prev) return vtsFitState(k);
  const relaxed = vtsMinStep({ ...spec, available: spec.available - (opts.hysteresis ?? 0) });
  return vtsFitState(Math.min(prev, relaxed));
}
