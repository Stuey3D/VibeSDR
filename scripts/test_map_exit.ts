// test_map_exit.ts — beta96 (Stuart 11-27, Almere's ADS-B radio from the Mac app: "right now i think if i were to go to almere's
// adsb map on the app i'd be stuck and need to force quit the app to exit the server"; recorded: the map had CLOSE MAP and no
// Servers chip, and CLOSE MAP dropped him on a 1090 MHz waterfall). THE WAY OFF A MAP (src/utils/mapExit.ts):
//   • the pure rules — which radios are map-locked, the only mode before hwinfo, what Back does on a map, where the Servers tab
//     sits, when "Other radios on this server" is offered; the AIR & SEA map is up on a locked radio whatever the view flags;
//   • the wiring (source checks): the Servers chip is drawn INSIDE every map's Modal from SDRScreen's one props object; Back
//     (Android's, a pad's, a keyboard's) goes through mapBackAction — the menu closes, a locked radio leaves the server, a
//     locked EXIT offers to leave the server; a map-locked radio's EXIT is LEAVE SERVER and leaves the server; leaving is never
//     behind the ask-before-tuning lock; the radio's /vibeserver.json onlyMode is read before the connect.
//   npx tsx scripts/test_map_exit.ts            (MAPEXIT_ROOT=<a tree> reads the sources from there — the fail-before run)
import { readFileSync } from 'node:fs';
import path from 'node:path';
import { mapLockedMode, onlyModeBeforeHwinfo, mapBackAction, mapChipTop, MAP_CHIP_MENU_H, otherRadiosDoor, doorOf, mapExitKey, MAP_LEAVE_OFFER } from '../src/utils/mapExit';
import { shownMapOf, type ShownMapState } from '../src/utils/mapPage';

