#!/usr/bin/env node
// ★★★ HOW LISTENERS REACH IT (Stuart, 2026-11-12: "make the options clear on the setup page"). Three choices over ONE
// key: Automatic is an empty dirPublicUrl (the tunnel, as ever), My own address is http://, My own domain with HTTPS is
// https://. dirReachOf() opens an existing server on the choice it already has; dirReachCheck() refuses an address that
// cannot work for the choice — wrong scheme, a LAN-only host, a path, or a radio's own port (those answer the machine
// only since beta70). Run against the page source, so it tests what ships.
//
//   node scripts/test-setup-dir-reach.mjs
import { readFileSync } from 'node:fs';

const src = readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const html = src.split('R"HTML(')[1].split(')HTML"')[0];
const grab = name => {
  const i = html.indexOf('function ' + name + '(');
  if (i < 0) throw new Error(name + ' is missing from the setup page');
  let d = 0;
  for (let k = html.indexOf('{', i); k < html.length; k++) {
    if (html[k] === '{') d++;
    else if (html[k] === '}' && --d === 0) return html.slice(i, k + 1);
  }
  throw new Error(name + ' never closes');
};
const { dirReachOf, dirReachCheck, dirRadioPorts } = new Function(
  grab('dirReachOf') + '\n' + grab('dirReachCheck') + '\n' + grab('dirRadioPorts') + '\nreturn { dirReachOf, dirReachCheck, dirRadioPorts };')();

let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); if (!c) fails++; };
const two = dirRadioPorts(48000, [{ serial: 'a' }, { serial: 'b' }]);
const chk = (reach, url) => dirReachCheck(reach, url, 48000, two);

console.log('setup page — how listeners reach it');
ok(dirReachOf('') === 'auto' && dirReachOf('  ') === 'auto' && dirReachOf(undefined) === 'auto', 'empty address → Automatic (the tunnel, as before)');
ok(dirReachOf('http://my.ddns.net:48000') === 'own', 'http:// → My own address');
ok(dirReachOf('https://sdr.example.org') === 'https' && dirReachOf('HTTPS://X.ORG') === 'https', 'https:// → My own domain with HTTPS');
ok(two.has(48001) && two.has(48002) && !two.has(48000), 'radio ports: base+1, base+2 — never the main port');
ok(dirRadioPorts(48000, [{ port: 50000 }, {}]).has(50000) && dirRadioPorts(48000, [{ port: 50000 }, {}]).has(48002),
   'a radio that names its own port is counted by it');

ok(chk('auto', 'nonsense') === '', 'Automatic never checks the (hidden) box');
ok(chk('own', 'http://my-sdr.ddns.net:48000') === '', 'own: http with the main port is fine');
ok(chk('own', 'http://81.2.69.160') === '', 'own: a public IP on port 80 is fine (router forwards 80 → 48000)');
ok(chk('https', 'https://sdr.example.org') === '' && chk('https', 'https://sdr.example.org/') === '', 'https: a domain at the root is fine (trailing slash too)');
ok(/My own domain with HTTPS/.test(chk('own', 'https://sdr.example.org')), 'own + https:// → points at the HTTPS choice');
ok(/needs an https/.test(chk('https', 'http://sdr.example.org')), 'https + http:// → refused, with the other choice named');
ok(/Type your/.test(chk('own', '')) && /Type your/.test(chk('https', ' ')), 'an empty address under a non-automatic choice is asked for');
ok(/not a web address/.test(chk('https', 'sdr.example.org')), 'no scheme → not a web address');
ok(/radio/.test(chk('own', 'http://my-sdr.ddns.net:48001')), '★★ a radio port (48001) is refused');
ok(/radio/.test(chk('https', 'https://sdr.example.org:48002')), '★★ …under https as well');
ok(/radio/.test(dirReachCheck('own', 'http://x.ddns.net:49001', 49000, dirRadioPorts(49000, [{}]))), 'the radio range follows a changed main port');
for (const lan of ['http://192.168.1.20:48000', 'http://10.0.0.5:48000', 'http://172.20.1.1:48000', 'http://pi.local:48000',
                   'http://localhost:48000', 'http://127.0.0.1:48000', 'http://[fd00::1]:48000'])
  ok(/own network/.test(chk('own', lan)), `LAN-only host refused: ${lan}`);
ok(chk('own', 'http://172.32.0.1:48000') === '', '172.32.x is public — not refused');
ok(/without a path/.test(chk('https', 'https://example.org/sdr')), 'a path is refused (the directory keeps the origin only)');
ok(/without a path/.test(chk('https', 'https://example.org/?x=1')), 'a query too');
process.exit(fails ? 1 : 0);
