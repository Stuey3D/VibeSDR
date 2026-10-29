#!/usr/bin/env node
// gen-cw-lm.mjs — the CW skimmer's SMALL language model (V12, 2026-10-29), generated from src/utils/cwVocab.ts.
//
//   node scripts/gen-cw-lm.mjs            → android/app/src/main/cpp/decoders/cw_lm_data.h  (+ prints its size)
//   node scripts/gen-cw-lm.mjs --corpus   → also writes the synthetic corpus to $TMPDIR/cw_lm_corpus.txt (to read it)
//
// ★★ Stuart chose THE CHEAP OPTION (2026-10-29 21:45): "i think we go for the cheap option … that may clear a lot of
//    things up" — a small statistical model of ham CW, a few hundred kB, that only BREAKS TIES between the readings the
//    timing allows (where a word ends, where a letter ends). It never changes a letter that was sent.
// ★ What it is: a character 5-gram model (a symbol given the four before it) with Witten–Bell smoothing written in
//   back-off form, plus the word list (codes, QSO words, names, towns, common words of other languages). Trained on a
//   SYNTHETIC corpus generated here — QSO templates (CQ, a rag-chew, a contest, a park activation), callsigns from the ITU
//   prefix series, RST, names, rigs, weather, and the same kinds of exchanges in Italian, Spanish, French and German so
//   the model does not take another language for errors. Nothing scraped; no other program's data or code used.
// ★ Deterministic: a fixed seed, so the header only changes when this file or cwVocab.ts does.
import { writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { tmpdir } from 'node:os';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const V = await import(join(root, 'src/utils/cwVocab.ts'));

// ── the alphabet: the decoder's symbols ──
// 0 = space; A–Z 1–26; 0–9 27–36; punctuation 37–49; prosigns 50–54. (Must match cw_lm.cpp's symbolOf.)
const PUNCT = ['/', '?', '=', '+', '.', ',', "'", '-', '(', ')', ':', '"', '@'];
const PROSIGNS = ['<SN>', '<AS>', '<SK>', '<KA>', '<HH>'];
function symbols(text) {
  const out = [];
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === ' ') { if (out.length && out[out.length - 1] !== 0) out.push(0); continue; }
    if (c >= 'A' && c <= 'Z') { out.push(c.charCodeAt(0) - 64); continue; }
    if (c >= '0' && c <= '9') { out.push(27 + c.charCodeAt(0) - 48); continue; }
    if (c === '<') { const e = text.indexOf('>', i); const p = PROSIGNS.indexOf(text.slice(i, e + 1)); if (p >= 0) out.push(50 + p); i = e; continue; }
    const p = PUNCT.indexOf(c); if (p >= 0) out.push(37 + p);
  }
  return out;
}

// ── a seeded random source ──
let seed = 0x5eed1234;
const rnd = () => { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed / 4294967296; };
const pick = (a) => a[Math.floor(rnd() * a.length)];
const chance = (p) => rnd() < p;

// ── callsigns: real ITU series shapes (prefix + call-area figure + 1–3 letter suffix) ──
const PREFIXES = ['G', 'M', 'G', 'M', '2E', 'GW', 'GM', 'GI', 'MW', 'MM', 'EI', 'F', 'DL', 'DK', 'DJ', 'DF', 'DG', 'DH', 'DO', 'DB',
  'ON', 'PA', 'PD', 'I', 'IZ', 'IK', 'IU', 'IW', 'EA', 'EB', 'EC', 'CT', 'HB9', 'OE', 'OK', 'OM', 'SP', 'SQ', 'HA', 'YO', 'LZ', 'S5',
  '9A', 'E7', 'YU', 'SV', 'OH', 'SM', 'SA', 'LA', 'OZ', 'ES', 'YL', 'LY', 'UA', 'RA', 'R', 'UR', 'UT', 'EW', '4X', 'K', 'W', 'N',
  'AA', 'KB', 'KD', 'WA', 'VE', 'VA', 'JA', 'JH', 'VK', 'ZL', 'ZS', 'PY', 'LU', 'CE', 'XE', 'TA', 'A6', '5B', 'EA8', 'CU', 'TF'];