let pass = 0, fail = 0;
const ok = (c: unknown, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const ROOT = process.env.MAPEXIT_ROOT || path.resolve(new URL('..', import.meta.url).pathname);
const rd = (f: string) => { try { return readFileSync(path.join(ROOT, f), 'utf8'); } catch { return ''; } };

console.log('── which radios are map-locked ──');
for (const m of ['adsb', 'ais', 'ndb', 'sondes', 'vormap', 'avdata']) ok(mapLockedMode(m) === m, `onlyMode "${m}": map-locked (its screen is the AIR & SEA map)`);
for (const m of ['', 'dab', 'ism', 'wfm', null, undefined]) ok(mapLockedMode(m as string) === '', `onlyMode ${JSON.stringify(m)}: not map-locked (a box, or nothing)`);
ok(onlyModeBeforeHwinfo(null, '', 'adsb') === 'adsb', 'before hwinfo: the radio\'s /vibeserver.json word (go straight to the map)');
ok(onlyModeBeforeHwinfo('', '', 'adsb') === '', 'hwinfo says "" (the signed-in admin): the radio screen back');
ok(onlyModeBeforeHwinfo('adsb', 'adsb', null) === 'adsb', 'hwinfo says adsb: locked');
ok(mapExitKey(true).label === 'LEAVE SERVER' && mapExitKey(true).leavesServer && !mapExitKey(false).leavesServer, 'a locked map\'s EXIT is LEAVE SERVER');

console.log('── the AIR & SEA map is up on a locked radio, whatever the view flags say ──');
const none: ShownMapState = { appleTv: false, adsbView: false, ndbView: false, aisView: false, aprsMap: false, dscMap: false, navMap: false,
  ismGallery: false, ismMap: false, ismOn: false, vmMap: false, vmOn: false, sdMap: false, sdOn: false, avMap: false, avOn: false, avFocusDatalink: true, picked: null };
ok(shownMapOf({ ...none, lockedMap: true }) === 'airsea', '★ locked, no view open (before the first report; after a CLOSE): the map — never the radio screen');
ok(shownMapOf(none) === null, 'not locked, nothing open: the radio screen, as before');
ok(shownMapOf({ ...none, lockedMap: true, appleTv: true }) === null, 'Apple TV: no WebView — its box stands in');

console.log('── Back on a map ──');
ok(mapBackAction({ chipOpen: true, locked: true, exitLocked: true, canLeave: true }) === 'collapse', 'the Servers menu open: Back closes it first');
ok(mapBackAction({ chipOpen: false, locked: true, exitLocked: false, canLeave: true }) === 'leave', '★ a map-locked radio: Back LEAVES THE SERVER');
ok(mapBackAction({ chipOpen: false, locked: false, exitLocked: true, canLeave: true }) === 'offer', '★ EXIT locked by the ask lock / a scan: Back OFFERS to leave the server (it did nothing — a force quit)');
ok(mapBackAction({ chipOpen: false, locked: false, exitLocked: false, canLeave: true }) === 'close', 'otherwise: the map\'s own EXIT, as before');
ok(mapBackAction({ chipOpen: false, locked: true, exitLocked: false, canLeave: false }) === 'close', 'no way to leave handed in: the map\'s own close');
ok(/does not change the radio/.test(MAP_LEAVE_OFFER.body('Ask in the chat before leaving')) && MAP_LEAVE_OFFER.leave === 'Leave this server', 'the offer says leaving does not move the radio');

console.log('── where the Servers tab sits on a map ──');
for (const [name, h, top, mapTop, strip] of [['iPhone 17 Pro portrait', 874, 62, 112, true], ['iPhone landscape', 402, 0, 50, true], ['iPad', 1133, 24, 74, false], ['before the bar reports', 874, 62, 0, false]] as const) {
  const y = mapChipTop(h, top, mapTop, strip);
  const floor = (mapTop > 0 ? mapTop : top + 48) + (strip ? 40 : 0);
  ok(y >= floor && (y + MAP_CHIP_MENU_H <= h || y === floor + 10), `${name}: y ${y} — under the top bar${strip ? ' and the strip' : ''}, its menu on screen`);
}

console.log('── "Other radios on this server" ──');
ok(otherRadiosDoor({ baseUrl: 'https://x.example', doorRadios: 2, siblingRadios: 0 })?.doorBase === 'https://x.example', 'a door of two: offered, back to that door');
ok(otherRadiosDoor({ baseUrl: 'https://x.example', doorRadios: 1, siblingRadios: 0 }) === null, 'a door of one: no row (no dead row)');
{ const d = otherRadiosDoor({ baseUrl: 'https://x.example/r/adsb%201', doorRadios: 0, siblingRadios: 3 });
  ok(d?.doorBase === 'https://x.example' && d.radioId === 'adsb 1', 'an address naming its radio (the server list\'s per-radio row): the server behind it, starting on this radio'); }
ok(otherRadiosDoor({ baseUrl: 'https://x.example/r/a', doorRadios: 0, siblingRadios: 1 }) === null, '…but only when that server has another radio');
ok(doorOf('https://x.example/r/a') === 'https://x.example' && doorOf('https://x.example') === '', 'doorOf');

console.log('── MapOverlay ──');
const mo = rd('src/components/MapOverlay.tsx');
const modal = mo.slice(mo.indexOf('<Modal'), mo.indexOf('</Modal>'));
// ★ beta97 (b97-integrate): the MAP CARDS replace the beta96 chip — the way off is the SERVERS card (LEAVE SERVER always works,
//   MapChrome servers), drawn in the map's Modal; the old ServersChip is not drawn over the cards (two ways off, one hidden).
ok(!/<ServersChip/.test(modal) && /servers=\{p\.cards\?\.servers \?\? null\}/.test(mo), '★ the way off on the map is the SERVERS card (inside the map\'s Modal), not the old chip');
ok(/onRequestClose=\{onBackPress\}/.test(mo), '★ Android\'s Back on a map is onBackPress');
ok(/useOverlay\(visible, \{ name: 'map', onBack: \(\) => backRef\.current\(\) \}\)/.test(mo) && /backRef\.current = onBackPress;/.test(mo), '…a pad\'s and a keyboard\'s Back too (one rule — it used to bypass the dial lock\'s guard)');
ok(/mapBackAction\(\{ chipOpen, locked: leaveServer, exitLocked, canLeave: !!servers \}\)/.test(mo), 'onBackPress asks mapBackAction');
ok(/case 'offer':[\s\S]{0,200}appAlert\(MAP_LEAVE_OFFER\.title[\s\S]{0,200}onPress: leave/.test(mo), 'the offer\'s "Leave this server" leaves');
ok(/const leave = \(\) => \{ servers\?\.onBack\(\); \};/.test(mo), 'leaving is the chip\'s own Back to server list (the same onBack)');
ok(/exit=\{p\.leaveServer \? \{ label: 'LEAVE SERVER',[^}]*locked: false, onPress: p\.onExit \}/.test(mo) && /leaveServer=\{leaveServer\}/.test(mo), 'a locked map\'s ✕ reads LEAVE SERVER from the first paint (before the only mode\'s CLOSE MAP; never dial-locked)');
ok(/if \(d === 'close'\) return closeGuarded\(\);/.test(mo), 'the page\'s EXIT keeps its dial-lock guard (a locked EXIT that would leave the MODE is refused with the reason)');

console.log('── SDRScreen ──');
const sc = rd('src/screens/SDRScreen.tsx');
ok(/const serversChipProps = \{/.test(sc) && /\{\.\.\.serversChipProps\}/.test(sc), '★ ONE props object for the waterfall\'s chip and the maps\' chips');
ok((sc.match(/servers=\{serversChipProps\}/g) || []).length >= 2, '★ every MapOverlay is given it (the AIR & SEA / server maps and the on-device spots map)');
ok(/leaveServer=\{!!mapLockedHere && shownMap === 'airsea'\}/.test(sc), 'a map-locked radio\'s map leaves the server');
ok(/onClose=\{\(\) => \(mapLockedHere \? onBackToPicker\(\) : shownMap === 'ais' \? exitAis\(\)/.test(sc), '★ its EXIT (LEAVE SERVER) leaves the server — never a CLOSE MAP onto a waterfall');
ok(/onClose=\{\(\) => \(mapLockedHere \? onBackToPicker\(\) : shownMap === 'ais' \? setAisViewOpen\(false\)/.test(sc), '…and a crash in it leaves the server too (no radio screen to put it away onto)');
ok(/const exitLabel = mapLockedHere \? 'LEAVE SERVER'/.test(sc), 'the pushed words say LEAVE SERVER (and do not match /^EXIT / — leaving is never dial-locked)');
ok(/lockedMap: !!mapLockedHere/.test(sc) && /const airSeaShown = \(!!mapLockedHere \|\|/.test(sc), 'the map is up the moment the lock is known');
ok(/if \(mapLockedHere === 'adsb' && !adsbViewOpen\) setAdsbViewOpen\(true\);/.test(sc), '…with the mode\'s own view (its aircraft flow), put back if anything puts it away');
ok(/mapLockedMode\(onlyModeBeforeHwinfo\(hwOnlyMode, hwOnlyMode \?\? '', preOnly\)\)/.test(sc), 'locked = the server\'s onlyMode (hwinfo; before it, the radio\'s /vibeserver.json)');
ok(/if \(occ\?\.onlyMode !== undefined\) setPreOnly\(occ\.onlyMode\);/.test(sc) && /const occR = await fetchOccupancy\(radioBaseUrl\(baseUrl, id\), 1500\)/.test(sc), '★ read BEFORE the connect (an address that is the radio; a radio chosen at the door) — no radio screen flash');
{ const b = sc.slice(sc.indexOf('const onBackToPicker = useCallback'), sc.indexOf('const onBackToPicker = useCallback') + 900);
  ok(b.length > 100 && !/askGate|askLocked|scanLock/.test(b), '★ leaving the server is never behind the ask-before-tuning lock (leaving is not tuning)'); }
ok(/onOtherRadios,/.test(sc) && /doorVisit\.stash\(\{ base: otherDoor\.doorBase/.test(sc), 'the Servers menu\'s Other radios: the door\'s radio list, this visit carried');

console.log('── ServersChip ──');
const ch = rd('src/components/ServersChip.tsx');
ok(/accessibilityLabel="Other radios on this server"/.test(ch) && /if \(onOtherRadios\) add\('radios', onOtherRadiosRow\);/.test(ch), 'the row, in the pad order, only when handed (no dead row)');
ok(/ref=\{screenNavRef\(navId, /.test(ch), 'its pad id is a prop (the map\'s copy never unregisters the waterfall\'s)');

console.log('── sdrTypes ──');
ok(/onlyMode:  typeof j\.onlyMode === 'string'/.test(rd('src/services/sdrTypes.ts')), 'fetchOccupancy reads the radio\'s onlyMode');

console.log(`test_map_exit: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
