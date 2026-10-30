// spokenCallsign.ts — CALLSIGNS HEARD IN THE TRANSCRIBE CAPTIONS (V12, 2026-10-30). Stuart: "when using the transcription
// … detect a callsign and put a link to it" — and the key part, the PHONETIC ALPHABET: "Golf Four Tango Papa Juliet" →
// G4TPJ. Every call found becomes a link to its QRZ.com page (cwFormat qrzCallUrl — a link only, nothing fetched).
//
// ★★ A MISSED LINK IS FINE, A WRONG LINK IS NOT. Every candidate must pass the callsign SHAPE (prefix, call-area figure,
//    1–4 letter suffix, portable forms) AND an ALLOCATED ITU series — the same rule as the CW skimmer's spots
//    (android/app/src/main/cpp/decoders/cw_skimmer.cpp `ituSeriesOk` / `isCallCore` / `isCallsign`, ported below 1:1;
//    the skimmer's check lives only in C++, the app had no TypeScript copy of it).
//
// What is read:
//   • NATO / ITU phonetics (Alfa … Zulu), case-insensitive, with the usual speech-recogniser spellings (Juliette,
//     Whisky, X ray, Keylo, Gulf);
//   • the REAL-WORD phonetics hams use on HF (Stuart, 2026-10-30: "some people use things like Kilowatt or Canada or
//     Queen") — a vetted list, each to its letter. ★ Mixed FREELY with NATO, figures and numerals in ONE run ("Hotel
//     Boston 9 Charlie Zebra Kilo" → HB9CZK): nothing here is country-specific. An ordinary word ("Canada", "Texas")
//     can only ever be part of a call because the WHOLE decoded run must pass the shape + ITU check, with a figure in
//     it and at least two letters after it;
//   • figures as words or numerals ("four", "4", "niner", "tree"); "for"/"fore"/"to"/"too" count as 4 / 2 ONLY
//     between two letters ("Golf for Tango"), "oh" as 0 only between two spelled characters;
//   • calls written plainly (G4TPJ, 2E0ABC, EA8/G4TPJ, G4TPJ/P, /M, /MM) — upper case as written, validated the same;
//   • "stroke" / "slash" between parts, and "portable" / "mobile" / "maritime mobile" straight after a call (/P /M /MM).
// Filler ("this is", "de", "from") is not a spelling word, so it simply ends one run and the next call starts another.
// Two calls spoken back to back with no filler are split by the shape itself (a suffix is at most 4 letters and a call
// has one figure group), greedy longest-first from the left. A repeated CQ links every occurrence — no dedupe.
//
// ★ Erasable TypeScript only (no enums, no imports): scripts/test_spoken_callsign.ts runs this file under Node.

// ── The ITU series + callsign shape (port of cw_skimmer.cpp) ──────────────────────────────────────────────────────
const isL = (c: string) => c >= 'A' && c <= 'Z';
const isD = (c: string) => c >= '0' && c <= '9';
/** ★★ THE ITU SERIES (Radio Regulations Appendix 42): letter+letter all but Q; figure+letter 2–9; one letter alone
 *  only B F G I K M N R W; letter+figure per the blocks below (A6, C3, E7, S5, T7, V5, Z3 …). */
function ituSeriesOk(pre: string, afterPre: string): boolean {
  if (!pre) return false;
  const a = pre[0];
  if (a === 'Q' || a === '0' || a === '1') return false;
  if (pre.length >= 2) return isD(a) ? isL(pre[1]) : (isL(pre[1]) || isD(pre[1]));
  if ('BFGIKMNRW'.includes(a)) return true;
  if (!afterPre || !isD(afterPre[0])) return false;
  const d = afterPre[0];
  const LD: Record<string, [string, string]> = {
    A: ['2', '9'], C: ['2', '9'], D: ['2', '9'], E: ['2', '7'], H: ['2', '9'], J: ['2', '8'], L: ['2', '9'],
    P: ['2', '9'], S: ['2', '9'], T: ['2', '8'], V: ['2', '8'], Y: ['2', '9'], Z: ['2', '8'] };
  const r = LD[a];
  return !!r && d >= r[0] && d <= r[1];
}
/** PREFIX DIGIT SUFFIX with no '/'. */
function isCallCore(s: string): boolean {
  if (s.length < 3 || s.length > 8 || !/^[A-Z0-9]+$/.test(s)) return false;
  let d = -1;
  for (let i = 0; i < s.length; i++) if (isD(s[i])) d = i;
  if (d < 0 || d === s.length - 1) return false;
  const suffix = s.length - 1 - d;
  if (suffix < 1 || suffix > 4) return false;
  let a = d; while (a > 0 && isD(s[a - 1])) a--;
  if (d - a > 1) return false;
  const pre = s.slice(0, a);
  if (!pre || pre.length > 3 || !/[A-Z]/.test(pre)) return false;
  return ituSeriesOk(pre, s.slice(a));
}
/** Words the skimmer never spots as calls (kept in step with cw_skimmer.cpp kWords) — and a few written tokens that
 *  have a call's shape but are something else. */
