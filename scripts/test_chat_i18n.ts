// ★★★ THE CANNED CHAT IN EVERY LANGUAGE (2026-10-22) — i18n/chat/*.json, scripts/gen-chat-phrases.mjs, src/services/dialChat.ts.
// Pure: no server, no network, silent. The ids end to end through a real server: scripts/test-chat-phrases.mjs.
//   1. the generated files are not stale, and every language has every id, no extras, no empty strings;
//   2. every language fits the widths English was fitted to — the banner line ("U2: " + it within 28), the ask banner
//      and the shared-tuner banner (no wider than "SHARED TUNER · ASK TO TUNE · 10/10 👤"), the watch's lines;
//   3. which language: picked > device > country > English, and the country map;
//   4. the menu: Default says what it resolves to, flags or badges, the pick remembered on the device;
//   5. the server tells each listener its OWN country, in its own dial, and nothing broadcast carries one.
import fs from 'node:fs';
import { spawnSync } from 'node:child_process';
import { loadChat, problems } from './gen-chat-phrases.mjs';
import {
  setChatLangInputs, chatLang, chatLangChoice, countryChatLang, deviceChatLang, chatLangPicked, chatLangDetected,
  DIAL_PHRASES, PHRASE_GROUP_LABEL, ASK_NUDGE_TEXT as _nudge, bannerLine, isTuningPhrase, askBannerText, sharedTunerText,
  chatT, phraseText, dialSummary, speakerName, joinedLine, flagsDrawn, chatLangFlag, chatLangLabel, chatLangDefaultLabel,
  chatLangKeyLabel, onChatLang, CHAT_LANG_NAMES,
} from '../src/services/dialChat.ts';
import * as dial from '../src/services/dialChat.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const read = (f: string) => fs.readFileSync(new URL(`../${f}`, import.meta.url), 'utf8');
void _nudge;

// ── 1. the files ──
console.log('── one source per language ──');
const chat = loadChat();
const langs = chat.langs.map((l: { lang: string }) => l.lang) as string[];
const bad = problems(chat);
ok(bad.length === 0, `every language has every one of the ${chat.meta.length} ids, no extras, no empty strings${bad.length ? ' — ' + bad.slice(0, 6).join('; ') : ''}`);
ok(langs[0] === 'en' && ['pt-BR', 'es', 'fr', 'de', 'it', 'nl', 'pl'].every((l) => langs.includes(l)), `${langs.length} languages: ${langs.join(' ')}`);
const gen = spawnSync(process.execPath, [new URL('./gen-chat-phrases.mjs', import.meta.url).pathname, '--check'], { encoding: 'utf8' });
ok(gen.status === 0, `the generated TS and both watches' tables are up to date${gen.status ? ' — ' + gen.stderr.trim() : ''}`);

// ── 2. the widths, in every language ──
console.log('── every language fits ──');
const WIDEST = 'SHARED TUNER · ASK TO TUNE · 10/10 👤'.length;        // what the narrowest layout was fitted to
const WATCH_MAX = 40, WATCH_SUB_MAX = 44;                              // the wrist's phrase keys; its two-line sub-captions
const report: string[] = [];
for (const l of langs) {
  setChatLangInputs({ picked: l });
  ok(chatLang() === l, `${l}: picked, in force`);
  // the pad
  ok(DIAL_PHRASES.length === chat.meta.length && DIAL_PHRASES.every((p, i) => p.id === chat.meta[i].id && !!p.text), `${l}: the pad has every phrase, in order`);
  ok(DIAL_PHRASES.filter((p) => p.group).every((p) => !!p.key) && !!PHRASE_GROUP_LABEL.sounds && !!PHRASE_GROUP_LABEL.answer, `${l}: group rows have their label and keys`);
  // the tuning banner
  const lines = DIAL_PHRASES.filter((p) => isTuningPhrase(p.id)).map((p) => bannerLine(9, p.id)!);
  const longest = lines.slice().sort((a, b) => b.length - a.length)[0];
  ok(lines.length === 14 && longest.length <= 28, `${l}: every tuning line fits the banner — longest "${longest}" (${longest.length}/28)`);
  // the ask banner and the shared-tuner banner
  const base = { askFirst: true, tuneLocked: true } as const;
  const ask = [askBannerText({ ...base }, false), askBannerText({ ...base, askState: 'open', askUntil: 1 }, false),
               askBannerText({ ...base, askState: 'no', askUntil: Date.now() + 300_000 }, false)].map((x) => x ?? '');
  const shared = [sharedTunerText(true, 1, 5), sharedTunerText(false, 10, 10), sharedTunerText(false, -1, 0)];
  const all = [...ask, ...shared, dial.ASK_NUDGE_TEXT];
  const wide = all.slice().sort((a, b) => b.length - a.length)[0];
  ok(all.every((x) => x.length > 0 && x.length <= WIDEST), `${l}: no banner wider than English's widest — longest "${wide}" (${wide.length}/${WIDEST})`);
  ok(chatT('bannerShort').length <= chatT('bannerShared').length, `${l}: the status row's short form ("${chatT('bannerShort')}") is no longer than the full one`);
  // every placeholder filled
  const d = { mode: 'open', tuner: 3, mine: false, you: 2, listeners: 4, decoding: false };
  const filled = [dialSummary(d), dialSummary({ ...d, ...base }), speakerName(3, 2), joinedLine(3, 2, 1_792_000_000), joinedLine(2, 2, 1_792_000_000),
                  chatT('othersMany', { n: 3 }), chatT('askButton', { ask: phraseText('ask_tune')! }), chatT('sharedLine', { station: '96.600 MHz WFM' })];
  ok(filled.every((x) => !/[{}]/.test(x)), `${l}: every {placeholder} filled — "${filled[0]}"`);
  // the watch
  const L = chat.langs.find((x: { lang: string }) => x.lang === l);
  const wlines = chat.meta.map((p: { id: string }) => L.phrases[p.id].watch ?? L.phrases[p.id].text) as string[];
  const wl = wlines.slice().sort((a, b) => b.length - a.length)[0];
  ok(wl.length <= WATCH_MAX, `${l}: every watch key fits — longest "${wl}" (${wl.length}/${WATCH_MAX})`);
  const subs = Object.entries(L.chrome).filter(([k]) => k.startsWith('watch')).map(([, v]) => v as string);
  const ws = subs.slice().sort((a, b) => b.length - a.length)[0];
  ok(ws.length <= WATCH_SUB_MAX, `${l}: every watch caption fits two lines — longest "${ws}" (${ws.length}/${WATCH_SUB_MAX})`);
  report.push(`${l}: banner ${longest.length}/28, banners ${wide.length}/${WIDEST}, watch ${wl.length}/${WATCH_MAX}`);
}
setChatLangInputs({ picked: null });
console.log('   .. ' + report.join(' · '));

