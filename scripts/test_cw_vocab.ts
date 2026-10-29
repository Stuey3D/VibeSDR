/**
 * test_cw_vocab.ts — V12 (2026-10-29) the CW box's PLAIN key, the CW TERMS GUIDE's search, the speed range and the QRZ
 * links: src/utils/cwVocab.ts (plainEnglish, searchCwTerms, matchCwLookalikes) and src/utils/cwFormat.ts (cwWpmText,
 * qrzCallUrl) — ONE source, read by the app (CwView, CwTermsGuide) and the web page (main.ts, cwGuide.ts, qrzLink.ts).
 * And that both clients use them, not copies.
 *
 * Run: node --no-warnings scripts/test_cw_vocab.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import {
  CW_LOOKALIKES, CW_TERMS, matchCwLookalikes, plainEnglish, searchCwTerms, CW_GUIDE_RST_LINE,
} from '../src/utils/cwVocab.ts';
import { cwHasWpmRange, cwWpmRangeOf, cwWpmText, qrzBaseCall, qrzCallUrl, QRZ_HOME } from '../src/utils/cwFormat.ts';
import { kbRateText, kbSpotRateText, parseCwSignals, parseCwSpot } from '../src/services/cwList.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const ok = (what: string, c: boolean) => { if (c) passes++; else { fails++; console.error(`FAIL ${what}`); } };

// ── 1. plainEnglish: codes in plain words; callsigns, names, numbers and other languages as sent ──
eq('TNX FER CALL', plainEnglish('TNX FER CALL'), 'thanks for the call');
eq('UR RST 589', plainEnglish('UR RST 589'), 'your signal report is 589');
eq('QTH', plainEnglish('QTH'), 'my location is');
eq('QTH NR TONBRIDGE', plainEnglish('QTH NR TONBRIDGE'), 'my location is near TONBRIDGE');
eq('HW?', plainEnglish('HW?'), 'how do you copy?');
eq('73', plainEnglish('73'), 'best regards');
eq('<SK>', plainEnglish('73 <SK>'), 'best regards (end of contact)');
eq('+ is AR', plainEnglish('TNX FER QSO + M0PKZ DE G4TPJ'), 'thanks for the contact (end of message) M0PKZ this is G4TPJ');
eq('+ glued to a word', plainEnglish('QSO+73'), 'contact (end of message) best regards');
eq('( is KN', plainEnglish('M0PKZ DE G4TPJ ('), 'M0PKZ this is G4TPJ (over, only the station I am working)');
eq('( glued to a callsign', plainEnglish('DE G4TPJ('), 'this is G4TPJ (over, only the station I am working)');
eq('KN as letters', plainEnglish('HW? M0PKZ DE G4TPJ KN'), 'how do you copy? M0PKZ this is G4TPJ over (only the station I am working)');
eq('BK', plainEnglish('UR 599 BK'), 'your 599 back to you');
eq('= is BT', plainEnglish('NAME JOHN = QTH LONDON'), 'my name is JOHN — my location is LONDON');
eq('a callsign unchanged', plainEnglish('G4ABC'), 'G4ABC');
eq('a name unchanged', plainEnglish('NAME JOHN'), 'my name is JOHN');
eq('a number unchanged', plainEnglish('599'), '599');
eq('a frequency unchanged', plainEnglish('QSY 7030'), 'change frequency 7030');
eq('DE between two callsigns', plainEnglish('M0PKZ DE G4TPJ'), 'M0PKZ this is G4TPJ');
eq('a German callsign is not DE', plainEnglish('DL1XYZ DE EA3QP'), 'DL1XYZ this is EA3QP');
eq('CQ said once, a callsign sent twice kept twice', plainEnglish('CQ CQ CQ DE G4ABC G4ABC K'), 'calling anyone this is G4ABC G4ABC over');
eq('an Italian line keeps its words; only 73 expands',
   plainEnglish('TUTTA LESTATE SI EST STARATO UN PO CON LA ANTENNA FILARE 73'),
   'TUTTA LESTATE SI EST STARATO UN PO CON LA ANTENNA FILARE best regards');
eq('garbled text passes through', plainEnglish('EEIT TTE QX7Z'), 'EEIT TTE QX7Z');
eq('lines kept', plainEnglish('CQ DE G4ABC\nTNX FER CALL'), 'calling anyone this is G4ABC\nthanks for the call');
eq('empty', plainEnglish(''), '');
eq('QRZ? is a question', plainEnglish('QRZ?'), 'who is calling me?');
eq('NR before a figure is "number"', plainEnglish('NR 5'), 'number 5');
eq('HER is English, not HR', plainEnglish('HER'), 'HER');

// ── 2. the guide's search ──
const all = searchCwTerms('');
ok('no search lists every term with a meaning', all.length === CW_TERMS.filter((t) => t.meaning).length && all.length > 50);
eq('QTH first for "qth"', searchCwTerms('qth')[0]?.code, 'QTH');
ok('"report" finds RST', searchCwTerms('report').some((t) => t.code === 'RST'));
ok('an alias finds its term (TKS → TNX)', searchCwTerms('TKS').some((t) => t.code === 'TNX'));
ok('the plain words find a term ("best regards" → 73)', searchCwTerms('best regards').some((t) => t.code === '73'));
eq('nothing for nonsense', searchCwTerms('zzqqxx').length, 0);
eq('look-alikes: all with no search', matchCwLookalikes('').length, CW_LOOKALIKES.length);
ok('look-alikes: "German" finds DE', matchCwLookalikes('german').some((l) => /DE is not/.test(l.title)));
ok('look-alikes: "zero" finds 0 and O', matchCwLookalikes('zero').some((l) => /0 \(zero\)/.test(l.title)));
ok('the RST line says readability, strength and tone', /readability/.test(CW_GUIDE_RST_LINE) && /strength/.test(CW_GUIDE_RST_LINE) && /tone/.test(CW_GUIDE_RST_LINE));

// ── 3. the speed as a range ──
eq('a range', cwWpmText({ wpm: 19, wpmLo: 17, wpmHi: 21 }), '17–21 wpm');
eq('equal = one figure', cwWpmText({ wpm: 21, wpmLo: 21, wpmHi: 21 }), '21 wpm');
eq('absent (an older server) = the wpm', cwWpmText({ wpm: 20.6 }), '21 wpm');
eq('reversed is put right', cwWpmText({ wpm: 19, wpmLo: 24, wpmHi: 16 }), '16–24 wpm');
eq('zero range = the wpm', cwWpmText({ wpm: 18, wpmLo: 0, wpmHi: 0 }), '18 wpm');
ok('cwHasWpmRange', cwHasWpmRange({ wpmLo: 17, wpmHi: 21 }) && !cwHasWpmRange({ wpmLo: 21, wpmHi: 21 }) && !cwHasWpmRange({}));
eq('the wire: kept when both are numbers', cwWpmRangeOf({ wpmLo: 17, wpmHi: 21.4 }), { wpmLo: 17, wpmHi: 21 });
eq('the wire: dropped when absent or junk', [cwWpmRangeOf({}), cwWpmRangeOf({ wpmLo: '17', wpmHi: 21 }), cwWpmRangeOf(null)], [{}, {}, {}]);
{
  const l = parseCwSignals({ signals: [{ id: 1, wpm: 19, wpmLo: 17, wpmHi: 21 }, { id: 2, wpm: 20 }] });
  eq('app rows: the range parsed', [l?.signals[0].wpmLo, l?.signals[0].wpmHi, 'wpmLo' in (l?.signals[1] ?? {})], [17, 21, false]);
  eq('app rows: drawn as a range / a figure', l?.signals.map(kbRateText), ['17–21 wpm', '20 wpm']);
  const sp = parseCwSpot({ callsign: 'G4ABC', wpm: 19, wpmLo: 18, wpmHi: 22 }, 0);
  eq('app spots: the range', sp && kbSpotRateText(sp), '18–22 wpm');
  const psk = parseCwSignals({ signals: [{ id: 1, wpm: 0, wpmLo: 1, wpmHi: 2, baud: 31 }] }, true);
  eq('PSK rows carry no range', [psk?.signals[0].wpmLo, kbRateText(psk!.signals[0])], [undefined, 'PSK31']);
}

// ── 4. QRZ ──
eq('a callsign page', qrzCallUrl('G4ABC'), 'https://www.qrz.com/db/G4ABC');
eq('lower case', qrzCallUrl('m0pkz'), 'https://www.qrz.com/db/M0PKZ');
eq('a portable call: the longest part', qrzCallUrl('EA8/G4ABC/P'), 'https://www.qrz.com/db/G4ABC');
eq('/MM', qrzCallUrl('G4ABC/MM'), 'https://www.qrz.com/db/G4ABC');
eq('a prefix call', qrzBaseCall('DL/PA3ABC'), 'PA3ABC');
eq('an FT8 hashed call', qrzCallUrl('<K1ABC>'), 'https://www.qrz.com/db/K1ABC');
eq('not a callsign: no link', [qrzCallUrl('—'), qrzCallUrl(''), qrzCallUrl('CQ'), qrzCallUrl('<...>'), qrzCallUrl('G4 ABC'), qrzCallUrl('a"b')],
   [null, null, null, null, null, null]);
eq('the home page', QRZ_HOME, 'https://www.qrz.com');

// ── 5. ONE SOURCE: both clients call these, never a copy ──
{
  const rd = (p: string) => readFileSync(new URL(`../${p}`, import.meta.url), 'utf8');
  const main = rd('web/client/src/main.ts'), cwv = rd('src/components/CwView.tsx'), qrz = rd('web/client/src/qrzLink.ts');
  ok('web renderCw draws the range with cwWpmText', /cwWpmText\(sg\)/.test(main) && /cwWpmText\(s\)/.test(main));
  ok('web PLAIN reads plainEnglish', /plainEnglish\(t\)/.test(main) && /plainEnglish\(raw\)/.test(main));
  ok('web links use qrzCallUrl', /qrzCallUrl\(call\)/.test(qrz));
  ok('web digital spots link the callsign', /callHtml\(sp\.callsign, escapeHtml\)/.test(main));
  ok('app CwView reads plainEnglish and links with qrzCallUrl', /plainEnglish\(t\)/.test(cwv) && /qrzCallUrl\(call\)/.test(cwv));
  ok('web guide reads searchCwTerms', /searchCwTerms\(q\)/.test(rd('web/client/src/cwGuide.ts')));
  ok('app guide reads searchCwTerms', /searchCwTerms\(q\)/.test(rd('src/components/CwTermsGuide.tsx')));
  // ★ the web page's PLAIN key and the plain-text pane exist
  const html = rd('web/client/index.html');
  ok('web: #cwPlain beside #decClr', /id="decClr"[^\n]*>CLR<\/button>\s*\n(?:\s*<!--[^\n]*-->\s*\n)?\s*<button class="btn" id="cwPlain"/.test(html));
  ok('web: #cwPlainText follows #decText', /<pre id="decText"><\/pre>\s*\n\s*<pre id="cwPlainText"/.test(html));
}

console.log(`${fails ? '✗' : '✓'} test_cw_vocab: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
