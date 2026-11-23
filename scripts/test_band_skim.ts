/*
 * test_band_skim.ts — V12 beta92 THE BAND SKIMMER's shared rules (src/services/bandSkim.ts), read by the web client AND the
 * app: the name, the band_skim frame's parser (and an OLDER server's frames — none, or fields missing), the VFO's width,
 * the waterfall marks, the "waiting for callsign" rows and the text under an expanded CW / RTTY spot.
 *   npx tsx scripts/test_band_skim.ts
 */
import { readFileSync } from 'node:fs';
import {
  BAND_SKIMMER_NAME, BAND_SKIM_KEY, BAND_SKIM_WAITING, BAND_SKIM_RTTY_WAITING, bandSkimTitle, parseBandSkim, bandSkimPassband,
  bandSkimLabels, isSkimLabel, skimLabelHint, withSkimLabels, bandSkimPending, bandSkimSignalFor, bandSkimText, skimColour,
} from '../src/services/bandSkim.ts';
import { knownLabelColour, SKIM_LABEL_COLOUR, type KnownSignal } from '../src/utils/knownSignals.ts';
import { DIGI_MODE_STYLE } from '../src/utils/digiModes.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const eq = (what: string, a: unknown, b: unknown) => ok(JSON.stringify(a) === JSON.stringify(b), `${what}: ${JSON.stringify(a)}${JSON.stringify(a) === JSON.stringify(b) ? '' : ' ≠ ' + JSON.stringify(b)}`);

console.log('▶ the name (Stuart 2026-11-23: "digi band should be called Band Skimmer as it covers cw which is analogue")');
eq('the name', BAND_SKIMMER_NAME, 'BAND SKIMMER');
eq('the compact key', BAND_SKIM_KEY, 'BAND SKIM');
eq('title on 40 m', bandSkimTitle('40 m'), 'BAND SKIMMER · 40 M');
eq('title off any band', bandSkimTitle(null), 'BAND SKIMMER');

// A frame as vibe_decoder_host.h skimJsonLocked_ writes it (test-bg-band's live path, trimmed)
const FRAME = { windows: [{ mode: 'RTTY', lo: 7040000, hi: 7050000, state: 'active' }, { mode: 'CW', lo: 7000000, hi: 7040000, state: 'active' }],
  signals: [
    { mode: 'RTTY', id: 1, hz: 7045000, snr: 20, wpm: 0, wpmLo: 0, wpmHi: 0, q: 'high', active: true, set: true, last: 1, call: 'G4RTY', country: 'England', settings: '45 baud / 170 Hz', text: 'CQ CQ DE G4RTY G4RTY K' },
    { mode: 'CW', id: 3, hz: 7012345, snr: 14, wpm: 22, wpmLo: 20, wpmHi: 24, q: 'medium', active: true, set: true, last: 1, call: '', country: '', settings: '', text: 'CQ CQ DE G4' },
    { mode: 'CW', id: 4, hz: 7020000, snr: 9, wpm: 25, wpmLo: 25, wpmHi: 25, q: 'high', active: true, set: true, last: 1, call: 'G4CWX', country: 'England', settings: '', text: 'CQ CQ DE G4CWX G4CWX K' },
    { mode: 'RTTY', id: 2, hz: 7047000, snr: 8, wpm: 0, wpmLo: 0, wpmHi: 0, q: 'poor', active: true, set: false, last: 1, call: '', country: '', settings: '', text: '' },
    { mode: 'CW', id: 5, hz: 7030000, snr: 6, wpm: 18, q: 'low', active: false, set: true, last: 1, call: '', country: '', settings: '', text: 'gone' },
  ] };

console.log('▶ the frame');
const sk = parseBandSkim(FRAME)!;
ok(!!sk && sk.windows.length === 2 && sk.signals.length === 5, 'parsed: 2 windows, 5 signals');
ok(sk.signals[1].wpmLo === 20 && sk.signals[1].wpmHi === 24, 'the CW speed range kept');
eq('an OLDER server: no frame at all', parseBandSkim(undefined), null);
eq('…a frame with neither list', parseBandSkim({}), { windows: [], signals: [] });
const thin = parseBandSkim({ signals: [{ mode: 'CW', hz: 7010000 }] })!;
ok(thin.signals.length === 1 && thin.signals[0].call === '' && thin.signals[0].text === '' && thin.signals[0].set === true && thin.signals[0].q === 'poor' && !('wpmLo' in thin.signals[0]),
   'a row with only mode and hz: blanks, settings "found", quality poor, no speed range');
eq('junk dropped (FT8, no hz, bad window)', parseBandSkim({ windows: [{ mode: 'FT8', lo: 1, hi: 2 }, { mode: 'CW', lo: 5, hi: 4 }], signals: [{ mode: 'FT8', hz: 7074000 }, { mode: 'CW' }] }), { windows: [], signals: [] });
ok(parseBandSkim({ signals: Array.from({ length: 300 }, (_, i) => ({ mode: 'CW', hz: 7000000 + i * 100, text: 'x'.repeat(999) })) })!.signals.length === 80, 'bounded: at most 80 rows…');
ok(parseBandSkim({ signals: [{ mode: 'CW', hz: 7000000, text: 'x'.repeat(999) }] })!.signals[0].text.length === 160, '…text at most 160 characters');

