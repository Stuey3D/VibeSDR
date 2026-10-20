// ★★★ GitHub #52 — per-server certificate pins: the key, the fingerprint, what a certificate says, which prompt.
// See src/utils/tlsPin.ts. The native enforcement has its own tests: scripts/test-tls-pin-apple.sh / -android.sh.
import { pinKey, normaliseFingerprint, formatFingerprint, fingerprintLines, parseCertificate, certNamesHost,
         decideTls, tlsTarget } from '../src/utils/tlsPin.ts';

let bad = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) !== JSON.stringify(want)) { bad++; console.log('FAIL', what, '→', JSON.stringify(got), 'want', JSON.stringify(want)); }
};

// OWRX+-style: `openssl req -x509 -days 3650 -subj /CN=openwebrx` — no SAN, CA:TRUE, self-signed.
const OWRX = 'MIIC6DCCAdCgAwIBAgIUalIc9qJwk70pnXgSava8iEeyH/owDQYJKoZIhvcNAQELBQAwFDESMBAGA1UEAwwJb3BlbndlYnJ4MB4XDTI2MTAyMDA2MzA1NloXDTM2MTAxNzA2MzA1NlowFDESMBAGA1UEAwwJb3BlbndlYnJ4MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEApyJIdn/791+7YnnzvZDSVjLhXQ/ylIHXHFjPVthUmT2XIGSFEJnyA7pqSoxJLlK+HB2xSVbCEFyamNQAFIE/teUdXbDlarPiCeFFJpSftNSABumqbefnFGlTpx37Mt0t1uosYg++RjbEsK0GiQKbfiTieUdPxEGWbO0czv2u1pujn1cjGLIQ94bvom3Nm9JLR4IdzfGtGVHe0zJBafiGevtq4I8eIUy5poHWx/o7dLzVKAb+mHvfkWOwlEp2B1E+GjzXfaPyHBYFRjWt50jjOdahfR7apXXotXVCebPknDyTt2L1dAgmg+y+y+3W0l7KtV6Hx0/58HMT09AWs+0AFwIDAQABozIwMDAdBgNVHQ4EFgQUSbyBzipVk0Ki0mB8Scnm7E0OFDowDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAY9zJ+oz928XI89MnUNKG79gHbP0xeldjPAHpeaJ/0yUlcYTJ4icUzT/P3Yb6bqofvx01GoAV+riTAQUXCf3Pragqxgr75nVQHcBeSthDhKWn2na2k5mUjjmfSRV8xYzh6iSs9EFg5T7ot7gMD/RbvIp5n3Z3q+iOFfyM7zS8MCswmsPzHLmXTihqUn/4xG1RtWJBVv7ygZchlSiNvz6w5Ip4C04CJ67kpMol4+sAmodXqckdqfTig7AInvBEifKLx8SdNK4s/2EbhpWuAMRCg59g1xDHsP/ZghT/2FTND7mHi4AKUuTo3j5KuR40vGv/u34ImOZ95KLDMmrnBWhFcQ==';
const OWRX_FP = 'e047362cf01981e1afb8f5129427eed2cc42d242d1c6d5bc5b64df3f78225875';
// P-256, CN=sdr.example.org, SAN DNS:sdr.example.org, DNS:*.example.net, IP:192.168.1.20, IP:2001:db8::1.
const SAN = 'MIIByTCCAW+gAwIBAgIUL1Jm8N34g5fdwC6zL9JzZUdHF14wCgYIKoZIzj0EAwIwKTEYMBYGA1UEAwwPc2RyLmV4YW1wbGUub3JnMQ0wCwYDVQQKDARUZXN0MB4XDTI2MTAyMDA2NDIyNFoXDTI2MTExOTA2NDIyNFowKTEYMBYGA1UEAwwPc2RyLmV4YW1wbGUub3JnMQ0wCwYDVQQKDARUZXN0MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEeZUtH0cDpBYVELl0+yB8ltRoaKNSI3hd3FBqHyHKhcCjG5zGxQGnmf+rhdw8qcE5UmRSArOuOFa7B24B4SSjQ6N1MHMwHQYDVR0OBBYEFIDawDkfURD1v+kWWTdAFCvmSAB6MA8GA1UdEwEB/wQFMAMBAf8wQQYDVR0RBDowOIIPc2RyLmV4YW1wbGUub3Jngg0qLmV4YW1wbGUubmV0hwTAqAEUhxAgAQ24AAAAAAAAAAAAAAABMAoGCCqGSM49BAMCA0gAMEUCIQCo6Et2jyl3tECGUY2DkSvHuumOk+iyHm8ZabvYiYjvRAIgFywRSGxB3RIWW1FU5My7pXKI27vh5lEaGUSNfJL4adA=';
const FP2 = '49bfbf3fa313377d37b5d1a2064ca08618953cc4ee28b0c23543a955279ab57d';

