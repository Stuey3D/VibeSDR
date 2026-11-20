// test-setup-android.mjs — the /setup page on an ANDROID phone's server (beta88, "Remote access to server settings").
// The page's own markup and its own androidPrune()/androidDeviceCard(), rendered in jsdom, against the config shape
// vibe_android_setup.h sends. What must hold:
//   1. Every control the phone supports (supportedControls() in the header) EXISTS on the page — a renamed id would
//      silently drop a setting from the remote page.
//   2. After the prune, every supported control is still visible, and NOT ONE other input/select/textarea is — no dead
//      controls on a phone (AGENTS.md: a control that only works on one platform must not be there).
//   3. The phone-only facts are drawn read-only (no inputs), with where to change them.
//   4. The sign-in path returns before the Linux-only loaders (sdr-changes, Wi-Fi, Windows, serial, benchmark).
//      node scripts/test-setup-android.mjs
import fs from 'node:fs';
import { JSDOM } from 'jsdom';

const page = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const header = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_android_setup.h', import.meta.url), 'utf8');
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

const html = page.match(/kVibeSetupPage = R"HTML\(([\s\S]*?)\)HTML"/)[1];
const body = html.replace(/<script>[\s\S]*?<\/script>/g, '');
const js = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]).join('\n');

// The supported ids, from the header itself — one list, two readers.
const block = header.slice(header.indexOf('supportedControls()'), header.indexOf('return ids;'));
const supported = [...block.matchAll(/"([A-Za-z]+)"/g)].map((m) => m[1]);
console.log(`setup page on an Android phone — ${supported.length} supported controls`);
ok(supported.length >= 20, 'the header lists the supported controls');

const dom = new JSDOM(`<!doctype html><body>${body}</body>`);
const { window } = dom;
const doc = window.document;

for (const id of supported) ok(!!doc.getElementById(id), `the page has #${id}`);

// The two functions under test, cut out of the page's own script.
const cut = (name) => {
  const a = js.indexOf(`function ${name}(`);
  if (a < 0) return '';
  let depth = 0, i = js.indexOf('{', a);
  for (; i < js.length; i++) { if (js[i] === '{') depth++; else if (js[i] === '}' && --depth === 0) break; }
  return js.slice(a, i + 1);
};
const code = cut('androidPrune') + '\n' + cut('androidDeviceCard');
ok(code.includes('function androidPrune') && code.includes('function androidDeviceCard'), 'androidPrune and androidDeviceCard are in the page');

const cfg = {
  configured: true, host: 'android',
  radios: [{ serial: 'android', driver: 'rtl', pin: '' }],
  android: {
    supported,
    device: { batteryOptimisation: 'on', startOnPowerSupported: false, startOnPower: false,
              radioPresent: true, usbPermission: true, keepRadioAlive: true },
  },
};
// The page's hidden-by-default cards: show #setup as sign-in would.
doc.getElementById('setup').classList.remove('hide');
new Function('document', '$', 'cfg', `${code}\nandroidPrune();`)(doc, (id) => doc.getElementById(id), cfg);

const hiddenByPrune = (el) => { for (let e = el; e; e = e.parentElement) if (e.classList && e.classList.contains('androidOff')) return true; return false; };
const setup = doc.getElementById('setup');
for (const id of supported) ok(!hiddenByPrune(doc.getElementById(id)), `#${id} stays visible`);

// ★ A modal the page opens only from a control that is itself gone (the serial dialog) is `hidden` already.
const inHiddenModal = (el) => { for (let e = el; e; e = e.parentElement) if (e.hasAttribute && e.hasAttribute('hidden')) return true; return false; };
const left = [...setup.querySelectorAll('input,select,textarea')]
  .filter((e) => e.type !== 'hidden' && !hiddenByPrune(e) && !inHiddenModal(e) && !supported.includes(e.id));
ok(left.length === 0, `no other control is left on the page (${left.map((e) => '#' + (e.id || e.name || e.tagName)).slice(0, 12).join(' ')})`);

for (const id of ['cpuGovernor', 'srvPort', 'dirList', 'rate', 'lockFreq', 'ppm', 'galleryMb']) {
  const el = doc.getElementById(id);
  if (!el) continue;
  const want = supported.includes(id);
  ok(hiddenByPrune(el) === !want, `#${id} ${want ? 'kept' : 'hidden'}`);
}

const dev = doc.getElementById('androidDevCard');
ok(!!dev && dev.parentElement && dev.parentElement.id === 'serverPane', 'the "This phone" card is drawn on the server tab');
ok(dev && dev.querySelectorAll('input,select,textarea,button').length === 0, '…read-only: no controls in it');
ok(dev && /Battery optimisation/.test(dev.textContent) && /Android may stop the server/.test(dev.textContent),
   '…battery optimisation ON is said, with what it means');
ok(dev && /Not offered on this Android version/.test(dev.textContent), '…start when power returns where Android does not allow it');
ok(dev && /Share this radio/.test(dev.textContent), '…and where on the phone to change them');

// The sign-in path: on a phone it must stop before the Linux machine's loaders.
const si = js.slice(js.indexOf('async function signIn('), js.indexOf('async function signIn(') + 6000);
const prune = si.indexOf('if (ANDROID) { androidPrune(); return; }');
ok(prune > 0, 'signIn returns into androidPrune on a phone');
for (const f of ['sdrChangesLoad()', 'wifiLoad()', 'winLoad()', 'serialStatus()', 'benchLoad()'])
  ok(si.indexOf(f) > prune, `…before ${f}`);
ok(/if \(!ANDROID\) pollDirStatus\(\)/.test(si), '…and the directory status poll is not started on a phone');

console.log(fail ? `   ${fail} FAILED` : '   all passed');
process.exit(fail ? 1 : 0);