const NOT_CALLS = new Set(['CQ', 'DE', 'TEST', 'TU', 'TNX', 'QRZ', 'QSL', 'QRL', 'QRS', 'QRQ', 'QTH', 'NAME', 'RST', 'UR',
  'FB', 'OM', 'GM', 'GA', 'GE', 'GN', 'PSE', 'AGN', 'HW', 'CPY', 'ES', '73', '88', '5NN', '599', 'K', 'KN', 'BK', 'R', 'RR',
  'POTA', 'SOTA', 'WWFF', 'IOTA', 'DX', 'NA', 'EU', 'AS', 'AF', 'OC', 'SA', 'JA', 'C3PO', 'W3C']);
/** ★ A callsign as the CW skimmer validates a spot: the shape, the ITU series, portable forms (EA8/G4ABC, G4ABC/P,
 *  G4ABC/MM — the longest part is the call, the others ≤ 4), and not one letter repeated round a figure ("EEE5E"). */
export function isValidCallsign(raw: string): boolean {
  const s = String(raw || '').toUpperCase();
  if (s.length < 3 || s.length > 14 || NOT_CALLS.has(s)) return false;
  const parts = s.split('/');
  if (parts.length > 3) return false;
  let best = 0;
  for (let i = 1; i < parts.length; i++) if (parts[i].length > parts[best].length) best = i;
  for (let i = 0; i < parts.length; i++) {
    if (!parts[i] || (parts[i].length > 4 && i !== best) || !/^[A-Z0-9]+$/.test(parts[i])) return false;
  }
  if (!isCallCore(parts[best])) return false;
  const letters = parts[best].replace(/[^A-Z]/g, '');
  return !(letters.length >= 3 && [...letters].every((c) => c === letters[0]));
}
/** The station's own call of a portable form (the longest part). */
function coreOf(call: string): string {
  let best = '';
  for (const p of call.split('/')) if (p.length > best.length) best = p;
  return best;
}

// ── The spoken alphabet ───────────────────────────────────────────────────────────────────────────────────────────
/** NATO / ITU phonetics and the spellings a speech recogniser writes for them. */
const NATO: Record<string, string> = {
  alpha: 'A', alfa: 'A', bravo: 'B', charlie: 'C', charly: 'C', delta: 'D', echo: 'E', foxtrot: 'F', golf: 'G', gulf: 'G',
  hotel: 'H', india: 'I', juliet: 'J', juliett: 'J', juliette: 'J', kilo: 'K', keylo: 'K', lima: 'L', mike: 'M',
  november: 'N', oscar: 'O', papa: 'P', quebec: 'Q', romeo: 'R', sierra: 'S', tango: 'T', uniform: 'U', victor: 'V',
  whiskey: 'W', whisky: 'W', xray: 'X', 'x-ray': 'X', yankee: 'Y', zulu: 'Z',
};
/** ★ The real-word phonetics heard on HF (Stuart, 2026-10-30) — the common ones only, each to its letter. Left out on
 *  purpose: "Radio" (said constantly on air: "Radio Four" would read as R4…), and any word that is another letter's
 *  NATO word. These are ordinary English words, so they only ever count inside a run the shape + ITU check accepts. */
