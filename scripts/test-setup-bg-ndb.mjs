// test-setup-bg-ndb.mjs — the setup page's background-decoding editor with the NDB SKIMMER VFO (V12, 2026-10-27), its
// REAL code against a DOM stand-in (test-setup-bg-ism.mjs's harness). Stuart: the NDB skimmer is a VFO decoder, beside
// NAVTEX and the 160 m digital modes on the same capture — not a whole-radio profile kind (that option is gone).
//   · "NDB skimmer (summary)" is in a VFO row's decoder list only where this radio reaches LF (an HF+ or an RSP; an RTL
//     only with direct sampling on or automatic; the running radio's own HW.ndbHw first; never a HackRF, an Airspy R2,
//     an Icom, or with NDB switched off) — and only once a profile;
//   · its row's frequency box and demodulator are GREYED OUT (disabled — the decoder sets them), with a line saying what
//     part of the band the RF centre holds, or why it cannot run;
//   · Stuart's Lenovo profile (NAVTEX 489.5 + 517.5, 160 m Band, NDB) at 1200 kHz saves and reads back unchanged;
//   · an older whole-radio {"ndb":true} profile loads as a VFO profile with the NDB skimmer;
//   · "Decodes" never offers NDB any more.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const a = src.indexOf('const BG_DECODERS'), b = src.indexOf('function collectRadio()');
if (a < 0 || b < a) { console.log('   FAIL the background decoding editor was not found in the page'); process.exit(1); }
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

class El {
  constructor(tag) { this.tag = tag; this.children = []; this.attrs = {}; this.textContent = ''; this.value = ''; this.checked = false; this.style = {}; this.disabled = false;
    this.classList = { set: new Set(), toggle: (c, on) => (on ? this.classList.set.add(c) : this.classList.set.delete(c)), contains: (c) => this.classList.set.has(c) };
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
const els = { bgProfiles: new El('div'), hwBg: new El('div'), bgOn: new El('input'), bgWait: mkSelect(), bgNdbLine: new El('span') };
for (const w of ['1', '2', '5', '10', '15', '30', '60']) { const o = new El('option'); o.value = w; els.bgWait.appendChild(o); }
const document = { createElement: (t) => (t === 'select' ? mkSelect() : new El(t)) };
const $ = (id) => els[id];
const cfg = { serverUtcOffsetMin: 60 };
let blocked = new Set();
const modeBlockSetStub = () => blocked;
const mk = (HW) => new Function('$', 'document', 'cfg', 'HW', 'modeBlockSet',
  `${src.slice(a, b)}\nreturn { bgLoad, bgCollect, bgRender, model: () => bgModel };`)($, document, cfg, HW, modeBlockSetStub);
const box = els.bgProfiles;
const sel = (cls) => box.all().find((e) => e.tag === 'select' && e.attrs.class === cls);
const vfoRows = () => box.all().filter((e) => e.attrs.class === 'bgVfo');
const decSel = (row) => row.children.find((e) => e.attrs.class === 'bgDec');
const modeSel = (row) => row.children.find((e) => e.attrs.class === 'bgDemod');
const freqIn = (row) => row.children.find((e) => e.tag === 'input');
const offers = (row) => decSel(row).children.some((o) => o.value === 'ndb-skim');
const note = () => box.all().find((e) => e.attrs.class === 'hint bgNdbNote');

const NICK = { on: true, waitMin: 5, profiles: [{ centre: 500000, from: '00:00', to: '00:00',
  vfos: [{ hz: 489500, mode: 'usb', dec: 'navtex' }, { hz: 517500, mode: 'usb', dec: 'navtex' }] }] };
const B160 = { id: '160m', name: '160 m', hz: 1840500, lo: 1836000, hi: 1845000, bandLo: 1800000, bandHi: 2000000, modes: ['FT8', 'WSPR', 'JS8', 'FT2'] };
const LENOVO = { on: true, waitMin: 5, profiles: [{ centre: 1200000, from: '00:00', to: '00:00', vfos: [
  { hz: 489500, mode: 'usb', dec: 'navtex' }, { hz: 517500, mode: 'usb', dec: 'navtex' },
  { hz: 1840500, mode: 'usb', dec: 'spots-band' }, { hz: 0, mode: 'usb', dec: 'ndb-skim' }] }] };

let api = mk(null);
api.bgLoad({ driver: 'airspyhf', rate: 768000, bgDecode: JSON.stringify(NICK) });
ok(!sel('bgKind'), 'an Airspy HF+: no "Decodes" choice at all — NDB is not a profile kind any more (and it reaches no ISM)');
ok(offers(vfoRows()[1]), 'a VFO row offers "NDB skimmer (summary)"');
ok(api.bgCollect() === JSON.stringify(NICK), 'an ordinary profile saves back byte for byte');
ok(!els.bgNdbLine.classList.contains('hide'), "the headline's NDB sentence is shown where it can run");
decSel(vfoRows()[1]).value = 'ndb-skim'; decSel(vfoRows()[1]).onchange();
let r = vfoRows()[1];
ok(decSel(r).value === 'ndb-skim', 'VFO 2 is now the NDB skimmer');
ok(freqIn(r).disabled === true && freqIn(r).value === '' && freqIn(r).attrs.placeholder === 'auto', 'its frequency box is greyed out (disabled, "auto")');
ok(modeSel(r).disabled === true && modeSel(r).value === 'usb', '…and its demodulator too');
ok(!!note() && note().textContent.startsWith('Covers 190–535 kHz of the NDB band') && note().textContent.includes('set by the decoder') && note().style.color === '',
   'the line: what it covers at 500 kHz / 768 kS/s, and that the decoder sets it — ' + (note() && note().textContent));
ok(!offers(vfoRows()[0]), 'VFO 1 no longer offers it — one NDB skimmer a profile');
const saved = api.bgCollect();
ok(saved.includes('{"hz":0,"mode":"usb","dec":"ndb-skim"}') && !saved.includes('"ndb":'), 'saves as {"hz":0,"dec":"ndb-skim"}: ' + saved);
api.bgLoad({ driver: 'airspyhf', rate: 768000, bgDecode: saved });
ok(decSel(vfoRows()[1]).value === 'ndb-skim' && api.bgCollect() === saved, 'reads back as the NDB skimmer and saves back unchanged');
// the centre moved far off the band: said in red
const ci = box.all().find((e) => e.tag === 'input' && e.attrs.placeholder === '500');
ci.value = '7000'; ci.oninput();
ok(note().style.color === '#ff8a7d' && note().textContent.includes('none of the NDB band'), 'a centre at 7 MHz: said in red — ' + note().textContent);
ci.value = '500'; ci.oninput();
ok(note().style.color === '', '…and back at 500 kHz, fine again');
// back to NAVTEX: the box comes back
decSel(vfoRows()[1]).value = 'navtex'; decSel(vfoRows()[1]).onchange();
ok(freqIn(vfoRows()[1]).disabled === false && !modeSel(vfoRows()[1]).disabled && !note(), 'back to NAVTEX: frequency and demodulator editable again, no NDB line');

// ★★ Stuart's Lenovo profile: NAVTEX ×2, 160 m Band, NDB — at 1200 kHz, 3 MS/s
api = mk({ ndbHw: true, spotBands: [B160] });
api.bgLoad({ driver: 'sdrplay', rate: 3000000, bgDecode: JSON.stringify(LENOVO) });
ok(vfoRows().length === 4 && decSel(vfoRows()[3]).value === 'ndb-skim' && decSel(vfoRows()[2]).value === 'spots-band', 'the Lenovo profile: four rows, Band and NDB among them');
ok(note().textContent.startsWith('Covers 190–535 kHz'), 'at 1200 kHz / 3 MS/s the skimmer covers the whole band');
ok(api.bgCollect() === JSON.stringify(LENOVO), 'the Lenovo profile saves back byte for byte');

// ★ an older whole-radio NDB profile, converted on load
const OLD = { on: true, waitMin: 5, profiles: [{ centre: 0, from: '18:00', to: '06:00', vfos: [], ndb: true }] };
api = mk(null);
api.bgLoad({ driver: 'airspyhf', rate: 768000, bgDecode: JSON.stringify(OLD) });
ok(vfoRows().length === 1 && decSel(vfoRows()[0]).value === 'ndb-skim', 'an older {"ndb":true} profile loads as one NDB skimmer VFO');
ok(api.bgCollect() === JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 362750, from: '18:00', to: '06:00', vfos: [{ hz: 0, mode: 'usb', dec: 'ndb-skim' }] }] }),
   '…centred on the band, its hours kept, saved in the new shape: ' + api.bgCollect());

