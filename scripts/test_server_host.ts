// What may stand where `host` stands in the web client's URLs (web/client/src/origin.ts)
// run: node --no-warnings scripts/test_server_host.ts
import { isValidServerHost } from '../web/client/src/origin.ts';
let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (got === want) passes++; else { fails++; console.log(`✗ ${what}: got ${JSON.stringify(got)} want ${JSON.stringify(want)}`); }
};

// --- what the page really uses: location.host + BASE_PATH, or a typed LAN address on the dev page
eq('LAN ip:port', isValidServerHost('192.168.86.134:48000'), true);
eq('localhost', isValidServerHost('localhost:48000'), true);
eq('tunnel name', isValidServerHost('pi500.vibeserver.vibesdr.net'), true);
eq('front door radio', isValidServerHost('pi500.vibeserver.vibesdr.net/r/airspy'), true);
eq('radio id %-encoded', isValidServerHost('box.local:8073/r/RTL%20V4'), true);
eq('ipv6 + port', isValidServerHost('[fe80::1]:48000'), true);
eq('underscore LAN name', isValidServerHost('my_pi.lan'), true);

// --- refused: anything that moves the authority, or is not a host at all
eq('userinfo', isValidServerHost('trusted.org@evil.example'), false);
eq('scheme left in', isValidServerHost('javascript:alert(1)'), false);
eq('http scheme', isValidServerHost('http://a.b'), false);
eq('query', isValidServerHost('a.b?x=1'), false);
eq('fragment', isValidServerHost('a.b#x'), false);
eq('backslash', isValidServerHost('a.b\\@evil.example'), false);
eq('other path', isValidServerHost('a.b/admin'), false);
eq('nested /r/', isValidServerHost('a.b/r/x/y'), false);
eq('space', isValidServerHost('a.b c'), false);
eq('empty', isValidServerHost(''), false);
eq('non-string', isValidServerHost(42), false);
eq('undefined', isValidServerHost(undefined), false);
eq('too long', isValidServerHost('a'.repeat(400)), false);

console.log(`server host: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
