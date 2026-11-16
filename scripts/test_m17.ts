/**
 * test_m17.ts — src/services/m17.ts, the ONE RULE the app and the web client both read for M17 as a mode (docs/v12/M17.md):
 * what is offered, the report, the VTS line, the info row, the packets kept, the passband, the message.
 *   Run: npx tsx scripts/test_m17.ts
 * ★ SILENT: no audio, no network.
 */
import { M17_PASSBAND, m17Info, m17Line, m17MergeMsgs, m17MsgLine, m17Offered, m17Position, m17StatusMessage, m17Vts, parseM17,
         type M17Msg } from '../src/services/m17.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { (c ? pass++ : fail++); console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const eq = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);

console.log('── offered only where the server lists it ──');
ok(!m17Offered(undefined) && !m17Offered({}) && !m17Offered({ modes: ['nfm', 'freedv'] }), 'no "m17" in modes (no libcodec2, no 2 m / 70 cm, another backend) → not offered');
ok(m17Offered({ modes: ['nfm', 'm17'] }), '"m17" listed → offered');

console.log('── the report ──');
ok(parseM17({ type: 'freedv' }) === null && parseM17(null) === null && parseM17('m17') === null, 'not a report → null');
const s0 = parseM17({ type: 'm17', state: 'search' })!;
ok(eq(s0, { sync: false }), 'searching, nothing heard yet');
const s1 = parseM17({ type: 'm17', state: 'sync', kind: 'voice', src: 'G4ABC', dst: '@ALL', can: 3, ber: 0.4, off: 150, text: 'VIBE\u0007 TEST' })!;
ok(s1.sync && s1.kind === 'voice' && s1.src === 'G4ABC' && s1.dst === '@ALL' && s1.can === 3 && s1.ber === 0 && s1.off === 150 && s1.text === 'VIBE TEST',
   'in sync: kind, callsigns, CAN, BER rounded, offset, text kept printable');
ok(parseM17({ type: 'm17', state: 'sync', kind: 'bogus', can: 99 })!.kind === undefined && parseM17({ type: 'm17', state: 'sync', can: 99 })!.can === undefined,
   'an unknown kind and an impossible CAN are dropped');
ok(parseM17({ type: 'm17', state: 'search', src: 'G4ABC', last: 'M0XYZ' })!.src === undefined && parseM17({ type: 'm17', state: 'search', last: 'M0XYZ' })!.last === 'M0XYZ',
   'no callsigns while searching, the last one heard instead');
const g = parseM17({ type: 'm17', state: 'sync', gnss: { lat: 52.62345, lon: -1.12346, alt: 120.5, spd: 48.5, brg: 271, r: 8 } })!;
ok(eq(g.gnss, { lat: 52.62345, lon: -1.12346, alt: 120.5, spd: 48.5, brg: 271, r: 8 }), 'GNSS read whole');
ok(parseM17({ type: 'm17', state: 'sync', gnss: { lat: 95, lon: 0 } })!.gnss === undefined, 'an impossible position is dropped');
const p = parseM17({ type: 'm17', state: 'search', msg: { id: 2, src: 'G4ABC', dst: 'M0XYZ', ptype: 5, bytes: 20, text: 'Hello from M17' } })!;
ok(eq(p.msg, { id: 2, ptype: 5, bytes: 20, src: 'G4ABC', dst: 'M0XYZ', text: 'Hello from M17' }), 'an SMS');
ok(parseM17({ type: 'm17', state: 'search', msg: { id: 3, ptype: 2, bytes: 41, text: 'x' } })!.msg!.text === undefined, 'text is read only for an SMS (protocol 5)');

console.log('── the VTS line ──');
ok(m17Vts(null) === 'M17 · searching' && m17Vts(s0) === 'M17 · searching', 'searching');
ok(m17Vts(parseM17({ type: 'm17', state: 'search', last: 'G4ABC' })) === 'M17 · last G4ABC', 'searching after an over: the last station');
ok(m17Vts(s1) === 'M17 · G4ABC', 'in sync to everybody (@ALL): the source alone');
ok(m17Vts(parseM17({ type: 'm17', state: 'sync', kind: 'voice', src: 'G4ABC/P', dst: 'M17-M17 C' })) === 'M17 · G4ABC/P → M17-M17 C', 'to a reflector: both');
ok(m17Vts(parseM17({ type: 'm17', state: 'sync', kind: 'voice', src: 'G4ABC', enc: 'aes' })) === 'M17 · G4ABC · encrypted', 'encrypted: said, not played');
ok(m17Vts(parseM17({ type: 'm17', state: 'sync', kind: 'packet', src: 'G4ABC', dst: 'M0XYZ' })) === 'M17 · G4ABC → M0XYZ · data', 'a packet');
ok(m17Vts(parseM17({ type: 'm17', state: 'sync', src: 'ABCDEFGHI', dst: 'JKLMNOPQR' })).length <= 32, 'short enough for the R36S with two full callsigns');
ok(m17Vts(parseM17({ type: 'm17', state: 'sync', kind: 'voice' })) === 'M17 · …', 'in sync before the callsign is known');

console.log('── the info row, packets and the rest ──');
ok(m17Position({ lat: 52.62346, lon: -1.12346, alt: 120.5, spd: 48.5 }) === '52.6235° N 1.1235° W, 121 m, 49 km/h', 'a position as a person reads it');
ok(m17MsgLine(p.msg!) === 'SMS G4ABC → M0XYZ: Hello from M17' && m17MsgLine({ id: 1, ptype: 2, bytes: 41, src: 'M0XYZ' }) === 'Packet (type 2, 41 bytes) from M0XYZ', 'packets as lines');
ok(m17Info(null) === '' && m17Info(s0) === '', 'nothing to say → empty');
ok(m17Info(parseM17({ type: 'm17', state: 'sync', via: 'G7XYZ via M17-GBR A', text: 'Hi', ber: 7 })) === 'G7XYZ via M17-GBR A · Hi · 7 % errors', 'via, text, then the error rate when it is high');
ok(m17Line(parseM17({ type: 'm17', state: 'sync', kind: 'voice', src: 'G4ABC', dst: '@ALL', text: 'VIBE TEST' })) === 'M17 · G4ABC · VIBE TEST'
   && m17Line(s0) === 'M17 · searching', 'one line for a bar with one text: the VTS line, then the info');
let list: M17Msg[] = [];
list = m17MergeMsgs(list, p); list = m17MergeMsgs(list, p);
ok(list.length === 1, 'a packet repeated in every report is kept once');
list = m17MergeMsgs(list, parseM17({ type: 'm17', state: 'search', msg: { id: 1, src: 'M0AAA', ptype: 5, bytes: 4, text: 'new' } }));
ok(list.length === 2 && list[0].text === 'new', 'a new receiver numbering from 1 again: still a new packet, newest first');
ok(eq(M17_PASSBAND, [-4500, 4500]) && eq(m17StatusMessage(), { type: 'm17_status' }), 'the 9 kHz passband, the report request');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