const L = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ';
function callsign() {
  const p = pick(PREFIXES);
  const n = Math.floor(rnd() * 10);
  const len = chance(0.15) ? 1 : chance(0.35) ? 2 : 3;
  let s = ''; for (let i = 0; i < len; i++) s += L[Math.floor(rnd() * 26)];
  let c = p + n + s;
  if (chance(0.06)) c += pick(['/P', '/M', '/QRP', '/MM']);
  if (chance(0.03)) c = pick(['EA8', 'F', 'DL', 'EI', 'GM']) + '/' + c;
  return c;
}
const rst = () => pick(['599', '599', '579', '589', '569', '559', '549', '479', '599', '5NN', '339', '449', '539']);
const num = (a, b) => String(a + Math.floor(rnd() * (b - a + 1)));
const name = () => pick(V.CW_NAMES);
const place = () => pick(V.CW_PLACES);
const rig = () => pick(['IC7300', 'IC7610', 'IC705', 'FT991', 'FTDX10', 'FT817', 'FT818', 'K3', 'K4', 'KX2', 'KX3', 'TS590', 'TS480',
  'QCX', 'HOMEBREW', 'ELECRAFT K2', 'IC718', 'FT450', 'G90', 'TS2000']);
const ant = () => pick(['DIPOLE', 'INV VEE', 'END FED', 'VERTICAL', 'LOOP', 'WINDOM', 'YAGI', 'LONG WIRE', 'G5RV', 'DOUBLET', 'HEX BEAM']);
const pwr = () => pick(['5W', '10W', '50W', '100W', '400W', '1KW', 'QRP 5W', '20W']);
const wx = () => pick([`WX FINE TEMP ${num(1, 25)}C`, 'WX RAIN', 'WX CLOUDY', `WX COLD ${num(0, 9)}C`, 'WX SUNNY', 'WX WINDY ES RAIN', `WX ${num(5, 20)}C`]);
const greet = () => pick(['GM', 'GA', 'GE', 'GN', 'HELLO', 'GE OM', 'GM DR OM', 'TNX FER CALL', 'TNX FER QSO', 'R R TNX']);
const bye = () => pick(['73', '73 ES GL', 'TNX QSO 73', 'CUL 73', 'HPE CUAGN 73', 'TNX FER NICE QSO 73', 'GL ES 73', 'BCNU 73', '73 GN']);
const end = () => pick(['K', 'KN', 'BK', '+', '<SK>', 'KN', 'K', 'AR']);

const lines = [];
function qso() {
  const a = callsign(), b = callsign(), na = name(), nb = name();
  const out = [];
  out.push(`${chance(0.3) ? 'QRL? ' : ''}CQ CQ CQ DE ${a} ${a} ${a} ${chance(0.5) ? 'PSE ' : ''}K`);
  out.push(`${a} DE ${b} ${b} ${end()}`);
  out.push(`${b} DE ${a} ${greet()} ${chance(0.5) ? 'DR ' + nb + ' ' : ''}= UR RST ${rst()} ${rst()} ${chance(0.5) ? 'QSB' : ''} = ` +
    `${pick(['NAME', 'OP', 'OP IS', 'NAME IS', 'NAME HR'])} ${na} ${na} = QTH ${pick(['', 'NR ', 'IS ', 'IS NR '])}${place()} ${place()} = HW? ${b} DE ${a} ${end()}`);
  out.push(`${a} DE ${b} R R TNX ${na} FER RPRT = UR RST ${rst()} = NAME ${nb} ${nb} = QTH ${place()} = ${pick(['', 'RIG ' + rig() + ' ', 'RIG HR ' + rig() + ' PWR ' + pwr() + ' '])}` +
    `${pick(['', 'ANT ' + ant() + ' '])}${pick(['', wx() + ' '])}= ${pick(['HW CPY?', 'HW?', 'BK', ''])} ${a} DE ${b} ${end()}`);
  if (chance(0.5)) out.push(`${b} DE ${a} FB ${nb} ${pick(['TNX FER INFO', 'SOLID CPY HR', 'UR SIGS FB', 'CONDX NOT GOOD', 'IN VIEW OF CONDX HERE WONT HOLD IT TONIGHT', 'HAD A FEW QSOS TODAY', 'GQRP VALVE WEEKEND', 'QRM HR', 'QRN HVY'])} = ${bye()} ${b} DE ${a} <SK>`);
  out.push(`${a} DE ${b} ${pick(['TNX FER QSO', 'TNX FER NICE QSO', 'FB', 'R TNX'])} ${na} ${bye()} <SK> ${chance(0.3) ? 'E E' : ''}`);
  return out;
}
function contest() {
  const a = callsign(); const out = [];
  out.push(`CQ TEST ${a} ${a} TEST`);
  for (let i = 0; i < 3; i++) { const b = callsign(); out.push(`${b}`); out.push(`${b} 5NN ${pick(['14', '15', '27', '28', '001', '0' + num(10, 99), num(100, 999), 'TT1', 'DL', 'EU'])}`); out.push(`TU ${a} TEST`); }
  return out;
}
function pota() {
  const a = callsign();
  return [`CQ POTA CQ POTA DE ${a} ${a} K`, `${callsign()} ${callsign()}`, `R ${callsign()} GM UR ${rst()} ${rst()} ${pick(['IL', 'ES', 'CA', 'NY', 'TX', 'ONT'])} ${pick(['IL', 'ES', 'CA'])} BK`,
    `BK TU UR ${rst()} 73 TU BK`, `QRZ? DE ${a} K`, `CQ SOTA DE ${a}/P K`];
}
function foreign() {
  const a = callsign(), b = callsign();
  return pick([
    [`${b} DE ${a} BUONA SERA ${name()} = RST ${rst()} = QTH ${pick(['ROMA', 'MILANO', 'TORINO', 'NAPOLI'])} = ANTENNA FILARE = GRAZIE PER IL QSO 73 ${end()}`,
      `${a} DE ${b} TUTTO OK GRAZIE = QUI TUTTA LA ESTATE SONO STATO CON LA ANTENNA FILARE = CIAO 73`],
    [`${b} DE ${a} BUENAS NOCHES = UR RST ${rst()} = NOMBRE ${pick(['JOSE', 'JUAN', 'PACO'])} = QTH ${pick(['MADRID', 'VALENCIA', 'BARCELONA'])} = MUCHAS GRACIAS POR EL QSO 73 ${end()}`,
      `${a} DE ${b} MUY BIEN GRACIAS = AQUI TODO BIEN = SALUDOS 73`],
    [`${b} DE ${a} BONSOIR = RST ${rst()} = PRENOM ${pick(['PIERRE', 'JEAN', 'MICHEL'])} = QTH ${pick(['PARIS', 'LYON'])} = MERCI POUR LE QSO 73 ${end()}`],
    [`${b} DE ${a} GUTEN ABEND = RST ${rst()} = NAME ${pick(['HANS', 'KLAUS', 'UWE'])} = QTH ${pick(['BERLIN', 'HAMBURG', 'MUENCHEN'])} = VIELEN DANK FUER DAS QSO 73 ${end()}`],
  ]);
}
// ★ The codes in use: every term's code and alias appears in context, so the model has seen each one.
function codes() {
  const t = pick(V.CW_TERMS);
  const c = pick([t.code, ...(t.aliases ?? [])]);
  return [`${callsign()} DE ${callsign()} ${c}${t.ask && chance(0.3) ? '?' : ''} ${pick(V.CW_QSO_WORDS)} ${end()}`];
}

