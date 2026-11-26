/**
 * navtexQuality — a NAVTEX message's DECODE QUALITY and SNR TIMELINE, for the app and the web client alike (beta96,
 * Stuart 11-26). DISPLAY ONLY: nothing here reaches the decoder or what counts as a decode.
 *
 * ★ Decode quality — SITOR-B sends every character twice (the DX copy, then the RX copy 350 ms later):
 *     clean   both copies valid and the same word;
 *     FEC     recovered from one copy (the other invalid, missing or overruled) or from the soft tiers;
 *     lost    neither copy usable ('_');
 *     copies disagreed — both valid but DIFFERENT words: one was hit. Counted inside clean-or-FEC by what was printed.
 *   From the server's op 0x07 (21 bytes since beta96: clean, repaired, lost, agreed, disagreed) or the gallery record
 *   (nqc / nqf / nql / nqd).
 * ★ SNR — measured by the decoder at each bit centre, in the BIT BANDWIDTH (100 Hz, as the SITOR-A status), sent a slice
 *   a second (op 0x08). A slice is tied to the TEXT: `tc[i]` is the character index (in the message text) where slice i
 *   ENDED, so the card can colour the lines decoded during a fade. Colours: < 5 dB red (that text may be junk), 5–10
 *   amber, > 10 green.
 */

export const SNR_RED_DB = 5;
export const SNR_GREEN_DB = 10;
/** ≤ this many slices a message; longer ones merge pairs (each slice then covers twice the time). */
export const SNR_MAX_SLICES = 120;

export type SnrBand = 'red' | 'amber' | 'green';
export function snrBand(db: number): SnrBand { return db < SNR_RED_DB ? 'red' : db <= SNR_GREEN_DB ? 'amber' : 'green'; }
/** The scale's colours — one set for web and app (readable on the default, silver and black themes). */
export const SNR_COLOURS: Record<SnrBand, string> = { red: '#e5484d', amber: '#f0a020', green: '#3dbb5f' };
export function snrColour(db: number): string { return SNR_COLOURS[snrBand(db)]; }

/** The FEC split of one message, as counts. */
export interface NavtexSplit { clean: number; fec: number; lost: number; disagreed: number }
/** The SNR of one message: the slices and the figures over the whole message. */
export interface NavtexSnr {
  /** Slice SNRs, dB (whole). */
  tl: number[];
  /** Where each slice ENDED in the message text (character index); slice i covers tc[i-1]..tc[i] (slice 0 from 0). */
  tc: number[];
  /** Seconds a slice (1, or 2, 4 … after merging). */
  sliceSec: number;
  /** Mean SNR (from mean powers), signal and noise dBFS (noise in 100 Hz). */
  avg: number; sig: number; noise: number;
  /** The lowest slice (dB) and the share of the text's characters decoded under 5 dB (whole %). */
  min: number; redPct: number;
}

/** "Decode quality: 98.6 % clean · 1.2 % FEC · 0.2 % lost", "512 characters", "copies disagreed 0.4 %" — or null. */
export function navtexSplitPct(s: NavtexSplit | null | undefined): { clean: number; fec: number; lost: number; disagreed: number; n: number } | null {
  if (!s) return null;
  const n = s.clean + s.fec + s.lost;
  if (!(n > 0)) return null;
  return { clean: 100 * s.clean / n, fec: 100 * s.fec / n, lost: 100 * s.lost / n, disagreed: 100 * s.disagreed / n, n };
}
const p1 = (v: number) => (Math.round(v * 10) / 10).toFixed(1);
export function navtexQualityLine(s: NavtexSplit | null | undefined): string | null {
  const p = navtexSplitPct(s);
  if (!p) return null;
  return `Decode quality: ${p1(p.clean)} % clean · ${p1(p.fec)} % FEC · ${p1(p.lost)} % lost`;
}
/** "512 characters · copies disagreed 0.4 %" (the disagreement only when there was one). */
export function navtexQualityDetail(s: NavtexSplit | null | undefined): string | null {
  const p = navtexSplitPct(s);
  if (!p) return null;
  return `${p.n.toLocaleString('en-GB')} characters` + (s!.disagreed > 0 ? ` · copies disagreed ${p1(p.disagreed)} %` : '');
}
/** "SNR 8 dB avg · 22 % weak" (weak = decoded under 5 dB) — or null. `long` adds the lowest slice and the levels. */
export function navtexSnrLine(q: NavtexSnr | null | undefined, long = false): string | null {
  if (!q || !q.tl.length) return null;
  const base = `SNR ${Math.round(q.avg)} dB avg · ${q.redPct} % weak`;
  return long ? `${base} · lowest ${q.min} dB · signal ${Math.round(q.sig)} dBFS, noise ${Math.round(q.noise)} dBFS in 100 Hz` : base;
}