console.log('▶ the VFO shows the skimmer\'s width');
const dial = 7074000;
eq('40 m: the band span widened to the CW window (7000) and RTTY (7050)', bandSkimPassband([-36000, 6000], sk, dial), [-74000, 6000]);
eq('an older server: the band span as before', bandSkimPassband([-36000, 6000], null, dial), [-36000, 6000]);
eq('a window 2 MHz away (a stale frame) is not this band\'s', bandSkimPassband([-12000, 12000], parseBandSkim({ windows: [{ mode: 'CW', lo: 14000000, hi: 14040000 }] }), dial), [-12000, 12000]);

console.log('▶ the waterfall marks');
const marks = bandSkimLabels(sk);
eq('every signal still heard, by hz', marks.map((m) => m.hz), [7012345, 7020000, 7045000, 7047000]);
eq('labelled by call, else by mode', marks.map((m) => m.label), ['CW', 'G4CWX', 'G4RTY', 'RTTY']);
ok(marks.every(isSkimLabel) && marks.every((m) => m.dialHz === m.hz && m.dec === ''), 'each a skim mark that tunes to itself (no decoder opened)');
ok(knownLabelColour(marks[0]) === DIGI_MODE_STYLE.CW.colour && knownLabelColour(marks[2]) === DIGI_MODE_STYLE.RTTY.colour,
   'in the mode\'s own colour (teal CW, olive RTTY) — the list\'s');
ok(SKIM_LABEL_COLOUR.CW === skimColour('CW') && SKIM_LABEL_COLOUR.RTTY === skimColour('RTTY'), 'knownSignals.ts\'s copy equals digiModes.ts\'s');
ok(skimLabelHint(marks[0]).includes(BAND_SKIM_WAITING), 'a mark with no call says "waiting for callsign" to a screen reader');
eq('an older server: no marks', bandSkimLabels(null), []);
const base: KnownSignal[] = [{ hz: 7038000, label: 'X', dec: '', dialHz: 7038000, mode: 'usb', prio: 6, src: 'table' }];
eq('merged with the labels, sorted', withSkimLabels(base, marks)!.map((m) => m.hz), [7012345, 7020000, 7038000, 7045000, 7047000]);
ok(withSkimLabels(null, marks)!.length === 4, 'labels switched off: the marks alone');
ok(withSkimLabels(null, []) === null && withSkimLabels(base, []) === base, 'no marks: the list untouched (the same object)');

console.log('▶ "waiting for callsign" rows and the expanded text');
eq('pending (ALL): no call yet, still heard, lowest first', bandSkimPending(sk).map((s) => s.mode + s.id), ['CW3', 'RTTY2']);
eq('pending (CW filter)', bandSkimPending(sk, 'CW').map((s) => s.id), [3]);
eq('pending (FT8 filter): none', bandSkimPending(sk, 'FT8'), []);
eq('pending on an older server: none', bandSkimPending(null), []);
const NOW = 1_800_000_000_000;
eq('a callless signal ON a station just spotted is that spot\'s row, not a second one',
   bandSkimPending(sk, 'ALL', [{ mode: 'CW', callsign: 'G4ABC', frequency: 7012400, timestamp: NOW - 60_000 }], NOW).map((s) => s.mode + s.id), ['RTTY2']);
eq('…but a spot from 20 min ago no longer stands for it', bandSkimPending(sk, 'ALL', [{ mode: 'CW', callsign: 'G4ABC', frequency: 7012400, timestamp: NOW - 1_200_000 }], NOW).map((s) => s.id), [3, 2]);
eq('…nor an FT8 spot there', bandSkimPending(sk, 'ALL', [{ mode: 'FT8', callsign: 'G4ABC', frequency: 7012345, timestamp: NOW }], NOW).map((s) => s.id), [3, 2]);
eq('a spot whose signal was picked up afresh (no call yet) still reads its text', bandSkimSignalFor(sk, { mode: 'CW', callsign: 'G4ABC', frequency: 7012400 })?.id, 3);
eq('a CW spot finds its signal', bandSkimSignalFor(sk, { mode: 'CW', callsign: 'G4CWX', frequency: 7020040 })?.id, 4);
eq('…not one 2 kHz away', bandSkimSignalFor(sk, { mode: 'CW', callsign: 'G4CWX', frequency: 7022000 }), null);
eq('an FT8 spot has none', bandSkimSignalFor(sk, { mode: 'FT8', callsign: 'G4CWX', frequency: 7020000 }), null);
eq('the CW text', bandSkimText(sk.signals[2]), 'CQ CQ DE G4CWX G4CWX K');
eq('an RTTY signal before its settings', bandSkimText(sk.signals[3]), BAND_SKIM_RTTY_WAITING);

console.log('▶ both readers use it');
const web = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
const app = readFileSync(new URL('../src/components/DecoderPanel.tsx', import.meta.url), 'utf8');
const scr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
ok(/bandSkimPending\(/.test(web) && /bandSkimPending\(/.test(app), 'the "waiting for callsign" rows: web and app');
ok(/bandSkimSignalFor\(/.test(web) && /bandSkimSignalFor\(/.test(app), 'the expanded CW / RTTY text: web and app');
ok(/bandSkimPassband\(/.test(web) && /bandSkimPassband\(/.test(scr), 'the VFO\'s width: web and app');
ok(/bandSkimLabels\(/.test(web) && /bandSkimLabels\(/.test(scr), 'the waterfall marks: web and app');
ok(/plainEnglish\(/.test(app) && /cwRead\(t\)/.test(web), 'EXPLAIN / RAW reads the band skimmer\'s text: web and app');

console.log(`test_band_skim: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
