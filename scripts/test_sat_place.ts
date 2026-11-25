// test_sat_place.ts — ★★ beta94 SATELLITE IN LOCAL HARDWARE (tester Mac Ha, Stuart 2026-11-25): src/services/satPlace.ts.
//   §1 hwinfo → the cap: `dopplerNeed: "location"` read only beside a reason.
//   §2 the key: on THIS app's own engine with no position it is drawn and SELECTABLE (opens on the prompt); on a real
//      server with no location it follows the menu's rule (not drawn unless on); absent `doppler` = no key; iOS / a
//      VibeServer reached over the local path is never "own engine".
//   §3 which position: an entered one wins, else the phone's; the line says which.
//   §4 the words: Stuart's (needs your location, won't work without it, nearest town is enough) — never the owner's setup page.
//   §5 NOTHING PUBLISHED (source guards): the phone's position never reaches setLocationJson / the server location / the
//      server process / the shim's published state.
// Pure; no device, no network, no audio.   npx tsx scripts/test_sat_place.ts
import fs from 'node:fs';
import path from 'node:path';
import { satCapFrom } from '../src/services/satellites';
import { satOwnEngine, satNeedsPlace, satOffer, satPlaceLine, localPlaceChoose,
         SAT_PLACE_TITLE, SAT_PLACE_TEXT, SAT_PLACE_PRIVATE, SAT_PLACE_DENIED } from '../src/services/satPlace';
import { gridCentre, latLonToGrid } from '../src/services/grid';

let fail = 0, pass = 0;
const ok = (c: boolean, what: string) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };
const root = path.resolve(__dirname, '..');
const read = (p: string) => fs.readFileSync(path.join(root, p), 'utf8');

console.log('§1 hwinfo → the cap');
const NOLOC = 'This server has no location set — the owner sets its locator on the setup page';
const capNoLoc = satCapFrom({ doppler: 1, dopplerWhy: NOLOC, dopplerNeed: 'location' });
ok(!!capNoLoc && !capNoLoc.ok && capNoLoc.need === 'location' && capNoLoc.why === NOLOC, 'no location: greyed, need location, the server’s words kept');
const capNoData = satCapFrom({ doppler: 1, dopplerWhy: 'NO SAT DATA — not downloaded yet' });
ok(!!capNoData && !capNoData.ok && capNoData.need === undefined, 'no orbit data: no need (nothing the app can give)');
ok(satCapFrom({ doppler: 1, dopplerNeed: 'location' })?.ok === true && satCapFrom({ doppler: 1, dopplerNeed: 'location' })?.need === undefined,
   'a stray dopplerNeed with no reason is ignored (ready)');
ok(satCapFrom({ dopplerWhy: NOLOC, dopplerNeed: 'location' }) === null, 'no doppler: no cap at all');
ok(satCapFrom({ doppler: 1, dopplerWhy: NOLOC, dopplerNeed: 'elsewhere' })?.need === undefined, 'an unknown need is not read');

console.log('§2 the key');
const own = satOwnEngine({ os: 'android', isLocal: true, localHost: undefined, hasNative: true });
ok(own, 'Android Local Hardware (USB / rtl_tcp / SpyServer through this phone) = own engine');
ok(!satOwnEngine({ os: 'android', isLocal: true, localHost: '192.168.1.20', hasNative: true }), 'a VibeServer over the local path is NOT own engine');
ok(!satOwnEngine({ os: 'android', isLocal: false, hasNative: true }), 'a remote server is NOT own engine');
ok(!satOwnEngine({ os: 'ios', isLocal: true, hasNative: true }), 'iOS is not (no orbit-data supply in its engine)');
ok(!satOwnEngine({ os: 'android', isLocal: true, hasNative: false }), 'an app without the native call is not');

let o = satOffer({ cap: capNoLoc, on: false, isVibe: true, ownEngine: true });
ok(o.avail && o.prompt && o.why === undefined, 'own engine, no position: drawn, SELECTABLE (not dimmed), opens on the prompt');
ok(satNeedsPlace(capNoLoc, true) && !satNeedsPlace(capNoLoc, false), 'the prompt only on the own engine');
o = satOffer({ cap: capNoLoc, on: false, isVibe: true, ownEngine: false });
ok(!o.avail && !o.prompt, 'a real server with no location: not drawn (the owner’s to fix — Stuart 10-27 menu rule)');
o = satOffer({ cap: capNoLoc, on: true, isVibe: true, ownEngine: false });
ok(o.avail && !o.prompt, '…unless on, so it can be left');
o = satOffer({ cap: capNoData, on: false, isVibe: true, ownEngine: true });
ok(!o.avail, 'own engine, no orbit data (not a position): the menu rule');
o = satOffer({ cap: { ok: true, why: '' }, on: false, isVibe: true, ownEngine: true });
ok(o.avail && !o.prompt, 'ready: drawn, straight to the timetable');
ok(!satOffer({ cap: null, on: false, isVibe: true, ownEngine: true }).avail, 'no doppler: no key (no dead controls)');
ok(!satOffer({ cap: capNoLoc, on: false, isVibe: false, ownEngine: true }).avail, 'not a VibeServer protocol: no key');
// once the position is given the engine answers ready (test-sat-observer / test-server-sat 4a prove the server half)
ok(satOffer({ cap: satCapFrom({ doppler: 1 }), on: true, isVibe: true, ownEngine: true }).prompt === false, 'position given → the timetable, not the prompt');

