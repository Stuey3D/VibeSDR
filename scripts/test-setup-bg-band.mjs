// test-setup-bg-band.mjs — the setup page's background-decoding editor with a BAND DIGITAL-SPOTS VFO (V12, 2026-10-27),
// its REAL code against a DOM stand-in (the harness of test-setup-bg-ism.mjs). Stuart: "Also full band digital modes
// too." A VFO row offers "Digital spots: Band (summary)" on USB only where this radio can run it (not on an Icom, not with
// digital spots or Band switched off here, not where the radio reaches no band); choosing it says which band and what is
// decoded there, or why it cannot run; it saves as {"dec":"spots-band"} and reads back.
// ★★ 2026-10-27 (Stuart): the decoder sets the frequency — a BAND PICKER (only the bands the profile's capture holds) in
// place of the frequency box, the demodulator greyed out; an older row's typed frequency loads into its band.
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
const decSel = (row) => row.children.find((e) => e.attrs.class === 'bgDec');
const modeSel = (row) => row.children.find((e) => e.attrs.class === 'bgDemod');
const freqIn = (row) => row.children.find((e) => e.tag === 'input');
const bandSel = (row) => row.children.find((e) => e.attrs.class === 'bgBandPick');
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
// ★★ V12 2026-10-27 (Stuart): the decoder sets the frequency — a BAND PICKER in place of the box, the demodulator greyed out
ok(!freqIn(vfoRows()[1]) && !!bandSel(vfoRows()[1]) && bandSel(vfoRows()[1]).value === '160m', 'a Band row has a band picker, no frequency box (160 m)');
ok(modeSel(vfoRows()[1]).disabled === true && modeSel(vfoRows()[1]).value === 'usb', '…and its demodulator is greyed out');
ok(note().textContent.includes('decoder sets the frequency'), '…said on its line');
const saved = api.bgCollect();
ok(saved.includes('{"hz":1840000,"mode":"usb","dec":"spots-band"}'), 'an older row\'s frequency inside the band is kept as it was: ' + saved);
api.bgLoad({ driver: 'sdrplay', bgDecode: saved });
ok(decSel(vfoRows()[1]).value === 'spots-band' && bandSel(vfoRows()[1]).value === '160m' && api.bgCollect() === saved, 'reads back into 160 m and saves back unchanged');
bandSel(vfoRows()[1]).value = '40m'; bandSel(vfoRows()[1]).onchange();
ok(note().textContent.startsWith('40 m: WSPR, PSK, FT4, FT2, FT8, JS8, JTTY on 7.0396') && api.bgCollect().includes('{"hz":7060600,"mode":"usb","dec":"spots-band"}'),
   'picking 40 m: every mode on it, stored as the band\'s dial — ' + note().textContent);
// back to another decoder: the line goes, the box comes back
decSel(vfoRows()[1]).value = 'ft8'; decSel(vfoRows()[1]).onchange();
ok(!note() && !!freqIn(vfoRows()[1]) && !modeSel(vfoRows()[1]).disabled, 'back to FT8: no Band line, the frequency box and demodulator are back');

// ★★ ONLY THE BANDS THIS CAPTURE HOLDS: Stuart's RSP at 1200 kHz, 3 MS/s — 160 m yes, 40 m no
const AT1200 = { on: true, waitMin: 5, profiles: [{ centre: 1200000, from: '00:00', to: '00:00',
  vfos: [{ hz: 489500, mode: 'usb', dec: 'navtex' }, { hz: 1840500, mode: 'usb', dec: 'spots-band' }] }] };
api.bgLoad({ driver: 'sdrplay', rate: 3000000, bgDecode: JSON.stringify(AT1200) });
ok(bandSel(vfoRows()[1]).children.map((o) => o.value).join() === '160m', 'at 1200 kHz / 3 MS/s the picker offers 160 m only: ' + bandSel(vfoRows()[1]).children.map((o) => o.value).join());
ok(api.bgCollect() === JSON.stringify(AT1200), '…and the profile saves back byte for byte');
const ci = box.all().find((e) => e.tag === 'input' && e.attrs.placeholder === '500');
ci.value = '7000'; ci.oninput();
ok(bandSel(vfoRows()[1]).children.map((o) => o.value).join() === '160m,40m' && bandSel(vfoRows()[1]).children[0].textContent.includes('outside this window')
   && note().style.color === '#ff8a7d', 'the centre moved to 7 MHz: 40 m appears, the chosen 160 m is kept but marked outside, said in red');
ci.value = '1200'; ci.oninput();
// a NAVTEX row turned into Band lands on a band this capture holds — never a stray frequency
decSel(vfoRows()[0]).value = 'spots-band'; decSel(vfoRows()[0]).onchange();
ok(bandSel(vfoRows()[0]).value === '160m' && api.model().profiles[0].vfos[0].hz === 1840500, 'a NAVTEX row turned into Band lands on 160 m (the band\'s dial)');
// the radio offline (no band list): the old typed frequency, which names its band
api = mk(null);
api.bgLoad({ driver: 'sdrplay', bgDecode: saved });
ok(!!freqIn(vfoRows()[1]) && !freqIn(vfoRows()[1]).disabled && modeSel(vfoRows()[1]).disabled, 'no band list (radio not running): a typed frequency names the band; the demodulator stays greyed');
freqIn(vfoRows()[1]).value = ''; freqIn(vfoRows()[1]).oninput();
ok(note().textContent.startsWith('Type any frequency in the band'), 'no frequency yet: how to name the band');
api = mk(HW);

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