// keys — the native halves build the same string
eq('key lower-case + trailing dot', pinKey('SDR.Example.org.', 8073), 'sdr.example.org:8073');
eq('key IPv6 brackets', pinKey('[2001:DB8::1]', 8073), '2001:db8::1:8073');
eq('key plain', pinKey('192.168.1.20', 443), '192.168.1.20:443');

// fingerprints
eq('normalise from colons', normaliseFingerprint('E0:47:36:2C:F0:19:81:E1:AF:B8:F5:12:94:27:EE:D2:CC:42:D2:42:D1:C6:D5:BC:5B:64:DF:3F:78:22:58:75'), OWRX_FP);
eq('normalise rejects short', normaliseFingerprint('abcd'), '');
eq('format like openssl', formatFingerprint(OWRX_FP), 'E0:47:36:2C:F0:19:81:E1:AF:B8:F5:12:94:27:EE:D2:CC:42:D2:42:D1:C6:D5:BC:5B:64:DF:3F:78:22:58:75');
eq('two lines of 16 bytes', fingerprintLines(OWRX_FP), ['E0:47:36:2C:F0:19:81:E1:AF:B8:F5:12:94:27:EE:D2', 'CC:42:D2:42:D1:C6:D5:BC:5B:64:DF:3F:78:22:58:75']);

// what the certificate says about itself
const c1 = parseCertificate(OWRX)!;
eq('owrx CN', c1.subjectCN, 'openwebrx');
eq('owrx self-signed', c1.selfSigned, true);
eq('owrx no SAN', c1.names, []);
eq('owrx notBefore', c1.notBefore?.toISOString(), '2026-10-20T06:30:56.000Z');
eq('owrx notAfter', c1.notAfter?.toISOString(), '2036-10-17T06:30:56.000Z');
eq('owrx does not name the host', certNamesHost(c1, 'sdr.example.org'), false);
eq('owrx names "openwebrx" via CN', certNamesHost(c1, 'openwebrx'), true);
const c2 = parseCertificate(SAN)!;
eq('SAN list', c2.names, ['sdr.example.org', '*.example.net', '192.168.1.20', '2001:db8::1']);
eq('SAN CN', c2.subjectCN, 'sdr.example.org');
eq('SAN names host', certNamesHost(c2, 'SDR.example.org'), true);
eq('SAN wildcard one label', certNamesHost(c2, 'rx.example.net'), true);
eq('SAN wildcard not two labels', certNamesHost(c2, 'a.b.example.net'), false);
eq('SAN IP', certNamesHost(c2, '192.168.1.20'), true);
eq('SAN IPv6', certNamesHost(c2, '[2001:db8::1]'), true);
eq('garbage → null', parseCertificate('bm90IGEgY2VydA=='), null);

// which prompt
const untrusted = { systemTrusted: false, reason: 'untrusted', sha256: OWRX_FP, pinned: null };
eq('first use → offer', decideTls(untrusted), { kind: 'offer', sha256: OWRX_FP });
eq('pinned and matching → none', decideTls(untrusted, OWRX_FP), { kind: 'none' });
eq('pinned (from native) matching → none', decideTls({ ...untrusted, pinned: OWRX_FP }), { kind: 'none' });
eq('pinned, different → changed', decideTls(untrusted, FP2), { kind: 'changed', sha256: OWRX_FP, previous: FP2 });
eq('pin case/colons ignored', decideTls(untrusted, formatFingerprint(OWRX_FP)), { kind: 'none' });
eq('expired → refuse even with the pin', decideTls({ ...untrusted, reason: 'expired' }, OWRX_FP), { kind: 'refuse', why: 'expired', sha256: OWRX_FP });
eq('public cert, wrong name → refuse', decideTls({ ...untrusted, reason: 'hostname' }), { kind: 'refuse', why: 'hostname', sha256: OWRX_FP });
eq('system-trusted → none', decideTls({ systemTrusted: true, reason: 'ok', sha256: OWRX_FP }, FP2), { kind: 'none' });
eq('unreachable → none', decideTls({ systemTrusted: false, reason: 'unreachable', sha256: '' }), { kind: 'none' });
eq('null → none', decideTls(null), { kind: 'none' });

// which addresses can be asked about
eq('https with port', tlsTarget('https://sdr.example.org:8073/owrx/'), { host: 'sdr.example.org', port: 8073 });
eq('wss default port', tlsTarget('wss://sdr.example.org/ws/'), { host: 'sdr.example.org', port: 443 });
eq('IPv6', tlsTarget('https://[2001:db8::1]:8073'), { host: '2001:db8::1', port: 8073 });
eq('plain http → null', tlsTarget('http://sdr.example.org:8073'), null);
eq('bare host → null', tlsTarget('sdr.example.org:8073'), null);

if (bad) { console.log(`${bad} FAILED`); process.exit(1); }
console.log('tls pin: all passed');
