// ★★★ SWITCHING TABS MUST NOT WRITE ONE RADIO'S SETTINGS INTO ANOTHER.
//
//     fill() deliberately leaves the sample rate alone — "the options do not exist until
//     renderHw() has heard back from the radio" — so renderHw() fills it in asynchronously. In the
//     window between clicking a tab and that fetch returning, the form still holds the PREVIOUS
//     radio's rate, and stashRadio() copies what is on screen into cfg.radios[curRadio].
//
//     Stuart hit it setting a landing station on two radios in one sitting: his Airspy came back
//     misaligned at every sample rate, because the rate stored for it was never one he had picked.
//
// ★★ This drives the page's REAL functions, with the DOM and fetch stubbed, and asserts on the
//    config object they mutate. Written to FAIL against the unguarded version first — the guard is
//    one `if`, and an `if` is exactly the kind of fix that can be silently reverted.
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
    id, value: '', checked: false, textContent: '', innerHTML: '', disabled: false, hidden: false,
    style: {}, _cls: new Set(['hide']),
    classList: {
      add: (c) => e._cls.add(c), remove: (c) => e._cls.delete(c),
      toggle: (c, on) => (on === undefined ? (e._cls.has(c) ? e._cls.delete(c) : e._cls.add(c))
                                           : (on ? e._cls.add(c) : e._cls.delete(c))),
      contains: (c) => e._cls.has(c),
    },
    addEventListener() {}, removeEventListener() {}, appendChild() {}, remove() {},
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

// ★★ THE SLOW FETCH IS THE WHOLE POINT. renderHw() awaits this; the test resolves it by hand so
//    the "tab switched, hardware not back yet" window can be held open and inspected.
// ★ The resolver is created UP FRONT, not inside fetch. Built the other way round, the test hung:
//   renderHw() fetches /vibeserver/radios before /vibeserver/hardware, so at the moment the test
//   tried to release the gate the resolver did not exist yet and the promise was never settled.
let hwGateResolve = () => {};
let hwGate = new Promise((res) => { hwGateResolve = res; });
const resetHwGate = () => { hwGate = new Promise((res) => { hwGateResolve = res; }); };
globalThis.fetch = async (url) => {
  if (String(url).includes('/vibeserver/hardware')) {
    await hwGate;
    return { ok: true, json: async () => ({ driver: 'airspyhf', present: true,
                                            rates: [912000], gains: [] }) };
  }
  return { ok: true, json: async () => ({ radios: [] }), text: async () => '' };
};

const page = new Function(`${js}
  return { get cfg(){return cfg}, set cfg(v){cfg=v},
           get curRadio(){return curRadio}, set curRadio(v){curRadio=v},
           get formRadio(){ return typeof formRadio === "undefined" ? null : formRadio },
           stashRadio, fill, collectRadio,
           refreshHw: (typeof refreshHw === "function") ? refreshHw : null };`)();

console.log('\nSwitching tabs must not carry a rate between radios');
{
  page.cfg = {
    name: 'test', radios: [
      { serial: 'RTL1', driver: 'rtl',      label: 'Dongle', rate: 2400000, mode: 'single', configured: true },
      { serial: 'AHF1', driver: 'airspyhf', label: 'HF+',    rate:  912000, mode: 'single', configured: true },
    ],
  };

  // On the dongle's tab, with its hardware fully rendered: the rate box shows the dongle's rate.
  page.curRadio = 0;
  $el('rate').value = '2400000';
  if (page.refreshHw) { const p = page.refreshHw(); hwGateResolve(); await p; }

  ok(page.formRadio !== null, '★ the page tracks which radio the form belongs to',
     '(formRadio is missing — the guard is not there at all)');

  // Now switch to the Airspy. fill() runs; the rate is NOT filled in — renderHw is still in flight.
  page.curRadio = 1;
  resetHwGate();
  const inFlight = page.refreshHw ? page.refreshHw() : null;

  // …and the owner presses save in that window. This is the exact sequence Stuart performed.
  page.stashRadio();

  ok(page.cfg.radios[1].rate === 912000,
     "★★★ the Airspy keeps its own rate — the dongle's 2.4 MSPS did not leak into it",
     `got ${page.cfg.radios[1].rate}`);
  ok(page.cfg.radios[0].rate === 2400000,
     'and the dongle is untouched', `got ${page.cfg.radios[0].rate}`);

  // Once the hardware answers, the form is this radio's and a stash is allowed again.
  if (inFlight) { hwGateResolve(); await inFlight; }
  ok(page.formRadio === 1, '★ after the render completes the form belongs to the open tab',
     `formRadio=${page.formRadio}`);
}

console.log('\nA radio is only sent the calibration its own driver has');
{
  page.cfg = { name: 't', radios: [{ serial: 'R', driver: 'rtl', mode: 'single', configured: true }] };
  page.curRadio = 0;
  $el('ppm').value = '12'; $el('ppb').value = '999';
  // ★ Both boxes left VISIBLE, as they are mid-switch. The old code read exactly this to decide.
  $el('hwPpm').classList.remove('hide'); $el('hwPpb').classList.remove('hide');
  const outRtl = page.collectRadio();
  ok(outRtl.ppm === 12, 'a dongle sends ppm', JSON.stringify(outRtl.ppm));
  ok(outRtl.ppb === undefined,
     '★★★ and never ppb, however the screen happens to look', JSON.stringify(outRtl.ppb));

  page.cfg.radios[0].driver = 'airspyhf';
  const outAhf = page.collectRadio();
  ok(outAhf.ppb === 999, 'an Airspy sends ppb', JSON.stringify(outAhf.ppb));
  ok(outAhf.ppm === undefined, '★★★ and never ppm', JSON.stringify(outAhf.ppm));
}

console.log(fail ? `\n${fail} FAILED\n` : '\nall good\n');
process.exit(fail ? 1 : 0);
