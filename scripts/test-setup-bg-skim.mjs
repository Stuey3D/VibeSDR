// test-setup-bg-skim.mjs — the setup page's background-decoding editor: THE SKIMMERS BY BAND (V12 beta90, Stuart 2026-11-21:
// "cw skimmer and rtty skimmer should be per band rather than entering a centre frequency", "48k should default to 7024
// for 40m 96k 7048", and "background should have the band presets but with an option to set their own centre"). Its REAL
// code against the DOM stand-in of test-setup-bg-band.mjs: a CW / RTTY / PSK skimmer row picks a BAND from the server's
// skimBands (window = segment bottom + width / 2), or "Own centre…" with the kHz box; a stored centre that is no band's
// loads as the owner's own; only bands whose window the capture holds; not offered where it cannot run.

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

const skimSel = (row) => row.children.find((e) => e.attrs.class === 'bgSkimPick');
const skimNote = (i) => box.all().filter((e) => e.attrs.class === 'hint bgSkimNote')[i];

// the server's skimmer segments for Region 1 (hwinfo / GET /vibeserver/hardware "skimBands" — vibe_skim_bands.h)
const SKIM = {
  region: 1,
  cw: [{ id: '160m', name: '160 m', lo: 1810000, hi: 1838000 }, { id: '40m', name: '40 m', lo: 7000000, hi: 7040000 }, { id: '20m', name: '20 m', lo: 14000000, hi: 14070000 }],
  rtty: [{ id: '40m', name: '40 m', lo: 7040000, hi: 7050000 }, { id: '20m', name: '20 m', lo: 14070000, hi: 14099000 }],
  psk: [{ id: '40m', name: '40 m', lo: 7040000, hi: 7050000 }, { id: '20m', name: '20 m', lo: 14070000, hi: 14099000 }],
};
const P40 = { on: true, waitMin: 5, profiles: [{ centre: 7000000, from: '00:00', to: '00:00',
  vfos: [{ hz: 7024000, mode: 'usb', dec: 'cw-skim-48' }, { hz: 7031500, mode: 'usb', dec: 'cw-skim-24' }, { hz: 7064000, mode: 'usb', dec: 'rtty-skim-48' }] }] };
let api = mk({ skimBands: SKIM });
api.bgLoad({ driver: 'sdrplay', rate: 2400000, bgDecode: JSON.stringify(P40) });
const rows = vfoRows;
ok(rows().length === 3, 'three skimmer rows (CW 48 kHz, CW 24 kHz own centre, RTTY 48 kHz)');
ok(!!skimSel(rows()[0]) && skimSel(rows()[0]).value === '40m' && !freqIn(rows()[0]), 'CW 48 kHz at 7024: the band picker on 40 m, no kHz box (Stuart: "48k should default to 7024 for 40m")');
ok(modeSel(rows()[0]).disabled === true, '…its demodulator greyed (the decoder is fed IQ)');
ok(skimNote(0) && skimNote(0).textContent.startsWith('40 m CW 7000–7048 kHz') && skimNote(0).style.color === '', 'its line: "40 m CW 7000–7048 kHz — every CW signal…" — ' + (skimNote(0) && skimNote(0).textContent));
ok(skimSel(rows()[1]).value === '__own' && freqIn(rows()[1]) && freqIn(rows()[1]).value === '7031.5', 'a stored 7031.5 (no band\'s centre) loads as "Own centre…" with the box filled');
ok(skimSel(rows()[1]).children.some((o) => o.value === '__own' && o.textContent === 'Own centre…'), '…the picker\'s last option is "Own centre…"');
ok(skimNote(1).textContent.startsWith('Your own centre: 7019.5–7043.5 kHz'), 'its line says what the typed centre covers — ' + skimNote(1).textContent);
ok(skimSel(rows()[2]).value === '40m' && skimNote(2).textContent.startsWith('40 m RTTY 7040–7088 kHz'), 'RTTY 48 kHz at 7064: 40 m (7040 + 24) — ' + skimNote(2).textContent);
ok(api.bgCollect() === JSON.stringify(P40), 'the profile saves back byte for byte (the stored value is still each window\'s centre)');
// a wider CW window keeps its band: 7024 → 7048
decSel(rows()[0]).value = 'cw-skim-96'; decSel(rows()[0]).onchange();
ok(api.model().profiles[0].vfos[0].hz === 7048000 && skimSel(rows()[0]).value === '40m', 'CW 48 → 96 kHz on 40 m: the window moves to 7048 (7000–7096)');
// "Own centre…" at any time: the box appears with the centre it had
skimSel(rows()[0]).value = '__own'; skimSel(rows()[0]).onchange();
ok(skimSel(rows()[0]).value === '__own' && freqIn(rows()[0]) && freqIn(rows()[0]).value === '7048', 'picking "Own centre…" shows the box, holding 7048');
freqIn(rows()[0]).value = '7050'; freqIn(rows()[0]).oninput();
ok(api.model().profiles[0].vfos[0].hz === 7050000 && skimNote(0).textContent.startsWith('Your own centre: 7002–7098 kHz'), 'typing 7050: stored, and its line follows — ' + skimNote(0).textContent);
// back to a band replaces the typed value
skimSel(rows()[1]).value = '40m'; skimSel(rows()[1]).onchange();
ok(api.model().profiles[0].vfos[1].hz === 7012000 && !freqIn(rows()[1]), 'the own-centre row picks 40 m: 7012 (24 kHz), the box gone');
// a NAVTEX row turned into an RTTY skimmer lands on the first band this capture holds
api.model().profiles[0].vfos.push({ hz: 0, mode: 'usb', dec: 'navtex' }); api.bgRender();
decSel(rows()[3]).value = 'rtty-skim-24'; decSel(rows()[3]).onchange();
ok(api.model().profiles[0].vfos[3].hz === 7052000 && skimSel(rows()[3]).value === '40m', 'a new RTTY 24 kHz row lands on 40 m, 7052 (the first band whose window fits)');
// only the bands whose window this capture holds; the chosen one kept but said outside
api.bgLoad({ driver: 'sdrplay', rate: 2400000, bgDecode: JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 14100000, from: '00:00', to: '00:00', vfos: [{ hz: 7024000, mode: 'usb', dec: 'cw-skim-48' }] }] }) });
ok(skimSel(rows()[0]).children.map((o) => o.value).join() === '40m,20m,__own' && skimSel(rows()[0]).children[0].textContent.includes('outside this window')
   && skimNote(0).style.color === '#ff8a7d', 'centre 14.1 MHz: 20 m offered, the chosen 40 m kept but marked outside, said in red — ' + skimNote(0).textContent);
