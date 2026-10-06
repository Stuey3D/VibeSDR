/**
 * The aerial's ranges and filters (src/utils/antennaBands.ts, 2026-10-06) — the parser both clients
 * use, the notice it produces, the once-per-entry rule, and the setup page's OWN copy of the
 * parser/writer (vibe_setup_page.h cannot import), which must read and write the same text.
 *
 * Run: npx tsx scripts/test_antenna_bands.ts
 */
import fs from 'node:fs';
import {
  parseAntennaRanges, parseAntennaFilters, parseAntennaBands, formatAntennaRanges, formatAntennaFilters,
  antennaNoticeAt, antennaNoticeTrack, hasAntennaBands, antennaBandsSummary, antennaBandsCard, cleanBandName,
  type AntennaNotice,
} from '../src/utils/antennaBands';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const MHz = 1e6, kHz = 1e3;

// ── Parsing ───────────────────────────────────────────────────────────────────────────────────
eq('empty → nothing', parseAntennaBands('', undefined), { ranges: [], filters: [] });
eq('empty is not "has bands"', hasAntennaBands(parseAntennaBands('', '')), false);
eq('non-string → nothing', parseAntennaRanges(42), []);

eq('wideband loop', parseAntennaRanges('0-300MHz Wideband loop'),
   [{ loHz: 0, hiHz: 300 * MHz, unit: 'MHz', name: 'Wideband loop', port: '' }]);
eq('dual-band vertical', parseAntennaRanges('144-146MHz 2 m; 430-440MHz 70 cm').map((r) => [r.loHz, r.hiHz, r.name]),
   [[144 * MHz, 146 * MHz, '2 m'], [430 * MHz, 440 * MHz, '70 cm']]);
eq('a unit on each end', parseAntennaRanges('500kHz-30MHz HF').map((r) => [r.loHz, r.hiHz, r.unit]),
   [[500 * kHz, 30 * MHz, 'MHz']]);
eq('space before the unit, en dash', parseAntennaRanges('144–148 MHz 2 m (US)').map((r) => [r.loHz, r.hiHz, r.name]),
   [[144 * MHz, 148 * MHz, '2 m (US)']]);
eq('kHz', parseAntennaRanges('153-279kHz LW')[0].hiHz, 279 * kHz);
eq('★ no unit anywhere is dropped, never guessed', parseAntennaRanges('144-146 2 m'), []);
eq('★ backwards range is dropped', parseAntennaRanges('146-144MHz'), []);
eq('a port prefix', parseAntennaRanges('[B] 430-440MHz 70 cm')[0].port, 'B');
eq('garbage between good entries is skipped', parseAntennaRanges('1-2MHz a; nonsense; 3-4MHz b').map((r) => r.name), ['a', 'b']);

const f = parseAntennaFilters('bandstop 87.5-108MHz FM band-stop; highpass 1.7MHz; lowpass 30MHz HF; band-pass 118-137MHz Airband');
eq('four filters', f.map((x) => x.kind), ['bandstop', 'highpass', 'lowpass', 'bandpass']);
eq('band-stop band', [f[0].loHz, f[0].hiHz, f[0].name], [87.5 * MHz, 108 * MHz, 'FM band-stop']);
eq('high-pass corner', [f[1].loHz, f[1].hiHz], [1.7 * MHz, null /* Infinity → null in JSON */]);
eq('low-pass corner', [f[2].loHz, f[2].hiHz], [0, 30 * MHz]);
eq('★ unknown kind dropped', parseAntennaFilters('notch 100MHz'), []);
eq('★ band-stop needs a band', parseAntennaFilters('bandstop 100MHz'), []);

// ── Writing: the canonical text round-trips ───────────────────────────────────────────────────
const rtR = '0-300MHz Wideband loop; [B] 144-146MHz 2 m; 153-279kHz';
eq('ranges round-trip', formatAntennaRanges(parseAntennaRanges(rtR)), rtR);
const rtF = 'bandstop 87.5-108MHz FM band-stop; highpass 1.7MHz; [A] lowpass 30MHz HF';
eq('filters round-trip', formatAntennaFilters(parseAntennaFilters(rtF)), rtF);
eq('★ a name cannot break the format', cleanBandName('a;b [c] "d"\\e'), 'a b c d e');
eq('★ names are capped', cleanBandName('x'.repeat(100)).length, 40);

// ── The notice ────────────────────────────────────────────────────────────────────────────────
const pi2 = parseAntennaBands('0-1700MHz', 'bandstop 87.5-108MHz FM band-stop');
eq('★★★ inside the FM band-stop', antennaNoticeAt(98.8 * MHz, pi2)?.text,
   'FM band-stop filter fitted — reception here is deliberately reduced');
