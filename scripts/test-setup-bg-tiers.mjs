// test-setup-bg-tiers.mjs — the setup page's background VFOs and the decoder SLOTS (Stuart, 2026-10-22): one radio → no tier
// list, the VFOs capped at the box's decoders ("+ Add another VFO" stops there; rows past it kept but said not to run);
// several radios → a TIER LIST (rank, drag handle, ▲/▼) and the note that real listeners get decoder priority. Adding a
// radio switches the page. The page's REAL code against a DOM stand-in.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const a = src.indexOf('const BG_DECODERS'), b = src.indexOf('function collectRadio()');
if (a < 0 || b < a) { console.log('   FAIL the background decoding editor was not found in the page'); process.exit(1); }
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

// ── a DOM just big enough for bgRender ──
class El {
  constructor(tag) { this.tag = tag; this.children = []; this.attrs = {}; this.textContent = ''; this.value = ''; this.checked = false;
    this.classList = { set: new Set(), toggle: (c, on) => (on ? this.classList.set.add(c) : this.classList.set.delete(c)), contains: (c) => this.classList.set.has(c), add: (c) => this.classList.set.add(c), remove: (c) => this.classList.set.delete(c) };
    this.options = this.children; }
  setAttribute(k, v) { this.attrs[k] = v; if (k === 'value') this.value = v; }
  appendChild(c) { this.children.push(c); if (this.tag === 'select' && this.children.length === 1 && !this._set) this.value = c.value; return c; }
  addEventListener(ev, f) { this['on' + ev] = f; }
  set innerHTML(_) { this.children.length = 0; }
  get innerHTML() { return ''; }
  all() { return [this, ...this.children.flatMap((c) => c.all())]; }
  text() { return this.textContent + this.children.map((c) => c.text()).join(' '); }
  getBoundingClientRect() { return this.rect || { top: 0, height: 0 }; }
}
// ★ A <select>'s value can only be one of its options — as a browser's.
const mkSelect = () => { const s = new El('select'); let v = ''; Object.defineProperty(s, 'value', { get: () => v, set: (x) => { v = s.children.some((o) => o.value === x) ? x : (s.children[0]?.value ?? ''); }, configurable: true }); return s; };
const els = { bgProfiles: new El('div'), hwBg: new El('div'), bgOn: new El('input'), bgWait: mkSelect(), bgKeepInRange: new El('input') };
for (const w of ['1', '2', '5', '10', '15', '30', '60']) { const o = new El('option'); o.value = w; els.bgWait.appendChild(o); }
const document = { createElement: (t) => (t === 'select' ? mkSelect() : new El(t)) };
const $ = (id) => els[id];
const cfg = { serverUtcOffsetMin: 60, decoderSlotsDefault: 8, radios: [{ driver: 'airspyhf' }] };
const api = new Function('$', 'document', 'cfg', `${src.slice(a, b)}\nreturn { bgLoad, bgCollect, bgRender, model: () => bgModel };`)($, document, cfg);
const box = els.bgProfiles;
const btns = (label) => box.all().filter((e) => e.tag === 'button' && e.textContent === label);
const rowsOf = () => box.children.find((c) => c.attrs.class === 'bgProf').children.filter((e) => e.attrs.class === 'bgVfo');
const hz = () => api.model().profiles[0].vfos.map((v) => v.hz / 1e3).join(',');
const mk = (n, keep = true) => ({ on: true, waitMin: 5, ...(keep ? {} : { keepInRange: false }), profiles: [{ centre: 500000, from: '00:00', to: '00:00',
  vfos: Array.from({ length: n }, (_, i) => ({ hz: 490000 + i * 1000, mode: 'usb', dec: 'navtex' })) }] });

console.log('── one radio, "keep decoding in range" OFF: no tier list; the VFOs are capped at the decoder slots ──');
els.bgKeepInRange.checked = false;
api.bgLoad({ driver: 'airspyhf', bgDecode: JSON.stringify(mk(7, false)) });
ok(!box.all().some((e) => e.attrs.class === 'bgHandle' || e.attrs.class === 'bgRank' || (e.attrs.class || '').includes('bgMove')),
   'no drag handle, no rank numbers, no ▲/▼');
ok(!box.text().includes('real listeners get decoder priority'), 'no tier note');
ok(box.text().includes('This server can run 8 decoders at once \u2014 that\u2019s the most VFOs a profile can have.'), 'the plain line: "This server can run 8 decoders at once — that’s the most VFOs a profile can have."');
ok(btns('+ Add another VFO').length === 1, '7 VFOs: "+ Add another VFO" still offered');
btns('+ Add another VFO')[0].onclick();
ok(api.model().profiles[0].vfos.length === 8 && btns('+ Add another VFO').length === 0, '★ at 8 (the slots) "+ Add another VFO" stops');
api.bgLoad({ driver: 'airspyhf', bgDecode: JSON.stringify(mk(10, false)) });
ok(rowsOf().length === 10 && box.text().includes('VFO 9 and 10 won\u2019t run \u2014 this server can run 8 decoders.'),
   '★ a box that went back to one radio: rows kept, "VFO 9 and 10 won’t run — this server can run 8 decoders."');

