// ★★★ EVERY RADIO'S SETUP TAB SHOWS ONLY WHAT THAT RADIO CAN DO (V12, Stuart 2026-11-05: "that settings audit needs to
//     apply to all radios, we shouldnt be offering modes etc that cannot be run on that radio").
//     The caps come from the SERVER's own function — vibeserver/test-radio-caps.cpp --json prints vibe_radio_caps.h's
//     answer for every driver (compiled here) — and are fed to the Setup page's real modeBlockRender() / capsApply() with
//     the DOM stubbed, as test-setup-icom.mjs does. Checks: each tab draws exactly the modes its caps allow (and no
//     other), the settings rows follow the caps, a hidden switch's saved value is kept, the running radio's own answer
//     outranks the door's, and an older server (no caps) draws everything as before.
//   node scripts/test-setup-radio-caps.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let fail = 0, pass = 0;
const ok = (cond, what) => { if (cond) { pass++; console.log(`   ok   ${what}`); } else { fail++; console.log(`   FAIL ${what}`); } };

// ── the server's answers ──
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-caps-'));
const bin = path.join(tmp, 'caps');
try {
  execFileSync('c++', ['-std=c++17', '-I', path.join(root, 'android/app/src/main/cpp'), '-I', path.join(root, 'android/app/src/main/cpp/decoders'),
                       '-o', bin, path.join(root, 'vibeserver/test-radio-caps.cpp')], { stdio: 'inherit' });
} catch { console.log('   FAIL could not compile vibeserver/test-radio-caps.cpp'); process.exit(1); }
const CAPS = JSON.parse(execFileSync(bin, ['--json']).toString());
fs.rmSync(tmp, { recursive: true, force: true });

// ── the page ──
const src = fs.readFileSync(path.join(root, 'android/app/src/main/cpp/vibe_setup_page.h'), 'utf8');
const html = src.match(/kVibeSetupPage = R"HTML\(([\s\S]*?)\)HTML"/)[1];
const js = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((x) => x[1]).join('\n');

const els = new Map();
const mkEl = (id) => {
  const e = {
    id, value: '', checked: false, textContent: '', innerHTML: '', disabled: false, hidden: false, options: [], dataset: {}, children: [],
    style: {}, _cls: new Set(),
    classList: {
      add: (c) => e._cls.add(c), remove: (c) => e._cls.delete(c),
      toggle: (c, on) => (on === undefined ? (e._cls.has(c) ? e._cls.delete(c) : e._cls.add(c)) : (on ? e._cls.add(c) : e._cls.delete(c))),
      contains: (c) => e._cls.has(c),
    },
    insertAdjacentHTML(_pos, h) { e.innerHTML += h; },
    addEventListener() {}, removeEventListener() {}, appendChild() {}, remove() {}, insertBefore() {},
    querySelector: () => null, closest: () => null, contains: () => false, parentNode: null,
    querySelectorAll: () => [], getAttribute: () => null, setAttribute() {}, focus() {},
  };
  return e;
};
const $el = (id) => { if (!els.has(id)) els.set(id, mkEl(id)); return els.get(id); };
globalThis.document = { getElementById: $el, querySelector: () => null, querySelectorAll: () => [], createElement: () => mkEl('new'),
                        addEventListener() {}, body: mkEl('body') };
globalThis.window = { location: { host: 'x', protocol: 'http:', href: '', origin: 'http://x', search: '', hostname: 'x' },
                      addEventListener() {}, sessionStorage: { getItem: () => null, setItem() {} } };
globalThis.location = window.location;
globalThis.sessionStorage = window.sessionStorage;
globalThis.localStorage = { getItem: () => null, setItem() {} };
try { globalThis.navigator = { userAgent: 'test' }; } catch { /* read-only on newer node */ }
globalThis.alert = () => {};
globalThis.setTimeout = (f) => { f(); return 0; };
globalThis.setInterval = () => 0;
globalThis.clearInterval = () => {};
globalThis.fetch = async () => ({ ok: true, json: async () => ({ radios: [] }), text: async () => '' });
const page = new Function(`${js}
  return { set cfg(v){cfg=v}, set curRadio(v){curRadio=v}, set hwCaps(v){HW_CAPS=v}, modeBlockRender, modeBlockSummary, capsApply,
           modeTree, BLOCKABLE };`)();
