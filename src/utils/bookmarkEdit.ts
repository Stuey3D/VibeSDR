/**
 * ★★ EDITING A BOOKMARK — one set of rules for the app and the web client (V12, GitHub discussion #59, Gesiima).
 *
 * Two lists, two owners, ONE form:
 *   • YOUR bookmarks (this device / this browser) — always yours to edit;
 *   • the SERVER's bookmarks — the owner's list, shared with every listener, so editing one is admin-only exactly as
 *     adding and deleting are (the server refuses anyone else; the clients never draw the button for them).
 * The fields are the ones the discussion asked for: name, frequency, mode and the passband (bandwidth_low/high, Hz
 * offsets from the carrier) — or "the mode's default", which is no edges at all.
 *
 * ★ The rules MIRROR the server's (android/app/src/main/cpp/vibe_bm_edit.h) so the form refuses before it sends, in the
 *   same words; the server still checks, and its words are shown as they come when it says no.
 * ★ Pure: no React, no DOM, no storage — scripts/test_bookmark_edit.ts runs it as it is.
 */
import type { UserBookmark } from '../services/userBookmarks';

/** The demodulators a VibeServer stores on a bookmark — its own list (vsModeKnown), mirrored so the picker never offers
 *  a mode the save would refuse. A bookmark may be for another radio, so this is NOT narrowed to what this receiver
 *  can play right now. */
export const BOOKMARK_MODES: readonly string[] = [
  'am', 'sam', 'usb', 'lsb', 'cw', 'cwu', 'cwl', 'nfm', 'fm', 'wfm', 'dab', 'drm',
  'sau', 'sal', 'ecssu', 'ecssl', 'amst', 'fmdx',
];
/** Passband edges are within this many Hz of the carrier (the server's kMaxEdgeHz). */
export const BM_EDGE_MAX_HZ = 500_000;
/** The highest frequency a bookmark may carry (the server's kMaxHz, 100 GHz). */
export const BM_MAX_HZ = 100e9;
/** The longest name (the server caps at 64 bytes; the app's name box at 60 characters). */
export const BM_NAME_MAX = 60;

export type FreqUnit = 'mhz' | 'khz' | 'hz';

/** What the form holds — text as typed, so a half-typed value is never coerced behind the user's back. */
export interface BookmarkEditForm {
  name: string;
  freqText: string;
  unit: FreqUnit;
  mode: string;
  /** true = no edges: the mode's default passband. */
  defaultPassband: boolean;
  lowText: string;
  highText: string;
}

/** A checked edit, ready to save. No edges = the mode's default. */
export interface BookmarkEditValue {
  name: string;
  frequency: number;
  mode: string;
  bandwidth_low: number | null;
  bandwidth_high: number | null;
}

export type BookmarkEditResult = { ok: true; value: BookmarkEditValue } | { ok: false; error: string };

/** "100,5" (a European decimal pad) and "100.5" both mean 100.5; "1,234.5" (pasted) drops the thousands comma. */
function decimal(s: string): string {
  const t = s.trim();
  if (t.includes(',') && t.includes('.')) return t.replace(/,/g, '');
  return t.replace(/,/g, '.');
}

/** The frequency box: a number in `unit`, or with its own unit ("7150k", "14.074 MHz", "198 kHz", "96100000 Hz").
 *  Null when it is not a frequency. */
export function parseBookmarkFrequency(text: string, unit: FreqUnit): number | null {
  const m = decimal(text).match(/^(\d+(?:\.\d*)?|\.\d+)\s*(ghz|mhz|khz|hz|g|m|k)?$/i);
  if (!m) return null;
  const n = parseFloat(m[1]);
  if (!Number.isFinite(n) || n <= 0) return null;
  const u = (m[2] || unit).toLowerCase();
  const mult = u.startsWith('g') ? 1e9 : u.startsWith('m') ? 1e6 : u.startsWith('k') ? 1e3 : 1;
  return Math.round(n * mult);
}

