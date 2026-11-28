// test-setup-order.mjs — b96-bgsplit: STUART'S ORDER of the Setup page's cards (11-26), the server tab and every radio tab.
// The page lays the cards out in DOM order at every width (no CSS `order`, no grid areas — checked here), so the DOM order IS
// the desktop and the phone order. And no script moves a listed card at run time (the locked layout used to pull "Where new
// listeners start" up beside the range). Silent, no browser.   node --max-old-space-size=256 scripts/test-setup-order.mjs
import fs from 'node:fs';
const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
let pass = 0, fail = 0;
const ok = (c, w) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${w}`); c ? pass++ : fail++; };
const strip = (s) => s.replace(/<!--[\s\S]*?-->/g, '');
const pane = (id, next) => { const a = src.indexOf(`<div id="${id}">`), b = next ? src.indexOf(`<div id="${next}">`) : src.indexOf('<div id="serialModal"'); return strip(src.slice(a, b)); };
const heads = (html) => [...html.matchAll(/<h2(?: [^>]*)?>([\s\S]*?)<\/h2>/g)].map((m) => m[1].replace(/<[^>]+>/g, '').replace(/&rsquo;/g, "'").replace(/&amp;/g, '&').trim());
const inOrder = (got, want) => { let i = 0; for (const h of got) if (h === want[i]) i++; return i === want.length; };

const radio = heads(pane('radioPane'));
const wantRadio = ['SDR display name', 'Operating mode', 'Range', 'Listeners', 'Antenna', 'Radio', "This radio's hardware",
  'Where listeners may tune', 'Gain limits', 'Modes and decoders', 'Raw IQ', 'Where new listeners land', 'Background decoding',
  'PIN for this radio', 'Broadcast FM defaults', 'Time limit'];   // ★ beta96 b96-sbsfeed: flight-tracker sharing is the SERVER tab's now
ok(inOrder(radio, wantRadio), `radio tab: ${radio.join(' · ')}`);
const pos = (h) => radio.indexOf(h);
ok(pos('Where new listeners land') === pos('Background decoding') - 1, '10 where new listeners land, then 11 background decoding');
const server = heads(pane('serverPane', 'radioPane'));
const wantServer = ['Radio order', 'Background decoding priority', 'What this box can carry', 'Processor', 'Wi-Fi', 'On your network', 'Where this receiver is', 'Network port',
  'Behind a reverse proxy', 'Advertise on VibeSDR.net', 'SSH', 'VibeSDR community map',
  // ★★ beta96 b96-sbsfeed: the standalone map's limits, the server-level feed sources and the flight-tracker hub, before PSK Reporter
  'Air & sea map', 'Aircraft feeds from other receivers', 'Share ADS-B with flight trackers', 'PSK Reporter',
  'Audio', 'Frame rate', 'Shortwave schedule', "This phone's server", 'Start with Windows', 'Message on the landing screen'];
ok(inOrder(server, wantServer), `server tab: ${server.join(' · ')}`);
// the moves
const rp = pane('radioPane');
const cardOf = (id) => { const a = rp.indexOf(`id="${id}"`); const c = rp.lastIndexOf('<div class="card', a); return rp.slice(c, c + 200).match(/id="([^"]+)"/)?.[1] || rp.slice(c, rp.indexOf('</h2>', c)); };
ok(rp.indexOf('id="radioUseWrap"') > rp.indexOf('id="usageCard"') && rp.indexOf('id="radioUseWrap"') < rp.indexOf('id="lockedOnly"'), 'Radio use (single mode) sits in Operating mode');
ok(/id="modeSingleUse"/.test(rp) && /Single use/.test(rp), 'the third tile: Single use');
ok(rp.indexOf('id="rawIqRow"') > rp.indexOf('id="rawIqCard"') && rp.indexOf('id="rawIqRow"') < rp.indexOf('id="startCard"'), 'Raw IQ is its own card');
for (const id of ['hwBg', 'hwSatPass', 'hwDx', 'bgModeList', 'bgKindManual'])
  ok(rp.indexOf(`id="${id}"`) > rp.indexOf('id="bgCard"') && rp.indexOf(`id="${id}"`) < rp.indexOf('id="radioPinCard"'), `${id} inside Background decoding`);
ok(!/id="mapKeep"/.test(rp), 'no keep-running tick under the landing any more (it is the background choice)');
ok(/id="landKindPlain"/.test(rp) && /id="landKindDab"/.test(rp) && /id="landKindMap"/.test(rp), 'the landing: frequency / DAB station / map');
// layout rules
ok(!/[;{\s]order\s*:\s*-?\d/.test(src.replace(/<!--[\s\S]*?-->/g, '').replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '')), 'no CSS order: phone width keeps the DOM order');
ok(!/insertBefore\(start, range\.nextSibling\)/.test(src), 'a locked radio no longer moves the landing card');
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
