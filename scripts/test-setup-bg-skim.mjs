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

// the server's skimmer bands for Region 2 (hwinfo / GET /vibeserver/hardware "skimBands" — vibe_skim_bands.h points():
// the segment's tiles from the bottom at each width, then the region's practice segments, labelled)
const P = (...hz) => hz.map((h) => (Array.isArray(h) ? { hz: h[0], note: h[1] } : { hz: h }));
const SKIM = {
  region: 2,
  cw: [{ id: '160m', name: '160 m', lo: 1810000, hi: 1840000, points: { 24000: P(1822000, 1846000), 48000: P(1834000), 96000: P(1858000) } },
       { id: '60m', name: '60 m', lo: 5351500, hi: 5366500, points: { 24000: P([5363500, '']), 48000: P([5375500, '']), 96000: P([5399500, '']) } },
       { id: '40m', name: '40 m', lo: 7000000, hi: 7040000, points: { 24000: P(7012000, 7036000), 48000: P(7024000), 96000: P(7048000) } },
       { id: '20m', name: '20 m', lo: 14000000, hi: 14070000, points: { 24000: P(14012000, 14036000, 14060000), 48000: P(14024000, 14072000), 96000: P(14048000) } }],
  rtty: [{ id: '40m', name: '40 m', lo: 7040000, hi: 7050000, points: { 24000: P(7052000, [7092000, 'US RTTY'], [7116000, 'US RTTY']), 48000: P(7064000, [7104000, 'US RTTY']), 96000: P(7088000, [7128000, 'US RTTY']) } }],
  psk: [{ id: '40m', name: '40 m', lo: 7040000, hi: 7050000, points: { 6000: P(7043000, 7049000, [7073000, 'US PSK31']), 12000: P(7046000, [7076000, 'US PSK31']) } }],
};
const opts = (sel) => sel.all().filter((e) => e.tag === 'option');
const groups = (sel) => sel.children.filter((e) => e.tag === 'optgroup').map((g) => g.attrs.label);
const P40 = { on: true, waitMin: 5, profiles: [{ centre: 7000000, from: '00:00', to: '00:00',
  vfos: [{ hz: 7036000, mode: 'usb', dec: 'cw-skim-24' }, { hz: 7031500, mode: 'usb', dec: 'cw-skim-24' }, { hz: 7104000, mode: 'usb', dec: 'rtty-skim-48' }] }] };
let api = mk({ skimBands: SKIM });
api.bgLoad({ driver: 'sdrplay', rate: 2400000, bgDecode: JSON.stringify(P40) });
const rows = vfoRows;
ok(rows().length === 3, 'three skimmer rows (CW 24 kHz at 7036, CW 24 kHz own centre, RTTY 48 kHz at 7104)');
const s0 = skimSel(rows()[0]);
ok(!!s0 && groups(s0).join() === '40 m' && !freqIn(rows()[0]), 'the picker groups its windows by band (<optgroup>): only bands whose windows this capture (7.0 MHz, 2.4 MS/s) holds: 40 m; no kHz box — ' + groups(s0).join());
ok(opts(s0).map((o) => o.textContent).join(' | ') === '7000–7024 kHz | 7024–7048 kHz | Own centre…',
   'its windows: 40 m 7000–7024 and 7024–7048 (Stuart\'s "multiple centre points"), "Own centre…" last — ' + opts(s0).map((o) => o.textContent).join(' | '));
