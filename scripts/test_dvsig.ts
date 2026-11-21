/**
 * test_dvsig.ts — src/services/dvsig.ts, the ONE RULE the app, the web client and (mirrored) the R36S read for DV ID — DMR,
 * D-STAR, YSF, NXDN, dPMR signalling, never the voice (docs/v12/DV-SIGNALLING.md): what is offered, the report, what a
 * public copy can and cannot carry, the VTS line, the log row, the map's points (amateur identities only), the messages.
 *   Run: npx tsx scripts/test_dvsig.ts
 * ★ SILENT: no audio, no network.
 */
import { DVSIG_PASSBAND, DV_PINS, dvKind, dvPosition, dvRow, dvWhere, dvWho, dvsigInfo, dvsigLine, dvsigMapPoints, dvsigOffered,
         dvsigPinMessage, dvsigStatusMessage, dvsigVts, parseDvsig } from '../src/services/dvsig.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { (c ? pass++ : fail++); console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };

console.log('── offered only where the server lists it ──');
ok(!dvsigOffered(undefined) && !dvsigOffered({}) && !dvsigOffered({ modes: ['nfm', 'm17'] }), 'no "dvsig" in modes → not offered');
ok(dvsigOffered({ modes: ['nfm', 'dvsig'] }), '"dvsig" listed → offered');

console.log('── the report ──');
ok(parseDvsig({ type: 'm17' }) === null && parseDvsig(null) === null, 'not a report → null');
const dmr = { id: 3, t: 1000, t1: 4000, sys: 'DMR', kind: 'voice', open: true, enc: false, slot: 2, cc: 1, src: 2345678, dst: 2350, grp: true,
              alias: 'G4ABC Stuart\u0007', gps: { lat: 52.6235, lon: -1.1235 }, ber: 1.4 };
const ids = parseDvsig({ type: 'dvsig', state: 'sync', pin: 'auto', amateur: true, band: '70 cm amateur', ids: true, sys: 'DMR', log: [dmr] })!;
const e = ids.log[0];
ok(ids.sync && ids.amateur && ids.ids && ids.band === '70 cm amateur' && ids.sys === 'DMR', 'state, the band, an identities copy');
ok(e.src === 2345678 && e.dst === 2350 && e.grp === true && e.alias === 'G4ABC Stuart' && e.gps?.lat === 52.6235 && e.ber === 1, 'the identities read, the alias printable, BER rounded');
// ★★ a public copy: even if a server (or a forgery) put identities in it, the client keeps none of them
const pub = parseDvsig({ type: 'dvsig', state: 'sync', pin: 'auto', amateur: false, band: '', ids: false, log: [dmr] })!;
const p = pub.log[0];
ok(p.src === undefined && p.dst === undefined && p.alias === undefined && p.gps === undefined && p.cc === 1 && p.slot === 2, 'a public copy keeps only the channel\'s numbers (CC, slot) — no ID, alias or position');
ok(parseDvsig({ type: 'dvsig', ids: true, log: [{ ...dmr, sys: 'TETRA' }, { ...dmr, gps: { lat: 0, lon: 0 } }] })!.log.length === 1, 'an unknown system is dropped');
ok(parseDvsig({ type: 'dvsig', ids: true, log: [{ ...dmr, gps: { lat: 0, lon: 0 } }] })!.log[0].gps === undefined, 'a 0,0 position is no position');
ok(parseDvsig({ type: 'dvsig', pin: 'tetra' })!.pin === 'auto' && parseDvsig({ type: 'dvsig', pin: 'ysf' })!.pin === 'ysf', 'an unknown pin reads as AUTO');

