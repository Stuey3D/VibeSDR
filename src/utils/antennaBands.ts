/**
 * ★★★ WHAT THE AERIAL CAN HEAR, AND WHAT THE OWNER HAS DELIBERATELY TAKEN AWAY (2026-10-06).
 *
 * A listener tuned FM on Stuart's Pi 2, where an FM band-stop is fitted because the local FM is so
 * strong it overloads everything else. FM sounded poor, and the only explanation was a note in the
 * antenna NAME, which nobody reads. Stuart: "That way they don't think the user's SDR or our
 * software is shit." So the owner can now say, per radio, which ranges the aerial covers and which
 * filters sit in the feed — and both clients say so, once, when the listener tunes into it.
 *
 * ★★ ONE PARSER, TWO CLIENTS. The app (SDRScreen) and the web client (web/client/src/main.ts) both
 *    import this file, so "is 98 MHz inside the band-stop" has one answer. The setup page (a C++ raw
 *    string, vibe_setup_page.h) cannot import anything and carries its OWN copy of the parser and
 *    writer — scripts/test_antenna_bands.ts reads that copy out of the header and checks the two
 *    agree on the same strings, the same way the ANT_ICONS keys are a contract between two copies.
 *
 * ★★ THE WIRE FORMAT IS THE TEXT THE OWNER'S CONFIG HOLDS — `antennaRanges` / `antennaFilters` on
 *    RadioConfig, passed through /vibeserver.json and the door's radio list untouched. Written the
 *    way `allowRanges` and `antennaMap` are, so the config file stays readable by a person:
 *
 *      antennaRanges   "0-300MHz Wideband loop; 144-146MHz 2 m; [B] 430-440MHz 70 cm"
 *      antennaFilters  "bandstop 87.5-108MHz FM band-stop; highpass 1.7MHz; [A] lowpass 30MHz"
 *
 *    • entries separated by `;` (a NAME may hold a comma — "Loop, loft" — so not `,`)
 *    • an optional `[port]` first: the RSP's socket the entry belongs to (antennaList). No port =
 *      every socket. A radio with one aerial never has one.
 *    • a range is `lo-hi` with ONE unit after it (`Hz` `kHz` `MHz` `GHz`); a unit on each end
 *      (`500kHz-30MHz`) is accepted too, because that is how people write it by hand
 *    • a filter is its kind (`bandstop` `bandpass` `highpass` `lowpass`), then a range for the two
 *      band kinds or a single frequency (the corner) for the two pass kinds
 *    • whatever follows is the optional name
 *
 * ★ Anything that does not parse is DROPPED, never guessed at: a misread entry would put a "filter
 *   fitted" notice on a frequency the owner never filtered, which is worse than saying nothing.
 * ★ Empty strings mean "nothing shown anywhere" — every server that never fills these in.
 */

export type FilterKind = 'bandstop' | 'bandpass' | 'highpass' | 'lowpass';
export type BandUnit = 'Hz' | 'kHz' | 'MHz' | 'GHz';

export interface AntennaRange {
  loHz: number;
  hiHz: number;
  /** The unit the owner wrote it in — kept so the setup page shows back what they typed. */
  unit: BandUnit;
  name: string;
  /** The socket this belongs to, '' = every socket. */
  port: string;
}

export interface AntennaFilter {
  kind: FilterKind;
  /** bandstop / bandpass: the band. highpass: loHz is the corner, hiHz = Infinity.
   *  lowpass: hiHz is the corner, loHz = 0. */
  loHz: number;
  hiHz: number;
  unit: BandUnit;
  name: string;
  port: string;
}

export interface AntennaBands { ranges: AntennaRange[]; filters: AntennaFilter[] }

export const NO_ANTENNA_BANDS: AntennaBands = { ranges: [], filters: [] };

const UNIT_HZ: Record<string, number> = { hz: 1, khz: 1e3, mhz: 1e6, ghz: 1e9 };
const UNIT_NAME: Record<string, BandUnit> = { hz: 'Hz', khz: 'kHz', mhz: 'MHz', ghz: 'GHz' };
const KINDS: FilterKind[] = ['bandstop', 'bandpass', 'highpass', 'lowpass'];

/** ★ The longest name kept. A name is shown in a one-line notice; a paragraph would not fit there. */
export const BAND_NAME_MAX = 40;
/** ★ The longest string either field may be — the server clamps at the same figure. */
export const BANDS_TEXT_MAX = 800;