const target = 2_400_000;   // characters of corpus
let size = 0;
while (size < target) {
  const r = rnd();
  const block = r < 0.5 ? qso() : r < 0.65 ? contest() : r < 0.75 ? pota() : r < 0.87 ? foreign() : codes();
  for (const l of block) { const s = l.replace(/\s+/g, ' ').trim(); lines.push(s); size += s.length + 1; }
}
if (process.argv.includes('--corpus')) writeFileSync(join(tmpdir(), 'cw_lm_corpus.txt'), lines.join('\n'));

// ── counts, orders 1..5 (each line is preceded by a space: the model's start of text) ──
const N = 5;
const counts = Array.from({ length: N + 1 }, () => new Map());   // order → Map(key → count)
const keyOf = (arr, i, n) => { let k = 0; for (let j = 0; j < n; j++) k = k * 64 + arr[i + j]; return k; };
for (const l of lines) {
  const s = [0, 0, 0, 0, ...symbols(l), 0];   // ★ the start: spaces (a word boundary), as the decoder's context starts
  for (let n = 1; n <= N; n++)
    for (let i = (n === 1 ? 4 : 4 - (n - 1)); i + n <= s.length; i++) {
      if (i + n - 1 < 4) continue;   // the predicted symbol is never a start pad
      const k = keyOf(s, i, n); counts[n].set(k, (counts[n].get(k) ?? 0) + 1);
    }
}
// ── Witten–Bell, then back-off form ──
const SYM = 55;
const ctxStats = Array.from({ length: N + 1 }, () => new Map());   // order of the full n-gram → Map(ctxKey → {c, t})
for (let n = 2; n <= N; n++) for (const [k, c] of counts[n]) {
  const h = Math.floor(k / 64); const s = ctxStats[n].get(h) ?? { c: 0, t: 0 }; s.c += c; s.t += 1; ctxStats[n].set(h, s);
}
let total1 = 0; for (const c of counts[1].values()) total1 += c;
const prob = Array.from({ length: N + 1 }, () => new Map());
// unigram: add-one over the alphabet
for (let x = 0; x < SYM; x++) prob[1].set(x, ((counts[1].get(x) ?? 0) + 1) / (total1 + SYM));
const pLower = (n, k) => {   // P_wb of the n-gram key k (its last n symbols), recursively
  if (n === 1) return prob[1].get(k % 64);
  const v = prob[n].get(k % 64 ** n); if (v !== undefined) return v;
  const h = Math.floor((k % 64 ** n) / 64); const st = ctxStats[n].get(h);
  if (!st) return pLower(n - 1, k);
  return (st.t * pLower(n - 1, k)) / (st.c + st.t);
};
for (let n = 2; n <= N; n++) for (const [k, c] of counts[n]) {
  const h = Math.floor(k / 64); const st = ctxStats[n].get(h);
  prob[n].set(k, (c + st.t * pLower(n - 1, k)) / (st.c + st.t));
}
// ★ Pruned: n-grams of order ≥ 3 seen once are left to the back-off (most are random callsign letters).
const keep = Array.from({ length: N + 1 }, () => []);
for (let x = 0; x < SYM; x++) keep[1].push(x);
for (let n = 2; n <= N; n++) for (const [k, c] of counts[n]) if (n < 3 || c >= 2) keep[n].push(k);
for (let n = 1; n <= N; n++) keep[n].sort((a, b) => a - b);
// back-off weights for each kept context (order n context → used by order n+1)
const kept = Array.from({ length: N + 1 }, (_, n) => new Set(keep[n]));
const pBack = (n, k) => {   // the model's probability in BACK-OFF form, as the decoder computes it
  if (n === 1) return prob[1].get(k % 64);
  const kk = k % 64 ** n;
  if (kept[n].has(kk)) return prob[n].get(kk);
  return bow(n - 1, Math.floor(kk / 64)) * pBack(n - 1, k);
};
const bowCache = Array.from({ length: N + 1 }, () => new Map());
function bow(n, h) {   // h: an order-n context key (n symbols); weight for backing off from order n+1 to n
  if (n === 0) return 1;
  const c = bowCache[n].get(h); if (c !== undefined) return c;
  let num = 1, den = 1;
  for (let x = 0; x < SYM; x++) {
    const k = h * 64 + x;
    if (kept[n + 1].has(k)) { num -= prob[n + 1].get(k); den -= pBack(n, k); }
  }
  const w = num <= 1e-9 || den <= 1e-9 ? 1 : num / den;
  bowCache[n].set(h, w); return w;
}
// ── quantise: −ln P in steps of 1/16 nat (uint8, 0…15.9 nats); ln bow in steps of 1/16 (int8) ──
const qP = (p) => Math.max(0, Math.min(255, Math.round(-Math.log(p) * 16)));
const qB = (w) => Math.max(-127, Math.min(127, Math.round(Math.log(w) * 16)));

