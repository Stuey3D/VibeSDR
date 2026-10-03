// Untrusted URLs and the map page's helpers (src/utils/safeUrl, src/utils/pageSafety)
// run: node --no-warnings scripts/test_safe_url.ts
import { parseUrlStrict, safeUrl, wsOriginFor, HTTP_SCHEMES, SERVER_SCHEMES } from '../src/utils/safeUrl.ts';
import { jsStringLiteral, isAllowedHostFetchPath } from '../src/utils/pageSafety.ts';
let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) passes++; else { fails++; console.log(`✗ ${what}: got ${JSON.stringify(got)} want ${JSON.stringify(want)}`); }
};

// --- accepted
eq('https host', safeUrl('https://sdr.example.org/', HTTP_SCHEMES), 'https://sdr.example.org');
eq('http LAN + port + path', safeUrl('http://192.168.1.20:8073/r/a', HTTP_SCHEMES), 'http://192.168.1.20:8073/r/a');
eq('ipv6 + port', parseUrlStrict('http://[fe80::1]:80/', HTTP_SCHEMES)?.host, '[fe80::1]');
eq('ws origin', wsOriginFor('ws://127.0.0.1:4321/x'), 'ws://127.0.0.1:4321');
eq('wss origin', wsOriginFor('wss://Sdr.Example.org'), 'wss://sdr.example.org');
eq('query kept', safeUrl('https://a.b/c?d=e&f=g', HTTP_SCHEMES), 'https://a.b/c?d=e&f=g');
eq('server scheme ws', !!parseUrlStrict('wss://a.b', SERVER_SCHEMES), true);

// --- refused
eq('javascript:', safeUrl('javascript:alert(1)', HTTP_SCHEMES), '');
eq('tel:', safeUrl('tel:123', HTTP_SCHEMES), '');
eq('vibesdr:', safeUrl('vibesdr://connect?url=x', HTTP_SCHEMES), '');
eq('ws not http', safeUrl('ws://a.b', HTTP_SCHEMES), '');
eq('quote', safeUrl("https://a.b/'+alert(1)+'", HTTP_SCHEMES), '');
eq('double quote', safeUrl('https://a.b/"x', HTTP_SCHEMES), '');
eq('backslash', safeUrl('https://a.b\\@evil.c', HTTP_SCHEMES), '');
eq('userinfo', safeUrl('https://trusted.org@evil.c', HTTP_SCHEMES), '');
eq('space', safeUrl('https://a.b/ x', HTTP_SCHEMES), '');
eq('newline', safeUrl('https://a.b/\nx', HTTP_SCHEMES), '');
eq('angle', safeUrl('https://a.b/<script>', HTTP_SCHEMES), '');
eq('no host', safeUrl('https:///x', HTTP_SCHEMES), '');
eq('port 0', safeUrl('https://a.b:0', HTTP_SCHEMES), '');
eq('port 70000', safeUrl('https://a.b:70000', HTTP_SCHEMES), '');
eq('bad host chars', safeUrl('https://a_b!.c', HTTP_SCHEMES), '');
eq('non-string', safeUrl(42, HTTP_SCHEMES), '');
eq('too long', safeUrl('https://a.b/' + 'x'.repeat(3000), HTTP_SCHEMES), '');
eq('ws origin of http', wsOriginFor('http://a.b'), '');

// --- the map page's string literal
const lit = jsStringLiteral("x';alert(1);//</script> ");
eq('literal parses back', Function('return ' + lit)(), "x';alert(1);//</script> ");
eq('literal has no raw <', lit.includes('<'), false);
eq('literal has no raw U+2028', lit.includes(' '), false);
eq('literal of non-string', jsStringLiteral({}), '""');

// --- hostFetch paths
eq('hfdl aircraft', isAllowedHostFetchPath('/addon/hfdl/aircraft'), true);
eq('hfdl groundstations', isAllowedHostFetchPath('/addon/hfdl/groundstations'), true);
eq('description', isAllowedHostFetchPath('/api/description'), true);
eq('other api', isAllowedHostFetchPath('/api/admin'), false);
eq('host change @', isAllowedHostFetchPath('/addon/hfdl/@evil.c'), false);
eq('double slash', isAllowedHostFetchPath('/addon/hfdl//evil.c'), false);
eq('backslash', isAllowedHostFetchPath('/addon/hfdl/\\evil'), false);
eq('dotdot', isAllowedHostFetchPath('/addon/hfdl/../admin'), false);
eq('no leading slash', isAllowedHostFetchPath('@evil.c/addon/hfdl/'), false);
eq('non-string', isAllowedHostFetchPath(null), false);

console.log(`safe url: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
