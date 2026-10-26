// test-setup-bg-ism.mjs — the setup page's background-decoding editor with an ISM PROFILE (V12, 2026-10-25), its REAL
// code against a DOM stand-in (the harness of test-setup-bgdecode.mjs): "Decodes" offers ISM only where this radio can
// run it (not on an Airspy HF+, an Icom, a locked radio, or with ISM blocked; only the bands the running radio reaches);
// choosing it swaps the RF centre and VFO rows for a band; it saves as {"ism":"433"} and reads back unchanged; going
// back to VFOs gives a VFO row; an ordinary profile still saves byte for byte as before.
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

// an RTL that is not running (no HW): all four bands by its driver
let api = mk(null);
api.bgLoad({ driver: 'rtlsdr', bgDecode: JSON.stringify(NICK) });
ok(!!sel('bgKind') && sel('bgKind').children.map((o) => o.value).join() === 'vfo,ism', 'an RTL profile offers "Decodes": Stations on VFOs / ISM sensors');
ok(sel('bgKind').value === 'vfo' && vfoRows().length === 2, "Nick's profile shows as VFOs, two rows");
ok(api.bgCollect() === JSON.stringify(NICK), 'an ordinary profile saves back byte for byte (no "ism" key)');
sel('bgKind').value = 'ism'; sel('bgKind').onchange();
ok(!!sel('bgIsmBand') && sel('bgIsmBand').children.map((o) => o.value).join() === '315,433,868,915', 'choosing ISM shows a band list: 315, 433, 868, 915');
ok(sel('bgIsmBand').value === '433', '...433.92 MHz first choice');
ok(vfoRows().length === 0 && !box.text().includes('RF Centre') && !box.text().includes('+ Add another VFO'), 'no RF centre, no VFO rows, no "add VFO"');
ok(box.text().includes('runs only while nobody is listening') && box.text().includes('nickname'), 'the note: whole radio, only while nobody listens, nicknames');
ok(box.text().includes('Background profile active between'), 'the profile still has its times');
sel('bgIsmBand').value = '868'; sel('bgIsmBand').onchange();
const saved = api.bgCollect();
ok(saved === JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 0, from: '00:00', to: '00:00', vfos: [], ism: '868' }] }), `saved: ${saved}`);
api.bgLoad({ driver: 'rtlsdr', bgDecode: saved });
ok(sel('bgKind').value === 'ism' && sel('bgIsmBand').value === '868' && api.bgCollect() === saved, 'reads back as ISM 868, and saves back unchanged');
sel('bgKind').value = 'vfo'; sel('bgKind').onchange();
ok(vfoRows().length === 1 && !('ism' in api.model().profiles[0]) && !api.bgCollect().includes('"ism"'), 'back to VFOs: one VFO row, no "ism" key');

// the running radio's own answer: only the bands it reaches
api = mk({ ismHwBands: ['433', '868'] });
api.bgLoad({ driver: 'sdrplay', bgDecode: JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 0, from: '00:00', to: '00:00', vfos: [], ism: '433' }] }) });
ok(sel('bgIsmBand').children.map((o) => o.value).join() === '433,868', 'a running radio offers only the bands it reaches (HW.ismHwBands: 433, 868)');

// where ISM cannot run: not offered
api = mk(null);
api.bgLoad({ driver: 'airspyhf', bgDecode: JSON.stringify(NICK) });
ok(!sel('bgKind') || !sel('bgKind').children.some((o) => o.value === 'ism'), 'an Airspy HF+: no ISM in "Decodes" (it reaches no ISM band — NDB, which it does reach, may be offered)');
ok(api.bgCollect() === JSON.stringify(NICK), '...and Nick\'s HF+ profile saves exactly as before');
api.bgLoad({ driver: 'rtlsdr', mode: 'locked', lockFreq: 7100000, lockRate: 2000000, users: 4, bgDecode: JSON.stringify(NICK) });
ok(!sel('bgKind'), 'a locked radio: not offered (background decoding never moves one)');
api.bgLoad({ driver: 'rtlsdr', mode: 'locked', lockFreq: 433920000, lockRate: 2000000, users: 4,
             bgDecode: JSON.stringify({ on: true, waitMin: 5, profiles: [{ centre: 0, from: '00:00', to: '00:00', vfos: [], ism: '433' }] }) });
ok(box.text().includes('this radio cannot do') && box.text().includes('locked'), 'a saved ISM profile on a locked radio says plainly why it cannot run');
blocked = new Set(['ism']);
api.bgLoad({ driver: 'rtlsdr', bgDecode: JSON.stringify(NICK) });
ok(!sel('bgKind'), 'ISM switched off for this radio by its owner: not offered');
blocked = new Set();
api = mk({ ismHwBands: [] });
api.bgLoad({ driver: 'rtlsdr', bgDecode: JSON.stringify(NICK) });
ok(!sel('bgKind'), 'a running radio that reaches no band (its allowed ranges): not offered');

console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
