// Portable Wi-Fi wording shared by the app and the web page (src/services/pocketConnection.ts).
import { pocketHere, pocketOnOwn, pocketLinkLine, pocketSwitchText } from '../src/services/pocketConnection';

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

console.log(fails ? `\n${fails} failed` : '\nall passed');
process.exit(fails ? 1 : 0);