const ALTERNATES: Record<string, string> = {
  america: 'A', amsterdam: 'A', boston: 'B', baltimore: 'B', canada: 'C', denmark: 'D', england: 'E', easy: 'E',
  france: 'F', florida: 'F', germany: 'G', george: 'G', honolulu: 'H', henry: 'H', italy: 'I', japan: 'J',
  kilowatt: 'K', king: 'K', kentucky: 'K', london: 'L', mexico: 'M', mary: 'M', norway: 'N', nancy: 'N',
  ontario: 'O', ocean: 'O', portugal: 'P', peter: 'P', pacific: 'P', queen: 'Q', roger: 'R', spain: 'S',
  santiago: 'S', sugar: 'S', texas: 'T', tokyo: 'T', tare: 'T', united: 'U', uruguay: 'U', victoria: 'V',
  venezuela: 'V', washington: 'W', william: 'W', yokohama: 'Y', yellow: 'Y', zanzibar: 'Z', zebra: 'Z',
};
const DIGITS: Record<string, string> = {
  zero: '0', one: '1', two: '2', three: '3', tree: '3', four: '4', fower: '4', five: '5', fife: '5', six: '6',
  seven: '7', eight: '8', nine: '9', niner: '9',
};
/** Said between two LETTERS only (Golf for Tango → G4T). */
const SOFT_DIGITS: Record<string, string> = { for: '4', fore: '4', to: '2', too: '2' };

/** One spelling unit: the characters it stands for and where it sits in the text. */
interface Unit { ch: string; kind: 'L' | 'D' | 'X' | 'S'; start: number; end: number }
export interface CallMatch {
  /** The call as linked: "G4TPJ", "EA8/G4TPJ", "G4TPJ/P". */
  call: string;
  /** Where it sits in the text — the whole spoken run, or the written token. */
  start: number; end: number;
  /** true = spelled out (phonetics / figures); false = written plainly. */
  spoken: boolean;
}