/** A name, made safe for the format: no separators, no brackets, no control characters. */
export function cleanBandName(s: unknown): string {
  return String(s ?? '').replace(/[\u0000-\u001f\u007f;[\]"\\]/g, ' ').replace(/\s+/g, ' ').trim()
    .slice(0, BAND_NAME_MAX).trim();
}

/** A port name as it may appear in `[...]`. */
function cleanPort(s: unknown): string {
  return String(s ?? '').replace(/[\u0000-\u001f\u007f;[\]]/g, '').trim().slice(0, 32);
}

// `[port]` prefix, then the rest.
const PORT_RE = /^\s*\[([^\]]*)\]\s*/;
// A frequency or a range, each end optionally carrying a unit, then an optional trailing unit.
// ★ en dash and em dash accepted: an owner pasting "144–146 MHz" from a band plan gets what they meant.
const RANGE_RE = /^\s*(\d+(?:\.\d+)?)\s*(hz|khz|mhz|ghz)?\s*[-–—]\s*(\d+(?:\.\d+)?)\s*(hz|khz|mhz|ghz)?\b\s*/i;
const ONE_RE = /^\s*(\d+(?:\.\d+)?)\s*(hz|khz|mhz|ghz)?\b\s*/i;

function splitEntries(s: unknown): string[] {
  if (typeof s !== 'string' || !s.trim()) return [];
  return s.slice(0, BANDS_TEXT_MAX).split(';').map((e) => e.trim()).filter(Boolean);
}

function takePort(e: string): [string, string] {
  const m = PORT_RE.exec(e);
  return m ? [cleanPort(m[1]), e.slice(m[0].length)] : ['', e];
}

/** ★ A unit is REQUIRED somewhere in the entry. "144-146" alone could be kHz or MHz, and guessing
 *  wrong would put the notice three orders of magnitude away from the band the owner meant. */
function parseRangeText(t: string): { lo: number; hi: number; unit: BandUnit; rest: string } | null {
  const m = RANGE_RE.exec(t);
  if (!m) return null;
  const u1 = m[2]?.toLowerCase(), u2 = m[4]?.toLowerCase();
  const unitHi = u2 ?? u1;
  const unitLo = u1 ?? u2;
  if (!unitHi || !unitLo) return null;
  const lo = parseFloat(m[1]) * UNIT_HZ[unitLo];
  const hi = parseFloat(m[3]) * UNIT_HZ[unitHi];
  if (!Number.isFinite(lo) || !Number.isFinite(hi) || hi <= lo || lo < 0) return null;
  return { lo, hi, unit: UNIT_NAME[unitHi], rest: t.slice(m[0].length) };
}

function parseOneText(t: string): { hz: number; unit: BandUnit; rest: string } | null {
  const m = ONE_RE.exec(t);
  if (!m || !m[2]) return null;
  const hz = parseFloat(m[1]) * UNIT_HZ[m[2].toLowerCase()];
  if (!Number.isFinite(hz) || hz <= 0) return null;
  return { hz, unit: UNIT_NAME[m[2].toLowerCase()], rest: t.slice(m[0].length) };
}

export function parseAntennaRanges(s: unknown): AntennaRange[] {
  const out: AntennaRange[] = [];
  for (const e of splitEntries(s)) {
    const [port, body] = takePort(e);
    const r = parseRangeText(body);
    if (!r) continue;
    out.push({ loHz: r.lo, hiHz: r.hi, unit: r.unit, name: cleanBandName(r.rest), port });
  }
  return out;
}

export function parseAntennaFilters(s: unknown): AntennaFilter[] {
  const out: AntennaFilter[] = [];
  for (const e of splitEntries(s)) {
    const [port, body] = takePort(e);
    const km = /^\s*([a-z-]+)\s+/i.exec(body);
    if (!km) continue;
    const kind = km[1].toLowerCase().replace(/-/g, '') as FilterKind;
    if (!KINDS.includes(kind)) continue;
    const t = body.slice(km[0].length);
    if (kind === 'bandstop' || kind === 'bandpass') {
      const r = parseRangeText(t);
      if (!r) continue;
      out.push({ kind, loHz: r.lo, hiHz: r.hi, unit: r.unit, name: cleanBandName(r.rest), port });
    } else {
      const o = parseOneText(t);
      if (!o) continue;
      out.push(kind === 'highpass'
        ? { kind, loHz: o.hz, hiHz: Infinity, unit: o.unit, name: cleanBandName(o.rest), port }
        : { kind, loHz: 0, hiHz: o.hz, unit: o.unit, name: cleanBandName(o.rest), port });
    }
  }
  return out;
}

export function parseAntennaBands(ranges: unknown, filters: unknown): AntennaBands {
  return { ranges: parseAntennaRanges(ranges), filters: parseAntennaFilters(filters) };
}

export function hasAntennaBands(b: AntennaBands | null | undefined): boolean {
  return !!b && (b.ranges.length > 0 || b.filters.length > 0);
}

/** A number in a unit, without float noise or trailing zeros: 87.5, 108, 0.1357. */
export function fmtInUnit(hz: number, unit: BandUnit): string {
  const v = hz / UNIT_HZ[unit.toLowerCase()];
  return String(Math.round(v * 1e6) / 1e6);
}

/** ★ The canonical text — what the setup page writes and the server stores. */
export function formatAntennaRanges(rs: AntennaRange[]): string {
  return rs.map((r) => {
    const name = cleanBandName(r.name);
    const port = cleanPort(r.port);
    return `${port ? `[${port}] ` : ''}${fmtInUnit(r.loHz, r.unit)}-${fmtInUnit(r.hiHz, r.unit)}${r.unit}`
      + (name ? ` ${name}` : '');
  }).join('; ');
}

export function formatAntennaFilters(fs: AntennaFilter[]): string {
  return fs.map((f) => {
    const name = cleanBandName(f.name);
    const port = cleanPort(f.port);
    const freq = f.kind === 'highpass' ? `${fmtInUnit(f.loHz, f.unit)}${f.unit}`
      : f.kind === 'lowpass' ? `${fmtInUnit(f.hiHz, f.unit)}${f.unit}`
      : `${fmtInUnit(f.loHz, f.unit)}-${fmtInUnit(f.hiHz, f.unit)}${f.unit}`;
    return `${port ? `[${port}] ` : ''}${f.kind} ${freq}${name ? ` ${name}` : ''}`;
  }).join('; ');
}

/** ★ Plain words for each kind — the notice is read by newcomers, not by RF engineers. */
export const FILTER_KIND_LABEL: Record<FilterKind, string> = {
  bandstop: 'Band-stop', bandpass: 'Band-pass', highpass: 'High-pass', lowpass: 'Low-pass',
};

/** Does this entry apply to the socket in use?
 *  ★★ An entry for a SPECIFIC socket is skipped while we do not know which socket is in use —
 *     claiming a filter on socket B while the radio is on A is a false notice, and silence is the
 *     honest answer to "we cannot tell". Port names compare case-insensitively ("Antenna A" / "A"
 *     are the radio's own names, and an owner types them however they like). */
function appliesTo(entryPort: string, port: string | null | undefined): boolean {
  if (!entryPort) return true;
  if (!port) return false;
  const a = entryPort.trim().toLowerCase(), b = port.trim().toLowerCase();
  return a === b || a === `antenna ${b}` || b === `antenna ${a}`;
}

export function filterRejects(f: AntennaFilter, hz: number): boolean {
  switch (f.kind) {
    case 'bandstop': return hz >= f.loHz && hz <= f.hiHz;
    case 'bandpass': return hz < f.loHz || hz > f.hiHz;
    case 'highpass': return hz < f.loHz;
    case 'lowpass':  return hz > f.hiHz;
  }
}

export interface AntennaNotice {
  /** Identifies WHICH zone you are in — a change of key is "entering" (see antennaNoticeTrack). */
  key: string;
  kind: 'filter' | 'range';
  /** The full sentence, shown on entry. */
  text: string;
  /** The quiet form, left on screen while you stay inside. */
  short: string;
}

/** "FM band-stop filter", "Band-stop filter" — a name that already says "filter" is not doubled. */
export function filterLabel(f: AntennaFilter): string {
  const name = cleanBandName(f.name);
  if (!name) return `${FILTER_KIND_LABEL[f.kind]} filter`;
  return /\bfilter$/i.test(name) ? name : `${name} filter`;
}

/**
 * ★★★ THE ONE QUESTION BOTH CLIENTS ASK: what, if anything, should a listener tuned to `hz` be told?
 *  • a filter that rejects this frequency wins — the owner has DELIBERATELY reduced reception here,
 *    and that is the fact that stops someone blaming the receiver
 *  • otherwise, if the owner listed the aerial's ranges and `hz` is in none of them: outside range
 *  • otherwise nothing.
 * `port` is the socket in use (caps.antenna), or null/undefined when the radio does not say.
 */
export function antennaNoticeAt(hz: number, bands: AntennaBands | null | undefined,
                                port?: string | null): AntennaNotice | null {
  if (!bands || !Number.isFinite(hz) || hz <= 0) return null;
  for (let i = 0; i < bands.filters.length; i++) {
    const f = bands.filters[i];
    if (!appliesTo(f.port, port) || !filterRejects(f, hz)) continue;
    const label = filterLabel(f);
    return {
      key: `f${i}:${f.kind}:${f.loHz}:${f.hiHz}`, kind: 'filter',
      text: `${label} fitted — reception here is deliberately reduced`,
      short: `${label} fitted`,
    };
  }
  const rs = bands.ranges.filter((r) => appliesTo(r.port, port));
  if (rs.length && !rs.some((r) => hz >= r.loHz && hz <= r.hiHz)) {
    return {
      key: 'range:out', kind: 'range',
      text: 'Outside this antenna’s range — reception may be poor',
      short: 'Outside antenna range',
    };
  }
  return null;
}

/**
 * ★★ ONCE PER ENTRY, NOT PER TUNE. Someone flicking about inside the FM band must not be nagged on
 *  every step, so the full sentence is shown when the KEY changes (you crossed into a new zone) and
 *  the quiet form stays while you remain. Leaving (null) clears it, so coming back is a new entry.
 *  Pure: the caller keeps `prevKey` and acts on `entered`.
 */
export function antennaNoticeTrack(prevKey: string | null, now: AntennaNotice | null):
    { key: string | null; entered: boolean } {
  const key = now ? now.key : null;
  return { key, entered: key !== null && key !== prevKey };
}

/** ★ For the directory card: "0–300 MHz · 2 m 144–146 MHz · FM band-stop". Short, or ''. */
export function antennaBandsSummary(b: AntennaBands | null | undefined, max = 4): string {
  if (!b) return '';
  const parts: string[] = [];
  for (const r of b.ranges) {
    const span = `${fmtInUnit(r.loHz, r.unit)}–${fmtInUnit(r.hiHz, r.unit)} ${r.unit}`;
    parts.push(r.name ? `${r.name} ${span}` : span);
  }
  for (const f of b.filters) parts.push(filterLabel(f));
  if (parts.length <= max) return parts.join(' · ');
  return parts.slice(0, max).join(' · ') + ` · +${parts.length - max}`;
}

/**
 * ★★ THE DIRECTORY CARD'S TWO LINES (2026-10-06) — what a listener reads BEFORE connecting:
 *      "Covers 0–300 MHz (Wideband loop) · 144–146 MHz (2 m, Ant B)"
 *      "FM band-stop filter fitted"   /   "Filters fitted: FM band-stop · High-pass 1.7 MHz"
 *  ★ Every entry is listed, whatever its socket: the card describes the RADIO and cannot know which
 *    socket a listener will land on — so a per-socket entry carries its socket's name rather than
 *    being hidden (contrast antennaNoticeAt, which knows the socket in use).
 *  ★ The same filter words as the in-app notice (filterLabel, "… fitted"), so the card and the
 *    notice a listener then meets say the same thing.
 *  ★★ ONE RULE, TWO READERS: directory/public/index.html is a static page that cannot import this
 *     file and carries its OWN copy between `// ANTBANDS-CARD-BEGIN` / `-END`;
 *     scripts/test_antenna_bands.ts runs both on the same strings and fails if they ever differ.
 *  Empty strings for a line with nothing to say. `max` entries per line, then "+N".
 */
export function antennaBandsCard(b: AntennaBands | null | undefined, max = 4):
    { covers: string; filters: string } {
  if (!b) return { covers: '', filters: '' };
  const portTag = (p: string) => {
    const c = cleanPort(p);
    return !c ? '' : /^ant/i.test(c) ? c : `Ant ${c}`;
  };
  const cap = (parts: string[]) => parts.length <= max ? parts.join(' · ')
    : parts.slice(0, max).join(' · ') + ` · +${parts.length - max}`;
  const span = (lo: number, hi: number, u: BandUnit) => `${fmtInUnit(lo, u)}–${fmtInUnit(hi, u)} ${u}`;
  const freqOf = (f: AntennaFilter) => f.kind === 'highpass' ? `${fmtInUnit(f.loHz, f.unit)} ${f.unit}`
    : f.kind === 'lowpass' ? `${fmtInUnit(f.hiHz, f.unit)} ${f.unit}` : span(f.loHz, f.hiHz, f.unit);

  const rs = b.ranges.map((r) => {
    const extra = [cleanBandName(r.name), portTag(r.port)].filter(Boolean).join(', ');
    return span(r.loHz, r.hiHz, r.unit) + (extra ? ` (${extra})` : '');
  });
  // A filter's own words: the owner's name for it, or its kind and frequency when it has none.
  const fname = (f: AntennaFilter) => {
    const n = cleanBandName(f.name).replace(/\s*\bfilter$/i, '');
    return n || `${FILTER_KIND_LABEL[f.kind]} ${freqOf(f)}`;
  };
  const withPort = (s: string, f: AntennaFilter) => (portTag(f.port) ? `${s} (${portTag(f.port)})` : s);
  const fs = b.filters;
  const filters = fs.length === 0 ? ''
    : fs.length === 1 ? `${withPort(`${fname(fs[0])} filter`, fs[0])} fitted`
    : `Filters fitted: ${cap(fs.map((f) => withPort(fname(f), f)))}`;
  return { covers: rs.length ? `Covers ${cap(rs)}` : '', filters };
}
