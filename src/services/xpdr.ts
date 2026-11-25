// xpdr.ts — ★★★ beta94 THE TRANSPONDER SIGNAL FINDER, the clients' half (both: the app and the web page —
// docs/v12/TRANSPONDER-FINDER.md). The server's "xpdr" decoder (vibe_xpdr_run.h) sends {"type":"xpdr_status","data":…}
// once a second; this parses it DEFENSIVELY (anything off the wire is bounded and type-checked), says what each row is in
// words, lays out the waterfall overlay (ORBCOMM's: the slice shaded, an arrow per signal — src/utils/orbcommView.ts) and
// says what LISTEN does: tune VFO A to the row's dial in USB (its sideband) and ask the finder to follow it.
//
// Stuart, 2026-11-24 (RS-44): "present it the same way as orbcomm a large wide vfo covering the whole receive range then
// the vfo a that snaps to the chosen voice signal, CW simply decodes" · "the little arrows you added for orbcom to show the
// signals being detected" · "if a user sees a signal that we havent detected they can tune to it manually".
// ★ Pure — scripts/test_xpdr_app.ts.
import type { OrbOverlay } from '../utils/orbcommView';

export type XpdrKind = 'cw' | 'voice';
export interface XpdrRow {
  id: string;
  kind: XpdrKind;
  /** RF now (Hz) — where the arrow goes */
  hz: number;
  /** satellite frame (Hz) — where LISTEN puts the dial (a voice's suppressed carrier, a CW tone) */
  dial: number;
  snr: number;
  /** seconds since last heard */
  ago: number;
  on: boolean;
  beacon: boolean;
  call: string;
  text: string;
  wpm: number;
}
export interface XpdrFollow { id: string; src: 'wait' | 'guess' | 'voice' | 'cw' | 'held' | ''; off: number }
export interface XpdrStatus {
  slice: { lo: number; hi: number } | null;
  span: { lo: number; hi: number } | null;
  beacon: number;
  dop: number;
  rows: XpdrRow[];
  follow: XpdrFollow | null;
}

/** A row is drawn FADED after this long unheard (Stuart: "a row fades after a minute of silence"). */
export const XPDR_FADE_SEC = 60;
/** The overlay's label — ORBCOMM's style, the finder's words. */
export const XPDR_SLICE_LABEL = 'TRANSPONDER · every signal found here';
/** The SATELLITE box's plain line for a transponder pick. */
export const XPDR_HINT = 'Every CW signal is decoded and every voice gets LISTEN. Tap the waterfall to tune anywhere else.';
export const XPDR_MAX_ROWS = 40;

const num = (v: unknown, d = 0): number => (typeof v === 'number' && Number.isFinite(v) ? v : d);
const str = (v: unknown, max: number): string => (typeof v === 'string' ? v.replace(/[^\x20-\x7e]/g, '').slice(0, max) : '');

/** {"type":"xpdr_status","data":…} → a status, or null when it is not one. */
export function parseXpdrStatus(d: unknown): XpdrStatus | null {
  if (!d || typeof d !== 'object') return null;
  const o = d as Record<string, unknown>;
  const box = (v: unknown) => {
    if (!v || typeof v !== 'object') return null;
    const b = v as Record<string, unknown>;
    const lo = num(b.lo), hi = num(b.hi);
    return lo > 0 && hi > lo && hi - lo < 2e6 ? { lo, hi } : null;
  };
  const rows: XpdrRow[] = [];
  if (Array.isArray(o.rows)) {
    for (const r of o.rows.slice(0, XPDR_MAX_ROWS)) {
      if (!r || typeof r !== 'object') continue;
      const x = r as Record<string, unknown>;
      const id = str(x.id, 12);
      const kind = x.kind === 'voice' ? 'voice' : x.kind === 'cw' ? 'cw' : null;
      const hz = num(x.hz), dial = num(x.dial);
      if (!id || !kind || !(hz > 0) || !(dial > 0)) continue;
      rows.push({ id, kind, hz, dial, snr: num(x.snr), ago: Math.max(0, num(x.ago)), on: x.on === true, beacon: x.beacon === true,
                  call: str(x.call, 12).replace(/[^A-Za-z0-9/]/g, '').toUpperCase(), text: str(x.text, 80), wpm: Math.max(0, Math.round(num(x.wpm))) });
    }
  }
  let follow: XpdrFollow | null = null;
  if (o.follow && typeof o.follow === 'object') {
    const f = o.follow as Record<string, unknown>;
    const src = (['wait', 'guess', 'voice', 'cw', 'held'] as const).find((s) => s === f.src) ?? '';
    const id = str(f.id, 12);
    if (id) follow = { id, src, off: num(f.off) };
  }
  return { slice: box(o.slice), span: box(o.span), beacon: num(o.beacon), dop: num(o.dop), rows, follow };
}