eq('the quiet form', antennaNoticeAt(98.8 * MHz, pi2)?.short, 'FM band-stop filter fitted');
eq('band edges are inside', [antennaNoticeAt(87.5 * MHz, pi2)?.kind, antennaNoticeAt(108 * MHz, pi2)?.kind], ['filter', 'filter']);
eq('just outside the band-stop: nothing', antennaNoticeAt(108.1 * MHz, pi2), null);
eq('no name: the kind, in plain words', antennaNoticeAt(98 * MHz, parseAntennaBands('', 'bandstop 87.5-108MHz'))?.short,
   'Band-stop filter fitted');
eq('a name that says "filter" is not doubled',
   antennaNoticeAt(98 * MHz, parseAntennaBands('', 'bandstop 87.5-108MHz FM trap filter'))?.short, 'FM trap filter fitted');

const hp = parseAntennaBands('', 'highpass 1.7MHz MW high-pass');
eq('★ below a high-pass corner = filtered', antennaNoticeAt(648 * kHz, hp)?.kind, 'filter');
eq('above it = fine', antennaNoticeAt(7.1 * MHz, hp), null);
const lp = parseAntennaBands('', 'lowpass 30MHz');
eq('above a low-pass corner = filtered', antennaNoticeAt(145 * MHz, lp)?.short, 'Low-pass filter fitted');
const bp = parseAntennaBands('', 'bandpass 118-137MHz Airband');
eq('outside a band-pass = filtered', antennaNoticeAt(98 * MHz, bp)?.short, 'Airband filter fitted');
eq('inside it = fine', antennaNoticeAt(125 * MHz, bp), null);

const dual = parseAntennaBands('144-146MHz 2 m; 430-440MHz 70 cm', '');
eq('★★ outside every range', antennaNoticeAt(98 * MHz, dual)?.text, 'Outside this antenna’s range — reception may be poor');
eq('inside one of them: nothing', [antennaNoticeAt(145 * MHz, dual), antennaNoticeAt(433 * MHz, dual)], [null, null]);
eq('★ a filter outranks the range', antennaNoticeAt(98 * MHz, parseAntennaBands('144-146MHz', 'bandstop 87.5-108MHz'))?.kind, 'filter');
eq('no ranges given = never "outside"', antennaNoticeAt(2 * 1e9, parseAntennaBands('', 'bandstop 87.5-108MHz')), null);

// Ports — the RSP's sockets.
const rsp = parseAntennaBands('[A] 0-30MHz HF wire; [B] 50-500MHz Discone', '[B] bandstop 87.5-108MHz FM');
eq('on A, 7 MHz is in range', antennaNoticeAt(7 * MHz, rsp, 'A'), null);
eq('on A, 145 MHz is outside A’s aerial', antennaNoticeAt(145 * MHz, rsp, 'A')?.kind, 'range');
eq('on B, 145 MHz is fine', antennaNoticeAt(145 * MHz, rsp, 'B'), null);
eq('★ B’s FM filter does not follow you to A', antennaNoticeAt(98 * MHz, rsp, 'A')?.kind, 'range');
eq('on B, FM is filtered', antennaNoticeAt(98 * MHz, rsp, 'B')?.kind, 'filter');
eq('"Antenna B" matches [B]', antennaNoticeAt(98 * MHz, rsp, 'Antenna B')?.kind, 'filter');
eq('★★ socket unknown: per-socket entries say nothing rather than guess', antennaNoticeAt(98 * MHz, rsp, null), null);

// ── Once per ENTRY ────────────────────────────────────────────────────────────────────────────
{
  let key: string | null = null;
  const shown: number[] = [];
  const tune = (hz: number) => {
    const n: AntennaNotice | null = antennaNoticeAt(hz, pi2);
    const t = antennaNoticeTrack(key, n);
    key = t.key;
    if (t.entered) shown.push(hz);
    return n;
  };
  tune(80 * MHz); tune(88 * MHz); tune(90 * MHz); tune(98.8 * MHz); tune(107.9 * MHz);
  eq('★★★ flicking about inside the band shows the sentence ONCE', shown, [88 * MHz]);
  eq('…and the quiet form stays while you remain', tune(100 * MHz)?.short, 'FM band-stop filter fitted');
  tune(118 * MHz);
  eq('leaving clears it', key, null);
  tune(95 * MHz);
  eq('coming back is a new entry', shown, [88 * MHz, 95 * MHz]);
}

eq('summary for a card', antennaBandsSummary(parseAntennaBands('144-146MHz 2 m', 'bandstop 87.5-108MHz FM band-stop')),
   '2 m 144–146 MHz · FM band-stop filter');

