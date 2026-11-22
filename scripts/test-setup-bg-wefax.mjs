// test-setup-bg-wefax.mjs — the setup page's background-decoding editor: WEFAX STATIONS (V12 2026-11-22, Stuart: "i'd also
// like the other wefax frequencies covered"). Its REAL code against the DOM stand-in of test-setup-bg-skim.mjs: a "WEFAX
// stations" row has no frequency box — it TICKS the stations' frequencies from the SERVER's list (HW.wefaxStations,
// vibe_wefax_stations.h, only what the radio tunes), says which are outside this window, stops at 8, defaults to what the
// window holds when a row becomes one, saves "fax":[Hz…] byte for byte, and is not offered where the server lists none.
// The /hardware and caps writers carry the list. Run: node scripts/test-setup-bg-wefax.mjs
import fs from 'node:fs';
const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const a = src.indexOf('const BG_DECODERS'), b = src.indexOf('function collectRadio()');
if (a < 0 || b < a) { console.log('   FAIL the background decoding editor was not found in the page'); process.exit(1); }
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

class El {
  constructor(tag) { this.tag = tag; this.children = []; this.attrs = {}; this.textContent = ''; this.value = ''; this.checked = false; this.disabled = false; this.style = {};
    this.classList = { set: new Set(), toggle: (c, on) => (on ? this.classList.set.add(c) : this.classList.set.delete(c)), contains: (c) => this.classList.set.has(c), add: (c) => this.classList.set.add(c), remove: (c) => this.classList.set.delete(c) };
    this.options = this.children; }
  setAttribute(k, v) { this.attrs[k] = v; if (k === 'value') this.value = v; }
  appendChild(c) { this.children.push(c); if (this.tag === 'select' && this.children.length === 1 && !this._set && c.tag === 'option') this.value = c.value; return c; }
  addEventListener(ev, f) { this['on' + ev] = f; }
  set innerHTML(_) { this.children.length = 0; }
  get innerHTML() { return ''; }
  all() { return [this, ...this.children.flatMap((c) => c.all())]; }
  text() { return this.textContent + this.children.map((c) => c.text()).join(' '); }
}
const mkSelect = () => { const s = new El('select'); let v = ''; const os = () => s.all().filter((e) => e.tag === 'option'); Object.defineProperty(s, 'value', { get: () => v, set: (x) => { v = os().some((o) => o.value === x) ? x : (os()[0]?.value ?? ''); }, configurable: true }); return s; };
const els = { bgProfiles: new El('div'), hwBg: new El('div'), bgOn: new El('input'), bgWait: mkSelect() };
for (const w of ['1', '2', '5', '10', '15', '30', '60']) { const o = new El('option'); o.value = w; els.bgWait.appendChild(o); }
const document = { createElement: (t) => (t === 'select' ? mkSelect() : new El(t)) };
const $ = (id) => els[id];
const cfg = { serverUtcOffsetMin: 60 };
const mk = (HW) => new Function('$', 'document', 'cfg', 'HW', 'modeBlockSet',
  `${src.slice(a, b)}\nreturn { bgLoad, bgCollect, bgRender, model: () => bgModel };`)($, document, cfg, HW, () => new Set());
const box = els.bgProfiles;
const rows = () => box.all().filter((e) => e.attrs.class === 'bgVfo');
const decSel = (row) => row.children.find((e) => e.attrs.class === 'bgDec');
const freqIn = (row) => row.children.find((e) => e.tag === 'input');
const ticks = () => box.all().filter((e) => e.attrs.class === 'bgFaxTick');
const tickText = (t) => box.all().find((e) => e.attrs.class === 'bgFaxFreq' && e.children[0] === t).children[1].textContent;
const note = () => box.all().find((e) => e.attrs.class === 'hint bgFaxNote');

// the server's list as GET /vibeserver/hardware sends it for an HF radio (vibe_wefax_stations.h stationsJson)
const STATIONS = [
  { id: 'gya', call: 'GYA', name: 'Northwood', country: 'United Kingdom', freqs: [2618500, 4610000, 8040000, 11086500] },
  { id: 'dwd', call: 'DDH3 / DDK3 / DDK6', name: 'Pinneberg (DWD)', country: 'Germany', freqs: [3855000, 7880000, 13882500] },
  { id: 'nmf', call: 'NMF', name: 'Boston', country: 'USA', freqs: [4235000, 6340500, 9110000, 12750000] },
];
const P = { on: true, waitMin: 5, profiles: [{ centre: 6000000, from: '00:00', to: '00:00',
  vfos: [{ hz: 0, mode: 'usb', dec: 'wefax-stations', fax: [4610000, 7880000, 8040000] }, { hz: 4608100, mode: 'usb', dec: 'wefax' }] }] };
