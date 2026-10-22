// test-setup-bg-release.mjs — the setup page's line when "Let another program use this radio" and Background Decoding are
// both ticked (Stuart, 2026-10-22: share the idle radio BY TIME — "You cannot release a radio and also hand it over to
// another app"). The page's REAL code against a DOM stand-in (the same one as test-setup-bgdecode.mjs, where present).
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
const els = { bgProfiles: new El('div'), hwBg: new El('div'), bgOn: new El('input'), bgWait: mkSelect(),
              releaseWhenIdle: new El('input'), bgReleaseNote: new El('p') };
for (const w of ['1', '2', '5', '10', '15', '30', '60']) { const o = new El('option'); o.value = w; els.bgWait.appendChild(o); }
const document = { createElement: (t) => (t === 'select' ? mkSelect() : new El(t)) };
const $ = (id) => els[id];
const cfg = { serverUtcOffsetMin: 60 };
const api = new Function('$', 'document', 'cfg', `${src.slice(a, b)}\nreturn { bgLoad, bgCollect, bgRender, model: () => bgModel };`)($, document, cfg);

const NICK = { on: true, waitMin: 5, profiles: [{ centre: 500000, from: '00:00', to: '00:00',
  vfos: [{ hz: 489500, mode: 'usb', dec: 'navtex' }, { hz: 517500, mode: 'usb', dec: 'navtex' }] }] };
const note = els.bgReleaseNote;
const ALLDAY = 'Your profile runs all day, so this radio is never handed to another program while Background Decoding is on.';
const WINDOW = 'Outside your background profile hours, another program may use this radio. During them, VibeServer keeps it for background decoding.';
ok(/id="bgReleaseNote"/.test(src), 'the line sits in the page under Background Decoding');
els.releaseWhenIdle.checked = false;
api.bgLoad({ driver: 'airspyhf', bgDecode: JSON.stringify(NICK) });
ok(note.classList.contains('hide'), 'release not ticked: no line');
els.releaseWhenIdle.checked = true; els.releaseWhenIdle.onchange();
ok(!note.classList.contains('hide') && note.textContent === ALLDAY, `release ticked + an all-day profile: "${note.textContent}"`);
const tt = els.bgProfiles.children.filter((c) => c.attrs.class === 'bgProf')[0].all().filter((e) => e.attrs.type === 'time');
tt[0].value = '18:00'; tt[1].value = '06:00'; tt[0].onchange();
ok(!note.classList.contains('hide') && note.textContent === WINDOW, `release ticked + an 18:00–06:00 profile: "${note.textContent}"`);
els.bgOn.checked = false; els.bgOn.onchange();
ok(note.classList.contains('hide'), 'background decoding switched off: the line goes');
ok(!els.releaseWhenIdle.attrs.disabled && !els.bgOn.attrs.disabled, 'both tick-boxes stay usable (nothing greyed)');
console.log(fail ? `\n${fail} FAILED` : '\nall passed');
process.exit(fail ? 1 : 0);