// where it is offered
const offered = (radio, HW) => { const x = mk(HW === undefined ? null : HW); x.bgLoad({ ...radio, bgDecode: JSON.stringify(NICK) }); return offers(vfoRows()[0]); };
ok(!offered({ driver: 'rtlsdr' }), 'an RTL without direct sampling: not offered');
ok(offered({ driver: 'rtlsdr', autoDirectSampling: true }), 'an RTL with automatic direct sampling: offered');
ok(offered({ driver: 'rtlsdr', directSampling: 2 }), 'an RTL on the Q branch: offered');
ok(offered({ driver: 'sdrplay' }), 'an RSP: offered');
for (const d of ['hackrf', 'airspy', 'icom']) ok(!offered({ driver: d }), `a ${d}: not offered`);
ok(offered({ driver: 'airspyhf', mode: 'locked', lockFreq: 400000, lockRate: 768000, users: 4 }), 'a radio LOCKED over the band: offered (nothing moves)');
blocked = new Set(['ndb']);
ok(!offered({ driver: 'airspyhf' }), 'NDB switched off for this radio by its owner: not offered');
blocked = new Set();
ok(!offered({ driver: 'airspyhf' }, { ndbHw: false }), "the running radio's own answer wins (HW.ndbHw false)");
ok(offered({ driver: 'rtlsdr' }, { ndbHw: true }), 'HW.ndbHw true (an RTL running in direct sampling): offered');
api = mk(null);
api.bgLoad({ driver: 'rtlsdr', bgDecode: JSON.stringify(NICK) });
ok(els.bgNdbLine.classList.contains('hide'), "…and where it is not, the headline's NDB sentence is hidden");

console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