// where a skimmer cannot run: not offered
blocked = new Set(['rtty']);
api.bgLoad({ driver: 'sdrplay', rate: 2400000, bgDecode: JSON.stringify(P40) });
ok(!decSel(rows()[0]).children.some((o) => o.value.startsWith('rtty-skim')), 'RTTY switched off here: no RTTY skimmer offered on other rows');
ok(decSel(rows()[2]).value === 'rtty-skim-48' && skimNote(2).style.color === '#ff8a7d' && skimNote(2).textContent.includes('switched off'), '…a saved RTTY skimmer row stays and says why');
blocked = new Set();
api.bgLoad({ driver: 'icom', bgDecode: JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 7000000, from: '00:00', to: '00:00', vfos: [{ hz: 7000000, mode: 'usb', dec: 'navtex' }] }] }) });
ok(!decSel(rows()[0]).children.some((o) => /skim-/.test(o.value) && o.value !== 'ndb-skim'), 'an Icom (one audio channel): no CW / RTTY / PSK skimmer offered');
// no band list (the server did not answer): the kHz box, as before
api = mk(null);
api.bgLoad({ driver: 'sdrplay', rate: 2400000, bgDecode: JSON.stringify(P40) });
ok(!skimSel(rows()[0]) && freqIn(rows()[0]) && freqIn(rows()[0]).value === '7024', 'no skimBands: the typed centre box, as before');
// GET /vibeserver/hardware and hwinfo carry it; the page takes the door's
{
  const shim = fs.readFileSync(new URL('../android/app/src/main/cpp/local_sdr_shim.cpp', import.meta.url), 'utf8');
  const h0 = shim.indexOf('reqLine.rfind("GET /vibeserver/hardware", 0) == 0) {');
  const h1 = shim.indexOf('reqLine.rfind("GET /vibeserver/rate-guard", 0) == 0', h0);
  const hw = h0 > 0 && h1 > h0 ? shim.slice(h0, h1) : '';
  ok(/vsSkimBandsJson\(true\)/.test(hw) && /vsSkimBandsJson\(false\)/.test(hw), 'GET /vibeserver/hardware carries skimBands (the radio\'s, and the door\'s whole list)');
  ok(/skimBands: own\.skimBands/.test(src), 'renderHw takes the door\'s skimBands for a radio that did not answer');
  ok(/PSK Reporter[\s\S]{0,900}RTTY skimmer/.test(src), 'the PSK Reporter card says which decoders report (the RTTY skimmer among them)');
}

console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