/** The frequency in the box's unit, without trailing zeros past what the value needs ("96.1", "7150", "198"). */
export function formatBookmarkFrequency(hz: number, unit: FreqUnit): string {
  if (!(hz > 0)) return '';
  if (unit === 'hz') return String(Math.round(hz));
  const v = unit === 'khz' ? hz / 1e3 : hz / 1e6;
  return String(Number(v.toFixed(unit === 'khz' ? 3 : 6)));
}

/** A whole number of Hz, sign allowed ("-3000", "+3000"); null otherwise. */
function edge(text: string): number | null {
  const t = text.trim();
  if (!/^[+-]?\d+$/.test(t)) return null;
  const n = parseInt(t, 10);
  return Number.isFinite(n) ? n : null;
}

/** The form's rules. `modes` = the list the picker offered (BOOKMARK_MODES unless the caller knows better). */
export function checkBookmarkEdit(f: BookmarkEditForm, modes: readonly string[] = BOOKMARK_MODES): BookmarkEditResult {
  const name = String(f.name || '').replace(/\s+/g, ' ').trim();
  if (!name) return { ok: false, error: 'The bookmark needs a name.' };
  if (name.length > BM_NAME_MAX) return { ok: false, error: `A name is at most ${BM_NAME_MAX} characters.` };
  const hz = parseBookmarkFrequency(f.freqText, f.unit);
  if (hz === null || !(hz >= 1)) return { ok: false, error: 'Type the frequency as a number (e.g. 96.1 MHz or 7150 kHz).' };
  if (hz > BM_MAX_HZ) return { ok: false, error: 'That frequency is past 100 GHz — check the units.' };
  const mode = String(f.mode || '').trim().toLowerCase();
  if (!mode) return { ok: false, error: 'Choose a mode for the bookmark.' };
  if (!modes.includes(mode)) return { ok: false, error: `"${mode.toUpperCase()}" is not a mode a bookmark can use.` };
  if (f.defaultPassband) return { ok: true, value: { name, frequency: hz, mode, bandwidth_low: null, bandwidth_high: null } };
  const lo = edge(f.lowText), hi = edge(f.highText);
  if (lo === null || hi === null) return { ok: false, error: 'Passband edges are whole hertz from the carrier (e.g. -5000 and 5000).' };
  if (Math.abs(lo) > BM_EDGE_MAX_HZ || Math.abs(hi) > BM_EDGE_MAX_HZ)
    return { ok: false, error: 'Passband edges must be within ±500000 Hz of the carrier.' };
  if (hi <= lo) return { ok: false, error: "The passband's low edge must be below its high edge." };
  return { ok: true, value: { name, frequency: hz, mode, bandwidth_low: lo, bandwidth_high: hi } };
}

/** A bookmark (any shape we hold) into the form, in the unit the client shows frequencies in. */
export function bookmarkEditForm(
  b: { name: string; frequency: number; mode?: string | null; bandwidth_low?: number | null; bandwidth_high?: number | null },
  unit: FreqUnit,
): BookmarkEditForm {
  const lo = b.bandwidth_low, hi = b.bandwidth_high;
  const has = typeof lo === 'number' && typeof hi === 'number' && Number.isFinite(lo) && Number.isFinite(hi) && hi > lo;
  return {
    name: b.name || '', freqText: formatBookmarkFrequency(b.frequency, unit), unit,
    mode: String(b.mode || 'am').toLowerCase(), defaultPassband: !has,
    lowText: has ? String(Math.round(lo as number)) : '', highText: has ? String(Math.round(hi as number)) : '',
  };
}

/** "Use what I'm listening to now": the dial's frequency, mode and passband into the form; the NAME is left alone
 *  (the use case is a small correction to a bookmark already named). A dial with no passband sets the mode default. */
export function fillFromDial(
  f: BookmarkEditForm,
  dial: { frequency: number; mode: string; bandwidthLow?: number | null; bandwidthHigh?: number | null },
): BookmarkEditForm {
  const lo = dial.bandwidthLow, hi = dial.bandwidthHigh;
  const has = typeof lo === 'number' && typeof hi === 'number' && Number.isFinite(lo) && Number.isFinite(hi) && hi > lo;
  return {
    ...f,
    freqText: formatBookmarkFrequency(dial.frequency, f.unit),
    mode: String(dial.mode || f.mode).toLowerCase(),
    defaultPassband: !has,
    lowText: has ? String(Math.round(lo as number)) : '',
    highText: has ? String(Math.round(hi as number)) : '',
  };
}

