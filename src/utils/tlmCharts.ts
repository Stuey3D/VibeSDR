/**
 * ★★ V12 beta91 TELEMETRY CHARTS (Stuart 2026-11-22: "especially for those who background decode it every pass") — the
 * one reading and geometry the app (src/components/TlmCharts.tsx) and the web client (web/client/src/tlmChartsWeb.ts)
 * both draw from.
 *
 *  • THIS PASS — one small chart per channel, the readings against time (the live frames, or a pass's gallery .csv).
 *  • ACROSS PASSES — the last TLM_PASSES passes in the gallery, one point per pass: its mean, with its min–max as a band.
 *
 * ★ Where the data comes from: the server writes a .csv item per pass (android/app/src/main/cpp/vibe_fctlm_gallery.h).
 *   Its headings are the channel's name, then "[unit]" (beta94: AMSAT-UK's published equations — V, mA, °C, mW, lux),
 *   "[raw]" only for a channel with NO published conversion (named in the charts' note), or nothing for a count / flag.
 *   Counts and flags are not charted (a reboot count is a number to read, not a curve); every analogue channel is.
 * ★★ beta94 (Stuart 11-25 "charts are useless"): a board whose data-valid flag is off sends no values — a blank cell in
 *   the CSV, no value live — so a channel with NO reading in the pass gets no chart (never a flat line of zeros), and the
 *   live pass says which boards the latest frame left out.
 * ★ Bounded: a pass is read to TLM_MAX_ROWS rows; the live history holds TLM_LIVE_MAX frames (an hour at one per 5 s).
 */

export interface TlmChannel { name: string; unit: string; raw: boolean }
/** One pass: its channels and, per channel, a reading per row (null = blank / not a number). */
export interface TlmPass {
  sat: string; start: number; title?: string; file?: string;
  channels: TlmChannel[];
  t: number[];
  v: (number | null)[][];
  /** ★ beta94 the boards the latest live frame said had no valid data ("EPS", …) */
  absent?: string[];
}
/** A live frame as the client keeps it (satTelemetryMerge's history). */
export interface TlmLiveFrame { t: number; values: { name: string; value: string; unit: string; raw: boolean }[]; absent?: string[] }

export const TLM_MAX_ROWS = 2000;
export const TLM_LIVE_MAX = 720;
/** The live box's pass: frames further apart begin a new one (vibe_fctlm_gallery.h kPassGapSec — the same 20 minutes). */
export const TLM_PASS_GAP_SEC = 20 * 60;
export const TLM_PASSES = 10;
/** Channels shown before "+ N MORE". */
export const TLM_FIRST = 8;
/** Frames further apart than this (three lost in a row) break a pass's line (a fade) — never drawn as if continuous. */
export const TLM_GAP_SEC = 20;

const FIXED = new Set(['utc', 'unix', 'frame', 'seq', 'mode', 'elevation [deg]', 'rs fixed']);

/** "Battery voltage [raw]" → raw · "Bus [V]" → V · "Reboot count" → a count. */
export function tlmHeading(h: string): TlmChannel {
  const m = /^(.*?)\s*\[([^\]]{1,12})\]\s*$/.exec(h.trim());
  if (!m) return { name: h.trim().slice(0, 48), unit: '', raw: false };
  const raw = m[2].toLowerCase() === 'raw';
  return { name: m[1].slice(0, 48), unit: raw ? '' : m[2], raw };
}
/** A channel worth a curve: analogue (raw, or with a unit). Counts and flags are read, not charted. */
export function tlmChartable(c: TlmChannel): boolean { return c.raw || !!c.unit; }