// ── 3. which language ──
console.log('── which language: picked > device > country > English ──');
ok(chatLangChoice({ picked: 'de', device: ['pt-BR'], cc: 'ES' }) === 'de', 'a pick beats everything');
ok(chatLangChoice({ picked: null, device: ['pt-BR'], cc: 'ES' }) === 'pt-BR', 'then the device language');
ok(chatLangChoice({ device: ['en-GB'], cc: 'PT' }) === 'en', 'an English device in Portugal stays English (the device is the person\'s own setting)');
ok(chatLangChoice({ device: ['ja-JP'], cc: 'BR' }) === 'pt-BR', 'a device language we do not speak falls to the country');
ok(chatLangChoice({ device: ['ja-JP', 'fr-CA'], cc: 'BR' }) === 'fr', '…but a later device language we DO speak comes first');
ok(chatLangChoice({ device: ['ja-JP'], cc: 'BE' }) === 'en' && chatLangChoice({ device: ['ja'], cc: 'CH' }) === 'en'
   && chatLangChoice({ device: ['ja'], cc: 'CA' }) === 'en', 'a country with more than one main language picks nothing (BE, CH, CA): English');
ok(chatLangChoice({ device: ['ja'] }) === 'en' && chatLangChoice({}) === 'en', 'no country (old server, LAN, loopback): English');
ok(chatLangChoice({ picked: 'xx', device: ['es-MX'] }) === 'es', 'a remembered pick we no longer have is ignored');
ok(deviceChatLang(['pt-PT']) === 'pt-BR' && deviceChatLang(['pt_BR']) === 'pt-BR' && deviceChatLang(['de-AT']) === 'de'
   && deviceChatLang(['nl-BE']) === 'nl' && deviceChatLang(['zh-Hans']) === null, 'device tags: pt-PT → pt-BR, underscores, regions');
const MAP: Record<string, string> = { BR: 'pt-BR', PT: 'pt-BR', AO: 'pt-BR', MZ: 'pt-BR', ES: 'es', MX: 'es', AR: 'es', CO: 'es', CL: 'es', PE: 'es',
  VE: 'es', UY: 'es', EC: 'es', BO: 'es', PY: 'es', CR: 'es', PA: 'es', DO: 'es', GT: 'es', HN: 'es', SV: 'es', NI: 'es', CU: 'es', PR: 'es',
  FR: 'fr', LU: 'fr', MC: 'fr', DE: 'de', AT: 'de', LI: 'de', IT: 'it', SM: 'it', VA: 'it', NL: 'nl', SR: 'nl', PL: 'pl' };
ok(Object.entries(MAP).every(([c, l]) => countryChatLang(c) === l), `the country map: ${Object.keys(MAP).length} countries`);
ok(['GB', 'US', 'BE', 'CH', 'CA', 'IN', ''].every((c) => countryChatLang(c) === null) && countryChatLang('br') === 'pt-BR', 'everything else maps to nothing (English); lower case is read');

