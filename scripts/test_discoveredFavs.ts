/**
 * The executable spec for src/services/discoveredFavs.ts — starring an mDNS-discovered server, and
 * repairing the favourites an earlier build saved without their protocol.
 *
 * Run: npx tsx scripts/test_discoveredFavs.ts
 */
import { favFromDiscovered, repairFromDiscovery, needsVibeProbe, tcpFavHttpBase }
  from '../src/services/discoveredFavs';
import type { TcpFav } from '../src/services/favourites';
import type { DiscoveredServer } from '../src/services/mdns';

let fails = 0;
const ok = (cond: boolean, what: string) => {
  if (cond) console.log('  ok   ' + what);
  else { console.log('  FAIL ' + what); fails++; }
};

const vibe: DiscoveredServer = { name: 'Pi 500', host: '192.168.1.50', port: 48000, proto: 'vibeserver', pin: true };
const rtl:  DiscoveredServer = { name: 'Shack rtl_tcp', host: '192.168.1.60', port: 1234, proto: 'rtltcp', pin: false };

console.log('starring keeps the protocol discovery found');
ok(favFromDiscovered(vibe).proto === 'vibeserver', 'a discovered VibeServer is saved as vibeserver');
ok(favFromDiscovered(rtl).proto === 'rtltcp', 'a discovered rtl_tcp is saved as rtltcp (explicitly)');
ok(favFromDiscovered(vibe).host === vibe.host && favFromDiscovered(vibe).port === 48000,
   'host and port survive');

console.log('repair from live discovery');
const broken: TcpFav = { name: 'Pi 500', host: '192.168.1.50', port: 48000 };              // what the old ☆ wrote
const genuine: TcpFav = { name: 'Shack', host: '192.168.1.60', port: 1234 };                 // legacy rtl_tcp
const sameBoxRtl: TcpFav = { name: 'Pi rtl', host: '192.168.1.50', port: 1234, proto: 'rtltcp' };
const spy: TcpFav = { name: 'Spy', host: '192.168.1.50', port: 48000, proto: 'spyserver' };
const fixed = repairFromDiscovery([broken, genuine, sameBoxRtl], [vibe, rtl]);
ok(fixed !== null, 'a change is reported');
ok(fixed?.[0].proto === 'vibeserver', 'the proto-less favourite on the advertised port becomes vibeserver');
ok(fixed?.[1].proto === undefined, 'a genuine rtl_tcp favourite is untouched');
ok(fixed?.[2].proto === 'rtltcp', 'an rtl_tcp on ANOTHER port of the same host is untouched');
ok(repairFromDiscovery([{ ...broken, proto: 'rtltcp' }], [vibe])?.[0].proto === 'vibeserver',
   'an explicit rtltcp on a port advertising _vibesdr._tcp is corrected');
ok(repairFromDiscovery([spy], [vibe]) === null, 'a SpyServer favourite is never rewritten');
ok(repairFromDiscovery([genuine], [rtl]) === null, 'nothing to do ⇒ null (no storage write)');
ok(repairFromDiscovery([broken], []) === null, 'no discovery ⇒ null');
ok(repairFromDiscovery([{ ...broken, host: '192.168.1.50'.toUpperCase() }], [vibe]) !== null,
   'host comparison ignores case');

console.log('probe-on-connect gating');
ok(needsVibeProbe(broken), 'a proto-less favourite off the rtl_tcp port is probed');
ok(!needsVibeProbe(genuine), 'a proto-less favourite on 1234 is not probed');
ok(!needsVibeProbe({ ...broken, proto: 'rtltcp' }), 'an explicit rtltcp is not probed');
ok(!needsVibeProbe({ ...broken, proto: 'vibeserver' }), 'an explicit vibeserver is not probed');
ok(tcpFavHttpBase(broken) === 'http://192.168.1.50:48000', 'the VibeServer address is http, not the vibeserver:// key');

if (fails) { console.log(`\n${fails} FAILED`); process.exit(1); }
console.log('\nall passed');