console.log('── what each system says ──');
ok(dvWhere(e) === 'CC 1 · TS2' && dvWho(e) === '2345678 → TG 2350', 'DMR: colour code and slot; source → talkgroup');
ok(dvWho({ ...e, grp: false, dst: 2342222 }) === '2345678 → 2342222', 'DMR private call');
// ★ V12 beta90 DMR ID → callsign (the server's RadioID lookup): "G4ABC (2355254) → TG 2351", the alias wins as the name
const looked = parseDvsig({ type: 'dvsig', ids: true, amateur: true, log: [{ ...dmr, alias: undefined, src: 2355254, dst: 2351, srcCall: 'GI4SZW' }] })!.log[0];
ok(dvWho(looked) === 'GI4SZW (2355254) → TG 2351', 'DMR: the looked-up callsign with its ID → the talkgroup');
ok(dvWho({ ...looked, alias: 'GI4SZW Seamus' }) === '2355254 → TG 2351', 'DMR: a talker alias wins — the ID stays a number beside it');
ok(dvWho({ ...looked, grp: false, dst: 2342222, dstCall: 'G4ABC' }) === 'GI4SZW (2355254) → G4ABC (2342222)', 'DMR private call: both ends named');
ok(parseDvsig({ type: 'dvsig', ids: true, log: [{ ...dmr, srcCall: 'G4"X<' }] })!.log[0].srcCall === undefined, 'a looked-up callsign is whitelisted');
ok(parseDvsig({ type: 'dvsig', ids: false, log: [{ ...dmr, srcCall: 'GI4SZW' }] })!.log[0].srcCall === undefined, 'a public copy keeps no looked-up callsign');
const ds = parseDvsig({ type: 'dvsig', ids: true, log: [{ id: 1, t: 1, sys: 'D-STAR', kind: 'voice', open: false, enc: false, from: 'G4ABC/ID51', to: 'CQCQCQ', via1: 'GB7XX  B', text: 'Hello' }] })!.log[0];
ok(dvWho(ds) === 'G4ABC/ID51 → CQCQCQ via GB7XX B' && dvRow(ds).includes('Hello'), 'D-STAR: MY → UR via RPT1, the slow-data message');
const ys = parseDvsig({ type: 'dvsig', ids: true, log: [{ id: 1, t: 1, sys: 'YSF', kind: 'voice', open: true, enc: false, mode: 'DN', dg: 23, from: 'G4ABC', to: 'ALL' }] })!.log[0];
ok(dvWhere(ys) === 'DG-ID 23 · DN' && dvWho(ys) === 'G4ABC → ALL', 'YSF: DG-ID and DN / VW; source → destination');
const nx = parseDvsig({ type: 'dvsig', ids: true, log: [{ id: 1, t: 1, sys: 'NXDN', kind: 'voice', open: true, enc: true, ran: 5, mode: 'NXDN48', src: 4321, dst: 77, grp: true }] })!.log[0];
ok(dvWhere(nx) === 'RAN 5 · NXDN48' && dvKind(nx) === 'encrypted' && dvRow(nx).includes('encrypted'), 'NXDN: RAN; encrypted is named, nothing more');
const pm = parseDvsig({ type: 'dvsig', ids: false, log: [{ id: 1, t: 1, sys: 'dPMR', kind: 'voice', open: true, enc: false, cc: 0x28A }] })!.log[0];
ok(dvWhere(pm) === 'CC 0x28A' && dvRow(pm) === 'CC 0x28A · heard', 'dPMR public: the channel code, "heard"');

console.log('── the VTS line and its info ──');
ok(dvsigVts(null) === 'DV · searching' && dvsigVts(parseDvsig({ type: 'dvsig', pin: 'dmr' })) === 'DMR · searching', 'searching (AUTO, or the pinned system)');
ok(dvsigVts(ids) === 'DMR · G4ABC Stuart', 'in a call: the talker alias');
ok(dvsigVts(pub) === 'DMR · heard · CC 1 · TS2', 'a public copy: DMR heard, colour code and slot — nobody named');
ok(dvsigVts(parseDvsig({ type: 'dvsig', ids: true, log: [{ ...dmr, open: false, alias: undefined }] })) === 'DV · last DMR 2345678 → TG 2350', 'between overs: the last heard');
ok(dvsigInfo(ids).includes('52.6235° N 1.1235° W') && dvsigInfo(pub).includes('IDs not shown'), 'the info row: the position; the public copy says why it names no one');
ok(dvsigLine(ids).startsWith('DMR · G4ABC Stuart · CC 1 · TS2'), 'one line for the app / R36S');
ok(dvPosition({ lat: -33.5, lon: 151.25 }) === '33.5000° S 151.2500° E', 'a position as a person reads it');

console.log('── the map: amateur identities only ──');
ok(dvsigMapPoints(ids).length === 1 && dvsigMapPoints(ids)[0].label === 'DMR G4ABC Stuart', 'an amateur identities copy: the point');
ok(dvsigMapPoints(pub).length === 0, 'a public copy: nothing');
ok(dvsigMapPoints({ ...ids, amateur: false }).length === 0, '★ the owner\'s own opted-in copy OFF the amateur bands: still nothing on the map');

console.log('── messages and constants ──');
ok(JSON.stringify(dvsigStatusMessage()) === '{"type":"dvsig_status"}' && JSON.stringify(dvsigPinMessage('nxdn')) === '{"type":"dvsig_pin","sys":"nxdn"}', 'the status and pin messages');
ok(DV_PINS.map((x) => x.id).join() === 'auto,dmr,dstar,ysf,nxdn,dpmr' && DVSIG_PASSBAND[1] - DVSIG_PASSBAND[0] === 12500, 'AUTO + five pins (the server\'s keys); the 12.5 kHz channel');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