ok(s0.value === '7036000', 'a stored 7036 at 24 kHz loads as 40 m\'s SECOND window');
ok(modeSel(rows()[0]).disabled === true, '…its demodulator greyed (the decoder is fed IQ)');
ok(skimNote(0) && skimNote(0).textContent.startsWith('40 m CW 7024–7048 kHz') && skimNote(0).textContent.includes('part of the band’s CW segment'), 'its line: ' + (skimNote(0) && skimNote(0).textContent));
ok(skimSel(rows()[1]).value === '__own' && freqIn(rows()[1]) && freqIn(rows()[1]).value === '7031.5', 'a stored 7031.5 (no listed window) loads as "Own centre…" with the box filled');
ok(skimNote(1).textContent.startsWith('Your own centre: 7019.5–7043.5 kHz'), 'its line says what the typed centre covers — ' + skimNote(1).textContent);
const s2 = skimSel(rows()[2]);
ok(s2.value === '7104000' && opts(s2).some((o) => o.textContent === '7080–7128 kHz · US RTTY'), 'Region 2 RTTY 48 kHz at 7104: the US RTTY practice window, labelled');
ok(skimNote(2).textContent.includes('US RTTY, outside the band plan’s RTTY segment'), '…its line says it is practice, outside the plan — ' + skimNote(2).textContent);
ok(api.bgCollect() === JSON.stringify(P40), 'the profile saves back byte for byte (the stored value is still each window\'s centre)');
// a pick of a window
s0.value = '7012000'; s0.onchange();
ok(api.model().profiles[0].vfos[0].hz === 7012000 && skimSel(rows()[0]).value === '7012000', 'picking 7000–7024: stored as 7012');
// width change keeps the band (the nearest of its windows)
decSel(rows()[0]).value = 'cw-skim-96'; decSel(rows()[0]).onchange();
ok(api.model().profiles[0].vfos[0].hz === 7048000, 'CW 24 → 96 kHz on 40 m: its one window there, 7048 (7000–7096)');
// "Own centre…" at any time
skimSel(rows()[0]).value = '__own'; skimSel(rows()[0]).onchange();
ok(skimSel(rows()[0]).value === '__own' && freqIn(rows()[0]) && freqIn(rows()[0]).value === '7048', 'picking "Own centre…" shows the box, holding 7048');
freqIn(rows()[0]).value = '7050'; freqIn(rows()[0]).oninput();
ok(api.model().profiles[0].vfos[0].hz === 7050000 && skimNote(0).textContent.startsWith('Your own centre: 7002–7098 kHz'), 'typing 7050: stored, its line follows');
skimSel(rows()[1]).value = '7036000'; skimSel(rows()[1]).onchange();
ok(api.model().profiles[0].vfos[1].hz === 7036000 && !freqIn(rows()[1]), 'the own-centre row picks 7024–7048: 7036, the box gone');
// a NAVTEX row turned into an RTTY skimmer lands on the first window that fits
api.model().profiles[0].vfos.push({ hz: 0, mode: 'usb', dec: 'navtex' }); api.bgRender();
decSel(rows()[3]).value = 'rtty-skim-24'; decSel(rows()[3]).onchange();
ok(api.model().profiles[0].vfos[3].hz === 7052000, 'a new RTTY 24 kHz row lands on the plan\'s 7052 (the first window that fits)');
// only windows the capture holds; the chosen one kept but said outside
api.bgLoad({ driver: 'sdrplay', rate: 2400000, bgDecode: JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 14100000, from: '00:00', to: '00:00', vfos: [{ hz: 7024000, mode: 'usb', dec: 'cw-skim-48' }] }] }) });
const sf = skimSel(rows()[0]);
ok(groups(sf).join() === '40 m,20 m' && opts(sf)[0].textContent.includes('outside this window') && opts(sf).filter((o) => /^140/.test(o.textContent)).length === 2
   && skimNote(0).style.color === '#ff8a7d', 'centre 14.1 MHz: 20 m\'s two 48 kHz windows offered, the chosen 40 m kept but marked outside, said in red');
// 60 m: a plain window — no licence note, a receiver needs none (Stuart 11-21)
api.bgLoad({ driver: 'sdrplay', rate: 2400000, bgDecode: JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 5400000, from: '00:00', to: '00:00', vfos: [{ hz: 5363500, mode: 'usb', dec: 'cw-skim-24' }] }] }) });
ok(skimSel(rows()[0]).value === '5363500' && groups(skimSel(rows()[0])).includes('60 m') && opts(skimSel(rows()[0])).some((o) => o.textContent === '5351.5\u20135375.5 kHz')
   && !/licen[cs]e/i.test(skimNote(0).textContent), '60 m: one window 5351.5–5375.5, no licence note — we only receive (Stuart 11-21) — ' + skimNote(0).textContent);
// an older server (no points): the one rule-centre per band
api = mk({ skimBands: { region: 1, cw: [{ id: '40m', name: '40 m', lo: 7000000, hi: 7040000 }], rtty: [], psk: [] } });
api.bgLoad({ driver: 'sdrplay', rate: 2400000, bgDecode: JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 7000000, from: '00:00', to: '00:00', vfos: [{ hz: 7024000, mode: 'usb', dec: 'cw-skim-48' }] }] }) });
ok(skimSel(rows()[0]).value === '7024000', 'an older server\'s skimBands (no points): its one window, segment bottom + width / 2');
api = mk({ skimBands: SKIM });
// where a skimmer cannot run: not offered
blocked = new Set(['rtty']);
api.bgLoad({ driver: 'sdrplay', rate: 2400000, bgDecode: JSON.stringify(P40) });
ok(!decSel(rows()[0]).children.some((o) => o.value.startsWith('rtty-skim')), 'RTTY switched off here: no RTTY skimmer offered on other rows');
ok(decSel(rows()[2]).value === 'rtty-skim-48' && skimNote(2).style.color === '#ff8a7d' && skimNote(2).textContent.includes('switched off'), '…a saved RTTY skimmer row stays and says why');
blocked = new Set();
api.bgLoad({ driver: 'icom', bgDecode: JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 7000000, from: '00:00', to: '00:00', vfos: [{ hz: 7000000, mode: 'usb', dec: 'navtex' }] }] }) });
ok(!decSel(rows()[0]).children.some((o) => /skim-/.test(o.value) && o.value !== 'ndb-skim'), 'an Icom (one audio channel): no CW / RTTY / PSK skimmer offered');
api = mk(null);
api.bgLoad({ driver: 'sdrplay', rate: 2400000, bgDecode: JSON.stringify(P40) });
ok(!skimSel(rows()[0]) && freqIn(rows()[0]) && freqIn(rows()[0]).value === '7036', 'no skimBands: the typed centre box, as before');
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