console.log('── one radio, "keep decoding in range" ON: slots can run short, so the tier list ──');
els.bgKeepInRange.checked = true;
api.bgLoad({ driver: 'airspyhf', bgDecode: JSON.stringify(mk(3)) });
ok(box.all().filter((e) => e.attrs.class === 'bgHandle').length === 3 && box.text().includes('real listeners get decoder priority'),
   '★ tier list and the note on a single radio while listeners keep background running');
ok(!box.text().includes('that\u2019s the most VFOs'), '...and no slot cap');
console.log('── a locked radio: no RF centre, the locked range said, the tier list, its user slots ──');
api.bgLoad({ driver: 'sdrplay', mode: 'locked', lockFreq: 7100000, lockRate: 2000000, users: 10, bgDecode: JSON.stringify(mk(2)) });
ok(!box.text().includes('RF Centre'), 'no RF Centre field');
ok(box.text().includes('This radio\u2019s range is locked (6.153\u20138.047 MHz): background VFOs must sit inside it.'), `the locked range said: ${(box.all().find((e) => e.attrs.id === 'bgLockedNote') || {}).textContent}`);
ok(box.all().filter((e) => e.attrs.class === 'bgHandle').length === 2, 'the tier list');
ok(box.text().includes('up to 10 background VFOs while nobody listens (this radio\u2019s 10 user slots) \u00b7 8 decoders on this server \u00b7 listeners always come first'), 'the slot line names both limits');
console.log('── a second radio is added: the tier list ──');
els.bgKeepInRange.checked = false;
cfg.radios.push({ driver: 'sdrplay' });
api.bgLoad({ driver: 'airspyhf', bgDecode: JSON.stringify(mk(3, false)) });
ok(box.all().filter((e) => e.attrs.class === 'bgHandle').length === 3, '★ a drag handle on every row');
ok(box.all().filter((e) => e.attrs.class === 'bgRank').map((e) => e.textContent).join() === '1,2,3', 'rank numbers 1, 2, 3');
ok(box.text().includes('On CPU-limited servers with a limited number of decoders, real listeners get decoder priority \u2014 the lowest VFO in this list pauses when a listener needs a decoder and resumes when one is free.'), 'Stuart\'s note');
ok(box.text().includes('3 VFOs \u00b7 up to 3 of this server\u2019s 8 decoder slots \u00b7 listeners always come first; the lowest in the list pauses when a listener needs a decoder'), 'the slot line');
ok(!box.text().includes('that\u2019s the most VFOs'), 'no single-radio cap line');
// ▲ / ▼ (keyboard and pad)
let r = rowsOf();
r[2].children.find((e) => e.attrs['aria-label'] === 'Move up').onclick();
ok(hz() === '490,492,491', `▲ on VFO 3 moves it above VFO 2 (${hz()})`);
r = rowsOf();
r[0].children.find((e) => e.attrs['aria-label'] === 'Move down').onclick();
ok(hz() === '492,490,491', `▼ on VFO 1 moves it down (${hz()})`);
ok(rowsOf()[0].children.find((e) => e.attrs['aria-label'] === 'Move up').attrs.disabled != null, 'the top row\'s ▲ is disabled');
// drag (pointer events: a mouse or a finger)
r = rowsOf();
r.forEach((row, k) => { row.rect = { top: k * 40, height: 40 }; });
const hd = r[2].children.find((e) => e.attrs.class === 'bgHandle');
hd.onpointerdown({ pointerId: 1, clientY: 100, preventDefault() {} });
hd.onpointermove({ clientY: 10 });
hd.onpointerup({});
ok(hz() === '491,492,490', `★ dragging VFO 3 to the top makes it rank 1 (${hz()})`);
ok(api.bgCollect() === JSON.stringify({ on: true, waitMin: 5, profiles: [{ ...mk(3, false).profiles[0], vfos: [491000, 492000, 490000].map((h) => ({ hz: h, mode: 'usb', dec: 'navtex' })) }], keepInRange: false }),
   '★ the order IS the stored array — priority survives save and reload');
api.bgLoad({ driver: 'airspyhf', bgDecode: api.bgCollect() });
ok(hz() === '491,492,490', 'reloaded: the same order');
for (let i = 0; i < 20; i++) { const a = btns('+ Add another VFO'); if (!a.length) break; a[0].onclick(); }
ok(api.model().profiles[0].vfos.length === 16, 'multi-radio: no cap but the sensible 16');

console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
