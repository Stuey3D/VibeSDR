// test-setup-bgdecode.mjs — the setup page's BACKGROUND DECODING editor (V12, 2026-10-22), its REAL code against a DOM
// stand-in: Nick's profile loads and saves back unchanged; the VFO rows run frequency → demodulator → decoder with only
// the decoders that fit the demodulator; "Want to add another background decoding profile?" appears ONLY under a
// profile that is not all day; profiles delete; the dial hint; off with nothing set stores nothing.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const a = src.indexOf('const BG_DECODERS'), b = src.indexOf('function collectRadio()');
if (a < 0 || b < a) { console.log('   FAIL the background decoding editor was not found in the page'); process.exit(1); }
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

// ── a DOM just big enough for bgRender ──
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
// ★ A <select>'s value can only be one of its options — as a browser's.
const mkSelect = () => { const s = new El('select'); let v = ''; Object.defineProperty(s, 'value', { get: () => v, set: (x) => { v = s.children.some((o) => o.value === x) ? x : (s.children[0]?.value ?? ''); }, configurable: true }); return s; };
const els = { bgProfiles: new El('div'), hwBg: new El('div'), bgOn: new El('input'), bgWait: mkSelect() };
for (const w of ['1', '2', '5', '10', '15', '30', '60']) { const o = new El('option'); o.value = w; els.bgWait.appendChild(o); }
const document = { createElement: (t) => (t === 'select' ? mkSelect() : new El(t)) };
const $ = (id) => els[id];
const cfg = { serverUtcOffsetMin: 60 };
const api = new Function('$', 'document', 'cfg', `${src.slice(a, b)}\nreturn { bgLoad, bgCollect, bgRender, model: () => bgModel };`)($, document, cfg);

const NICK = { on: true, waitMin: 5, profiles: [{ centre: 500000, from: '00:00', to: '00:00',
  vfos: [{ hz: 489500, mode: 'usb', dec: 'navtex' }, { hz: 517500, mode: 'usb', dec: 'navtex' }] }] };
api.bgLoad({ driver: 'airspyhf', bgDecode: JSON.stringify(NICK) });
const box = els.bgProfiles;
const buttons = () => box.all().filter((e) => e.tag === 'button');
const btn = (label) => buttons().find((e) => e.textContent === label);
ok(els.bgOn.checked && els.bgWait.value === '5', 'Nick\'s config loads: on, 5 minutes');
ok(box.children.length === 1 && box.text().includes('Profile 1'), 'one profile drawn');
ok(box.all().filter((e) => e.textContent.startsWith('VFO ')).length === 2, 'two VFO rows');
ok(box.text().includes('Listening for NAVTEX on 518.000 kHz (the dial sits 0.5 kHz below it in USB)'), 'the dial hint for 517.5 USB');
ok(box.text().includes('UTC — all day'), '00:00 – 00:00 says all day');
ok(!btn('Want to add another background decoding profile?'), '★ NO "add another profile" under an all-day profile');
ok(api.bgCollect() === JSON.stringify(NICK), `saved back unchanged: ${api.bgCollect()}`);

// the decoders offered follow the demodulator
const row2 = box.children[0].children.filter((e) => e.attrs.class === 'bgVfo')[1];
const [md, dc] = row2.children.filter((e) => e.tag === 'select');
ok(dc.children.map((o) => o.value).join() === 'navtex,rtty,rtty-ham,rtty-weather,wefax,sstv,ft8', 'USB offers NAVTEX, RTTY, WEFAX, SSTV, FT8');
md.value = 'lsb'; md.onchange();
ok(!dc.children.some((o) => o.value === 'wefax' || o.value === 'sstv' || o.value === 'ft8') && dc.value === 'navtex',
   'LSB offers only NAVTEX and RTTY (no negative charts), and keeps NAVTEX');
ok(row2.text().includes('Listening for NAVTEX on 517.000 kHz'), '...and the hint follows: 517.5 LSB is listening on 517.0');
md.value = 'nfm'; md.onchange();
ok(dc.children.map((o) => o.value).join() === 'sstv' && api.model().profiles[0].vfos[1].dec === 'sstv', 'NFM offers only SSTV, and the VFO becomes SSTV');

// times: a window brings the "another profile" line; following it adds a second profile
const times = box.children[0].all().filter((e) => e.attrs.type === 'time');
times[0].value = '18:00'; times[1].value = '06:00'; times[0].onchange();
ok(!!btn('Want to add another background decoding profile?'), '★ a profile with times brings "Want to add another background decoding profile?"');
ok(box.text().includes('UTC (19:00 – 07:00 server time)'), 'the server\'s local time beside the UTC times (server at UTC+1)');
btn('Want to add another background decoding profile?').onclick();
ok(api.model().profiles.length === 2 && box.text().includes('Profile 2'), 'a second profile, with the same options');
ok(!btn('Want to add another background decoding profile?'), '...whose all-day default offers no third');
btn('+ Add another VFO').onclick();
ok(api.model().profiles[0].vfos.length === 3, '+ Add another VFO adds a row');
buttons().filter((e) => e.textContent === 'Remove')[2].onclick();
ok(api.model().profiles[0].vfos.length === 2, 'Remove takes it away');
buttons().filter((e) => e.textContent === 'Delete profile')[1].onclick();
ok(api.model().profiles.length === 1 && api.model().profiles[0].from === '18:00', 'Delete profile removes that profile only');

// off with nothing set stores nothing; an Icom does not get the control
api.bgLoad({ driver: 'rtlsdr', bgDecode: '' });
ok(!els.bgOn.checked && box.children.length === 0 && api.bgCollect() === '', 'a radio that never had it: off, nothing drawn, nothing stored');
api.bgLoad({ driver: 'icom', bgDecode: '' });
ok(els.hwBg.classList.contains('hide'), 'an Icom: hidden (one physical receiver, no IQ to cut channels from)');

console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
