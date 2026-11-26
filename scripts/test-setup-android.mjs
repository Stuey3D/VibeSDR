// test-setup-android.mjs — the /setup page on an ANDROID phone's server (beta88, "Remote access to server settings";
// every setting remote since 2026-11-20). The page's own markup and its own androidPrune()/androidSrvCard()/
// androidDeviceCard(), rendered in jsdom, against the config shape vibe_android_setup.h sends — for each kind of radio and
// device. What must hold:
//   1. Every control the phone offers (the header's controlTable, gated by radio and device exactly as the header gates
//      it) EXISTS on the page — a renamed id would silently drop a setting from the remote page.
//   2. After the prune, every offered control is visible, and NOT ONE other input/select/textarea is — no dead controls
//      (AGENTS.md: a control that only works on one radio must not be there). A generated input with no id of its own
//      counts as its nearest ancestor with one.
//   3. The radio's driver decides: an RTL never shows the SDRplay card or the HF+ calibration, an Icom never a capture
//      width, Lite never raw IQ, a television never the battery pause.
//   4. Simple hides the Advanced-only controls, live; no web client forces Simple.
//   5. The phone-only facts are drawn read-only (no inputs), with where to change them — the device-only list.
//   6. The sign-in path returns before the Linux-only loaders (sdr-changes, Wi-Fi, Windows, serial, benchmark).
//      node scripts/test-setup-android.mjs
import fs from 'node:fs';
import { JSDOM } from 'jsdom';

const page = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const header = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_android_setup.h', import.meta.url), 'utf8');
let fail = 0, pass = 0;
const ok = (c, what) => { if (!c) console.log(`   FAIL ${what}`); else if (process.env.VERBOSE) console.log(`   ok   ${what}`); c ? pass++ : fail++; };

const html = page.match(/kVibeSetupPage = R"HTML\(([\s\S]*?)\)HTML"/)[1];
const body = html.replace(/<script>[\s\S]*?<\/script>/g, '');
const js = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]).join('\n');

