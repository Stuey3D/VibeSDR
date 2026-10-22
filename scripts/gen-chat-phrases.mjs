// gen-chat-phrases.mjs — THE CANNED CHAT'S WORDS, FROM ONE PLACE PER LANGUAGE (2026-10-22).
//
// ★★★ WHY. Stuart in the UK and Kiko in Brazil on one shared tuner: Stuart asks "Can I tune?", Kiko reads
//     "Posso sintonizar?", Kiko answers in Portuguese and Stuart reads English. The wire already carries
//     phrase IDS, never text (chatPhrases() in local_sdr_shim.cpp: "each client renders it in its own words
//     and its own language") — so nothing changes on the server; each client just needs the words.
//     CHAT ONLY, approved by Stuart: "chat can be a little bit more flexible as long as the message gets
//     across." Everything else in the app stays English (reviewed translation only).
// ★★★ WRITTEN NATIVELY PER MEANING, NOT TRANSLATED FROM THE ENGLISH (Stuart, 2026-10-22: "proper messages
//     customised for each language … as long as the message is equivalent on both sides it doesnt have to be
//     exact"). Each language says what a listener there would actually type — its own idiom, one casual
//     register throughout, hobby slang where it has some ("Condx", "Bela escuta!") — with the same MEANING and
//     the same weight: an ask stays an ask, a gentle no stays gentle, a yes stays a yes. That weight is load-
//     bearing: ask before tuning reads the YES / NO / ask ids (vibe_tune_etiquette.h replyMeaning, isAsk), so a
//     rewording may never move a phrase between them. Radio terms stay terms (RTTY, FT8, DX, SSB …).
// ★★ ONE SOURCE OF TRUTH. i18n/chat/phrases.json holds the ids in order and what each one MEANS (group,
//    band, wiki page, decoder); i18n/chat/<lang>.json holds the WORDS. This writes the four readers:
//      src/services/chatWords.generated.ts   — the app AND the browser (web/client imports src/services)
//      spike/WristSDR/WristSDR/Chat.swift     — Jr,    between the GENERATED markers
//      ios/VibeSDRWatch/Chat.swift            — Buddy, between the GENERATED markers
//    The Swift goes INSIDE Chat.swift rather than a file of its own: both watch projects list their files
//    by hand, and a file Xcode does not know about is a Cloud build failure (typecheck-jr.sh's lesson).
// ★★ ADDING A LANGUAGE = one new i18n/chat/<tag>.json (copy en.json, translate, set "lang" and "match")
//    and `node scripts/gen-chat-phrases.mjs`. scripts/test_chat_i18n.ts holds it to the same rules as English.
// ★ WHOLE SENTENCES PER ID, never a label + key concatenated: word order and agreement differ. The "This
//   sounds" row draws a label and keys, but the line in the conversation is always the full `text`.
//
// usage: node scripts/gen-chat-phrases.mjs           write the generated files
//        node scripts/gen-chat-phrases.mjs --check   exit 1 if any generated file is stale (run-tests.sh)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const dir = path.join(root, 'i18n/chat');
const read = (f) => JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8'));

/** The watch draws only these chrome strings (DialChatView + CannedDial.speaker). */
export const SWIFT_CHROME = ['chatTitle', 'you', 'user', 'admin', 'listening', 'youDecoding', 'userDecoding', 'youTunedLast',
  'userTuning', 'watchOwnerTunes', 'watchNobody', 'bannerAwaiting', 'watchAskedToWait', 'bannerAskBefore', 'watchAskSub',
  'watchAskOpenSub', 'watchAskNoSub', 'watchEmpty'];

/** Every language, English first (the source and the fallback), the rest in file-name order. */
export function loadChat() {
  const meta = read('phrases.json').phrases;
  const files = fs.readdirSync(dir).filter((f) => f.endsWith('.json') && f !== 'phrases.json' && f !== 'en.json').sort();
  const langs = [read('en.json'), ...files.map(read)];
  return { meta, langs };
}

/** What is wrong with the files, as sentences — empty when they are sound. The generator refuses to write
 *  from broken files; the test prints the same list. */
