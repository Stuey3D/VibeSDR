/**
 * Faceplate display text (src/constants/displayText.ts) — the 14-segment cell rules, accent folding
 * for the segment and dot-matrix ROMs, unit-safe upper case, flag → ISO, and the fallback for
 * strings that fold to nothing.
 *
 * Brief: docs/BRIEF-faceplates.md §7, §7.1.
 *
 * Run: node --no-warnings scripts/test_faceplate_text.ts   (run-tests.sh does)
 */
import {
  toSegCells, segCellCount, segGhost, segCellList, foldForSeg, foldForDot, foldToAscii, dotoHas,
  toUpperDisplay, flagToIso, foldIsUsable, displayOrFallback, setTransliterator, SEG_BLANK,
  toSegRun, vfdStripText, cellWindow, steppedOffset, statusGainParts, statusGainText, STATUS_DIRECT_SAMPLE, transliterateNonLatin,
  TRANSLIT_CACHE_MAX, vfdFreqCells, vfdFreqLayout,
} from '../src/constants/displayText.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);

// ── toSegCells: EVERY printable ASCII character, one cell each (§7) ──────────
// Expected cell for each character, spelled out rather than derived, so the table IS the spec.
const FULL = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789?@&$%\'"()*+,-/<=>^_\\|`';
for (let c = 0x20; c <= 0x7e; c++) {
  const ch = String.fromCharCode(c);
  let want: string;
  if (ch === ' ') want = '!';                       // DSEG's space is 200 wide: send the blank cell
  else if (ch === '!') want = '|';                  // DSEG's ! IS the blank cell
  else if (ch === '~') want = '-';                  // DSEG's ~ is all-segments-on (the ghost)
  else if (ch === ':') want = '-';                  // 200 wide: its own cell as a dash
  else if (ch === '#') want = 'H';
  else if (ch === ';') want = ',';
  else if (ch === '[') want = '(';
  else if (ch === ']') want = ')';
  else if (ch === '.') want = '!.';                 // alone: a blank cell carrying the point
  else if (ch >= 'a' && ch <= 'z') want = ch.toUpperCase();
  else if (FULL.includes(ch)) want = ch;
  else want = '!';                                  // { } — missing, not in the map: blank
  eq(`toSegCells(${JSON.stringify(ch)})`, toSegCells(ch), want);
  // ★ Exactly one cell per character, symbols included.
  eq(`one cell for ${JSON.stringify(ch)}`, segCellCount(toSegCells(ch)), 1);
  // ★ Never a character DSEG cannot draw at full width (space, :, #, ;, [, ], {, }).
  ok(`no narrow/missing glyph for ${JSON.stringify(ch)}`, !/[ :#;[\]{}]/.test(toSegCells(ch)));
}
// A whole printable-ASCII run keeps the grid: 95 characters, the one '.' rides on the cell before it.
{
  let all = '';
  for (let c = 0x20; c <= 0x7e; c++) all += String.fromCharCode(c);
  const cells = toSegCells(all);
  eq('all printable ASCII: 94 cells (the . is a DP)', segCellCount(cells), 94);
  eq('ghost matches the cell count', segGhost(segCellCount(cells)).length, 94);
}
eq('decimal point rides on the previous cell', toSegCells('96.6'), '96.6');
eq('…and counts no cell of its own', segCellCount('96.6'), 3);
eq('a second point gets a blank cell', toSegCells('A..'), 'A.!.');
eq('spaces become blank cells', toSegCells('BBC R4'), 'BBC!R4');
eq('real ! and ~', toSegCells('HI! ~'), 'HI|!-');
eq('colon is a dash cell', toSegCells('08:37'), '08-37');
eq('# ; [ ]', toSegCells('#1;[A]'), 'H1,(A)');
eq('unknown → blank, never tofu', toSegCells('a{b}€'), 'A!B!!');
eq('ghost is ~ × n', segGhost(4), '~~~~');
eq('segCellList keeps the point with its cell', segCellList('96.6!'), ['9', '6.', '6', '!']);
eq('typographic punctuation folds to its cell', toSegCells('Rock’n’Roll – live…'), "ROCK'N'ROLL!-!LIVE...".replace('LIVE...', 'LIVE.!.!.'));

// ── Accent folding (§7): NFD + the special cases NFD misses ──────────────────
const SEG_FOLD: [string, string][] = [
  ['Ö', 'O'], ['É', 'E'], ['Ç', 'C'], ['Ñ', 'N'], ['Å', 'A'],
  ['ß', 'SS'], ['ẞ', 'SS'], ['Æ', 'AE'], ['æ', 'AE'], ['Œ', 'OE'], ['œ', 'OE'],
  ['Ø', 'O'], ['ø', 'O'], ['Ł', 'L'], ['ł', 'L'], ['Đ', 'D'], ['đ', 'D'], ['Ð', 'D'],
  ['Þ', 'TH'], ['þ', 'TH'], ['ı', 'I'],
  ['Radio Česko', 'RADIO CESKO'], ['Österreich 1', 'OSTERREICH 1'], ['São Paulo', 'SAO PAULO'],
];
for (const [src, want] of SEG_FOLD) eq(`foldForSeg(${src})`, foldForSeg(src), want);
eq('seg cells from an accented name', toSegCells('Rádio Nacional'), 'RADIO!NACIONAL');
eq('foldToAscii keeps case', foldToAscii('Łódź'), 'Lodz');

// ── Dot matrix: keep what Doto can draw, fold the rest ───────────────────────
eq('Doto keeps Latin-1 accents', foldForDot('Rádio Nacional São Paulo'), 'Rádio Nacional São Paulo');
eq('Doto keeps Ł / Ő (Latin Extended-A)', foldForDot('Łódź Győr'), 'Łódź Győr');
eq('Doto keeps ß', foldForDot('Straße'), 'Straße');
eq('decomposed input is composed first', foldForDot('Café'), 'Café');
eq('Doto folds what it lacks (ǎ is not in Doto)', foldForDot('ǎ'), 'a');
eq('undrawable → space, never tofu', foldForDot('A☃B'), 'A B');
ok('Doto has é', dotoHas('é'));
ok('Doto lacks the arrow (so the status gain arrow is DRAWN)', !dotoHas('↓'));

// ── toUpperDisplay: units keep their case (§7 TRAP) ──────────────────────────
eq('units kept', toUpperDisplay('gain 25.4dB · if 2800 kHz'), 'GAIN 25.4dB · IF 2800 kHz');
eq('every unit', toUpperDisplay('dB dBm dBFS Hz kHz MHz k/s fps'), 'dB dBm dBFS Hz kHz MHz k/s fps');
eq('glued to a number', toUpperDisplay('6k/s 5fps -73dBm 14.230MHz'), '6k/s 5fps -73dBm 14.230MHz');
eq('dBFS not read as dB + FS', toUpperDisplay('-3dBFS'), '-3dBFS');
eq('a unit glued to letters is a word', toUpperDisplay('Hzone fpsx'), 'HZONE FPSX');
eq('plain words', toUpperDisplay('Radio Caroline'), 'RADIO CAROLINE');
eq('empty', toUpperDisplay(''), '');

// ── Flags → ISO (§7.1) ───────────────────────────────────────────────────────
eq('🇬🇧 → GB', flagToIso('🇬🇧'), 'GB');
eq('🇧🇷 → BR', flagToIso('🇧🇷'), 'BR');
eq('England subdivision → GB', flagToIso('🏴\u{E0067}\u{E0062}\u{E0065}\u{E006E}\u{E0067}\u{E007F}'), 'GB');
eq('no flag', flagToIso(undefined), '');
eq('not a flag', flagToIso('📱'), '');
eq('ISO code through the segment cells', toSegCells(flagToIso('🇩🇪')), 'DE');

// ── Non-Latin: nothing usable → frequency + Latin callsign ───────────────────
ok('Latin fold is usable', foldIsUsable('Österreich 1', foldForSeg('Österreich 1'), 'seg'));
ok('Cyrillic is NOT usable on seg (no ICU yet)', !foldIsUsable('Радио России', foldForSeg('Радио России'), 'seg'));
ok('Cyrillic is NOT usable on dot', !foldIsUsable('Радио России', foldForDot('Радио России'), 'dot'));
ok('"Радио 1" → "1" is not the name', !foldIsUsable('Радио 1', foldForSeg('Радио 1'), 'seg'));
eq('fallback: frequency', displayOrFallback('Радио России', 'seg', '7.310 MHz'), '7.310 MHz');
eq('fallback keeps a Latin callsign', displayOrFallback('BBC Русская служба', 'seg', '9.410 MHz'), '9.410 MHz BBC');
eq('usable text passes through (seg)', displayOrFallback('Radio Česko', 'seg', 'x'), 'RADIO CESKO');
eq('usable text passes through (dot)', displayOrFallback('Radio Česko', 'dot', 'x'), 'Radio Česko');
eq('CJK falls back', displayOrFallback('中国之声', 'dot', '9.500 MHz'), '9.500 MHz');
// ── The native ICU hook (brief §7): transliterate → fold → fallback ──────────
// A FAKE `Any-Latin; Latin-ASCII` so plain Node can run it; the real one is VibeLocalSDR.transliterate.
{
  const FAKE: Record<string, string> = {
    'Радио России': 'Radio Rossii',
    'Русская служба': 'Russkaya sluzhba',
    'Ελληνική Ραδιοφωνία': 'Ellenike Radiophonia',
    '中国之声': '',                                     // ICU "gave nothing back" → fallback
  };
  const sent: string[] = [];
  setTransliterator((s) => { sent.push(s); return s in FAKE ? FAKE[s] : s; });

  eq('Cyrillic → transliterated → seg cells', toSegCells('Радио России'), 'RADIO!ROSSII');
  eq('…the whole phrase went to ICU as ONE run', sent, ['Радио России']);
  eq('Cyrillic on seg is usable now', displayOrFallback('Радио России', 'seg', '7.310 MHz'), 'RADIO ROSSII');
  eq('Cyrillic on dot', displayOrFallback('Радио России', 'dot', '7.310 MHz'), 'Radio Rossii');
  eq('memoised: a repeat never crosses the bridge again', sent.length, 1);
  eq('Greek', toSegCells('Ελληνική Ραδιοφωνία'), 'ELLENIKE!RADIOPHONIA');
  eq('CJK with an empty answer → frequency', displayOrFallback('中国之声', 'seg', '9.500 MHz'), '9.500 MHz');
  eq('CJK with an empty answer on dot → frequency', displayOrFallback('中国之声', 'dot', '9.500 MHz'), '9.500 MHz');
  sent.length = 0;
  eq('mixed Latin + Cyrillic: only the Cyrillic run is sent', toSegCells('BBC Русская служба'), 'BBC!RUSSKAYA!SLUZHBA');
  eq('…the Latin callsign stayed in JS', sent, ['Русская служба']);
  // ★ Latin accents never go to ICU (Latin-ASCII would strip what Doto can draw).
  sent.length = 0;
  eq('dot keeps the Latin accent beside transliterated Cyrillic', foldForDot('Rádio Русская служба'), 'Rádio Russkaya sluzhba');
  sent.length = 0;
  eq('a Latin-only name never calls the transliterator', [foldForDot('Rádio Nacional'), sent.length], ['Rádio Nacional', 0]);
  eq('the VTS strip on seg uses the transliteration', vfdStripText('Радио России', undefined, 'seg', 'x'), 'Radio Rossii');
  // A transliterator that throws or returns junk is treated as "no answer" — never a crash.
  setTransliterator(() => { throw new Error('native gone'); });
  eq('a throwing transliterator → fallback, no throw', displayOrFallback('Радио России', 'seg', '7.310 MHz'), '7.310 MHz');
  setTransliterator(() => (undefined as unknown as string));
  eq('a non-string answer → fallback', displayOrFallback('Радио России', 'seg', '7.310 MHz'), '7.310 MHz');
  // Bounded memo: many distinct names never grow it past TRANSLIT_CACHE_MAX.
  let calls = 0;
  setTransliterator((s) => { calls++; return s.length ? 'x' : s; });
  for (let i = 0; i < TRANSLIT_CACHE_MAX + 50; i++) transliterateNonLatin(String.fromCharCode(0x4e00 + i));
  const before = calls;
  transliterateNonLatin(String.fromCharCode(0x4e00 + TRANSLIT_CACHE_MAX + 49)); // recent → cached
  transliterateNonLatin(String.fromCharCode(0x4e00));  // evicted → asked again
  eq('LRU keeps the recent (0 calls), evicts the oldest (1 call)', calls - before, 1);
  setTransliterator(null);
  eq('uninstalled: back to the frequency fallback', displayOrFallback('Радио России', 'seg', '7.310 MHz'), '7.310 MHz');
}

// ── The VTS strip on a VFD ───────────────────────────────────────────────────
{
  const r = toSegRun('BAND 14.000 MHz');
  // ★ Units go THROUGH the segments in capitals, like the rest of the text (Stuart, 2026-10-02:
  //   "it should just read like standard radio text does") — no separate unit overlay any more.
  eq('units read like the rest of the text: MHZ in the segments', r.cells.join(''), 'BAND!14.000!MHZ');
  eq('…and no separate unit overlay', r.units, []);
  eq('a unit glued to a number, in the segments', toSegRun('-1.2kHz').cells.join(''), '-1.2KHZ');
  eq('a unit-looking word is just letters', toSegRun('HZONE').units, []);
  eq('no units: plain cells', toSegRun('Radio 1').cells.join(''), 'RADIO!1');
  eq('seg strip keeps the unit\'s case for toSegRun', vfdStripText('Tuned 7.1 MHz', undefined, 'seg', 'x'), 'Tuned 7.1 MHz');
  eq('seg strip folds accents', vfdStripText('Rádio Nacional', undefined, 'seg', 'x'), 'Radio Nacional');
  eq('dot strip: UPPER, units kept, accents kept', vfdStripText('Rádio 5 kHz', undefined, 'dot', 'x'), 'RÁDIO 5 kHz');
  eq('secondary joins with a slash', vfdStripText('A', 'B', 'dot', 'x'), 'A  /  B');
  eq('non-Latin name → frequency', vfdStripText('Радио России', undefined, 'seg', '7310 kHz'), '7310 kHz');
  eq('cellWindow centres a short run in whole cells', cellWindow(['A', 'B'], 5, 0, '!'), ['!', 'A', 'B', '!', '!']);
  eq('cellWindow slides a long run', cellWindow(['A', 'B', 'C', 'D'], 2, 1, '!'), ['B', 'C']);
  eq('cellWindow clamps the offset', cellWindow(['A', 'B', 'C'], 2, 9, '!'), ['B', 'C']);
  // ★★ Stepped: whole cells, a 1.5 s pause at the start, ~300 ms per step, never a fraction.
  eq('pause at the start', [0, 1499].map(t => steppedOffset(t, 20, 14, false)), [0, 0]);
  eq('one whole cell per 300 ms', [1500, 1799, 1800, 2100].map(t => steppedOffset(t, 20, 14, false)), [1, 1, 2, 3]);
  eq('stops at the end', steppedOffset(99_999, 20, 14, false), 6);
  ok('always an integer', [0, 17, 1633, 4321, 7777].every(t => Number.isInteger(steppedOffset(t, 40, 14, true))));
  eq('loops back to the start after the end pause', steppedOffset(1500 + 6 * 300 + 1500 + 10, 20, 14, true), 0);
  eq('a run that fits never moves', steppedOffset(5000, 10, 14, true), 0);
}

// ── Status display: the gain arrow is drawn, the value written "25.4dB" (§8.1) ──
eq('gain down', statusGainParts('GAIN ↓ 25.4 dB'), { label: 'GAIN', dir: 'down', value: '25.4dB', tail: '' });
eq('gain up', statusGainParts('GAIN ↑ 3.0 dB'), { label: 'GAIN', dir: 'up', value: '3.0dB', tail: '' });
eq('gain held, no arrow', statusGainParts('GAIN · 12.5 dB (held)'), { label: 'GAIN', dir: null, value: '12.5dB', tail: ' (held)' });
eq('plain AGC is not a gain reading', statusGainParts('AGC'), null);
// ★★ Direct sampling: the gain item says so instead of a gain the bypassed tuner is not applying (B10).
eq('Q branch → Direct Sample', statusGainText('GAIN ↓ 25.4 dB', 2), 'Direct Sample');
eq('I branch → Direct Sample', statusGainText('AGC', 1), STATUS_DIRECT_SAMPLE);
eq('even with no gain text yet', statusGainText('', 2), 'Direct Sample');
eq('on the tuner → the gain as written', statusGainText('GAIN ↓ 25.4 dB', 0), 'GAIN ↓ 25.4 dB');
eq('unknown (−1 / absent) → the gain as written', [statusGainText('AGC', -1), statusGainText('AGC', undefined)], ['AGC', 'AGC']);
eq('"Direct Sample" is not parsed as a gain reading', statusGainParts(STATUS_DIRECT_SAMPLE), null);
ok('Doto really has no arrow to draw it with', !dotoHas('↓') && !dotoHas('↑'));

// ── The VCR frequency readout: Hz digits dark until in use, the core centred (2026-10-02) ──────────
eq('VCR MHz: 104.200000 shows 104.200, Hz cells dark, centred', vfdFreqCells('104.200000', 'MHz'), '   104.200   ');
eq('VCR MHz: a Hz digit in use lights, nothing moves', vfdFreqCells('104.200500', 'MHz'), '   104.2005  ');
eq('VCR MHz: every Hz digit in use', vfdFreqCells('104.200501', 'MHz'), '   104.200501');
eq('VCR MHz: same cell count whatever is lit', vfdFreqCells('104.200501', 'MHz').length, vfdFreqCells('104.200000', 'MHz').length);
eq('VCR kHz: whole kHz figure lit, point dark with the Hz', vfdFreqCells('1250.000', 'kHz'), '   1250   ');
eq('VCR kHz: 7074.5', vfdFreqCells('7074.500', 'kHz'), '   7074.5  ');
eq('VCR: thousands commas ignored', vfdFreqCells('1,296.000000', 'MHz'), '   1296.000   ');
eq('VCR: Hz unit untouched', vfdFreqCells('14230000', 'Hz'), '14230000');
eq('VCR: the FM screen (no Hz digits) untouched', vfdFreqCells('96.600', 'MHz'), '96.600');
// ★ The Hz cells (and the matching dark ones on the left) are drawn ¾ size — the counts SegDigits gets.
eq('VCR small cells: MHz Hz digits, 3 each end', [vfdFreqLayout('128.590000', 'MHz').smallLead, vfdFreqLayout('128.590000', 'MHz').smallTail], [3, 3]);
eq('VCR small cells: kHz Hz digits, 3 each end', [vfdFreqLayout('1250.000', 'kHz').smallLead, vfdFreqLayout('1250.000', 'kHz').smallTail], [3, 3]);
eq('VCR small cells: none where the text is untouched', [vfdFreqLayout('96.600', 'MHz').smallLead, vfdFreqLayout('96.600', 'MHz').smallTail], [0, 0]);

// ★★ Above 9.999 GHz (a Ku LNB / transverter in front of the radio): the VCR readout takes a fifth MHz cell —
//    QO-100 10 489.5 MHz, 24 GHz 24 048.1 MHz — with the same dark/¾ Hz cell rules either side.
eq('VCR: QO-100 10489.5 MHz', vfdFreqLayout('10489.500000', 'MHz'), { text: '   10489.500   ', smallLead: 3, smallTail: 3 });
eq('VCR: 24 GHz with Hz in use', vfdFreqLayout('24048.100250', 'MHz'), { text: '   24048.10025 ', smallLead: 3, smallTail: 3 });
eq('VCR: 10.489 GHz in kHz', vfdFreqCells('10489500.000', 'kHz'), '   10489500   ');

void SEG_BLANK;
console.log(`faceplate text: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
