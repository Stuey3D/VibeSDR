/**
 * test_freedv.ts — src/services/freedv.ts, the ONE RULE the app and the web client both read for FreeDV as a mode
 * (docs/v12/FREEDV.md): what is offered, the report, the VTS line, the passband side, the message.
 *   Run: node --no-warnings --max-old-space-size=512 scripts/test_freedv.ts
 * ★ SILENT: no audio, no network.
 */
import { FREEDV_SUBS, freedvOffered, freedvPassband, freedvSidebandAt, freedvSubMessage, freedvVts, parseFreeDv } from '../src/services/freedv.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { (c ? pass++ : fail++); console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const eq = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);

console.log('── offered only where the server lists it ──');
ok(freedvOffered(undefined).length === 0 && freedvOffered({}).length === 0, 'no list (another backend, an older server) → none');
ok(freedvOffered({ modes: ['usb', 'lsb', 'drm'], freedvSubs: ['700D'] }).length === 0, '"freedv" not in modes (no libcodec2) → none, whatever freedvSubs says');
ok(eq(freedvOffered({ modes: ['freedv'], freedvSubs: ['1600', '700D'] }), ['700D', '1600']), 'our key order, only what the library runs');
ok(eq(freedvOffered({ modes: ['freedv'], freedvSubs: ['700D', '700E', '1600', '2020'] }), [...FREEDV_SUBS]), 'unknown sub-modes ignored');

console.log('── the report ──');
ok(parseFreeDv({ type: 'drm' }) === null && parseFreeDv(null) === null, 'not a report → null');
const s0 = parseFreeDv({ type: 'freedv', sub: 'auto', sb: 'usb', state: 'search', mode: '' })!;
ok(s0 && !s0.sync && s0.mode === '' && s0.sub === 'auto' && s0.snr === undefined, 'searching');
const s1 = parseFreeDv({ type: 'freedv', sub: 'auto', sb: 'lsb', state: 'sync', mode: '700D', snr: 6.4, text: 'G4ABC Stuart\u0007' })!;
ok(s1.sync && s1.mode === '700D' && s1.snr === 6 && s1.sb === 'lsb' && s1.text === 'G4ABC Stuart', 'in sync: mode, SNR rounded, sideband, text kept printable');
ok(parseFreeDv({ type: 'freedv', sub: '2020', state: 'search' })!.sub === 'auto', 'an unknown choice reads as AUTO');
ok(parseFreeDv({ type: 'freedv', state: 'search', mode: '700D', snr: 3 })!.snr === undefined, 'no SNR while searching');

console.log('── the VTS line ──');
ok(freedvVts(null) === 'FreeDV · searching', 'before the first report');
ok(freedvVts(s0) === 'FreeDV · searching', 'AUTO searching');
ok(freedvVts(parseFreeDv({ type: 'freedv', sub: '1600', state: 'search' })) === 'FreeDV 1600 · searching', 'a manual choice searching names it');
ok(freedvVts(s1) === 'FreeDV 700D · 6 dB · G4ABC Stuart', 'in sync: mode · SNR · text');
ok(freedvVts(s1).length <= 40, 'short enough for the R36S');

console.log('── the passband side and the message ──');
ok(eq(freedvPassband('usb'), [50, 2700]) && eq(freedvPassband('lsb'), [-2700, -50]), 'USB above the dial, LSB below');
ok(freedvSidebandAt(7.177e6) === 'lsb' && freedvSidebandAt(3.643e6) === 'lsb' && freedvSidebandAt(14.236e6) === 'usb' && freedvSidebandAt(10e6) === 'usb', 'the server\'s rule: LSB below 10 MHz');
ok(eq(freedvSubMessage('700E'), { type: 'freedv_sub', sub: '700E' }) && eq(freedvSubMessage(null), { type: 'freedv_sub' }), 'freedv_sub, with and without a choice');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
