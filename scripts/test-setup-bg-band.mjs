// test-setup-bg-band.mjs — the setup page's background-decoding editor with a BAND DIGITAL-SPOTS VFO (V12, 2026-10-27),
// its REAL code against a DOM stand-in (the harness of test-setup-bg-ism.mjs). Stuart: "Also full band digital modes
// too." A VFO row offers "Digital spots: Band (summary)" on USB only where this radio can run it (not on an Icom, not with
// digital spots or Band switched off here, not where the radio reaches no band); choosing it says which band the
// frequency names and what is decoded there, or why it cannot run; it saves as {"dec":"spots-band"} and reads back.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const a = src.indexOf('const BG_DECODERS'), b = src.indexOf('function collectRadio()');
if (a < 0 || b < a) { console.log('   FAIL the background decoding editor was not found in the page'); process.exit(1); }
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

class El {
  constructor(tag) { this.tag = tag; this.children = []; this.attrs = {}; this.textContent = ''; this.value = ''; this.checked = false; this.style = {};
    this.classList = { set: new Set(), toggle: (c, on) => (on ? this.classList.set.add(c) : this.classList.set.delete(c)), contains: (c) => this.classList.set.has(c), add: (c) => this.classList.set.add(c), remove: (c) => this.classList.set.delete(c) };
    this.options = this.children; }
  setAttribute(k, v) { this.attrs[k] = v; if (k === 'value') this.value = v; }
  appendChild(c) { this.children.push(c); if (this.tag === 'select' && this.children.length === 1 && !this._set) this.value = c.value; return c; }
  addEventListener(ev, f) { this['on' + ev] = f; }
  set innerHTML(_) { this.children.length = 0; }
  get innerHTML() { return ''; }
  all() { return [this, ...this.children.flatMap((c) => c.all())]; }
  text() { return this.textContent + this.children.map((c) => c.text()).join(' '); }
}
const mkSelect = () => { const s = new El('select'); let v = ''; Object.defineProperty(s, 'value', { get: () => v, set: (x) => { v = s.children.some((o) => o.value === x) ? x : (s.children[0]?.value ?? ''); }, configurable: true }); return s; };
const els = { bgProfiles: new El('div'), hwBg: new El('div'), bgOn: new El('input'), bgWait: mkSelect() };
for (const w of ['1', '2', '5', '10', '15', '30', '60']) { const o = new El('option'); o.value = w; els.bgWait.appendChild(o); }
const document = { createElement: (t) => (t === 'select' ? mkSelect() : new El(t)) };
const $ = (id) => els[id];
const cfg = { serverUtcOffsetMin: 60 };
let blocked = new Set();
const modeBlockSetStub = () => blocked;
const mk = (HW) => new Function('$', 'document', 'cfg', 'HW', 'modeBlockSet',
  `${src.slice(a, b)}\nreturn { bgLoad, bgCollect, bgRender, model: () => bgModel };`)($, document, cfg, HW, modeBlockSetStub);
const box = els.bgProfiles;
const vfoRows = () => box.all().filter((e) => e.attrs.class === 'bgVfo');
const decSel = (row) => row.children.filter((e) => e.tag === 'select').slice(-1)[0];
const modeSel = (row) => row.children.filter((e) => e.tag === 'select')[0];
const freqIn = (row) => row.children.find((e) => e.tag === 'input');
const note = () => box.all().find((e) => e.attrs.class === 'hint bgBandNote');

// the running radio's own list of bands (hwinfo spotBands), as an RSP on HF reports it
const B40 = { id: '40m', name: '40 m', hz: 7060600, lo: 7039650, hi: 7081550, bandLo: 7000000, bandHi: 7300000,
              modes: ['WSPR', 'PSK', 'FT4', 'FT2', 'FT8', 'JS8', 'JTTY'] };
const B160 = { id: '160m', name: '160 m', hz: 1840500, lo: 1836000, hi: 1845000, bandLo: 1800000, bandHi: 2000000, modes: ['FT8', 'WSPR', 'JS8', 'FT2'] };
const HW = { spotBands: [B160, B40] };
const LENOVO = { on: true, waitMin: 5, profiles: [{ centre: 1000000, from: '00:00', to: '00:00',
  vfos: [{ hz: 489500, mode: 'usb', dec: 'navtex' }, { hz: 1840000, mode: 'usb', dec: 'ft8' }] }] };