/** The modes the picker offers: the bookmark list, plus the row's own mode if it is something else (an import from
 *  another SDR), so opening the form never silently changes it — saving such a row asks for a mode the server takes. */
export function bookmarkModeChoices(current: string, modes: readonly string[] = BOOKMARK_MODES): string[] {
  const c = String(current || '').toLowerCase();
  return c && !modes.includes(c) ? [...modes, c] : [...modes];
}

/** The query for POST /bookmarks that EDITS the server row `old` (no leading "?" or "&", no credentials). */
export function serverEditQuery(old: { frequency: number; sid?: number | null }, v: BookmarkEditValue): string {
  return `was=${Math.round(old.frequency)}`
    + (typeof old.sid === 'number' && old.sid > 0 ? `&was_sid=${old.sid}` : '')
    + `&frequency=${Math.round(v.frequency)}&name=${encodeURIComponent(v.name)}&mode=${encodeURIComponent(v.mode)}`
    + (v.bandwidth_low !== null && v.bandwidth_high !== null
      ? `&bandwidth_low=${Math.round(v.bandwidth_low)}&bandwidth_high=${Math.round(v.bandwidth_high)}` : '');
}

/** The server's 1 kHz grouping (bmKey): two frequencies in the same bucket are the same bookmark to it. */
const bmKey = (hz: number) => Math.round(hz / 1000) * 1000;

/** ★ AN OLDER SERVER IGNORES `was`: it ADDS the new row and leaves the old one. True when the list it answered with still
 *  holds the old row after a move — the caller then deletes the old one itself, so the edit lands either way. */
export function serverLeftOldRow(
  list: ReadonlyArray<{ frequency: number; sid?: number | null; manual?: boolean }>,
  old: { frequency: number; sid?: number | null },
  v: BookmarkEditValue,
): boolean {
  if (bmKey(old.frequency) === bmKey(v.frequency)) return false;     // edited in place: nothing left behind
  const sid = typeof old.sid === 'number' && old.sid > 0 ? old.sid : -1;
  return list.some((b) => bmKey(b.frequency) === bmKey(old.frequency)
    && (typeof b.sid === 'number' && b.sid > 0 ? b.sid : -1) === sid);
}

/** The server's refusal, in its own words (`{"error":"…"}`), or a plain fallback when it sent none. */
export function serverEditError(status: number, body: string): string {
  try {
    const j = JSON.parse(body);
    if (j && typeof j.error === 'string' && j.error.trim()) return j.error.trim().slice(0, 200);
  } catch { /* not JSON */ }
  if (status === 401 || status === 403) return 'The server refused the edit — are you still signed in as admin?';
  return `The server refused the edit (HTTP ${status}).`;
}

/** ★ YOUR bookmark `old` replaced by the edit, everything else on it kept (group, comment, scope, iCloud opt-in), and
 *  stamped so sync takes the edit as the newest. Matched as deletion matches (name + frequency + scope). Another row that
 *  would now be its exact twin (same name and frequency) is dropped — the list is keyed that way (mergeBookmarks). */
export function applyPersonalEdit(list: UserBookmark[], old: UserBookmark, v: BookmarkEditValue, now = Date.now()): UserBookmark[] {
  const same = (b: UserBookmark) => b.name === old.name && b.frequency === old.frequency && b.scope === old.scope;
  const idx = list.findIndex(same);
  if (idx < 0) return list;
  const edited: UserBookmark = {
    ...list[idx], name: v.name, frequency: v.frequency, mode: v.mode,
    bandwidth_low: v.bandwidth_low, bandwidth_high: v.bandwidth_high, updatedAt: now,
  };
  const out: UserBookmark[] = [];
  list.forEach((b, i) => {
    if (i === idx) out.push(edited);
    else if (!(b.name === edited.name && b.frequency === edited.frequency)) out.push(b);
  });
  return out;
}
