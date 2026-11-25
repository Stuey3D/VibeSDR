/**
 * ★★ V12 beta94 A SPOT'S MESSAGE IN THE APP (Stuart 11-25, UberSDR's "Message" column): src/utils/spotMessage.ts reads a
 * digital_spot's `msg` (and UberSDR's spellings), cleans and bounds it, and the expanded row draws it on its own line —
 * none = no line. The app's DecoderClient and DecoderPanel are checked to use it (they import React Native, so they are
 * read, not run). The web's parser (decoders.ts) is cut out and run on the same frames: ONE RULE, TWO READERS.
 *
 * Run: npx tsx scripts/test_spot_message_app.ts   (run-tests.sh does)
 */
import { readFileSync } from 'node:fs';
import { spotMsgOf, spotMsgLine, spotDetailLine, SPOT_MSG_MAX } from '../src/utils/spotMessage';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const ok = (what: string, c: boolean) => eq(what, c, true);

// ── the parser keeps msg ──
const ft8 = { mode: 'FT8', callsign: 'IW2FAC', snr: -12, frequency: 14075200, band: '20m', grid: 'JN45', how: '', country: 'Italy',
              timestamp: 1764060000000, dt: 0.1, msg: 'CQ IW2FAC JN45' };
eq('a beta94 FT8 spot keeps its msg', spotMsgOf(ft8), 'CQ IW2FAC JN45');
eq('a report spot (no grid) keeps the report', spotMsgOf({ ...ft8, callsign: 'W9DHI', grid: '', msg: 'ZL1RKO W9DHI -16' }), 'ZL1RKO W9DHI -16');
eq('WSPR: CALL GRID dBm', spotMsgOf({ mode: 'WSPR', callsign: 'K1ABC', msg: 'K1ABC FN42 37' }), 'K1ABC FN42 37');
eq('an older VibeServer (no msg): undefined', spotMsgOf({ ...ft8, msg: undefined }), undefined);
eq('an empty msg: undefined', spotMsgOf({ ...ft8, msg: '   ' }), undefined);
eq('not a string: undefined', spotMsgOf({ ...ft8, msg: 42 as unknown as string }), undefined);
eq('UberSDR\'s "message" still read', spotMsgOf({ callsign: 'F5MXH', message: 'F5MXH N2XSG RR73' }), 'F5MXH N2XSG RR73');
eq('…and "text"', spotMsgOf({ callsign: 'EA5OL', text: 'EA5OL KC1UVP FN42' }), 'EA5OL KC1UVP FN42');
eq('control characters become spaces, collapsed', spotMsgOf({ msg: 'CQ\n\u0001 DX  G4ABC' }), 'CQ DX G4ABC');
eq('a bidi override is removed', spotMsgOf({ msg: 'CQ ‮G4ABC' }), 'CQ G4ABC');
ok(`bounded to ${SPOT_MSG_MAX}`, (spotMsgOf({ msg: 'X'.repeat(500) }) ?? '').length === SPOT_MSG_MAX);
eq('null data', spotMsgOf(null), undefined);

// ── the expanded lines ──
eq('the detail line: parts joined with ·, missing ones drop out', spotDetailLine(['JN45', undefined, '', '12:00:15z']), 'JN45 · 12:00:15z');
eq('the message line: the message', spotMsgLine('CQ IW2FAC JN45'), 'CQ IW2FAC JN45');
eq('no message: no line', spotMsgLine(undefined), '');
eq('a band skimmer\'s own text wins (CW / RTTY): no message line', spotMsgLine('CQ G4ABC', 'CQ CQ DE G4ABC K'), '');

// ── the app uses it ──
const dc = readFileSync(new URL('../src/services/DecoderClient.ts', import.meta.url), 'utf8');
const dp = readFileSync(new URL('../src/components/DecoderPanel.tsx', import.meta.url), 'utf8');
ok('DecoderClient\'s digital_spot reads msg through spotMsgOf', /m\.type === 'digital_spot'[\s\S]{0,1500}msg: spotMsgOf\(d\)/.test(dc));
ok('DecoderPanel draws the message on its own line, expanded only', /expanded && !!spotMsgLine\(s\.msg, skimText\) &&/.test(dp));
ok('…and no longer inside the one-line detail', !/\[\s*s\.msg,\s*s\.grid/.test(dp));

// ── one rule, two readers: the web's parser on the same frames ──
const dec = readFileSync(new URL('../web/client/src/decoders.ts', import.meta.url), 'utf8');
const m = dec.match(/\.\.\.\(typeof d\.msg === 'string'[^\n]*\n/);
ok('the web parser reads msg', !!m);
if (m) {
  // eslint-disable-next-line no-new-func
  const webMsg = new Function('d', `return ({ ${m[0].trim().replace(/,$/, '')} }).msg;`) as (d: unknown) => string | undefined;
  for (const d of [ft8, { msg: 'ZL1RKO W9DHI -16' }, {}, { msg: '  ' }, { msg: 'K1ABC FN42 37' }])
    eq(`web and app agree on ${JSON.stringify((d as { msg?: string }).msg)}`, webMsg(d), spotMsgOf(d as Record<string, unknown>));
}
const main = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
ok('the web draws it under the detail line, the skim text winning', /skimSig \? bandSkimTextHtml\(skimSig\) : spotMsgHtml\(sp\)/.test(main));

console.log(`spot message (app): ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