let api = mk(HW);
api.bgLoad({ driver: 'sdrplay', bgDecode: JSON.stringify(LENOVO) });
ok(vfoRows().length === 2, 'the Lenovo-style profile: NAVTEX + 160 m FT8, two rows');
ok(decSel(vfoRows()[1]).children.some((o) => o.value === 'spots-band'), 'a USB row offers "Digital spots: Band (summary)"');
ok(api.bgCollect() === JSON.stringify(LENOVO), 'an ordinary profile still saves back byte for byte');
modeSel(vfoRows()[1]).value = 'lsb'; modeSel(vfoRows()[1]).onchange();
ok(!decSel(vfoRows()[1]).children.some((o) => o.value === 'spots-band'), '…but not on LSB (no demodulator runs; Band is USB dials)');
modeSel(vfoRows()[1]).value = 'usb'; modeSel(vfoRows()[1]).onchange();
// 160 m FT8 → 160 m Band
decSel(vfoRows()[1]).value = 'spots-band'; decSel(vfoRows()[1]).onchange();
ok(!!note() && note().textContent.startsWith('160 m: FT8, WSPR, JS8, FT2 on 1.8360') && note().style.color === '',
   'choosing Band: the line names the band and its modes — ' + (note() && note().textContent));
const saved = api.bgCollect();
ok(saved.includes('{"hz":1840000,"mode":"usb","dec":"spots-band"}'), 'saves as {"dec":"spots-band"}: ' + saved);
api.bgLoad({ driver: 'sdrplay', bgDecode: saved });
ok(decSel(vfoRows()[1]).value === 'spots-band' && api.bgCollect() === saved, 'reads back as Band and saves back unchanged');
// a frequency on 40 m, then on no band
freqIn(vfoRows()[1]).value = '7074'; freqIn(vfoRows()[1]).oninput();
ok(note().textContent.startsWith('40 m: WSPR, PSK, FT4, FT2, FT8, JS8, JTTY on 7.0396') && note().textContent.includes('RF centre must hold'),
   'typing 7074: 40 m and every mode on it — ' + note().textContent);
freqIn(vfoRows()[1]).value = '6500'; freqIn(vfoRows()[1]).oninput();
ok(note().textContent.includes('not in a band') && note().style.color === '#ff8a7d', 'typing 6500: said in red, not in a band — ' + note().textContent);
freqIn(vfoRows()[1]).value = ''; freqIn(vfoRows()[1]).oninput();
ok(note().textContent.startsWith('Type any frequency in the band'), 'no frequency yet: how to name the band');
// back to another decoder: the line goes
decSel(vfoRows()[1]).value = 'ft8'; decSel(vfoRows()[1]).onchange();
ok(!note(), 'back to FT8: no Band line');

// where Band cannot run
blocked = new Set(['spotsband']);
api.bgLoad({ driver: 'sdrplay', bgDecode: JSON.stringify(LENOVO) });
ok(!decSel(vfoRows()[1]).children.some((o) => o.value === 'spots-band'), 'Band switched off for this radio (Modes and decoders): not offered');
api.bgLoad({ driver: 'sdrplay', bgDecode: saved });
ok(decSel(vfoRows()[1]).value === 'spots-band' && note() && note().style.color === '#ff8a7d' && note().textContent.includes('switched off'),
   '…a saved Band row stays visible and says plainly why it will not run');
blocked = new Set(['spots']);
api.bgLoad({ driver: 'sdrplay', bgDecode: JSON.stringify(LENOVO) });
ok(!decSel(vfoRows()[1]).children.some((o) => o.value === 'spots-band'), 'digital spots switched off altogether: not offered');
blocked = new Set();
api = mk({ spotBands: [] });
api.bgLoad({ driver: 'rtlsdr', bgDecode: JSON.stringify(LENOVO) });
ok(!decSel(vfoRows()[1]).children.some((o) => o.value === 'spots-band'), 'a running radio that reaches no band (an RTL without direct sampling): not offered');
api = mk(null);
api.bgLoad({ driver: 'rtlsdr', bgDecode: JSON.stringify(LENOVO) });
ok(decSel(vfoRows()[1]).children.some((o) => o.value === 'spots-band'), 'a radio not running (no hwinfo): offered — the save checks the band');

console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