/** The pass's sat and start, from the CSV's first comment line "# FUNcube telemetry · AO-73 · …". */
function headSat(line: string): string {
  const parts = line.replace(/^#\s*/, '').split(' · ');
  return parts.length > 1 ? parts[1].trim().slice(0, 40) : '';
}

/** A pass's gallery .csv → a TlmPass (null when it is not one). */
export function tlmParseCsv(text: string, maxRows = TLM_MAX_ROWS): TlmPass | null {
  if (typeof text !== 'string' || !text) return null;
  const lines = text.split(/\r?\n/);
  let sat = '', i = 0;
  for (; i < lines.length && lines[i].startsWith('#'); i++) if (!sat) sat = headSat(lines[i]);
  if (i >= lines.length) return null;
  const head = lines[i].split(',');
  const iUnix = head.indexOf('unix');
  if (iUnix < 0) return null;
  const cols: number[] = [], channels: TlmChannel[] = [];
  head.forEach((h, k) => { if (!FIXED.has(h.trim().toLowerCase())) { cols.push(k); channels.push(tlmHeading(h)); } });
  const t: number[] = [];
  const v: (number | null)[][] = channels.map(() => []);
  for (let r = i + 1; r < lines.length && t.length < maxRows; r++) {
    const l = lines[r];
    if (!l || l.startsWith('#')) continue;
    const c = l.split(',');
    const ts = Number(c[iUnix]);
    if (!isFinite(ts) || ts <= 0) continue;
    t.push(ts);
    cols.forEach((k, j) => { const x = c[k] === undefined || c[k] === '' ? NaN : Number(c[k]); v[j].push(isFinite(x) ? x : null); });
  }
  if (!t.length) return null;
  return { sat, start: t[0], channels, t, v };
}

/** The live frames (newest last) → a TlmPass, channels in the first frame's order. */
export function tlmLivePass(frames: TlmLiveFrame[] | undefined, sat = ''): TlmPass | null {
  if (!frames?.length) return null;
  const order: TlmChannel[] = [];
  const idx = new Map<string, number>();
  for (const f of frames) for (const x of f.values) if (!idx.has(x.name)) { idx.set(x.name, order.length); order.push({ name: x.name, unit: x.raw ? '' : x.unit, raw: x.raw }); }
  const t: number[] = [];
  const v: (number | null)[][] = order.map(() => []);
  for (const f of frames) {
    t.push(f.t);
    const row: (number | null)[] = order.map(() => null);
    for (const x of f.values) { const n = Number(x.value); const k = idx.get(x.name); if (k != null && isFinite(n)) row[k] = n; }
    row.forEach((n, k) => v[k].push(n));
  }
  const absent = frames[frames.length - 1].absent;
  return { sat, start: t[0], channels: order, t, v, ...(absent?.length ? { absent: absent.slice() } : {}) };
}

/** The history the live box keeps: a frame with values and a time appended, oldest dropped past TLM_LIVE_MAX. */
export function tlmLivePush(hist: TlmLiveFrame[] | undefined, f: TlmLiveFrame | null): TlmLiveFrame[] {
  let h = hist ? hist.slice() : [];
  if (!f || !f.values.length || !(f.t > 0)) return h;
  // ★ a frame long after the last (the next pass on the same pick, hours on) begins THIS PASS again — the server's rule
  if (h.length && Math.abs(f.t - h[h.length - 1].t) > TLM_PASS_GAP_SEC) h = [];
  if (h.length && h[h.length - 1].t === f.t) h[h.length - 1] = f; else h.push(f);
  return h.length > TLM_LIVE_MAX ? h.slice(h.length - TLM_LIVE_MAX) : h;
}

/** ★ The order the charts come in: the power system first (what someone watching a bird's health looks at), then the
 *  temperatures, the solar side, the radio — then the rest as the frame carries them; raw channels (no published
 *  conversion) last. Only chartable channels that have at least one reading. */
const PRIORITY = [
  'Battery voltage', 'Total photo current', 'Total system current', 'Battery temp', 'Photo voltage 1', 'Photo voltage 2',
  'Photo voltage 3', 'Photo current 1', 'Photo current 2', 'Photo current 3',
  'Solar panel temp X+', 'Solar panel temp X-', 'Solar panel temp Y+', 'Solar panel temp Y-',
  'PA board temperature', 'RF temperature', 'PA forward power', 'PA board current', '3.3 V bus voltage', '5.0 V bus voltage',
  'Sun sensor X+', 'Sun sensor X-', 'Sun sensor Y+', 'Sun sensor Y-', 'Sun sensor Z+', 'Sun sensor Z-',
];
export function tlmHasReading(vals: (number | null)[] | undefined): boolean {
  return !!vals && vals.some((x) => x != null && isFinite(x));
}
export function tlmChartOrder(p: TlmPass | null): number[] {
  if (!p) return [];
  const ok = p.channels.map((c, i) => i).filter((i) => tlmChartable(p.channels[i]) && tlmHasReading(p.v[i]));
  const rank = (i: number) => {
    if (p.channels[i].raw) return 2 * PRIORITY.length + 1000 + i;
    const r = PRIORITY.indexOf(p.channels[i].name); return r < 0 ? PRIORITY.length + i : r;
  };
  return ok.sort((a, b) => rank(a) - rank(b));
}

/** "1234 raw" · "8.13 V" · "—". Rounded to what a reading needs (≤ 3 significant decimals). */
export function tlmValueText(x: number | null | undefined, c: TlmChannel): string {
  if (x == null || !isFinite(x)) return '—';
  const s = Math.abs(x) >= 100 || Number.isInteger(x) ? String(Math.round(x)) : String(Math.round(x * 100) / 100);
  return c.raw ? `${s} raw` : c.unit ? `${s} ${c.unit}` : s;
}
/** A chart's range in words: "1427–1457 raw" · "8.1–8.3 V" · "" when there is none. */
export function tlmRangeText(vals: (number | null)[], c: TlmChannel, hiVals?: (number | null)[]): string {
  const ok = (a: (number | null)[]) => a.filter((x): x is number => x != null && isFinite(x));
  const lo = ok(vals), hi = ok(hiVals ?? vals);
  if (!lo.length || !hi.length) return '';
  const a = tlmValueText(Math.min(...lo), { name: '', unit: '', raw: false }), b = tlmValueText(Math.max(...hi), c);
  return `${a}\u2013${b}`;
}
/** ★ beta94 The charts' note: the raw channels BY NAME (no blanket "raw" footnote), then times and saving. */
export function tlmFootNote(p: TlmPass | null): string {
  const raw = p ? tlmChartOrder(p).filter((i) => p.channels[i].raw).map((i) => p.channels[i].name) : [];
  return (raw.length ? `raw (no published conversion): ${raw.join(', ')} · ` : '') + 'times UTC · saved to the gallery a pass at a time';
}
/** ★ beta94 "No EPS data in the latest frame (its data-valid flag is off)" — '' when every board sent data. */
export function tlmAbsentText(absent: string[] | undefined, where = 'the latest frame'): string {
  const a = (absent ?? []).filter((x) => typeof x === 'string' && x).slice(0, 6);
  if (!a.length) return '';
  const list = a.length === 1 ? a[0] : `${a.slice(0, -1).join(', ')} or ${a[a.length - 1]}`;
  return `No ${list} data in ${where} (${a.length === 1 ? 'its data-valid flag is' : 'their data-valid flags are'} off)`;
}
/** ★ beta94 the smallest span a chart's axis shows, by unit — so one count of ADC noise on a steady battery is not drawn
 *  as a cliff (a 7.876 V battery's axis spans at least 0.1 V). 0 = the old rule (±5 % around a flat channel). */
export function tlmMinSpan(c: TlmChannel | undefined): number {
  switch (c?.raw ? '' : c?.unit) {
    case 'V': return 0.1;
    case '\u00b0C': return 2;
    case 'mA': case 'mW': return 10;
    case 'lux': return 100;
    case 'h': return 1;
    default: return 0;
  }
}
/** "14:02" UTC */
export function tlmHm(t: number): string {
  const d = new Date(t * 1000);
  return `${String(d.getUTCHours()).padStart(2, '0')}:${String(d.getUTCMinutes()).padStart(2, '0')}`;
}
/** "22 Nov" UTC */
export function tlmDay(t: number): string {
  const d = new Date(t * 1000);
  return `${d.getUTCDate()} ${['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'][d.getUTCMonth()]}`;
}
/** "THIS PASS · 23 frames · 14:02–14:13 UTC" */
export function tlmPassHead(p: TlmPass | null, live: boolean): string {
  if (!p || !p.t.length) return live ? 'THIS PASS · waiting for a frame' : 'THIS PASS';
  const n = p.t.length;
  return `${live ? 'THIS PASS' : 'THE PASS'} · ${n} frame${n === 1 ? '' : 's'} · ${tlmHm(p.t[0])}–${tlmHm(p.t[n - 1])} UTC`;
}
export function tlmAcrossHead(n: number, passes?: TlmPass[]): string {
  if (!n) return 'LAST PASSES · none saved in the gallery yet';
  const span = passes && passes.length ? ` · ${tlmDay(passes[0].start)} ${tlmHm(passes[0].start)} \u2192 ${tlmDay(passes[passes.length - 1].start)} ${tlmHm(passes[passes.length - 1].start)} UTC` : '';
  return `LAST ${n} PASS${n === 1 ? '' : 'ES'}${span} · each pass's mean, its range shaded`;
}

// ── Geometry ───────────────────────────────────────────────────────────────────────────────────────────────────
export interface Spark {
  /** the line (SVG path d), broken at gaps */
  line: string;
  /** the min–max band (across passes), '' for a pass */
  band: string;
  /** the newest point, for its dot (null = none) */
  last: { x: number; y: number } | null;
  lo: number; hi: number; n: number;
  /** the newest reading */
  lastV: number | null;
}
const f1 = (x: number) => (Math.round(x * 10) / 10).toString();
/** y range with a little headroom; a flat channel sits on the middle. */
export function tlmRange(vals: (number | null)[], extra: (number | null)[] = [], minSpan = 0): { lo: number; hi: number } {
  let lo = Infinity, hi = -Infinity;
  for (const a of [vals, extra]) for (const x of a) if (x != null && isFinite(x)) { if (x < lo) lo = x; if (x > hi) hi = x; }
  if (!isFinite(lo)) return { lo: 0, hi: 1 };
  if (minSpan > 0 && hi - lo < minSpan) { const m = (lo + hi) / 2; return { lo: m - minSpan / 2, hi: m + minSpan / 2 }; }
  if (hi - lo < 1e-9) { const d = Math.max(1, Math.abs(lo) * 0.05); return { lo: lo - d, hi: hi + d }; }
  return { lo, hi };
}
/**
 * One channel's chart in a w × h box (pad px inside): the readings against time `t`, the line broken where frames are
 * more than `gap` s apart. `bandLo`/`bandHi` (across passes) draw the min–max band, and widen the range to hold it.
 */
export function tlmSpark(t: number[], vals: (number | null)[], w: number, h: number, pad = 3, gap = TLM_GAP_SEC,
                         bandLo?: (number | null)[], bandHi?: (number | null)[], minSpan = 0): Spark {
  const { lo, hi } = tlmRange(vals, [...(bandLo ?? []), ...(bandHi ?? [])], minSpan);
  const t0 = t.length ? t[0] : 0, t1 = t.length ? t[t.length - 1] : 1;
  const span = t1 - t0 > 0 ? t1 - t0 : 1;
  const X = (ts: number) => (t.length < 2 ? w / 2 : pad + ((ts - t0) / span) * (w - 2 * pad));
  const Y = (x: number) => pad + (1 - (x - lo) / (hi - lo)) * (h - 2 * pad);
  let line = '', prevT = -Infinity, open = false, n = 0, last: Spark['last'] = null, lastV: number | null = null;
  for (let i = 0; i < t.length; i++) {
    const x = vals[i];
    if (x == null || !isFinite(x)) { open = false; continue; }
    const px = X(t[i]), py = Y(x);
    line += `${open && t[i] - prevT <= gap ? 'L' : 'M'}${f1(px)} ${f1(py)}`;
    open = true; prevT = t[i]; n++; last = { x: px, y: py }; lastV = x;
  }
  // a lone point is a dot only — a zero-length line draws nothing on some renderers
  let band = '';
  if (bandLo && bandHi) {
    const up: string[] = [], dn: string[] = [];
    for (let i = 0; i < t.length; i++) {
      const a = bandLo[i], b = bandHi[i];
      if (a == null || b == null || !isFinite(a) || !isFinite(b)) continue;
      up.push(`${f1(X(t[i]))} ${f1(Y(b))}`); dn.unshift(`${f1(X(t[i]))} ${f1(Y(a))}`);
    }
    if (up.length >= 2) band = `M${up.join('L')}L${dn.join('L')}Z`;
  }
  return { line, band, last, lo, hi, n, lastV };
}

// ── Across passes ─────────────────────────────────────────────────────────────────────────────────────────────
export interface TlmAcross { t: number[]; mean: (number | null)[]; min: (number | null)[]; max: (number | null)[] }
/** One channel (by NAME) across the passes (oldest first): a point per pass — its mean, min and max. */
export function tlmAcross(passes: TlmPass[], name: string): TlmAcross {
  const out: TlmAcross = { t: [], mean: [], min: [], max: [] };
  for (const p of passes) {
    const k = p.channels.findIndex((c) => c.name === name);
    let s = 0, n = 0, lo = Infinity, hi = -Infinity;
    if (k >= 0) for (const x of p.v[k]) if (x != null && isFinite(x)) { s += x; n++; if (x < lo) lo = x; if (x > hi) hi = x; }
    out.t.push(p.start);
    out.mean.push(n ? s / n : null); out.min.push(n ? lo : null); out.max.push(n ? hi : null);
  }
  return out;
}

// ── The gallery: which items are a satellite's passes ─────────────────────────────────────────────────────────
export interface TlmGalleryItem { f: string; k: string; t: number; title: string }
/** A satellite's pass CSVs (title "AO-73 telemetry · pass 14:02 UTC · …"), newest first, at most `limit`; `upTo` keeps
 *  only those that began no later (the gallery's own pass: the passes up to it). */
export function tlmPassItems<T extends TlmGalleryItem>(items: T[] | undefined, sat: string, limit = TLM_PASSES, upTo = Infinity): T[] {
  if (!Array.isArray(items)) return [];
  const pre = sat ? `${sat} telemetry · pass ` : '';
  return items
    .filter((it) => it && it.k === 'fctlm' && /\.csv$/i.test(it.f) && (!pre || (it.title || '').startsWith(pre)) && it.t <= upTo)
    .sort((a, b) => b.t - a.t)
    .slice(0, limit);
}
/** The satellite a pass item's title names ("AO-73 telemetry · pass …" → "AO-73"). */
export function tlmSatOfTitle(title: string): string {
  const k = (title || '').indexOf(' telemetry · pass ');
  return k > 0 ? title.slice(0, k) : '';
}
/** ★ Fetch the passes (oldest first, for the across chart) through `get` (a path → its text, or null) — one at a time,
 *  so a Pi 2 serving the gallery is never asked for ten files at once. */
export async function tlmLoadPasses(get: (path: string) => Promise<string | null>, sat: string, upTo = Infinity,
                                    limit = TLM_PASSES): Promise<TlmPass[]> {
  const list = await get('/vibeserver/gallery.json?kind=fctlm&limit=300');
  let items: TlmGalleryItem[] = [];
  try { const j = list ? JSON.parse(list) : null; items = Array.isArray(j?.items) ? j.items : []; } catch { items = []; }
  const pick = tlmPassItems(items, sat, limit, upTo);
  const out: TlmPass[] = [];
  for (const it of pick) {
    const body = await get(`/vibeserver/gallery/file/${encodeURIComponent(it.f)}`).catch(() => null);
    const p = body ? tlmParseCsv(body) : null;
    if (p) { p.title = it.title; p.file = it.f; out.push(p); }
  }
  return out.sort((a, b) => a.start - b.start);
}
