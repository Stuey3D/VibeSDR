#!/usr/bin/env node
// ★★★ NEW WI-FI HARDWARE ON A SET-UP BOX (Stuart, 2026-10-25: "dont force a resetup if a wifi dongle is detected but when a
// user goes into setup show an option for new wifi hardware detected, configure it now?"). The root service leaves a new
// radio alone and lists it in its state (newWifi); the setup page asks. Run against the page source, no browser:
//   · newWifi → "New Wi-Fi hardware detected — configure it now?" with the radio named, Configure + Not now;
//   · ignoredWifi only → a quiet "not in use" line with "Use it", no Not now;
//   · neither → nothing drawn;
//   · Configure posts {action:"configure"} to /vibeserver/pocket/newwifi, waits for the box to manage the radio, clears the
//     owner's "no Wi-Fi present" tick and opens the Wi-Fi form; Not now posts {action:"ignore"}.
//
//   node scripts/test-setup-wifi-new.mjs
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
const el = () => {
  const cls = new Set(['hide']);
  return { textContent: '', disabled: false, clicked: 0, scrolled: 0,
           classList: { toggle: (c, on) => (on ? cls.add(c) : cls.delete(c)), contains: c => cls.has(c), add: c => cls.add(c), remove: c => cls.delete(c) },
           click() { this.clicked++; }, scrollIntoView() { this.scrolled++; } };
};
const page = () => {
  const els = {};
  const $ = id => (els[id] = els[id] || el());
  return { els, $ };
};

let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); if (!c) fails++; };
console.log('setup page — new Wi-Fi hardware on a set-up box');

const render = WIFI => {
  const p = page();
  new Function('WIFI', '$', `${grab('wifiNewRender')}; wifiNewRender();`)(WIFI, p.$);
  return p.els;
};
let e = render({ hasWifi: false, wired: true, newWifi: [{ iface: 'wlx00c0ca123456', driver: 'rtl8xxxu', usb: '0bda:8179' }], ignoredWifi: [] });
ok(!e.wifiNew.classList.contains('hide'), 'a new dongle: the prompt is drawn');
ok(/New Wi-Fi hardware detected — configure it now\?/.test(e.wifiNewText.textContent), 'in Stuart\'s words: "New Wi-Fi hardware detected — configure it now?"');
ok(/wlx00c0ca123456 \(USB 0bda:8179, rtl8xxxu\)/.test(e.wifiNewText.textContent), '…naming the radio (interface, USB id, driver)');
ok(/no hotspot, no network/.test(e.wifiNewText.textContent), '…and saying the box leaves it off meanwhile');
ok(e.wifiNewYes.textContent === 'Configure' && !e.wifiNewNo.classList.contains('hide'), 'Configure and Not now');
ok(!/in place of/.test(e.wifiNewText.textContent), 'a box with no Wi-Fi of its own: nothing about replacing a radio');
e = render({ hasWifi: true, newWifi: [{ iface: 'wlan1', driver: 'mt7601u', usb: '148f:7601' }] });
ok(/in place of the radio it uses now/.test(e.wifiNewText.textContent), 'a box already on Wi-Fi: says Configure replaces its radio');
e = render({ hasWifi: false, newWifi: [], ignoredWifi: [{ iface: 'wlan1', driver: 'mt7601u', usb: '148f:7601' }] });
ok(!e.wifiNew.classList.contains('hide') && /not in use: wlan1/.test(e.wifiNewText.textContent) && e.wifiNewYes.textContent === 'Use it'
   && e.wifiNewNo.classList.contains('hide'), 'a radio the owner said Not now to: a quiet "not in use — Use it" line, never asked again');
e = render({ hasWifi: true, newWifi: [], ignoredWifi: [] });
ok(e.wifiNew.classList.contains('hide'), 'no new hardware: nothing drawn');
e = render(null);
ok(e.wifiNew.classList.contains('hide'), 'no state yet: nothing drawn');

// ── the two buttons, against a stub root service ──
const choose = async (action, after) => {
  const p = page();
  const posts = [];
  const state = { WIFI: { hasWifi: false, newWifi: [{ iface: 'wlx00c0ca123456' }] }, WIFI_IGNORE: true, stored: {}, rendered: 0 };
  p.$('wifiSummary').classList.add('hide');
  const fn = new Function('S', '$', 'wifiPost', 'wifiLoad', 'wifiRender', 'localStorage', 'setTimeout', `
    let WIFI = S.WIFI, WIFI_IGNORE = S.WIFI_IGNORE;
    async ${grab('wifiNewChoose')}
    return wifiNewChoose(${JSON.stringify(action)}).then(() => { S.WIFI = WIFI; S.WIFI_IGNORE = WIFI_IGNORE; });`);
  // ★ wifiLoad() must update the closure's WIFI: done through a getter on S, read back each turn.
  await fn(state, p.$, async (path, body) => { posts.push([path, body]); }, async () => { Object.assign(state.WIFI, after); },
    () => { state.rendered++; }, { setItem: (k, v) => { state.stored[k] = v; } }, (f) => f());
  return { posts, state, els: p.els };
};
let r = await choose('configure', { hasWifi: true, newWifi: [] });
ok(r.posts.length === 1 && r.posts[0][0] === '/vibeserver/pocket/newwifi' && r.posts[0][1].action === 'configure', 'Configure posts {action:"configure"}');
ok(r.state.WIFI_IGNORE === false && r.state.stored.vibeWifiIgnore === '0', '…clears the owner\'s "no Wi-Fi present" tick (they chose Wi-Fi)');
ok(r.state.rendered >= 1 && r.els.wifiCard.scrolled === 1, '…draws the Wi-Fi form and brings it into view');
r = await choose('ignore', { newWifi: [] });
ok(r.posts[0][1].action === 'ignore' && r.state.WIFI_IGNORE === true && r.els.wifiNewMsg.textContent === '', 'Not now posts {action:"ignore"} and changes nothing else');
r = await choose('configure', { lastError: 'no new Wi-Fi hardware to configure' });
ok(/no new Wi-Fi hardware to configure/.test(r.els.wifiNewMsg.textContent), 'the box refuses: its reason is shown');

process.exit(fails ? 1 : 0);