export function problems({ meta, langs }) {
  const out = [];
  const ids = meta.map((p) => p.id);
  if (new Set(ids).size !== ids.length) out.push('phrases.json lists an id twice');
  const en = langs[0];
  const holes = (s) => [...String(s).matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort().join(',');
  const seenLang = new Set();
  for (const L of langs) {
    const who = `${L.lang ?? '?'}.json`;
    if (!L.lang || seenLang.has(L.lang)) out.push(`${who}: "lang" missing or repeated`);
    seenLang.add(L.lang);
    const have = Object.keys(L.phrases ?? {});
    for (const id of ids) if (!have.includes(id)) out.push(`${who}: no words for ${id}`);
    for (const id of have) if (!ids.includes(id)) out.push(`${who}: ${id} is not a phrase id (phrases.json)`);
    for (const p of meta) {
      const w = L.phrases?.[p.id]; if (!w) continue;
      const e = en.phrases[p.id];
      for (const [f, v] of Object.entries(w)) if (typeof v !== 'string' || !v.trim()) out.push(`${who}: ${p.id}.${f} is empty`);
      if (!w.text) out.push(`${who}: ${p.id} has no text`);
      if (!!p.group !== !!w.key) out.push(`${who}: ${p.id} ${p.group ? 'needs' : 'must not have'} a key (its group row)`);
      if (!!e.banner !== !!w.banner) out.push(`${who}: ${p.id} ${e.banner ? 'needs' : 'must not have'} a banner form`);
      for (const f of Object.keys(w)) if (!['text', 'key', 'banner', 'watch'].includes(f)) out.push(`${who}: ${p.id}.${f} is not a field`);
    }
    const groups = [...new Set(meta.map((p) => p.group).filter(Boolean))];
    for (const g of groups) if (!L.groups?.[g]?.trim()) out.push(`${who}: no label for the ${g} group`);
    const ck = Object.keys(en.chrome);
    for (const k of ck) {
      const v = L.chrome?.[k];
      if (typeof v !== 'string' || !v.trim()) { out.push(`${who}: chrome.${k} missing or empty`); continue; }
      if (holes(v) !== holes(en.chrome[k])) out.push(`${who}: chrome.${k} fills {${holes(v)}}, English fills {${holes(en.chrome[k])}}`);
    }
    for (const k of Object.keys(L.chrome ?? {})) if (!ck.includes(k)) out.push(`${who}: chrome.${k} is not in en.json`);
    if (!/^[A-Z]{2}$/.test(L.flag ?? '')) out.push(`${who}: "flag" must be the country code its flag shows (GB, BR …)`);
    for (const m of L.match ?? []) if (!/^[a-z]{2,3}$/.test(m)) out.push(`${who}: match "${m}" is not a language subtag`);
    for (const c of L.countries ?? []) {
      if (!/^[A-Z]{2}$/.test(c)) out.push(`${who}: country "${c}" is not an ISO 3166 alpha-2 code`);
      const other = langs.find((o) => o !== L && (o.countries ?? []).includes(c));
      if (other) out.push(`${who}: ${c} is also claimed by ${other.lang}.json — a country with two languages picks none`);
    }
  }
  return out;
}

const q = (s) => JSON.stringify(s);
const qs = (s) => '"' + String(s).replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"';   // a Swift string literal
const swiftName = (lang) => lang.replace(/-/g, '_');

/** "BR" → "pt-BR", …: a listener's COUNTRY (the server's GeoIP "cc"), for when the device's language is not one of ours.
 *  ★ Only countries with ONE main language are listed — BE, CH, CA … pick nothing and fall through to English. */
function countryLang(langs) {
  const m = {};
  for (const L of langs) for (const c of L.countries ?? []) m[c] = L.lang;
  return m;
}

/** "pt" → "pt-BR", "en" → "en", …: each file's own primary subtag, plus any it claims in "match". */
function langOf(langs) {
  const m = {};
  for (const L of langs) {
    m[L.lang.toLowerCase().split('-')[0]] ??= L.lang;
    for (const x of L.match ?? []) m[x] = L.lang;
  }
  return m;
}

export function renderTs({ meta, langs }) {
  const en = langs[0];
  const keys = Object.keys(en.chrome).filter((k) => !k.startsWith('watch'));
  const L = [];
  L.push('/* GENERATED by scripts/gen-chat-phrases.mjs from i18n/chat/*.json — DO NOT EDIT: edit the JSON and run it.');
  L.push(' * ★★★ The canned chat\'s words in every language the clients speak, read through src/services/dialChat.ts by the');
  L.push(' *     app AND the browser. The ids are the server\'s and never change with the language — only the words do. */');
  L.push('');
  L.push(`export type ChatLang = ${langs.map((x) => q(x.lang).replace(/"/g, "'")).join(' | ')};`);
  L.push(`export type ChatChromeKey =\n  ${keys.map((k) => `'${k}'`).join(' | ')};`);
  L.push("export type ChatPhraseMeta = { id: string; group?: string; band?: 'hf' | 'vhf'; wiki?: string;");
  L.push("                               decoder?: 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'time' };");
  L.push('export type ChatPhraseWords = { text: string; key?: string; banner?: string };');
  L.push('export type ChatWords = { name: string; groups: Record<string, string>; phrases: Record<string, ChatPhraseWords>;');
  L.push('                          chrome: Record<ChatChromeKey, string> };');
  L.push('');
  L.push('/** In the order a conversation runs: ask, act, answer, thank (i18n/chat/phrases.json). */');
  L.push('export const CHAT_PHRASE_META: readonly ChatPhraseMeta[] = [');
  for (const p of meta) L.push(`  ${JSON.stringify(p).replace(/"(\w+)":/g, '$1: ').replace(/,(?=\w+: )/g, ', ').replace(/"/g, "'")},`);
  L.push('];');
  L.push('');
  L.push('/** A language subtag ("pt", "de") → the words that speak it. Anything not here is English. */');
  L.push(`export const CHAT_LANG_OF: Readonly<Record<string, ChatLang>> = ${JSON.stringify(langOf(langs)).replace(/"(\w+)":/g, "$1: ").replace(/"/g, "'").replace(/,/g, ', ')};`);
  L.push('/** A listener\'s country (ISO alpha-2, the server\'s "cc") → its language, where it has only one main language. */');
  L.push(`export const CHAT_LANG_OF_COUNTRY: Readonly<Record<string, ChatLang>> = ${JSON.stringify(countryLang(langs)).replace(/"(\w+)":/g, "$1: ").replace(/"/g, "'").replace(/,/g, ', ')};`);
  L.push('');
  L.push('/** The country whose flag stands for each language in the chat\'s language menu (a badge where flags are not drawn). */');
  L.push(`export const CHAT_LANG_FLAG: Readonly<Record<ChatLang, string>> = {${langs.map((x) => `${q(x.lang).replace(/"/g, "'")}: '${x.flag}'`).join(', ')}};`);
  L.push('');
  L.push('export const CHAT_WORDS: Readonly<Record<ChatLang, ChatWords>> = {');
  for (const x of langs) {
    L.push(`  ${q(x.lang)}: {`);
    L.push(`    name: ${q(x.name)},`);
    L.push(`    groups: ${JSON.stringify(x.groups)},`);
    L.push('    phrases: {');
    for (const p of meta) {
      const w = x.phrases[p.id];
      const o = { text: w.text, ...(w.key ? { key: w.key } : {}), ...(w.banner ? { banner: w.banner } : {}) };
      L.push(`      ${p.id}: ${JSON.stringify(o)},`);
    }
    L.push('    },');
    L.push('    chrome: {');
    for (const k of keys) L.push(`      ${k}: ${q(x.chrome[k])},`);
    L.push('    },');
    L.push('  },');
  }
  L.push('};');
  return L.join('\n') + '\n';
}

export const SWIFT_BEGIN = '// ── BEGIN GENERATED by scripts/gen-chat-phrases.mjs from i18n/chat/*.json — DO NOT EDIT: edit the JSON and run it ──';
export const SWIFT_END = '// ── END GENERATED ──';

export function renderSwift({ meta, langs }) {
  const L = [SWIFT_BEGIN];
  L.push('/// ★★★ THE CANNED CHAT\'S WORDS, ONE TABLE PER LANGUAGE — the same files the phone and the browser are built from.');
  L.push('/// The ids never change with the language; only the words do. `CannedDial` picks the table once, from the watch\'s');
  L.push('/// own language list.');
  L.push('enum ChatWords {');
  L.push('  /// ★ The answers and their band ("" = either). Everything else is always offered.');
  L.push('  static let answerBand: [String: String] = [');
  L.push('    ' + meta.filter((p) => p.group === 'answer').map((p) => `${qs(p.id)}: ${qs(p.band ?? '')}`).join(', '));
  L.push('  ]');
  L.push('  /// A language subtag ("pt", "de") → its table. Anything not here is English.');
  L.push(`  static let langOf: [String: String] = [${Object.entries(langOf(langs)).map(([k, v]) => `${qs(k)}: ${qs(v)}`).join(', ')}]`);
  L.push('  /// A listener\'s country (the server\'s "cc") → its language, where it has only one main language.');
  L.push(`  static let countryLang: [String: String] = [${Object.entries(countryLang(langs)).map(([k, v]) => `${qs(k)}: ${qs(v)}`).join(', ')}]`);
  for (const x of langs) {
    const n = swiftName(x.lang);
    L.push(`  // ── ${x.name} ──`);
    L.push(`  static let ${n}: [(id: String, text: String)] = [`);
    for (const p of meta) { const w = x.phrases[p.id]; L.push(`    (${qs(p.id)}, ${qs(w.watch ?? w.text)}),`); }
    L.push('  ]');
    L.push(`  static let ${n}Chrome: [String: String] = [`);
    for (const k of SWIFT_CHROME) L.push(`    ${qs(k)}: ${qs(x.chrome[k])},`);
    L.push('  ]');
  }
  L.push(`  static let phrases: [String: [(id: String, text: String)]] = [${langs.map((x) => `${qs(x.lang)}: ${swiftName(x.lang)}`).join(', ')}]`);
  L.push(`  static let chrome: [String: [String: String]] = [${langs.map((x) => `${qs(x.lang)}: ${swiftName(x.lang)}Chrome`).join(', ')}]`);
  L.push('}');
  L.push(SWIFT_END);
  return L.join('\n');
}

export const TS_OUT = 'src/services/chatWords.generated.ts';
export const SWIFT_OUT = ['spike/WristSDR/WristSDR/Chat.swift', 'ios/VibeSDRWatch/Chat.swift'];

/** Every generated file as it should be: [path, wanted contents, current contents]. */
export function outputs(chat = loadChat()) {
  const res = [[TS_OUT, renderTs(chat)]];
  const block = renderSwift(chat);
  for (const f of SWIFT_OUT) {
    const cur = fs.readFileSync(path.join(root, f), 'utf8');
    const s = cur.indexOf(SWIFT_BEGIN), e = cur.indexOf(SWIFT_END);
    if (s < 0 || e < s) throw new Error(`${f}: the GENERATED markers are missing`);
    res.push([f, cur.slice(0, s) + block + cur.slice(e + SWIFT_END.length)]);
  }
  return res.map(([f, want]) => {
    const p = path.join(root, f);
    return [f, want, fs.existsSync(p) ? fs.readFileSync(p, 'utf8') : ''];
  });
}

if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  const chat = loadChat();
  const bad = problems(chat);
  if (bad.length) { console.error(bad.map((b) => '  ✗ ' + b).join('\n')); process.exit(1); }
  const check = process.argv.includes('--check');
  let stale = 0;
  for (const [f, want, cur] of outputs(chat)) {
    if (want === cur) continue;
    stale++;
    if (check) console.error(`  ✗ ${f} is stale — run node scripts/gen-chat-phrases.mjs`);
    else { fs.writeFileSync(path.join(root, f), want); console.log(`  wrote ${f}`); }
  }
  console.log(`chat phrases: ${chat.langs.length} languages × ${chat.meta.length} phrases${check ? (stale ? `, ${stale} stale` : ', up to date') : ''}`);
  process.exit(check && stale ? 1 : 0);
}