const hidden = (id) => $el(id).classList.contains('hide');
const drawn = () => [...$el('modeBlockList').innerHTML.matchAll(/data-mode="([^"]+)"/g)].map((m) => m[1]);
const ALL = page.BLOCKABLE.filter((m) => !m.node && !m.on).map((m) => m.id);

function tab(driverKey, extra = {}) {
  const r = { serial: 's-' + driverKey, driver: driverKey.replace(/^rtl.*/, 'rtlsdr'), mode: 'single', users: 4, ...extra };
  page.cfg = { radios: [r], radioCaps: CAPS[driverKey] ? { [r.serial]: CAPS[driverKey] } : undefined };
  page.curRadio = 0;
  page.hwCaps = null;
  $el('modeBlockList').innerHTML = '';
  page.capsApply();
  return r;
}

for (const key of Object.keys(CAPS)) {
  console.log(`\n${key}`);
  const c = CAPS[key];
  tab(key);
  const d = drawn();
  const want = ALL.filter((id) => c.modes.includes(id));
  ok(want.every((id) => d.includes(id)), `draws every mode its caps allow (${want.length})`);
  const extra = d.filter((id) => !c.modes.includes(id) && ALL.includes(id));
  ok(extra.length === 0, `draws nothing its caps exclude${extra.length ? ' — drew ' + extra.join(', ') : ''}`);
  ok(hidden('usageCard') === !c.settings.usage, `"How will it be used?" ${c.settings.usage ? 'shown' : 'hidden'}`);
  ok(hidden('rawIqRow') === !c.settings.rawIq && hidden('nbWideRow') === !c.settings.nbWide && hidden('vfoBRow') === !c.settings.vfoB,
     `Raw IQ / impulse NB / VFO A/B ${c.settings.rawIq ? 'shown' : 'hidden'}`);
  ok(hidden('fmDefCard') === !c.settings.fmDefaults, `Broadcast FM defaults ${c.settings.fmDefaults ? 'shown' : 'hidden'}`);
  ok(hidden('hwDx') === !c.settings.dxScan, `DX scanner ${c.settings.dxScan ? 'shown' : 'hidden'}`);
  const hintN = (($el('modeBlockList').innerHTML.match(/capsHint/g)) || []).length;
  ok(hintN === c.hints.length, `${c.hints.length} hint line(s) under the modes`);
}

console.log('\nThe Icom tab, exactly');
{
  const r = tab('icom', { blockedModes: 'dab,wfm,rtty' });
  const d = drawn().sort().join(',');
  ok(d === 'am,aprs,cw,cwl,cwu,dsc,ft8,hell,jtty,lsb,mfsk,navtex,nfm,psk,rtty,selcal,sitora,spots,sstv,time,usb,wefax',
     `only the radio's own modes and the audio decoders: ${d}`);
  ok(r.blockedModes === 'dab,wfm,rtty', '★ hiding is not clearing: the saved list is untouched');
  page.modeBlockSummary();
  const note = $el('modeBlockNote').textContent;
  ok(/RTTY/.test(note) && !/DAB|WFM/.test(note), `the summary names only what is drawn ("${note}")`);
}

console.log('\nThe running radio outranks the door');
{
  tab('rtl');
  ok(!drawn().includes('wefax'), 'the door (an RTL, no direct sampling): no WEFAX');
  page.hwCaps = { serial: 's-rtl', caps: CAPS.rtlds };
  $el('modeBlockList').innerHTML = '';
  page.capsApply();
  ok(drawn().includes('wefax') && drawn().includes('ndb'), 'the radio itself says direct sampling: WEFAX and NDB appear');
  page.hwCaps = { serial: 'someone-else', caps: CAPS.rtlds };
  $el('modeBlockList').innerHTML = '';
  page.capsApply();
  ok(!drawn().includes('wefax'), "another tab's answer is never used for this one");
}

console.log('\nAn older server (no caps)');
{
  tab('none-such');
  const d = drawn();
  ok(ALL.every((id) => d.includes(id)), `every switch drawn, as before (${ALL.length})`);
  ok(!hidden('usageCard') && !hidden('rawIqRow') && !hidden('vfoBRow'), 'and every setting');
}

console.log(fail ? `\n${fail} failed, ${pass} passed` : `\nall good (${pass})`);
process.exit(fail ? 1 : 0);