console.log('§3 which position');
const gps = { lat: 52.2471, lon: -0.8917 };
const ent = { lat: 51.5, lon: -0.1, label: 'London' };
ok(localPlaceChoose(ent, gps)?.src === 'entered' && localPlaceChoose(ent, gps)?.label === 'London', 'an entered town wins over the phone’s location');
ok(localPlaceChoose(null, gps)?.src === 'gps', 'none entered: the phone’s location');
ok(localPlaceChoose(null, null) === null, 'neither: none (the prompt)');
const c = gridCentre(gps.lat, gps.lon);
ok(latLonToGrid(c.lat, c.lon) === latLonToGrid(gps.lat, gps.lon) && (c.lat !== gps.lat || c.lon !== gps.lon), `snapped to its grid-square centre (${latLonToGrid(gps.lat, gps.lon)} ${c.lat}, ${c.lon})`);
ok(satPlaceLine({ ...ent, src: 'entered' }, 'IO91wm') === 'Position: London · IO91wm (entered)', `entered: "${satPlaceLine({ ...ent, src: 'entered' }, 'IO91wm')}"`);
ok(satPlaceLine({ lat: 1, lon: 1, label: 'IO92nf', src: 'entered' }, 'IO92nf') === 'Position: IO92nf (entered)', 'an entered locator is not said twice');
ok(satPlaceLine({ ...gps, src: 'gps' }, 'IO92nf') === 'Position: this phone’s location · IO92nf', 'the phone’s location says so');
ok(satPlaceLine(null, '') === '', 'none: no line');

console.log('§4 the words');
const all = [SAT_PLACE_TITLE, SAT_PLACE_TEXT, SAT_PLACE_PRIVATE, SAT_PLACE_DENIED].join(' ');
ok(/need your location/i.test(SAT_PLACE_TEXT) && /Doppler/.test(SAT_PLACE_TEXT) && /cannot work/.test(SAT_PLACE_TEXT), 'satellites need your location, Doppler, will not work without it');
ok(/nearest town is enough/.test(SAT_PLACE_TEXT), 'the nearest town is enough');
ok(/never\s+sent to a server or published/.test(SAT_PLACE_PRIVATE), 'says it is never sent or published');
ok(!/owner|setup page|temporary/i.test(all), 'never the owner’s setup page, never "temporary"');
ok(!/\b(color|center|meter)\b/i.test(all), 'UK English');

console.log('§5 nothing published (source guards)');
const lp = read('src/services/localPlace.ts');
ok(!/setLocationJson|publishLocation|setManualServerLocation|lsv_server_location|LOC_KEY\b/.test(lp.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '')),
   'localPlace.ts: no setLocationJson / server location / its key');
ok(/LOCAL_PLACE_KEY = 'vibe\.localPlace'/.test(lp), 'its own storage key (not the server location’s)');
ok(/gridCentre\(p\.lat, p\.lon\)/.test(lp) && /setSatObserver\?\.\(c\.lat, c\.lon\)/.test(lp), 'grid-square centre before it reaches the engine');
const kt = read('android/app/src/main/java/com/vibesdr/app/VibeLocalSdrModule.kt');
const ktFn = kt.slice(kt.indexOf('fun setSatObserver('), kt.indexOf('/** Learned station bookmarks'));
ok(ktFn.length > 0 && !/tell\(|VibeServerLink|setLocationJson/.test(ktFn), 'the module: this process’s engine only — the server process is never told');
const shim = read('android/app/src/main/cpp/local_sdr_shim.cpp');
const fnAt = shim.indexOf('void LocalSdrShim::setSatObserver(');
const fn = shim.slice(fnAt, shim.indexOf('\n}\n', fnAt));
ok(fnAt > 0 && !/g_locJson|setReceiverPosition|setBandRegion|g_rxInUs|setLocationJson/.test(fn) && /g_satObs\.device\(/.test(fn),
   'the shim: setSatObserver touches the satellite engine only (no /location, DAB position, ITU region)');
const scr = read('src/screens/SDRScreen.tsx');
ok(!/setLocationJson/.test(scr.slice(scr.indexOf('const applyLocalPlace'), scr.indexOf('const satPlaceClear'))), 'the screen’s position handlers never publish');
ok(/if \(!satOwnHere\) return;\s+\/\/ ★ loopback only/.test(scr), 'the cap re-read is loopback only (own engine)');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
