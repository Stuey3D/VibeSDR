// test_lrpt_app.ts — V12 METEOR-M LRPT, the clients' half (src/services/lrpt.ts): lrpt_status parsed defensively, the
// status line at each stage of a pass, the attach parameters, and the key's place (decoderBands: VHF, near 137.1 / 137.9).
// The server side is vibeserver/test-lrpt.cpp (the decoder) and test-lrpt-host.cpp (the host, preview, gallery, reach).
import { lrptAttachParams, lrptStatusText, parseLrptStatus } from '../src/services/lrpt';
import { REACH_DECODERS } from '../src/services/sdrTypes';
import { CLIENT_DECODERS } from '../src/constants/modeBox';

let fail = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

// a real status frame's data, as vibe_lrpt_run.h sends it (from test-lrpt-host)
const real = { on: true, carrier: false, sync: true, mode: 'QPSK', diff: 0, symbolRate: 72000, lock: 63, offsetHz: -1067,
  frames: 47, bad: 1, packets: 603, lines: 112, channels: ['1 visible', '2 near-IR', '5 thermal'] };
const st = parseLrptStatus(real);
ok(!!st && st.frames === 47 && st.lines === 112 && st.channels.length === 3, 'a real status parses');
ok(lrptStatusText(st) === '112 lines · channels 1, 2, 5 · 47 frames', `the line during a pass: ${lrptStatusText(st)}`);
ok(lrptStatusText(parseLrptStatus({ ...real, sync: false })) === '112 lines · channels 1, 2, 5 · 47 frames · between frames, still listening', 'a gap in the frames is said as a gap (an older server: no reading)');
ok(lrptStatusText(parseLrptStatus({ ...real, sync: false, snrDb: 3.24, needDb: 5 })) === '112 lines · channels 1, 2, 5 · 47 frames · between frames, still listening · signal +3.2 dB over the noise (needs about +5)', 'beta91: the gap with the signal reading');
ok(lrptStatusText(parseLrptStatus({ carrier: true, mode: 'OQPSK', offsetHz: 812 })) === 'carrier found (OQPSK, +812 Hz) · looking for frames', 'carrier found');
ok(lrptStatusText(parseLrptStatus({ sync: true, mode: 'QPSK', frames: 9, bad: 2 })) === 'QPSK locked · frames 9 (2 unreadable)', 'frames before any picture');
ok(lrptStatusText(parseLrptStatus({})) === 'listening for a METEOR satellite', 'nothing yet');
ok(lrptStatusText(parseLrptStatus({ on: true, lock: 73 })) === 'listening for a METEOR satellite', 'beta91: the lock % is never shown (an older server sends only that)');
ok(lrptStatusText(parseLrptStatus({ on: true, lock: 64, snrDb: 0.1, needDb: 5 })) === 'listening for a METEOR satellite · signal +0.1 dB over the noise (needs about +5)', 'beta91: noise reads about 0 dB, and what decoding needs');
ok(lrptStatusText(parseLrptStatus({ on: true, snrDb: 4.26, needDb: 5, saved: 1 })) === 'last pass saved to the gallery · listening for a METEOR satellite · signal +4.3 dB over the noise (needs about +5)', 'beta91: after a pass is filed the LIVE state shows, never a stale "picture complete"');
ok(lrptStatusText(parseLrptStatus({ on: true, snrDb: -0.4 })).includes('signal −0.4 dB'), 'a reading under the noise is signed');
{ const j = parseLrptStatus({ snrDb: 'x', needDb: Infinity, saved: -3, inRange: 7 }); ok(!!j && j.snrDb === undefined && j.needDb === undefined && j.saved === 0 && j.inRange === 1, 'beta91 fields bounded'); }
ok(lrptStatusText(null) === 'waiting for a METEOR pass', 'no status at all');
ok(lrptStatusText(parseLrptStatus({ why: 'the IQ slice runs at 50000 S/s — LRPT needs 1.8 to 8 times the symbol rate' })).startsWith('the IQ slice'), 'a reason is shown');
// defensive: junk never throws, never shows NaN, bounds lengths
const junk = parseLrptStatus({ frames: 'x', lines: NaN, channels: [1, 'a'.repeat(99), null, '2 near-IR', 'x', 'y', 'z', 'w'], mode: 'Q'.repeat(50) });
ok(!!junk && junk.frames === 0 && junk.lines === 0 && junk.channels.length <= 6 && junk.channels.every((c) => c.length <= 24) && junk.mode.length <= 8, 'junk is bounded');
ok(parseLrptStatus(null) === null && parseLrptStatus('x') === null, 'not an object: null');
ok(!/NaN|undefined/.test(lrptStatusText(junk)), `no NaN in the line: ${lrptStatusText(junk)}`);
// attach
ok(JSON.stringify(lrptAttachParams()) === '{"modulation":"auto","diff":"auto"}', 'attach: no rate named = AUTO (72k / 80k)');
ok(lrptAttachParams(80000).symbol_rate === 80000 && lrptAttachParams(79000).symbol_rate === 80000 && lrptAttachParams(72000).symbol_rate === 72000, 'attach: a satellite entry names its rate');
ok(lrptAttachParams(0).symbol_rate === undefined, 'attach: 0 = AUTO');
// registration
ok((REACH_DECODERS as readonly string[]).includes('lrpt'), 'greyed with its reason off 137 MHz (REACH_DECODERS)');
ok((CLIENT_DECODERS as readonly string[]).includes('lrpt'), 'a client decoder (CLIENT_DECODERS)');

console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
