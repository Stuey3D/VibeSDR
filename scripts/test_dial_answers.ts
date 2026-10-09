// ★★★ THE ANSWERS TO "Anyone know what this is?" (Stuart, 2026-10-09) — src/services/dialChat.ts, and the two watches'
// copies of the same rule. Pure: no server, no network. The five-places id check is test-chat-phrases.mjs.
//   1. the everyday pad carries NO answers; an open question adds only the half for the dial's band;
//   2. every link is a Signal Identification Wiki page; every decoder is one this app runs;
//   3. both watches mark the same answers with the same bands as the app.
import fs from 'node:fs';
import { DIAL_PHRASES, padPhrases, isQuestion, isAnswer, phraseWiki, phraseDecoder, answerBand, SIGID_HOME, bannerLine, isTuningPhrase, BannerGate } from '../src/services/dialChat.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const ids = (l: { id: string }[]) => l.map((p) => p.id);
const answers = DIAL_PHRASES.filter((p) => p.group === 'answer');

// ── 1. the pad ──
ok(!ids(padPhrases(false, 7_000_000)).some(isAnswer), 'no question open: the pad has no answers (HF)');
ok(!ids(padPhrases(false, 145_000_000)).some(isAnswer), 'no question open: the pad has no answers (VHF)');
const hf = ids(padPhrases(true, 6_206_000)), vhf = ids(padPhrases(true, 145_500_000));
ok(hf.includes('is_wefax') && hf.includes('not_sure') && !hf.includes('is_dmr'), 'open on HF: WEFAX and "Not sure", no DMR');
ok(vhf.includes('is_dmr') && vhf.includes('not_sure') && !vhf.includes('is_wefax'), 'open on VHF: DMR and "Not sure", no WEFAX');
ok(answerBand(29_999_999) === 'hf' && answerBand(30_000_000) === 'vhf', 'the line between the halves is 30 MHz');
const nHf = hf.filter(isAnswer).length, nVhf = vhf.filter(isAnswer).length;
ok(nHf <= 16 && nVhf <= 16, `each half stays a handful (${nHf} HF, ${nVhf} VHF)`);
ok(padPhrases(true, 7e6).length - padPhrases(false, 7e6).length === nHf, 'an open question adds the answers and nothing else');
ok(isQuestion('what_is_this') && !isQuestion('hello') && isAnswer('not_sure') && !isAnswer('what_is_this'), 'question / answer ids');

// ── 2. links and decoders ──
ok(SIGID_HOME === 'https://www.sigidwiki.com/wiki/Signal_Identification_Guide', 'the wiki home page');
const badWiki = answers.filter((p) => p.wiki && !phraseWiki(p.id)!.startsWith('https://www.sigidwiki.com/wiki/'));
ok(badWiki.length === 0, 'every answer link is a Signal Identification Wiki page');
ok(answers.filter((p) => p.id !== 'not_sure' && p.id !== 'is_time').every((p) => !!phraseWiki(p.id)), 'every named signal links (time signals have no page; "Not sure" needs none)');
const RUN = ['rtty', 'navtex', 'wefax', 'sstv', 'time'];
ok(answers.every((p) => !phraseDecoder(p.id) || RUN.includes(phraseDecoder(p.id)!)), 'every DECODE is a decoder this app runs');
ok(phraseDecoder('is_wefax') === 'wefax' && phraseDecoder('is_time') === 'time' && phraseDecoder('is_dmr') === null, 'WEFAX and time decode; DMR does not');
ok(answers.filter((p) => p.id !== 'not_sure').every((p) => p.band === 'hf' || p.band === 'vhf'), 'every named answer has a band');

// ── 3. the watches ──
for (const f of ['ios/VibeSDRWatch/Chat.swift', 'spike/WristSDR/WristSDR/Chat.swift']) {
  const t = fs.readFileSync(new URL('../' + f, import.meta.url), 'utf8');
  const m = t.match(/static let answerBand: \[String: String\] = \[\n\s*([^\n]*)/);
  const table = Object.fromEntries([...(m?.[1] ?? '').matchAll(/"([a-z0-9_]+)": "(hf|vhf|)"/g)].map((x) => [x[1], x[2]]));
  const want = Object.fromEntries(answers.map((p) => [p.id, p.band ?? '']));
  ok(JSON.stringify(table) === JSON.stringify(want), `${f.split('/')[0] === 'ios' ? 'Buddy' : 'Jr'} marks the same answers with the same bands`);
}

// ── 4. the shared-tuner banner (Stuart, 2026-10-09) ──
const longest = DIAL_PHRASES.filter((p) => isTuningPhrase(p.id)).map((p) => bannerLine(9, p.id)!).sort((a, b) => b.length - a.length)[0];
ok(longest.length <= 28, `every banner line fits — longest "${longest}" (${longest.length})`);
ok(bannerLine(2, 'ask_tune') === 'U2: Can I tune?', 'the banner names the asker: "U2: Can I tune?"');
ok(bannerLine(2, 'is_wefax') === null && bannerLine(2, 'hello') === null && bannerLine(2, 'sounds_awesome') === null,
   'answers, greetings and "This sounds" never take the banner');
{
  const g = new BannerGate();
  ok(g.allow(2, 'ask_tune', 0), 'a first tuning line takes the banner');
  ok(!g.allow(3, 'ask_tune', 5_000), 'somebody else 5 s later does not (10 s between any two)');
  ok(g.allow(3, 'ask_tune', 11_000), '…but does after 10 s');
  ok(!g.allow(2, 'yes_hold', 21_000), 'the same person within 30 s does not');
  ok(g.allow(2, 'yes_hold', 31_000), '…but does after 30 s');
  ok(!g.allow(2, 'yes_hold', 62_000), 'the same person repeating the same line within 2 min does not');
  ok(!g.allow(4, 'hello', 100_000), 'a non-tuning phrase never does');
}

console.log(`test_dial_answers: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
