// test-setup-bg-ndb.mjs — the setup page's background-decoding editor with an NDB PROFILE (V12, 2026-10-27), its REAL
// code against a DOM stand-in (test-setup-bg-ism.mjs's harness): "Decodes" offers NDB only where this radio reaches LF
// (an HF+ or an RSP; an RTL only with direct sampling on or automatic; the running radio's own HW.ndbHw first; never a
// HackRF, an Airspy R2, an Icom, a locked radio, or with NDB blocked); choosing it swaps the RF centre and VFOs for a
// note; it saves as {"ndb":true} and reads back unchanged; back to VFOs gives a VFO row.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const a = src.indexOf('const BG_DECODERS'), b = src.indexOf('function collectRadio()');
if (a < 0 || b < a) { console.log('   FAIL the background decoding editor was not found in the page'); process.exit(1); }
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

class El {
  constructor(tag) { this.tag = tag; this.children = []; this.attrs = {}; this.textContent = ''; this.value = ''; this.checked = false;
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
const sel = (cls) => box.all().find((e) => e.tag === 'select' && e.attrs.class === cls);
const vfoRows = () => box.all().filter((e) => e.attrs.class === 'bgVfo');

const NICK = { on: true, waitMin: 5, profiles: [{ centre: 500000, from: '00:00', to: '00:00',
  vfos: [{ hz: 489500, mode: 'usb', dec: 'navtex' }, { hz: 517500, mode: 'usb', dec: 'navtex' }] }] };
const kinds = () => (sel('bgKind') ? sel('bgKind').children.map((o) => o.value).join() : '');

let api = mk(null);
api.bgLoad({ driver: 'airspyhf', bgDecode: JSON.stringify(NICK) });
ok(kinds() === 'vfo,ndb', `an Airspy HF+ offers "Decodes": VFOs / NDB (no ISM): ${kinds()}`);
ok(api.bgCollect() === JSON.stringify(NICK), 'an ordinary profile saves back byte for byte (no "ndb" key)');
sel('bgKind').value = 'ndb'; sel('bgKind').onchange();
ok(vfoRows().length === 0 && !box.text().includes('RF Centre'), 'NDB: no RF centre, no VFO rows');
ok(box.text().includes('LF beacon band') && box.text().includes('nobody is listening') && box.text().includes('gallery'), 'the note: the whole band, only while nobody listens, the gallery');
const saved = api.bgCollect();
ok(saved === JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 0, from: '00:00', to: '00:00', vfos: [], ndb: true }] }), `saved: ${saved}`);
api.bgLoad({ driver: 'airspyhf', bgDecode: saved });
ok(sel('bgKind').value === 'ndb' && api.bgCollect() === saved, 'reads back as NDB, and saves back unchanged');
sel('bgKind').value = 'vfo'; sel('bgKind').onchange();
ok(vfoRows().length === 1 && !api.bgCollect().includes('"ndb"'), 'back to VFOs: one VFO row, no "ndb" key');

api.bgLoad({ driver: 'rtlsdr', bgDecode: JSON.stringify(NICK) });
ok(!kinds().includes('ndb'), 'an RTL without direct sampling: no NDB');
api.bgLoad({ driver: 'rtlsdr', autoDirectSampling: true, bgDecode: JSON.stringify(NICK) });
ok(kinds().includes('ndb'), `an RTL with automatic direct sampling: NDB offered (${kinds()})`);
api.bgLoad({ driver: 'rtlsdr', directSampling: 2, bgDecode: JSON.stringify(NICK) });
ok(kinds().includes('ndb'), 'an RTL on the Q branch: NDB offered');
api.bgLoad({ driver: 'sdrplay', bgDecode: JSON.stringify(NICK) });
ok(kinds().includes('ndb'), 'an RSP: NDB offered');
for (const d of ['hackrf', 'airspy', 'icom']) { api.bgLoad({ driver: d, bgDecode: JSON.stringify(NICK) }); ok(!kinds().includes('ndb'), `a ${d}: no NDB`); }
api.bgLoad({ driver: 'airspyhf', mode: 'locked', lockFreq: 7100000, lockRate: 768000, users: 4, bgDecode: JSON.stringify(NICK) });
ok(!kinds().includes('ndb'), 'a locked radio: not offered');
api.bgLoad({ driver: 'airspyhf', mode: 'locked', lockFreq: 7100000, lockRate: 768000, users: 4,
             bgDecode: JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 0, from: '00:00', to: '00:00', vfos: [], ndb: true }] }) });
ok(box.text().includes('this radio cannot do') && box.text().includes('locked'), 'a saved NDB profile on a locked radio says plainly why it cannot run');
blocked = new Set(['ndb']);
api.bgLoad({ driver: 'airspyhf', bgDecode: JSON.stringify(NICK) });
ok(!kinds().includes('ndb'), 'NDB switched off for this radio by its owner: not offered');
blocked = new Set();
api = mk({ ndbHw: false, ismHwBands: [] });
api.bgLoad({ driver: 'airspyhf', bgDecode: JSON.stringify(NICK) });
ok(!kinds().includes('ndb'), "the running radio's own answer wins (HW.ndbHw false)");
api = mk({ ndbHw: true, ismHwBands: [] });
api.bgLoad({ driver: 'rtlsdr', bgDecode: JSON.stringify(NICK) });
ok(kinds().includes('ndb'), 'HW.ndbHw true (an RTL running in direct sampling): offered');

console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
