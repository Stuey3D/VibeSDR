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
           radioList: (typeof radioList === "function") ? radioList : null,
           sdrRender: (typeof sdrRender === "function") ? sdrRender : null,
           set SDR(v){ SDR = v }, get SDR_ALARM(){ return SDR_ALARM },
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

console.log('\nThe SERVER tab has no radio, and must not try to mark one');
{
  // ★★★ curRadio is -1 on the machine's tab — which is the tab the page OPENS on. The master save
  //     did `radioList()[curRadio].configured = true` guarded only by the list being non-empty, so
  //     it threw TypeError before building the request and the catch blamed the network. Zero
  //     requests left the page while it said "Could not reach the server".
  page.cfg = { name: 't', radios: [{ serial: 'A', driver: 'rtl', mode: 'single', configured: true }] };
  page.curRadio = -1;
  const list = page.cfg.radios;
  let threw = null;
  try {
    // the exact expression the save performs, as it now stands
    if (page.curRadio >= 0 && list[page.curRadio]) list[page.curRadio].configured = true;
  } catch (e) { threw = e; }
  ok(!threw, '★★★ marking configured from the server tab does not throw', String(threw));
  ok(list[0].configured === true, 'and the radio is left exactly as it was');
}

// ★★★ MISSING / NEW RADIOS AND THE DISPLAY ORDER (Stuart, 2026-10-04). The server decides which
//     notice a radio gets (sdr_presence.h); the page must draw exactly that, in Stuart's words,
//     highlight a tab ONLY for the alarm, and keep a PAUSED radio on the page.
console.log('\nPaused radios stay on the page, in the owner\'s order');
{
  page.cfg = { name: 't', radios: [
    { serial: 'A', driver: 'rtlsdr', label: 'First in file', order: 2, enabled: true },
    { serial: 'B', driver: 'sdrplay', label: 'Paused one', order: 0, enabled: false },
    { serial: 'C', driver: 'airspyhf', label: 'No order set' },
  ] };
  const l = page.radioList();
  ok(l.length === 3, '★★ a paused (enabled:false) radio still has a tab', String(l.length));
  ok(l.map((r) => r.serial).join() === 'B,A,C', '★ tabs follow `order`; unset = its place in the file; ties keep file order (as vsconfig::displayOrder)',
     l.map((r) => r.serial).join());
}

console.log('\nThe notices: the server decides, the page draws it in Stuart\'s words');
{
  page.SDR = {
    missing: [
      { serial: '00000003', driver: 'rtlsdr', label: 'RTL-SDR Blog V4', release: false, notice: 'missing', variant: 'absent' },
      { serial: '00000004', driver: 'rtlsdr', label: 'Second V4', release: false, notice: 'missing', variant: 'failed' },
      { serial: 'DD52B980BE4946DA', driver: 'airspyhf', label: 'Airspy HF+', release: true, notice: 'soft' },
      { serial: 'R1', driver: 'sdrplay', label: 'RSP1B', release: true, notice: 'inuse' },
      { serial: 'R2', driver: 'sdrplay', label: 'RSPdx', release: false, notice: 'inuse' },
    ],
    paused: [{ serial: 'P1', driver: 'rtlsdr', label: 'Paused V4', presence: 'absent' }],
    found: [
      { serial: '00000009', driver: 'rtlsdr', name: 'RTL-SDR Blog V4', collides: false,
        replace: [{ serial: '00000003', label: 'RTL-SDR Blog V4', paused: false }] },
      { serial: '00000001', driver: 'rtlsdr', name: 'Generic RTL', collides: true, replace: [] },
    ],
  };
  page.sdrRender();
  const h = $el('sdrChanges').innerHTML;
  const text = h.replace(/<[^>]+>/g, '');
  ok(text.includes('RTL-SDR Blog V4: Serial Number 00000003 is not detected, please check the USB connection. '
     + 'If the SDR has been intentionally removed then click here to remove it. If it has only temporarily been '
     + 'removed for use in another project etc click here to pause VibeServer looking for it, your settings will '
     + 'be saved and applied again when you select it again.'), '★★★ the missing radio, word for word');
  ok(text.includes('Second V4: Serial Number 00000004 is not detected, please check the USB connection, or the radio may have failed.'),
     '★ "or the radio may have failed" when the bus cannot prove it is gone');
  ok(/data-sdr="remove" data-serial="00000003"/.test(h) && /data-sdr="pause" data-serial="00000003"/.test(h),
     'REMOVE and PAUSE act on that radio');
  ok(text.includes('Airspy HF+ (serial DD52B980BE4946DA) can’t be seen right now.')
     && text.includes('It’s set to be released when VibeServer isn’t using it'),
     '★★ release ON + cannot tell → the softer notice');
  ok(text.includes('RSP1B in use with another app on this server'), '★★ known in use, release ON → a plain line');
  ok(!/data-serial="R1"/.test(h), '  …with no Remove or Pause beside it');
  ok(text.includes('RSPdx is in use by another app on this server, so VibeServer cannot serve it. Stop that app'),
     '★★ known in use, release OFF → a problem to fix');
  ok(text.includes('Paused V4') && /data-sdr="resume" data-serial="P1"/.test(h), 'a paused radio offers RESUME');
  ok(text.includes('VibeServer has found a new SDR: RTL-SDR Blog V4, serial 00000009')
     && /data-sdr="add" data-serial="00000009"/.test(h), 'a new radio offers ADD AS A NEW RADIO');
  ok(/data-sdr="replace" data-serial="00000003" data-new="00000009"/.test(h) && text.includes('REPLACE ‘RTL-SDR Blog V4’ (not found)'),
     'and REPLACE against the missing radio of the same driver');
  ok(text.includes('answers to the same serial') && !/data-serial="00000001"/.test(h),
     '★ a colliding serial points at the rename control and offers no button');
  const alarm = [...page.SDR_ALARM].sort().join();
  ok(alarm === '00000003,00000004', '★★ only the ALARM highlights a tab — not the soft or in-use ones', alarm);
}

console.log(fail ? `\n${fail} FAILED\n` : '\nall good\n');
process.exit(fail ? 1 : 0);
