/**
 * Where the receiver is, for the spots map and spot distances (src/services/receiverPlace.ts).
 *
 * The rule: the RADIO's own position (a VibeServer's GET /location, a Kiwi's gps) first; this phone's
 * GPS or a picked town only when the radio cannot say; SET LOCATION only when neither knows.
 * The case that broke (Stuart, 2026-11-25): his RSP1A server answers /location with
 * {"name":"SDRplay RSP1A","lat":52.3125,"lon":-0.875,"grid":"IO92nh"} and the map still offered SET LOCATION.
 *
 * Run: npx tsx scripts/test_receiver_place.ts
 */
import { validPos, serverPosFromLocation, receiverPos, shouldAskPhoneForPlace } from '../src/services/receiverPlace';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
function near(what: string, got: { lat: number; lon: number } | null, lat: number, lon: number) {
  if (got && Math.abs(got.lat - lat) < 1e-6 && Math.abs(got.lon - lon) < 1e-6) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${lat},${lon}`);
}

// ── /location replies → the server's position ─────────────────────────────────────────────────
const rsp = { name: 'SDRplay RSP1A', lat: 52.3125, lon: -0.875, grid: 'IO92nh' };   // the live reply, verbatim
eq('Stuart\'s RSP1A /location', serverPosFromLocation(rsp), { lat: 52.3125, lon: -0.875 });
near('locator only (a phone server set up with a grid)', serverPosFromLocation({ name: 'Moto', grid: 'IO92nh' }),
     52.3125, -0.875);
near('older servers call it locator', serverPosFromLocation({ locator: 'IO92' }), 52.5, -1);
eq('name only = owner published no position', serverPosFromLocation({ name: 'VibeSDR' }), null);
eq('empty reply', serverPosFromLocation({}), null);
eq('not JSON object', serverPosFromLocation('<!doctype html>'), null);
eq('null', serverPosFromLocation(null), null);
eq('0,0 is "not set", and no grid', serverPosFromLocation({ lat: 0, lon: 0 }), null);
near('0,0 with a grid → the grid', serverPosFromLocation({ lat: 0, lon: 0, grid: 'IO92nh' }), 52.3125, -0.875);
eq('lat/lon as strings are not trusted', serverPosFromLocation({ lat: '52.3', lon: '-0.8' }), null);
eq('out of range lat', serverPosFromLocation({ lat: 152.3, lon: -0.8 }), null);
eq('NaN', serverPosFromLocation({ lat: NaN, lon: 1 }), null);
eq('garbage grid', serverPosFromLocation({ grid: 'ZZ99' }), null);
eq('RR73 is never a grid', serverPosFromLocation({ grid: 'RR73' }), null);

// ── precedence ────────────────────────────────────────────────────────────────────────────────
const server = { lat: 52.3125, lon: -0.875 };
const phone = { lat: 37.33, lon: -122.03 };                        // the simulator's default Cupertino
eq('server wins over the phone', receiverPos(server, phone), server);
eq('server, phone unknown', receiverPos(server, null), server);
eq('server unknown → phone', receiverPos(null, phone), phone);
eq('server not answered yet → phone', receiverPos(undefined, phone), phone);
eq('neither → null (SET LOCATION)', receiverPos(null, null), null);
eq('server 0,0 is not a position', receiverPos({ lat: 0, lon: 0 }, phone), phone);
eq('phone 0,0 is not a position', receiverPos(null, { lat: 0, lon: 0 }), null);
eq('validPos rejects null', validPos(null), false);
eq('validPos 0,0', validPos({ lat: 0, lon: 0 }), false);
eq('validPos on the equator', validPos({ lat: 0, lon: 10 }), true);

// ── is this phone asked for its position (an OS prompt)? ──────────────────────────────────────
eq('network server (UberSDR / Kiwi): never', shouldAskPhoneForPlace(false, false, undefined), false);
eq('Local Hardware / OTG: always', shouldAskPhoneForPlace(true, false, undefined), true);
eq('VibeServer, /location not answered: wait', shouldAskPhoneForPlace(true, true, undefined), false);
eq('VibeServer with a position: never', shouldAskPhoneForPlace(true, true, server), false);
eq('VibeServer without one: ask', shouldAskPhoneForPlace(true, true, null), true);

console.log(`${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
