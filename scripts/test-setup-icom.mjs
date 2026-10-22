// ★★★ THE ICOM CARD ON THE SETUP PAGE — shown for an Icom ONLY, with the preflight in plain words and the
//     menu item to change; the IQ cards hidden for it and PUT BACK for the next radio (a tab switch must not
//     leave a dongle's tab with its rate and gain cards gone). Drives the page's real icomRender() with the
//     DOM stubbed, as test-setup-tabswitch.mjs does.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const html = src.match(/kVibeSetupPage = R"HTML\(([\s\S]*?)\)HTML"/)[1];
const js = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((x) => x[1]).join('\n');

let fail = 0;
const ok = (cond, what, extra = '') => {
  if (cond) { console.log(`   ok   ${what}`); return; }
  fail++; console.log(`   FAIL ${what} ${extra}`);
};

// ── A DOM just real enough ──────────────────────────────────────────────────
// ★ Every element answers, so the page's own null-guards behave as they do in a browser.
const els = new Map();
const mkEl = (id) => {
  const e = {
    id, value: '', checked: false, textContent: '', innerHTML: '', disabled: false, hidden: false, options: [], dataset: {}, children: [],
    style: {}, _cls: new Set(['hide']),
    classList: {
      add: (c) => e._cls.add(c), remove: (c) => e._cls.delete(c),
      toggle: (c, on) => (on === undefined ? (e._cls.has(c) ? e._cls.delete(c) : e._cls.add(c))
                                           : (on ? e._cls.add(c) : e._cls.delete(c))),
      contains: (c) => e._cls.has(c),
    },
    addEventListener() {}, removeEventListener() {}, appendChild() {}, remove() {}, insertBefore() {},
    querySelector: () => null, closest: () => null, contains: () => false,
    querySelectorAll: () => [], getAttribute: () => null, setAttribute() {}, focus() {},
  };
  return e;
};
const $el = (id) => { if (!els.has(id)) els.set(id, mkEl(id)); return els.get(id); };

globalThis.document = {
  getElementById: $el,
  querySelector: () => null,
  querySelectorAll: () => [],
  createElement: () => mkEl('new'),
  addEventListener() {},
  body: mkEl('body'),
};
globalThis.window = { location: { host: 'x', protocol: 'http:', href: '', origin: 'http://x', search: '', hostname: 'x' },
                      addEventListener() {}, sessionStorage: { getItem: () => null, setItem() {} } };
globalThis.location = window.location;
globalThis.sessionStorage = window.sessionStorage;
globalThis.localStorage = { getItem: () => null, setItem() {} };
try { globalThis.navigator = { userAgent: 'test' }; } catch { /* node 26 makes it read-only */ }
globalThis.alert = () => {};
globalThis.setTimeout = (f) => { f(); return 0; };
globalThis.setInterval = () => 0;
globalThis.clearInterval = () => {};

globalThis.fetch = async () => ({ ok: true, json: async () => ({ radios: [] }), text: async () => '' });
const page = new Function(`${js}
  return { set cfg(v){cfg=v}, icomRender };`)();
const hidden = (id) => $el(id).classList.contains('hide');

console.log('\nThe Icom card');
for (const id of ['rangeCard', 'radioHwCard', 'radioStartCard', 'startCard']) $el(id).classList.remove('hide');
const hw = { driver: 'icom', radio: { driver: 'icom', model: 'IC-7300', civ: '94', baud: 115200, linesHeldLow: true,
  scope: { fps: 19.9 }, audio: 'ALSA hw:2,0 (CODEC)', audioProblem: '',
  preflight: { passed: false, verified: false, summary: 'VibeServer will not start this radio, because it could transmit: USB SEND is RTS',
               items: [{ name: 'USB SEND', value: 'RTS', ok: false, gate: true, where: 'MENU > SET > Connectors > USB SEND/Keying > USB SEND' },
                       { name: 'VOX', value: 'OFF', ok: true, gate: true, where: 'the VOX key' }] } } };
const r = { serial: 'IC-7300 03001234', driver: 'icom', label: 'Icom', icomBaud: 19200, icomOwnerLock: undefined, icomCiv: 0x94 };
page.cfg = { radios: [r] };
page.icomRender(hw, r, 'icom');
ok(!hidden('icomCard'), 'an Icom shows the Icom card');
ok(['rangeCard', 'gainCard', 'radioHwCard', 'radioStartCard', 'startCard'].every(hidden), 'and hides the window, gain, rate, hardware and landing cards');
ok($el('icomModel').innerHTML.includes('IC-7300') && $el('icomModel').innerHTML.includes('DTR/RTS held low'), 'model, address and the DTR/RTS fact are shown');
ok($el('icomPreflight').innerHTML.includes('change it at <b>MENU &gt; SET &gt; Connectors'), 'a failed preflight item names the exact menu item to change');
ok($el('icomBaud').value === '19200', 'the saved baud is shown (19200)');
ok($el('icomOwnerLock').checked === true, 'owner lock defaults ON when the config has never said');
ok($el('icomCiv').value === '94', 'CI-V address shown as hex');

page.cfg = { radios: [r], icomStatus: { 'IC-7300 03001234': 'VibeServer will not start this radio, because it could transmit: VOX is ON' } };
page.icomRender({ offline: true }, r, 'icom');
ok($el('icomPreflight').innerHTML.includes('VOX is ON'), 'not running: the reason from its status file is shown');

page.icomRender({ driver: 'rtl' }, { driver: 'rtl' }, 'rtl');
ok(hidden('icomCard'), 'a dongle tab hides the Icom card');
ok(!hidden('rangeCard') && !hidden('radioHwCard') && !hidden('radioStartCard') && !hidden('startCard'), '★ and gets its own cards BACK after an Icom tab');
console.log(fail ? `\n${fail} failed` : '\nall good');
process.exit(fail ? 1 : 0);
