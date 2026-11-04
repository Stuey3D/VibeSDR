// test_rsp_card.ts — the Android server screen's SDRplay card (src/services/rspCard.ts): RadioConfig's defaults, a stored
// card read back safely, the keys the engine reads, and which rows each RSP model has (the Setup page's rspCaps).
import { RSP_DEFAULTS, readRspCard, rspConfigKeys, rspCaps } from '../src/services/rspCard.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
console.log('SDRplay card (Android server screen)');

const d = readRspCard('');
ok(JSON.stringify(d) === JSON.stringify(RSP_DEFAULTS), 'nothing stored = RadioConfig\'s defaults');
ok(d.userNotch === true && d.dabAgcOverride === true && d.dabAgcTarget === -40 && d.agcSet === -30 && !d.rfAgc,
   'defaults: listeners may toggle notches, DAB target on at -40, IF target -30 (SDRplay\'s own), RF AGC off');
ok(readRspCard('{"agcSet":-999}').agcSet === -30, 'the "never chosen" sentinel reads as -30, never as a slider position');
ok(readRspCard('{"dabAgcTarget":5,"agcSet":-90}').dabAgcTarget === -10 && readRspCard('{"agcSet":-90}').agcSet === -72, 'targets clamped to the Setup page\'s ranges');
ok(readRspCard('{"userNotch":false,"rspHdr":true}').userNotch === false && readRspCard('{"rspHdr":true}').rspHdr === true, 'stored booleans kept');
ok(readRspCard('garbage').rfNotch === false, 'an unreadable card = the defaults');
const k = rspConfigKeys({ ...RSP_DEFAULTS, rfNotch: true });
ok(k.rfNotch === true && 'agcSetLock' in k && !('antennaMap' in k), 'engine keys carry the card (the aerial map travels as antennaMap/antennaLocked)');

ok(rspCaps(null, 'SDRplay RSPdx-R2').hdr && !rspCaps(null, 'SDRplay RSPdx-R2').amNotch, 'RSPdx-R2: HDR, no AM notch');
ok(rspCaps(null, 'RSPduo').amNotch && rspCaps(null, 'RSPduo').extRef && !rspCaps(null, 'RSPduo').hdr, 'RSPduo: AM notch + reference');
ok(rspCaps(null, 'RSP2').extRef && !rspCaps(null, 'RSP2').dabNotch && rspCaps(null, 'RSP2').antennas.length === 3, 'RSP2: reference, no DAB notch, 3 sockets');
ok(!rspCaps(null, 'RSP1').rfNotch && !rspCaps(null, 'RSP1').dabNotch, 'RSP1: no notches at all');
ok(rspCaps(null, 'RSP1A').rfNotch && rspCaps(null, 'RSP1A').antennas.length === 0, 'RSP1A: notches, one socket');
ok(!rspCaps(null, 'something').known && rspCaps(null, 'something').hdr, 'unknown model: everything offered (the radio refuses what it lacks)');
ok(rspCaps({ driver: 'sdrplay', model: 'RSPdx', hdr: false }, 'RSPdx').hdr === false, 'the radio\'s own hwinfo answer wins over the name');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
