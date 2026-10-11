// Portable Wi-Fi wording shared by the app and the web page (src/services/pocketConnection.ts).
import { pocketHere, pocketOnOwn, pocketOnNet, pocketLinkLine, pocketSwitchText } from '../src/services/pocketConnection';

let fails = 0;
const ok = (c: boolean, what: string) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };

console.log('test_pocket_connection');
ok(!pocketHere(null) && !pocketHere({ pocket: true, local: false }) && !pocketHere({ pocket: false, local: true }),
   'hidden unless a pocket box AND on its own network (never through the tunnel)');
ok(pocketHere({ pocket: true, local: true }), 'shown on a pocket box on its own network');

const home = { pocket: true, local: true, mode: 'client', ssid: 'Meow!', apSsid: 'Pocket-SDR',
               link: { signal: -78, tx: 13, rx: 72, retry: 9 }, advice: 'weak' };
ok(pocketLinkLine(home) === 'On “Meow!” · −78 dBm · 13 Mbit/s · 9 % retries', 'the link line: network, signal, rate, retries');
ok(pocketLinkLine({ ...home, link: { signal: -60, tx: null, rx: null, retry: null } }) === 'On “Meow!” · −60 dBm',
   'figures not reported are left out, never shown as 0');
ok(!pocketOnOwn(home) && pocketOnOwn({ mode: 'fallback-ap' }), 'which side is lit');
ok(pocketLinkLine({ mode: 'fallback-ap', apSsid: 'Pocket-SDR', hold: true }) === 'On its own Wi-Fi “Pocket-SDR” — held while the radio is in use',
   'on its own Wi-Fi, held');

const own = pocketSwitchText(home, 'own');
ok(/leave this session, join “Pocket-SDR” on this device, and start again/.test(own.body),
   "switching says you must leave this session, join the box's hotspot and start again (Stuart)");
ok(/leave “Meow!”/.test(own.body) && /30 minutes unused/.test(own.body), '…names the network it leaves, and when it goes back');
ok(/leave this session, join that network on this device, and start again/.test(pocketSwitchText(home, 'network').body),
   'switching back says the same the other way');

// ── a saved network chosen by name (Stuart, 2026-10-11: "Stuey3D's iPhone | Meow! | Own WiFi") ──
const phone = { rank: 1, ssid: "Stuey3D's iPhone" }, meow = { rank: 2, ssid: 'Meow!' };
const onMeow = { ...home, ssid: 'Meow!', nets: [phone, meow] };
ok(pocketOnNet(onMeow, meow) && !pocketOnNet(onMeow, phone) && !pocketOnNet({ mode: 'fallback-ap', ssid: 'Meow!' }, meow),
   'the lit network is the one the box is on, and none while it is on its own Wi-Fi');
const pick = pocketSwitchText(onMeow, phone);
ok(pick.title === 'Switch to “Stuey3D\'s iPhone”?', 'the title names the network');
ok(/look for “Stuey3D's iPhone” for the next 90 seconds/.test(pick.body) && /Personal Hotspot/.test(pick.body),
   'it says how long the box looks, and to turn the hotspot on now');
ok(/it stays on “Meow!”/.test(pick.body), '…and where it stays if it is not found');
ok(pocketLinkLine({ ...onMeow, trying: "Stuey3D's iPhone", tryLeft: 64 }) === 'Looking for “Stuey3D\'s iPhone” — 64 s left · turn its hotspot on now',
   'while looking, the line says so');
ok(/held while the radio is in use$/.test(pocketLinkLine({ ...onMeow, holdNet: 'Meow!' })), 'a chosen network is shown as held');

console.log(fails ? `\n${fails} failed` : '\nall passed');
process.exit(fails ? 1 : 0);