let h = `// cw_lm_data.h — GENERATED by scripts/gen-cw-lm.mjs from src/utils/cwVocab.ts. Do not edit; run the script.\n`;
h += `// A character 5-gram model of ham CW in back-off form (Witten–Bell), trained on a synthetic corpus of ${lines.length} lines.\n`;
h += `// Keys: the n symbols base 64, oldest first. P: −ln P × 16. BOW: ln(back-off weight) × 16.\n#pragma once\n#include <cstdint>\n\nnamespace vibe { namespace cw { namespace lmdata {\n`;
let bytes = 0;
for (let n = 1; n <= N; n++) {
  const ks = keep[n];
  h += `constexpr int kN${n} = ${ks.length};\n`;
  h += `static const uint32_t kKey${n}[] = {${ks.join(',')}};\n`;
  h += `static const uint8_t kP${n}[] = {${ks.map((k) => qP(prob[n].get(k))).join(',')}};\n`;
  bytes += ks.length * 5;
  if (n < N) { h += `static const int8_t kB${n}[] = {${ks.map((k) => qB(bow(n, k))).join(',')}};\n`; bytes += ks.length; }
}
// ── the word list: sorted, NUL-separated ──
const words = new Set();
for (const t of V.CW_TERMS) { for (const c of [t.code, ...(t.aliases ?? [])]) if (/^[A-Z0-9]+$/.test(c)) words.add(c); }
for (const list of [V.CW_QSO_WORDS, V.CW_NAMES, V.CW_OTHER_LANGUAGE_WORDS, V.CW_PLACES]) for (const w of list) if (/^[A-Z0-9]+$/.test(w)) words.add(w);
const wl = [...words].sort();
h += `constexpr int kWords = ${wl.length};\nstatic const char* const kWord[] = {${wl.map((w) => `"${w}"`).join(',')}};\n`;
bytes += wl.join(' ').length + wl.length * 4;
h += `}}} // namespace vibe::cw::lmdata\n`;
const out = join(root, 'android/app/src/main/cpp/decoders/cw_lm_data.h');
writeFileSync(out, h);
console.log(`cw_lm_data.h: ${lines.length} corpus lines (${(size / 1e6).toFixed(1)} M chars); n-grams ${keep.slice(1).map((k) => k.length).join('/')}; ${wl.length} words; ≈ ${(bytes / 1024).toFixed(0)} kB of tables`);