// ── The setup page's copy must read and write the same text ───────────────────────────────────
{
  const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
  const m = /\/\/ ANTBANDS-BEGIN([\s\S]*?)\/\/ ANTBANDS-END/.exec(src);
  if (!m) { fails++; console.error('FAIL the setup page carries no ANTBANDS block'); }
  else {
    const page = new Function(`${m[1]}; return { antParseRanges, antParseFilters, antFormatRanges, antFormatFilters };`)();
    const samples = [rtR, '500kHz-30MHz HF; 144–148 MHz 2 m (US); junk; 146-144MHz', '[Tuner 1 50Ω] 0-30MHz'];
    for (const s of samples) {
      eq(`page ranges agree: ${s}`, page.antFormatRanges(page.antParseRanges(s)), formatAntennaRanges(parseAntennaRanges(s)));
    }
    const fsamples = [rtF, 'band-pass 118-137MHz Airband; notch 1MHz; bandstop 100MHz; highpass 1.7 MHz MW'];
    for (const s of fsamples) {
      eq(`page filters agree: ${s}`, page.antFormatFilters(page.antParseFilters(s)), formatAntennaFilters(parseAntennaFilters(s)));
    }
  }
}

// ── The directory card (2026-10-06): the two lines, and the directory page's own copy ─────────
const card = (r: string, f: string) => antennaBandsCard(parseAntennaBands(r, f));
eq('card: Stuart’s example', card('0-300MHz Wideband loop; [B] 144-146MHz 2 m', 'bandstop 87.5-108MHz FM band-stop; highpass 1.7MHz'),
   { covers: 'Covers 0–300 MHz (Wideband loop) · 144–146 MHz (2 m, Ant B)',
     filters: 'Filters fitted: FM band-stop · High-pass 1.7 MHz' });
eq('card: one named filter', card('', 'bandstop 87.5-108MHz FM band-stop').filters, 'FM band-stop filter fitted');
eq('card: a name saying "filter" is not doubled', card('', 'bandstop 87.5-108MHz FM trap filter').filters, 'FM trap filter fitted');
eq('card: an unnamed filter says its frequency', card('', 'lowpass 30MHz').filters, 'Low-pass 30 MHz filter fitted');
eq('card: a socket on a filter', card('', '[Antenna A] bandpass 118-137MHz').filters, 'Band-pass 118–137 MHz filter (Antenna A) fitted');
eq('card: nothing set = nothing shown', card('', ''), { covers: '', filters: '' });
eq('card: nothing parses = nothing shown', card('144-146 2 m', 'notch 1MHz'), { covers: '', filters: '' });
eq('card: more than four', card('1-2MHz; 3-4MHz; 5-6MHz; 7-8MHz; 9-10MHz; 11-12MHz', '').covers,
   'Covers 1–2 MHz · 3–4 MHz · 5–6 MHz · 7–8 MHz · +2');
{
  const src = fs.readFileSync(new URL('../directory/public/index.html', import.meta.url), 'utf8');
  const m = /\/\/ ANTBANDS-CARD-BEGIN([\s\S]*?)\/\/ ANTBANDS-CARD-END/.exec(src);
  if (!m) { fails++; console.error('FAIL the directory page carries no ANTBANDS-CARD block'); }
  else {
    const antCardLines = new Function(`${m[1]}; return antCardLines;`)();
    const rs = ['', rtR, '0-300MHz Wideband loop; [B] 144-146MHz 2 m', '500kHz-30MHz HF; 144–148 MHz 2 m (US); junk; 146-144MHz',
      '[Tuner 1 50Ω] 0-30MHz', '[Antenna C] 1-2GHz L band; 153-279kHz', '1-2MHz; 3-4MHz; 5-6MHz; 7-8MHz; 9-10MHz',
      '0.1357-0.1378MHz 2200 m', 'x'.repeat(900), `1-2MHz ${'n'.repeat(80)}`, '1-2MHz a "quoted" <b>name</b>'];
    const fsx = ['', rtF, 'band-pass 118-137MHz Airband; notch 1MHz; bandstop 100MHz; highpass 1.7 MHz MW',
      'bandstop 87.5-108MHz FM trap filter', '[B] lowpass 30MHz', 'lowpass 30MHz; highpass 1MHz; bandstop 1-2MHz; bandpass 3-4MHz; lowpass 5MHz',
      'highpass 1.7MHz <script>x</script>'];
    for (let i = 0; i < Math.max(rs.length, fsx.length); i++) {
      const r = rs[i % rs.length], f = fsx[i % fsx.length];
      eq(`directory page agrees: ${r.slice(0, 40)} | ${f.slice(0, 40)}`, antCardLines(r, f), card(r, f));
    }
    for (const r of rs) eq(`directory page agrees (ranges): ${r.slice(0, 40)}`, antCardLines(r, '').covers, card(r, '').covers);
    for (const f of fsx) eq(`directory page agrees (filters): ${f.slice(0, 40)}`, antCardLines('', f).filters, card('', f).filters);
    eq('directory page: non-strings say nothing', antCardLines(undefined, 42), { covers: '', filters: '' });
  }
}

console.log(`\n${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
