#!/usr/bin/env node
// ★★★ A TYPED WI-FI NAME IS SAVED AS IT IS ON THE AIR (Stuart's Portable road test, 2026-10-14: the box never joined his
// iPhone's hotspot). An iPhone's hotspot is "Stuart’s iPhone" with a CURLY apostrophe; typed on a Mac it is straight,
// and NetworkManager matches byte for byte. The setup page's wifiOnAir() swaps a typed name for the ONE scanned network
// that means the same — and never guesses between two. Run against the page source, so it tests what ships.
//
//   node scripts/test-setup-wifi-name.mjs
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
const make = new Function('WIFI', grab('wifiKey') + '\n' + grab('wifiOnAir') + '\nreturn wifiOnAir;');
const CURLY = 'Stuart’s iPhone';
const one = make({ scan: [{ ssid: CURLY }, { ssid: 'Meow!' }] });
const two = make({ scan: [{ ssid: CURLY }, { ssid: 'Stuart‘s iPhone' }] });
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); if (!c) fails++; };
console.log('setup page — a typed Wi-Fi name is saved as it is on the air');
ok(one("Stuart's iPhone") === CURLY, "typed with a straight apostrophe: saved as the iPhone's curly one");
ok(one("stuart's iphone ") === CURLY, '…capitals and a trailing space aside');
ok(one('Meow!') === 'Meow!', 'an exact name is kept');
ok(one('Hidden net') === 'Hidden net', 'a name not on the air at all is kept as typed (a phone that is off now)');
ok(two("Stuart's iPhone") === "Stuart's iPhone", 'two networks could be it: neither is guessed');
ok(make(null)('x') === 'x', 'no scan yet: kept as typed');
process.exit(fails ? 1 : 0);