/** Is this row FADED (not heard for a minute)? */
export const xpdrFaded = (r: XpdrRow): boolean => !r.on && r.ago >= XPDR_FADE_SEC;

/** The rows in the order the box lists them: on air now first (voices before CW, strongest first), then the rest by
 *  how recently heard. The beacon is one more CW row (Stuart: "the beacon is just one more CW row"). */
export function xpdrRowsSorted(rows: XpdrRow[]): XpdrRow[] {
  return rows.slice().sort((a, b) => {
    if (a.on !== b.on) return a.on ? -1 : 1;
    if (a.on && a.kind !== b.kind) return a.kind === 'voice' ? -1 : 1;
    return a.on ? b.snr - a.snr : a.ago - b.ago;
  });
}

/** The row's frequency in words: "435.6220" (MHz, satellite frame — what the dial shows after LISTEN). */
export const xpdrFreqText = (r: XpdrRow): string => (r.dial / 1e6).toFixed(4);
/** What the row is, in words: "VOICE", "CW 22 wpm G4ABC", "BEACON". */
export function xpdrWhat(r: XpdrRow): string {
  if (r.kind === 'voice') return 'VOICE';
  const parts = [r.beacon ? 'BEACON' : 'CW'];
  if (r.wpm > 0) parts.push(`${r.wpm} wpm`);
  if (r.call) parts.push(r.call);
  return parts.join(' ');
}
/** When it was last heard: "now", "12 s ago", "3 min ago". */
export function xpdrAgo(r: XpdrRow): string {
  if (r.on) return 'now';
  if (r.ago < 60) return `${Math.round(r.ago)} s ago`;
  return `${Math.round(r.ago / 60)} min ago`;
}
/** The follow in words for the box ("VFO A follows · measured from the voice"), '' = not following. */
export function xpdrFollowText(f: XpdrFollow | null): string {
  if (!f) return '';
  switch (f.src) {
    case 'wait': return 'LISTEN: tuning…';
    case 'guess': return 'VFO A follows · predicted from the pass';
    case 'voice': return 'VFO A follows · measured from the voice';
    case 'cw': return 'VFO A follows · locked to the tone';
    case 'held': return 'VFO A follows · held between overs';
    default: return '';
  }
}

/** ★ LISTEN = tune VFO A to the row (a voice's carrier in USB — the downlink's sideband; a CW row in CW) and ask the
 *  finder to follow it. Returns what to send. Only voices have LISTEN (Stuart: "CW simply decodes"); a CW row returns
 *  null. */
export function xpdrListen(r: XpdrRow, sideband: 'usb' | 'lsb' = 'usb'): { hz: number; mode: string; follow: { type: 'xpdr_follow'; id: string } } | null {
  if (r.kind !== 'voice') return null;
  return { hz: Math.round(r.dial), mode: sideband, follow: { type: 'xpdr_follow', id: r.id } };
}

/** The waterfall overlay — ORBCOMM's (src/utils/orbcommView.ts): the slice shaded and labelled, an arrow per signal at its
 *  RF, labelled CW / VOICE (the call once known), solid while heard, hollow when faded. null = nothing to draw. */
export function xpdrOverlayFrom(st: XpdrStatus | null | undefined): OrbOverlay | null {
  if (!st || !st.slice) return null;
  const carriers = xpdrRowsSorted(st.rows)
    .filter((r) => r.hz > 0)
    .slice(0, 16)
    .map((r) => ({ rf: r.hz, locked: r.on || !xpdrFaded(r), label: r.kind === 'voice' ? 'VOICE' : r.call || (r.beacon ? 'BEACON' : 'CW') }));
  return { lo: st.slice.lo, hi: st.slice.hi, carriers, title: XPDR_SLICE_LABEL };
}