let api = mk({ wefaxStations: STATIONS });
api.bgLoad({ driver: 'sdrplay', rate: 6000000, bgDecode: JSON.stringify(P) });
ok(rows().length === 2 && decSel(rows()[0]).value === 'wefax-stations', 'a WEFAX stations row (and an ordinary single WEFAX VFO beside it)');
ok(!freqIn(rows()[0]) && freqIn(rows()[1]) && freqIn(rows()[1]).value === '4608.1', '★ no frequency box on the stations row (its ticks are below); the single WEFAX VFO keeps its 4608.1 box');
ok(ticks().length === 11, 'every frequency the server lists is a tick box (4 + 3 + 4 = 11)');
const on = ticks().filter((t) => t.checked).map((t) => +t.value);
ok(on.join() === '4610000,8040000,7880000', 'the stored ticks load ticked: ' + on.join());
const t2618 = ticks().find((t) => t.value === '2618500');
ok(tickText(t2618) === '2618.5 (outside this window)' && tickText(ticks().find((t) => t.value === '4610000')) === '4610.0',
   'centre 6 MHz at 6 MS/s: 2618.5 is said to be outside this window, 4610.0 is not');
ok(note() && note().textContent.startsWith('Decodes 3 of the 3 ticked in one decoder slot'), 'its line: ' + (note() && note().textContent));
ok(api.bgCollect() === JSON.stringify(P), '★ the profile saves back byte for byte ("fax":[Hz…] on the stations row only)');
// tick and untick
const t6340 = ticks().find((t) => t.value === '6340500'); t6340.checked = true; t6340.onchange();
ok(api.model().profiles[0].vfos[0].fax.join() === '4610000,7880000,8040000,6340500', 'ticking Boston 6340.5 adds it');
const t4610 = ticks().find((t) => t.value === '4610000'); t4610.checked = false; t4610.onchange();
ok(!api.model().profiles[0].vfos[0].fax.includes(4610000), 'unticking 4610 removes it');
// the cap of 8
for (const t of ticks()) if (!t.checked && api.model().profiles[0].vfos[0].fax.length < 8) { t.checked = true; t.onchange(); }
ok(api.model().profiles[0].vfos[0].fax.length === 8 && ticks().filter((t) => !t.checked).every((t) => t.disabled), '★ at 8 ticked, the rest are greyed (the server refuses more)');
// a new row becoming WEFAX stations: what this window holds, ticked
api.bgLoad({ driver: 'sdrplay', rate: 2000000, bgDecode: JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 4400000, from: '00:00', to: '00:00', vfos: [{ hz: 517500, mode: 'usb', dec: 'navtex' }] }] }) });
decSel(rows()[0]).value = 'wefax-stations'; decSel(rows()[0]).onchange();
ok(api.model().profiles[0].vfos[0].hz === 0 && api.model().profiles[0].vfos[0].fax.join() === '4610000,3855000,4235000',
   '★ a row turned into WEFAX stations ticks what 4.4 MHz ± ~0.7 MHz holds: 4610, 3855, 4235 — ' + api.model().profiles[0].vfos[0].fax.join());
decSel(rows()[0]).value = 'wefax'; decSel(rows()[0]).onchange();
ok(!('fax' in api.model().profiles[0].vfos[0]), '…and back to a single WEFAX VFO: its ticks are gone');
// not offered where the server lists no station (an older server, a radio that reaches none)
api = mk({ wefaxStations: [] });
api.bgLoad({ driver: 'sdrplay', rate: 2000000, bgDecode: JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 4400000, from: '00:00', to: '00:00', vfos: [{ hz: 4608100, mode: 'usb', dec: 'wefax' }] }] }) });
ok(!decSel(rows()[0]).children.some((o) => o.value === 'wefax-stations') && decSel(rows()[0]).children.some((o) => o.value === 'wefax'),
   '★ no station this radio reaches: WEFAX stations is not offered (the single WEFAX VFO still is)');
api.bgLoad({ driver: 'sdrplay', rate: 2000000, bgDecode: JSON.stringify(P) });
ok(decSel(rows()[0]).value === 'wefax-stations' && note().style.color === '#ff8a7d' && note().textContent.includes('reaches none'), '…a saved stations row stays and says why');
// the server writes the list (one writer) for /hardware and the Android screen's caps
{
  const shim = fs.readFileSync(new URL('../android/app/src/main/cpp/local_sdr_shim.cpp', import.meta.url), 'utf8');
  const h0 = shim.indexOf('reqLine.rfind("GET /vibeserver/hardware", 0) == 0) {');
  const h1 = shim.indexOf('reqLine.rfind("GET /vibeserver/rate-guard", 0) == 0', h0);
  const hw = h0 > 0 && h1 > h0 ? shim.slice(h0, h1) : '';
  ok(/vsWefaxStationsJson\(true\)/.test(hw) && /vsWefaxStationsJson\(false\)/.test(hw), 'GET /vibeserver/hardware carries wefaxStations (the radio\'s, and the door\'s whole list)');
  const c0 = shim.indexOf('std::string LocalSdrShim::backgroundCapsJson()');
  ok(c0 > 0 && /vsWefaxStationsJson\(radioOpen\)/.test(shim.slice(c0, c0 + 6000)) && /"wefax-stations"\) j \+= ",\\"fax\\":true"/.test(shim.slice(c0, c0 + 6000)),
     'backgroundCapsJson carries it too, and marks the decoder fax:true');
  ok(/wefaxStations: own\.wefaxStations/.test(src), 'renderHw takes the door\'s list for a radio that did not answer');
}
console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