// ── The header's table, read as text and gated the way the header gates it (rowApplies) ──
const D = { D_RTL: 1, D_AHF: 2, D_ASP: 4, D_HRF: 8, D_RSP: 16, D_ICOM: 32 };
D.D_ANY = 63; D.D_RADIO = 63 & ~32; D.D_HWSETUP = 1 | 4 | 8 | 16;
const P = { P_ADV: 1, P_NOT_LITE: 2, P_NOT_TV: 4, P_TUNNEL: 8, P_LITE: 16, 0: 0 };
const mask = (expr, tbl) => expr.split('|').map((t) => t.trim()).reduce((a, t) => a | (tbl[t] ?? NaN), 0);
const tbody = header.slice(header.indexOf('controlTable() {'), header.indexOf('return t;'));
const TABLE = [...tbody.matchAll(/\{\s*((?:"[^"]*"\s*)+),\s*([A-Z_| ]+?),\s*([A-Z_| 0]+?)\s*\}/g)].map((m) => ({
  ids: [...m[1].matchAll(/"([^"]*)"/g)].map((x) => x[1]).join('').split(/\s+/).filter(Boolean),
  drivers: mask(m[2], D), needs: mask(m[3], P),
}));
ok(TABLE.length > 40 && TABLE.every((r) => Number.isFinite(r.drivers) && Number.isFinite(r.needs)), `the header's control table is read (${TABLE.length} rows)`);
const ALL = TABLE.flatMap((r) => r.ids);
const driverBit = (d) => ({ airspyhf: 2, airspy: 4, hackrf: 8, sdrplay: 16, icom: 32 })[d] ?? 1;
const offered = (pf) => {
  const sup = [], adv = [];
  for (const r of TABLE) {
    if (!(r.drivers & driverBit(pf.driver))) continue;
    if ((r.needs & 2) && pf.lite) continue;
    if ((r.needs & 16) && !pf.lite) continue;
    if ((r.needs & 4) && pf.tv) continue;
    if ((r.needs & 8) && !pf.tunnel) continue;
    sup.push(...r.ids); if (r.needs & 1) adv.push(...r.ids);
  }
  return { sup, adv };
};
console.log(`setup page on an Android phone — ${ALL.length} controls in the table`);

// The functions under test, cut out of the page's own script.
const cut = (name) => {
  const a = js.indexOf(`function ${name}(`);
  if (a < 0) return '';
  let depth = 0, i = js.indexOf('{', a);
  for (; i < js.length; i++) { if (js[i] === '{') depth++; else if (js[i] === '}' && --depth === 0) break; }
  return js.slice(a, i + 1);
};
const fns = ['androidKeeps', 'androidPrune', 'androidSrvCard', 'androidSrvCollect', 'androidDeviceCard', 'androidRadioFill', 'androidRadioCollect'];
const code = fns.map(cut).join('\n');
for (const f of fns) ok(code.includes(`function ${f}(`), `${f} is in the page`);

// ★ The SDRplay card is drawn by renderHw into #hw (it is not static markup): stand-ins with the page's ids, so the RSP
//   case is judged on the ids the page would draw.
const RSP_GENERATED = ['rspVibeAgc', 'rfNotch', 'dabNotch', 'rspAmNotch', 'rspHdr', 'rspExtRef', 'rspDabDecim', 'rfAgc',
  'autoNotch', 'userNotch', 'dabAgcOverride', 'dabAgcTarget'];

const hiddenByPrune = (el) => { for (let e = el; e; e = e.parentElement) if (e.classList && e.classList.contains('androidOff')) return true; return false; };
const inHiddenModal = (el) => { for (let e = el; e; e = e.parentElement) if (e.hasAttribute && e.hasAttribute('hidden')) return true; return false; };
const governing = (e) => { let x = e; while (x && !x.id) x = x.parentElement; return x ? x.id : ''; };

function render(pf, server = { webServer: true, compressAudio: true, advanced: true }) {
  const dom = new JSDOM(`<!doctype html><body>${body}</body>`);
  const doc = dom.window.document;
  doc.getElementById('setup').classList.remove('hide');
  if (pf.driver === 'sdrplay') {
    const hw = doc.getElementById('hw');
    for (const id of RSP_GENERATED) { const l = doc.createElement('label'); l.innerHTML = `<input type="checkbox" id="${id}">`; hw.appendChild(l); }
  }
  const { sup, adv } = offered(pf);
  const cfg = {
    configured: true, host: 'android', radios: [{ serial: 'android', driver: pf.driver, pin: '' }],
    android: { supported: sup, advancedOnly: adv, server,
      device: { batteryOptimisation: 'on', startOnPowerSupported: false, startOnPower: false, radioPresent: true,
                usbPermission: true, keepRadioAlive: true, lite: !!pf.lite, ...(pf.driver === 'icom' ? { micPermission: false } : {}) } },
  };
  new Function('document', '$', 'cfg', 'ANDROID', `${code}\nandroidPrune();\nthis.prune = androidPrune;`)
    .call(dom.window, doc, (id) => doc.getElementById(id), cfg, true);
  return { doc, sup, adv, prune: dom.window.prune };
}

const CASES = [
  { name: 'RTL-SDR', pf: { driver: 'rtl', tunnel: true } },
  { name: 'SDRplay RSP', pf: { driver: 'sdrplay', tunnel: true } },
  { name: 'Airspy HF+', pf: { driver: 'airspyhf', tunnel: true } },
  { name: 'Airspy R2', pf: { driver: 'airspy', tunnel: false } },
  { name: 'HackRF on Lite', pf: { driver: 'hackrf', lite: true, tunnel: false } },
  { name: 'Icom on a TV', pf: { driver: 'icom', tv: true, tunnel: true } },
];
for (const c of CASES) {
  const { doc, sup, adv } = render(c.pf);
  const setup = doc.getElementById('setup');
  const supSet = new Set(sup);
  let missing = sup.filter((id) => !doc.getElementById(id));
  ok(missing.length === 0, `${c.name}: every offered control is on the page${missing.length ? ' — missing ' + missing.join(' ') : ''}`);
  const hid = sup.filter((id) => doc.getElementById(id) && hiddenByPrune(doc.getElementById(id)));
  ok(hid.length === 0, `${c.name}: every offered control stays visible (Advanced)${hid.length ? ' — hidden ' + hid.join(' ') : ''}`);
  const left = [...setup.querySelectorAll('input,select,textarea')]
    .filter((e) => e.type !== 'hidden' && !hiddenByPrune(e) && !inHiddenModal(e) && !supSet.has(governing(e)));
  ok(left.length === 0, `${c.name}: no control the phone does not offer is left (${left.map((e) => '#' + (e.id || governing(e) || e.tagName)).slice(0, 12).join(' ')})`);
  // ★ The radio decides: the controls of OTHER radios are gone.
  const isCtrl = (el) => /^(INPUT|SELECT|TEXTAREA)$/.test(el.tagName) || el.classList.contains('mode');
  const notHere = ALL.filter((id) => !supSet.has(id) && doc.getElementById(id) && isCtrl(doc.getElementById(id)) && !hiddenByPrune(doc.getElementById(id)) && !inHiddenModal(doc.getElementById(id)));
  ok(notHere.length === 0, `${c.name}: no other radio's control survives${notHere.length ? ' — ' + notHere.join(' ') : ''}`);
  // ★★★ beta96 no Simple / Advanced on the phone any more: with the web client OFF (no admin page) the admin-page rows hide,
  //     the rest stay; with it on, everything the phone offers is shown (the stored `advanced` no longer decides).
  const s = render(c.pf, { webServer: false, compressAudio: true, advanced: true });
  const advShown = s.adv.filter((id) => s.doc.getElementById(id) && !hiddenByPrune(s.doc.getElementById(id)));
  ok(advShown.length === 0, `${c.name}: web client off hides every admin-page control${advShown.length ? ' — ' + advShown.join(' ') : ''}`);
  const restHidden = s.sup.filter((id) => !s.adv.includes(id) && s.doc.getElementById(id) && hiddenByPrune(s.doc.getElementById(id)));
  ok(restHidden.length === 0, `${c.name}: …and keeps the rest${restHidden.length ? ' — hidden ' + restHidden.join(' ') : ''}`);
}

// ── Specific promises, by radio ──
{
  const vis = (doc, id) => !!doc.getElementById(id) && !hiddenByPrune(doc.getElementById(id));
  const rtl = render({ driver: 'rtl', tunnel: true }).doc;
  ok(vis(rtl, 'ppm') && vis(rtl, 'adsb256') && vis(rtl, 'rtlAgc') && vis(rtl, 'androidTunerBw') && vis(rtl, 'androidDsFixed'), 'RTL: ppm, ADS-B 2.56, VibeAGC, the IF filter and the fixed branch are offered');
  ok(!vis(rtl, 'ppb') && !vis(rtl, 'icomBaud') && !vis(rtl, 'antennaMap'), 'RTL: no HF+ calibration, Icom settings or RSP socket map');
  ok(vis(rtl, 'dirList') && vis(rtl, 'place') && vis(rtl, 'rate') && vis(rtl, 'maxFps'), 'RTL: the listing, the location, the capture width and the waterfall rate');
  const ahf = render({ driver: 'airspyhf', tunnel: true }).doc;
  ok(vis(ahf, 'ppb') && !vis(ahf, 'ppm') && !vis(ahf, 'biasT') && !vis(ahf, 'convLo') && !vis(ahf, 'rtlAgc'), 'HF+: its ppb; no ppm, bias-T, converter or VibeAGC');
  const icom = render({ driver: 'icom', tv: true, tunnel: true }).doc;
  ok(vis(icom, 'icomBaud') && !vis(icom, 'rate') && !vis(icom, 'nbWide') && !vis(icom, 'batteryPauseAt') && !vis(icom, 'fmDeemph'), 'Icom on a TV: its own; no capture width, blanker, battery or FM defaults');
  const lite = render({ driver: 'hackrf', lite: true, tunnel: false }).doc;
  ok(!vis(lite, 'rawIqOn') && vis(lite, 'dabScanLabels') && !vis(lite, 'dirList'), 'Lite without the tunnel: no raw IQ, the DAB label scan, no listing');
  const rsp = render({ driver: 'sdrplay', tunnel: true }).doc;
  ok(vis(rsp, 'rfNotch') && vis(rsp, 'rspVibeAgc') && vis(rsp, 'antennaMap') && vis(rsp, 'ppm') && !vis(rsp, 'adsb256'), 'RSP: its card, sockets by band and ppm; none of the RTL\'s');
}

// ── The phone's own server card — ★★★ beta96 no Simple / Advanced any more: the full set follows the web client ──
{
  const { doc } = render({ driver: 'rtl', tunnel: true }, { webServer: true, compressAudio: false, advanced: false });
  const card = doc.getElementById('androidSrvCard');
  ok(!!card && card.parentElement && card.parentElement.id === 'serverPane', 'the "This phone\'s server" card is drawn on the server tab');
  ok(doc.getElementById('androidWebServer').checked && !doc.getElementById('androidCompress').checked && !doc.getElementById('androidAdvanced'),
     '…filled from the phone (web on, compression off), and no Simple / Advanced choice (beta96)');
  ok(!hiddenByPrune(doc.getElementById('users')) && !hiddenByPrune(doc.getElementById('gainMax')), 'web client on: the full set (listeners, gain limits) is shown');
  doc.getElementById('androidWebServer').checked = false;
  doc.getElementById('androidWebServer').dispatchEvent(new doc.defaultView.Event('change'));
  ok(hiddenByPrune(doc.getElementById('users')), 'no web client → the admin-page rows go with it (the admin page IS the web client)');
}

// ── The device-only facts ──
{
  const { doc } = render({ driver: 'icom', tunnel: true });
  const dev = doc.getElementById('androidDevCard');
  ok(!!dev && dev.parentElement && dev.parentElement.id === 'serverPane', 'the "This phone" card is drawn on the server tab');
  ok(dev && dev.querySelectorAll('input,select,textarea,button').length === 0, '…read-only: no controls in it');
  const t = dev ? dev.textContent : '';
  ok(/Battery optimisation/.test(t) && /Android may stop the server/.test(t), '…battery optimisation ON is said, with what it means');
  ok(/Not offered on this Android version/.test(t), '…start when power returns where Android does not allow it');
  ok(/USB permission/.test(t) && /Keep radio alive/.test(t), '…the USB permission and keep radio alive');
  ok(/Microphone/.test(t), '…the microphone permission for an Icom');
  ok(/Protocol/.test(t) && /rtl_tcp is chosen on the phone/.test(t), '…the protocol, and why it is the phone\'s');
  ok(/Remote access to server settings/.test(t) && /Admin password/.test(t) && /Never sent to this page/.test(t), '…the remote switch and the admin password, never sent');
  ok(/Share this radio/.test(t), '…and where on the phone to change them');
  const lite = render({ driver: 'rtl', lite: true }).doc.getElementById('androidDevCard');
  ok(lite && !/Protocol/.test(lite.textContent), 'Lite: no protocol row (it has no rtl_tcp)');
}

// ── The phone's radio rows are posted with the radio ──
{
  const { doc } = render({ driver: 'rtl', tunnel: true });
  const w = doc.defaultView;
  const run = (src) => new Function('document', '$', 'r', `${cut('androidRadioFill')}\n${cut('androidRadioCollect')}\n${src}`)(doc, (id) => doc.getElementById(id));
  run('androidRadioFill({ tunerBwAuto: true, directSampling: 2, converterInputLoHz: 1500000 });');
  ok(doc.getElementById('androidTunerBw').checked && doc.getElementById('androidDsFixed').value === '2' && doc.getElementById('androidConvInLo').value === '1.5',
     'the phone\'s radio rows are filled from the radio');
  const out = new Function('document', '$', `${cut('androidRadioCollect')}\nreturn androidRadioCollect();`)(doc, (id) => doc.getElementById(id));
  ok(out.androidPage === true && out.tunerBwAuto === true && out.directSampling === 2 && out.converterInputLoHz === 1500000, '…and posted back with the radio');
  void w;
}

// ── The sign-in path: on a phone it must stop before the Linux machine's loaders ──
const si = js.slice(js.indexOf('async function signIn('), js.indexOf('async function signIn(') + 6000);
const prune = si.indexOf('if (ANDROID) { androidPrune(); return; }');
ok(prune > 0, 'signIn returns into androidPrune on a phone');
for (const f of ['sdrChangesLoad()', 'wifiLoad()', 'winLoad()', 'serialStatus()', 'benchLoad()'])
  ok(si.indexOf(f) > prune, `…before ${f}`);
ok(/if \(!ANDROID\) pollDirStatus\(\)/.test(si), '…and the directory status poll is not started on a phone');
ok(/if \(ANDROID\) androidPrune\(\); \}\);/.test(cut('refreshHw')), 'the prune runs again once the radio\'s own rows are drawn (renderHw)');
ok(/\.\.\.\(ANDROID \? androidRadioCollect\(\) : \{\}\)/.test(cut('collectRadio')) && /\.\.\.\(ANDROID \? androidSrvCollect\(\) : \{\}\)/.test(cut('collect')),
   'collect / collectRadio carry the phone\'s own rows only on a phone');

console.log(fail ? `   ${pass} passed, ${fail} FAILED` : `   all ${pass} passed`);
process.exit(fail ? 1 : 0);
