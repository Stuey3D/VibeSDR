/**
 * test_packet.ts — src/services/packet.ts, the ONE RULE the app and the web client both read for the packet decoder's
 * choices (AUTO / 1200 / 9600 / 300, FX.25, IL2P) and its frame log (docs/v12/AX25-9600-FX25-IL2P.md).
 *   Run: npx tsx scripts/test_packet.ts
 * ★ SILENT: no audio, no network.
 */
import { packetChoices, packetDefault, packetFecShown, packetFrameHead, packetHexGrouped, packetMergeLog, packetParams,
         packetStatusExtra, packetTune, parsePacketCaps, parsePacketFrame } from '../src/services/packet.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { (c ? pass++ : fail++); console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const eq = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);

console.log('── what is offered comes from the server ──');
const old = parsePacketCaps({ aprs: true });
const live = parsePacketCaps({ aprs: true, aprsFec: true, pkt9600: true });
const grey = parsePacketCaps({ aprs: true, aprsFec: true, pkt9600: false, pkt9600Why: 'Outside this radio\'s locked range (88.0–96.0 MHz) — 9600 packet is on 2 m and 70 cm' });
const icom = parsePacketCaps({ aprs: true, aprsFec: true });
ok(eq(packetChoices(old).map((c) => c.id), ['1200', '300']), 'an older server: VHF 1200 and HF 300 only, as before');
ok(eq(packetChoices(live).map((c) => c.id), ['auto', '1200', '9600', '300']) && packetChoices(live).every((c) => !c.why), '9600 live: AUTO · 1200 · 9600 · 300, all live');
const g = packetChoices(grey);
ok(g.find((c) => c.id === '9600')?.why?.includes('locked range') === true && g.find((c) => c.id === 'auto')?.why !== undefined, '9600 unreachable: AUTO and 9600 greyed WITH the reason (never hidden, never live)');
ok(eq(packetChoices(icom).map((c) => c.id), ['1200', '300']), 'an Icom (no pkt9600 at all): no 9600, no AUTO');
ok(packetDefault(live) === 'auto' && packetDefault(grey) === '1200' && packetDefault(old) === '1200', 'default: AUTO where 9600 runs, else 1200');
ok(packetDefault(grey, '9600') === '1200' && packetDefault(live, '9600') === '9600' && packetDefault(live, '300') === '300', 'a stored choice the radio cannot do falls back');
ok(packetFecShown(live, '1200') && packetFecShown(live, '9600') && !packetFecShown(live, 'auto') && !packetFecShown(live, '300') && !packetFecShown(old, '1200'),
   'FX.25 / IL2P switches under 1200 and 9600 only, and only where the server has them');

console.log('── the attach parameters ──');
ok(eq(packetParams('1200', true, true, old), { baud_rate: 1200 }), 'older server: exactly what it knew');
ok(eq(packetParams('auto', false, false, live), { baud_rate: 1200, packet: 'auto' }), 'AUTO: baud_rate 1200 (an old server runs 1200), packet auto');
ok(eq(packetParams('9600', true, false, live), { baud_rate: 9600, packet: '9600', fx25: 1, il2p: 0 }), '9600 with FX.25, without IL2P');
ok(eq(packetParams('300', true, true, live), { baud_rate: 300 }), 'HF 300: nothing else');
ok(packetTune('9600', 144.8e6) === null && packetTune('auto', 144.8e6)?.hz === 144800000 && packetTune('300', 144.8e6)?.mode === 'usb',
   'TUNE: none for 9600 (no one channel), the region\'s channel for AUTO / 1200, 30 m USB for 300');

console.log('── the frame log ──');
const f = parsePacketFrame({ age: 0, from: 'OM9GRB', to: 'CQ', ctl: 'UI', pid: 'F0', fec: 'fx25', fixed: 4, sat: 'GRBAlpha', len: 120,
                             hex: '6a98f983b8c80015', text: 'j......<', extra: 'x' })!;
ok(f && f.sat === 'GRBAlpha' && f.fec === 'fx25' && f.fixed === 4 && f.len === 120, 'a telemetry frame: satellite, FEC, fixes, length');
ok(packetFrameHead(f) === 'OM9GRB → CQ · UI F0 · GRBAlpha · FX.25 · 4 fixed · 120 bytes', `its heading: ${packetFrameHead(f)}`);
ok(packetHexGrouped('6a98f983b8c80015') === '6a98f983 b8c80015' && packetHexGrouped('00'.repeat(70), 64).endsWith('…'), 'hex grouped, cut at 64 bytes');
ok(parsePacketFrame({ from: '<b>', to: 'CQ' }) === null && parsePacketFrame(null) === null, 'junk is no frame');
const x = parsePacketFrame({ from: 'G4ABC', to: 'APRS', ctl: 'UI<script>', fec: 'weird', len: 5, hex: 'zz41', text: 'A\u0001<b>' })!;
ok(x.ctl === 'UIscript' && x.fec === 'ax25' && x.hex === '41' && x.text === 'A.<b>', 'everything cleaned (the caller escapes HTML)');
const il = parsePacketFrame({ from: 'KK4HEJ', to: 'CQ', ctl: 'UI', fec: 'il2p', crc: 1, len: 0, hex: '', text: '' })!;
ok(packetFrameHead(il) === 'KK4HEJ → CQ · UI · IL2P · CRC · 0 bytes', `IL2P with its CRC: ${packetFrameHead(il)}`);
const log = packetMergeLog([f], [il, x]);
ok(log[0] === x && log[1] === il && log[2] === f, 'a joiner\'s log (oldest first) goes on top, newest first');
ok(packetMergeLog([], Array.from({ length: 80 }, () => f)).length === 60, 'at most 60 kept');
ok(packetStatusExtra({ fx25: 3, il2p: 1, fixed: 12, other: 2 }) === ' · 3 FX.25 · 1 IL2P · 12 bytes fixed · 2 other frames' && packetStatusExtra({}) === '',
   'the status line\'s packet half');

console.log(`\ntest_packet: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