// ── Slices from the live stream (op 0x08) — the assembler keeps them in linear power ──────────────────────────────
export interface SliceAcc { sig: number; noise: number; at: number }
/** Add one decoder slice to a message's list (≤ SNR_MAX_SLICES, pairs merged at EQUAL length — an odd last slice becomes
 *  half of the next one, never a short one). Returns the new slice length. Mutates `list` and `pend`. */
export function addSlice(list: SliceAcc[], pend: { sig: number; noise: number; at: number; n: number }, sliceSec: number,
                         sigDb: number, noiseDb: number, at: number): number {
  pend.sig += Math.pow(10, sigDb / 10); pend.noise += Math.pow(10, noiseDb / 10); pend.at = at; pend.n++;
  if (pend.n < sliceSec) return sliceSec;
  list.push({ sig: pend.sig / pend.n, noise: pend.noise / pend.n, at: pend.at });
  pend.sig = 0; pend.noise = 0; pend.n = 0;
  if (list.length <= SNR_MAX_SLICES) return sliceSec;
  const m: SliceAcc[] = [];
  for (let i = 0; i + 1 < list.length; i += 2) m.push({ sig: (list[i].sig + list[i + 1].sig) / 2, noise: (list[i].noise + list[i + 1].noise) / 2, at: list[i + 1].at });
  if (list.length % 2) { const l = list[list.length - 1]; pend.sig = l.sig * sliceSec; pend.noise = l.noise * sliceSec; pend.at = l.at; pend.n = sliceSec; }
  list.length = 0; list.push(...m);
  return sliceSec * 2;
}
/** The raw-text offset → cleaned-text offset map of cleanText (CR runs + optional LF → one '\n', then the leading blank
 *  lines trimmed) — so a slice recorded at a raw character count lands on the right character of the shown text. */
export function cleanedOffsets(raw: string): { map: Int32Array; lead: number } {
  const map = new Int32Array(raw.length + 1);
  let o = 0, c = '';
  for (let i = 0; i < raw.length; i++) {
    map[i] = o;
    if (raw[i] === '\r') {
      let j = i;
      while (j + 1 < raw.length && raw[j + 1] === '\r') map[++j] = o;
      if (j + 1 < raw.length && raw[j + 1] === '\n') map[++j] = o;
      i = j; o++; c += '\n';
    } else { o++; c += raw[i]; }
  }
  map[raw.length] = o;
  const m = /^(?:[ \t]*\n)+/.exec(c);
  return { map, lead: m ? m[0].length : 0 };
}
/** The message's SNR from its slices (raw offsets into `raw`, the body before cleaning) and its cleaned `text`. */
export function snrOfSlices(slices: SliceAcc[], raw: string, text: string, sliceSec: number): NavtexSnr | null {
  if (!slices.length) return null;
  const { map, lead } = cleanedOffsets(raw);
  const tl: number[] = [], tc: number[] = [];
  let ss = 0, nn = 0, lo = Infinity, red = 0, all = 0, prev = 0;
  slices.forEach((s, i) => {
    const db = 10 * Math.log10(Math.max(1e-30, s.sig) / Math.max(1e-30, s.noise));
    let e = map[Math.min(s.at, raw.length)] - lead; e = Math.max(0, Math.min(e, text.length));
    if (i === slices.length - 1) e = text.length;
    for (let k = prev; k < e; k++) if (!/\s/.test(text[k])) { all++; if (db < SNR_RED_DB) red++; }
    prev = Math.max(prev, e);
    tl.push(Math.round(Math.max(-99, Math.min(99, db)))); tc.push(e);
    ss += s.sig; nn += s.noise; lo = Math.min(lo, db);
  });
  const sig = 10 * Math.log10(Math.max(1e-30, ss / slices.length)), noise = 10 * Math.log10(Math.max(1e-30, nn / slices.length));
  return { tl, tc, sliceSec, avg: sig - noise, sig, noise, min: Math.round(Math.max(-99, lo)), redPct: all ? Math.round(100 * red / all) : 0 };
}

// ── From a gallery record (the index line's members) ─────────────────────────────────────────────────────────────
export interface NavtexQualityFields { nqc?: number; nqf?: number; nql?: number; nqd?: number; snr?: number; sdb?: number; ndb?: number;
                                       smin?: number; red?: number; ts?: number; tl?: number[]; tc?: number[] }