interface Tok { w: string; raw: string; start: number; end: number; gapPunct: boolean }
function tokenize(text: string): Tok[] {
  const out: Tok[] = [];
  const re = /[A-Za-z0-9][A-Za-z0-9'/-]*/g;
  let m: RegExpExecArray | null, last = 0;
  while ((m = re.exec(text))) {
    let raw = m[0];
    while (raw.length > 1 && /['/-]$/.test(raw)) raw = raw.slice(0, -1);
    const between = text.slice(last, m.index);
    out.push({ w: raw.toLowerCase(), raw, start: m.index, end: m.index + raw.length, gapPunct: /[.!?;:]/.test(between) });
    last = m.index + raw.length;
  }
  // "x ray" (two words) → one token, X
  for (let i = out.length - 2; i >= 0; i--) {
    if (out[i].w === 'x' && out[i + 1].w === 'ray') out.splice(i, 2, { ...out[i], w: 'xray', raw: 'xray', end: out[i + 1].end });
  }
  return out;
}

/** The letter a word spells, or ''. */
function letterOf(t: Tok): string {
  return NATO[t.w] ?? ALTERNATES[t.w] ?? (/^[B-HJ-Z]$/.test(t.raw) ? t.raw : '');   // ★ a lone capital, never A or I
}
/** A short written chunk inside a spoken run: "G4", "2E0", "EA8", "M0" — upper case, with a figure. */
function chunkOf(t: Tok): string {
  return /^[A-Z0-9]{2,4}$/.test(t.raw) && /[0-9]/.test(t.raw) && !/^[0-9]{2,}$/.test(t.raw) ? t.raw : '';
}

/** ★ Every callsign in one caption line, in order. Pure and cheap — called once per finalised line (cached by the
 *  caller). */
export function findCallsigns(text: string): CallMatch[] {
  const toks = tokenize(String(text || ''));
  const found: CallMatch[] = [];
  // ── 1. Units: what each token spells (or null = a run ends here) ──
  const units: (Unit | null)[] = toks.map((t, i) => {
    const l = letterOf(t);
    if (l) return { ch: l, kind: 'L', start: t.start, end: t.end };
    if (DIGITS[t.w]) return { ch: DIGITS[t.w], kind: 'D', start: t.start, end: t.end };
    if (/^[0-9]$/.test(t.raw)) return { ch: t.raw, kind: 'D', start: t.start, end: t.end };
    if (t.w === 'stroke' || t.w === 'slash') return { ch: '/', kind: 'S', start: t.start, end: t.end };
    const c = chunkOf(t);
    if (c) return { ch: c, kind: 'X', start: t.start, end: t.end };
    return null;
  });
  // The soft figures, now the letters on each side are known.
  const isLetterUnit = (u: Unit | null | undefined) => !!u && (u.kind === 'L' || (u.kind === 'X' && /[A-Z]$/.test(u.ch)));
  for (let i = 1; i + 1 < toks.length; i++) {
    if (units[i]) continue;
    const w = toks[i].w;
    if (SOFT_DIGITS[w] && isLetterUnit(units[i - 1]) && isLetterUnit(units[i + 1]))
      units[i] = { ch: SOFT_DIGITS[w], kind: 'D', start: toks[i].start, end: toks[i].end };
    else if (w === 'oh' && units[i - 1] && units[i + 1] && units[i - 1]!.kind !== 'S' && units[i + 1]!.kind !== 'S')
      units[i] = { ch: '0', kind: 'D', start: toks[i].start, end: toks[i].end };
  }
  // ── 2. Runs of units; a sentence stop (. ! ? ; :) also ends a run ──
  const plainAt = new Map<number, CallMatch>();
  toks.forEach((t, i) => {
    // A call written plainly — upper case as written, validated the same way.
    if (/^[A-Z0-9]+(?:\/[A-Z0-9]+){0,2}$/.test(t.raw) && /[A-Z]/.test(t.raw) && /[0-9]/.test(t.raw) && isValidCallsign(t.raw)) {
      plainAt.set(i, { call: t.raw, start: t.start, end: t.end, spoken: false });
    }
  });
  let i = 0;
  while (i < toks.length) {
    if (plainAt.has(i) && !units[i]) { found.push(withPortable(plainAt.get(i)!, toks, i + 1)); i++; continue; }
    if (!units[i]) { i++; continue; }
    let j = i + 1;
    while (j < toks.length && units[j] && !toks[j].gapPunct) j++;
    found.push(...scanRun(units.slice(i, j) as Unit[], toks, i, plainAt));
    i = j;
  }
  return found;
}

/** "portable" / "mobile" / "maritime mobile" straight after a call → /P /M /MM (only if not already portable). */
function withPortable(m: CallMatch, toks: Tok[], next: number): CallMatch {
  if (/\/(P|M|MM)$/.test(m.call)) return m;
  const a = toks[next]?.w, b = toks[next + 1]?.w;
  if (a === 'portable') return { ...m, call: `${m.call}/P`, end: toks[next].end };
  if (a === 'mobile') return { ...m, call: `${m.call}/M`, end: toks[next].end };
  if (a === 'maritime' && b === 'mobile') return { ...m, call: `${m.call}/MM`, end: toks[next + 1].end };
  return m;
}

/** ★ The run split into calls by a small DP: the split that COVERS THE MOST of the run wins, so two calls said back to
 *  back with no filler come apart ("…papa juliet mike zero…" → G4TPJ + M0PKZ, not G4TPJM + nothing). Ties go to
 *  the split with fewer 4-letter suffixes (rarer than 1–3), then fewer calls — a repeated CQ ("…Kilo Hotel Bravo…")
 *  reads as HB9CZK HB9CZK, not HB9CZKH B9CZK. */
function scanRun(run: Unit[], toks: Tok[], base: number, plainAt: Map<number, CallMatch>): CallMatch[] {
  const n = run.length;
  type Best = { cover: number; pen: number; calls: number; next: number; call: string };
  const best: Best[] = new Array(n + 1);
  best[n] = { cover: 0, pen: 0, calls: 0, next: n, call: '' };
  const better = (x: Best, y: Best) => x.cover !== y.cover ? x.cover > y.cover : x.pen !== y.pen ? x.pen < y.pen : x.calls < y.calls;
  for (let a = n - 1; a >= 0; a--) {
    let b0: Best = { ...best[a + 1], next: a + 1, call: '' };   // skip this unit
    for (let b = Math.min(n, a + 16); b > a + 1; b--) {
      const seg = run.slice(a, b);
      if (seg[0].kind === 'S' || seg[seg.length - 1].kind === 'S') continue;
      const s = seg.map((u) => u.ch).join('');
      if (!spelledOk(seg, s)) continue;
      const suffix = (coreOf(s).match(/[A-Z]+$/) ?? [''])[0];
      const c: Best = { cover: best[b].cover + (b - a), pen: best[b].pen + (suffix.length >= 4 ? 1 : 0),
        calls: best[b].calls + 1, next: b, call: s };
      if (better(c, b0)) b0 = c;
    }
    best[a] = b0;
  }
  const out: CallMatch[] = [];
  for (let a = 0; a < n;) {
    const r = best[a];
    if (r.call) out.push(withPortable({ call: r.call, start: run[a].start, end: run[r.next - 1].end, spoken: true }, toks, base + r.next));
    else if (plainAt.has(base + a)) out.push(withPortable(plainAt.get(base + a)!, toks, base + a + 1));   // "K1AB" written
    a = r.next;
  }
  return out;
}

/** ★ A spelled candidate: the shape + ITU series, a call of at least 4 characters with at least TWO suffix letters
 *  ("Golf Four Tango" is a half call, not G4T), and at least one spoken word in it (a lone written token is handled
 *  as a plain call). */
function spelledOk(seg: Unit[], s: string): boolean {
  if (seg.length < 2 || !isValidCallsign(s)) return false;
  const core = coreOf(s);
  if (core.length < 4) return false;
  const suffix = (core.match(/[A-Z]+$/) ?? [''])[0];
  return suffix.length >= 2;
}

/** A caption line cut into plain text and callsign pieces, in order — for drawing the links. */
export type CaptionPiece = { text: string; call?: undefined } | { text: string; call: string; spoken: string };
export function captionPieces(line: string): CaptionPiece[] {
  const ms = findCallsigns(line);
  if (!ms.length) return [{ text: line }];
  const out: CaptionPiece[] = [];
  let at = 0;
  for (const m of ms) {
    if (m.start > at) out.push({ text: line.slice(at, m.start) });
    out.push({ text: m.call, call: m.call, spoken: line.slice(m.start, m.end) });
    at = m.end;
  }
  if (at < line.length) out.push({ text: line.slice(at) });
  return out;
}

const CACHE = new Map<string, CaptionPiece[]>();
const CACHE_MAX = 800;
function piecesOf(line: string): CaptionPiece[] {
  let p = CACHE.get(line);
  if (!p) {
    p = captionPieces(line);
    if (CACHE.size >= CACHE_MAX) CACHE.delete(CACHE.keys().next().value as string);
    CACHE.set(line, p);
  }
  return p;
}
const TRANSLATED = '   → ';

/** ★ The whole TRANSCRIBE transcript cut into plain runs and links, for TranscriptText. Each LINE is parsed once (a
 *  bounded cache keyed by the line text); lines with no call stay joined in one plain run. A translated line ("   → …",
 *  under its source) keeps the links found on the SOURCE — any call the translation lost is added at its end. */
export function transcriptPieces(text: string): CaptionPiece[] {
  const out: CaptionPiece[] = [];
  let plain = '';
  const flush = () => { if (plain) { out.push({ text: plain }); plain = ''; } };
  const lines = text.split('\n');
  let srcCalls: string[] = [];
  lines.forEach((line, i) => {
    const nl = i < lines.length - 1 ? '\n' : '';
    const ps = line ? piecesOf(line) : [{ text: '' } as CaptionPiece];
    const calls = ps.filter((p) => p.call).map((p) => p.call as string);
    const lost = line.startsWith(TRANSLATED) ? srcCalls.filter((c) => !calls.includes(c)) : [];
    if (!line.startsWith(TRANSLATED)) srcCalls = calls;
    if (calls.length === 0 && lost.length === 0) { plain += line + nl; return; }
    for (const p of ps) {
      if (p.call) { flush(); out.push(p); } else plain += p.text;
    }
    for (const c of lost) { plain += ' · '; flush(); out.push({ text: c, call: c, spoken: c }); }
    plain += nl;
  });
  flush();
  return out;
}
