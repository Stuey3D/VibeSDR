#!/usr/bin/env node
// ★★★ A BOX WITHOUT WI-FI CAN FINISH SETUP (Nick's Orange Pi One, 2026-10-25: "I can't complete the setup — I get the error
// Wi-Fi not saved — Give this box's own hotspot a name"). On a wired box the Wi-Fi card stays drawn with only its FORM
// hidden, and wifiDirty() asked whether the CARD was hidden — so the empty hotspot name made every collect throw, that
// counted as "unsaved Wi-Fi", and the main Save failed. Also the owner's manual "Disable Wi-Fi / no Wi-Fi present" tick
// (Stuart: "autodetect wifi and hide the options, if that fails then a manual toggle"). Run against the page source.
//
//   node scripts/test-setup-wifi-wired.mjs
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
/** wifiDirty with the page's own wifiIgnored, a card that is shown, and a form that cannot be collected (no hotspot name). */
const dirty = (WIFI, ignore) => new Function('WIFI', 'WIFI_IGNORE', `
  const POCKET = {}; let WIFI_BASELINE = "";
  const $ = () => ({ classList: { contains: () => false } });
  const wifiCollect = () => { throw "Give this box’s own hotspot a name."; };
  ${grab('wifiIgnored')}
  ${grab('wifiDirty')}
  return wifiDirty();`)(WIFI, ignore);

let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); if (!c) fails++; };
console.log('setup page — a box without Wi-Fi can finish setup');
ok(dirty({ hasWifi: false, wired: true }, false) === false, 'no Wi-Fi radio: an untouched card is not "unsaved Wi-Fi" (Nick\'s Orange Pi One)');
ok(dirty({ hasWifi: true, wifiOff: true, wired: true }, false) === false, 'Wi-Fi switched off by the owner: nothing to save either');
ok(dirty({ present: false }, true) === false, 'the box has not reported its Wi-Fi, owner ticked "no Wi-Fi present": not dirty');
ok(dirty({ hasWifi: true, wired: true }, false) === true, 'a box WITH Wi-Fi and an unfinished form still says so (no change there)');
ok(/id="wifiIgnore"/.test(html) && /Disable Wi-Fi \/ no Wi-Fi present/.test(html), 'the manual tick is on the page, in Stuart\'s words');
process.exit(fails ? 1 : 0);