const num = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);
/** The split, or null for a record without one (an older server). */
export function splitOfItem(it: NavtexQualityFields): NavtexSplit | null {
  if (!num(it.nqc) || !num(it.nqf) || !num(it.nql)) return null;
  return { clean: it.nqc, fec: it.nqf, lost: it.nql, disagreed: num(it.nqd) ? it.nqd : 0 };
}
/** The SNR, or null for a record without one. A malformed timeline (lengths differ) is dropped, never half-drawn. */
export function snrOfItem(it: NavtexQualityFields): NavtexSnr | null {
  if (!num(it.snr) || !Array.isArray(it.tl) || !Array.isArray(it.tc) || !it.tl.length || it.tl.length !== it.tc.length) return null;
  if (!it.tl.every(num) || !it.tc.every(num)) return null;
  return { tl: it.tl.slice(0, 512), tc: it.tc.slice(0, 512), sliceSec: num(it.ts) ? it.ts : 1, avg: it.snr, sig: num(it.sdb) ? it.sdb : NaN,
           noise: num(it.ndb) ? it.ndb : NaN, min: num(it.smin) ? it.smin : Math.min(...it.tl), redPct: num(it.red) ? it.red : 0 };
}

// ── Mapping onto what is SHOWN ───────────────────────────────────────────────────────────────────────────────────
/** Character ranges [start, end) of the SHOWN text (offset `base`: where the message text starts in it, `total` its
 *  length) with each slice's dB. The first range reaches back to `from` (the ZCZC header line is the first slice's), the last
 *  to `total` (NNNN is the last slice's). Empty ranges (a fade with nothing printed) are kept — the bar shows them. */
export function snrRanges(q: NavtexSnr, base: number, total: number, from = 0): { start: number; end: number; db: number }[] {
  const out: { start: number; end: number; db: number }[] = [];
  let prev = 0;
  for (let i = 0; i < q.tl.length; i++) {
    const start = i === 0 ? from : base + prev;
    let end = base + Math.max(prev, q.tc[i]);
    if (i === q.tl.length - 1) end = total;
    out.push({ start, end: Math.max(start, end), db: q.tl[i] });
    prev = Math.max(prev, q.tc[i]);
  }
  return out;
}
/** ★ The WORST (lowest) slice dB for each rendered LINE — `lines` are [start, end) character ranges of the wrapped lines
 *  as laid out at the current width (so it follows re-wrapping). A line no slice touches gets null (nothing drawn). */
export function lineWorst(lines: { start: number; end: number }[], ranges: { start: number; end: number; db: number }[]): (number | null)[] {
  return lines.map((l) => {
    let w: number | null = null;
    for (const r of ranges) {
      const overlaps = r.end > r.start ? r.start < Math.max(l.end, l.start + 1) && r.end > l.start
                                       : r.start >= l.start && r.start < Math.max(l.end, l.start + 1);   // an empty slice: the line it sits on
      if (overlaps && (w === null || r.db < w)) w = r.db;
    }
    return w;
  });
}
/** Where the message text starts in a navtexBody() string ("ZCZC GB98\n" / "[start lost]\n" first). */
export function bodyTextBase(body: string): number { const i = body.indexOf('\n'); return i < 0 ? body.length : i + 1; }
/** Where the header line (ZCZC …) starts in a gallery FILE — after the head and its blank line; -1 when not found. */
export function fileBodyStart(file: string): number { const b = file.indexOf('\n\n'); return b < 0 ? -1 : b + 2; }
/** Where the message text starts in a gallery FILE (navtexFileText / the server's fileText: a head, a blank line, then
 *  the header line). -1 when it cannot be found (not a NAVTEX file). */
export function fileTextBase(file: string): number {
  const b = file.indexOf('\n\n');
  if (b < 0) return -1;
  const h = file.indexOf('\n', b + 2);
  return h < 0 ? -1 : h + 1;
}
/** ★ The app's laid-out lines (React Native onTextLayout: each rendered line's own text, in order) → their character
 *  ranges in `text`. The line texts are the source cut where it wrapped; a platform that trims a line's text is re-found
 *  from where the last line ended. A line's ending '\n' is not one of its characters. */
export function lineRangesOf(text: string, lines: { text: string }[]): { start: number; end: number }[] {
  let at = 0;
  return lines.map((l) => {
    let start = at;
    if (text.substr(start, l.text.length) !== l.text) { const f = text.indexOf(l.text.replace(/\n$/, ''), start); if (f >= 0) start = f; }
    const end = start + l.text.length;
    at = end;
    return { start, end: Math.max(start, end - (l.text.endsWith('\n') ? 1 : 0)) };
  });
}