// ── 4. the menu ──
console.log('── the language menu ──');
setChatLangInputs({ picked: null, device: ['ja-JP'], cc: null });
let told = 0; const off = onChatLang(() => { told++; });
setChatLangInputs({ cc: 'BR' });
ok(chatLang() === 'pt-BR' && told === 1 && chatLangPicked() === 'auto', 'the country arriving switches Default, and says so');
ok(chatLangDefaultLabel(true) === 'Padrão · 🇧🇷 Português (Brasil)', `Default says what it resolves to, in the language showing: "${chatLangDefaultLabel(true)}"`);
setChatLangInputs({ picked: 'en' });
ok(chatLangKeyLabel(true) === '🇬🇧 English' && chatLangDefaultLabel(true) === 'Default · 🇧🇷 Português (Brasil)', 'picked English: the key says English, Default still says what it would be');
setChatLangInputs({ cc: 'DE' });
ok(told === 3 && chatLangDetected() === 'de', 'a new country with a language picked still refreshes the Default entry');
off();
ok(chatLangLabel('pl', false) === 'PL Polski' && chatLangFlag('nl', false).emoji === null && chatLangFlag('nl', true).emoji === '🇳🇱',
   'without flags, the country code (drawn as a badge); with them, the emoji');
ok(Object.keys(CHAT_LANG_NAMES).every((l) => chatLangFlag(l as 'en', true).emoji!.length === 4), 'every language has a two-letter flag');
ok(!flagsDrawn('Windows') && !flagsDrawn('Win32') && !flagsDrawn('Mozilla/5.0 (Windows NT 10.0; Win64; x64)')
   && flagsDrawn('macOS') && flagsDrawn('MacIntel') && flagsDrawn('Linux x86_64') && flagsDrawn('Android') && flagsDrawn('iPhone') && flagsDrawn(''),
   'flags everywhere but Windows (its emoji font draws them as two letters)');
setChatLangInputs({ picked: null, device: null, cc: null });
{
  const web = read('web/client/src/chat.ts'), app = read('src/services/chatLangNative.ts'), drawer = read('src/components/ChatDrawer.tsx');
  ok(/function storedLang\(\)[^\n]*try \{ return localStorage\.getItem\(LANG_KEY\)/.test(web) && /function storeLang[\s\S]{0,200}try \{/.test(web),
     'web: the pick is kept in localStorage, every read and write inside try/catch');
  ok(/AsyncStorage\.getItem\(CHAT_LANG_KEY\)/.test(app) && /AsyncStorage\.setItem\(CHAT_LANG_KEY, v\)/.test(app) && /removeItem\(CHAT_LANG_KEY\)/.test(app),
     'app: the pick is kept in AsyncStorage; Default forgets it');
  const menu = web.slice(web.indexOf('function langMenu'), web.indexOf('let langUnsub'));
  ok(/createElement\('button'\)[\s\S]{0,200}chatLangKey/.test(menu) && !/createElement\('select'\)/.test(menu),
     'web: the menu is buttons (what the pad walks), not a <select>');
  ok(/shareKey\('__lang'/.test(drawer) && /shareKey\(`__lang_\$\{v\}`/.test(drawer), 'app: the menu is the drawer\'s own keys');
  ok(/Settings\.get\('AppleLanguages'\)/.test(app) && /localeIdentifier/.test(app) && !/from 'expo-localization'/.test(app), 'app: the device list with no new native module');
}

// ── 5. the server ──
console.log('── the listener\'s country goes to that listener only ──');
{
  const shim = read('android/app/src/main/cpp/local_sdr_shim.cpp');
  const fn = shim.slice(shim.indexOf('std::string dialJsonLocked('), shim.indexOf('// ── ★★★ ASK BEFORE TUNING (vibe_tune_etiquette.h'));
  ok(/sockCc\.find\(const_cast<net::Socket\*>\(sk\)\)/.test(fn) && /\\"cc\\":\\""/.test(fn), 'the dial message adds cc from the socket it is going to');
  const calls = [...shim.matchAll(/dialJsonLocked\(([a-z]+)/g)].length - 1, own = [...shim.matchAll(/dialJsonLocked\(sess, sock\.get\(\)\)/g)].length;
  ok(calls === 3 && own === 3, `every dial sent names its own socket (${own} of ${calls} senders)`);
  ok([...shim.matchAll(/\\"cc\\"/g)].length === 6, 'cc appears nowhere new but there (the admin connection lists already had theirs)');
  ok(/const std::string cc = vsCountry\(sock->peerAddress\(\)\);\n\s*\{ std::lock_guard<std::mutex> lk\(clientMtx\);[^\n]*sockCc\[sock\.get\(\)\] = cc;/.test(shim),
     'looked up once at the handshake, outside the lock');
}

console.log(`test_chat_i18n: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
