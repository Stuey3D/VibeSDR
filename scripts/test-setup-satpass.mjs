// test-setup-satpass.mjs — V12 beta90 UNATTENDED SATELLITE PASSES on the setup page (docs/v12/SATELLITE-BEST-SERVER.md):
// the page's REAL code against a DOM stand-in (as test-setup-bgdecode.mjs). Loads and saves back inside the
// background-decoding setting as "satPasses"; its own switch (background decoding off, passes on, is stored); ticks
// add / remove downlinks by catalogue number + label; untouched it stores nothing; hidden on an Icom; the locked note.
import fs from 'node:fs';

const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const a = src.indexOf('const BG_DECODERS'), b = src.indexOf('function collectRadio()');
if (a < 0 || b < a) { console.log('   FAIL the background decoding editor was not found in the page'); process.exit(1); }
let fail = 0, pass = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; else pass++; };
class El {
  constructor(tag) { this.tag = tag; this.children = []; this.attrs = {}; this.textContent = ''; this.value = ''; this.checked = false; this.disabled = false; this.style = {};
    this.classList = { set: new Set(), toggle: (c, on) => (on ? this.classList.set.add(c) : this.classList.set.delete(c)), contains: (c) => this.classList.set.has(c) };
    this.options = this.children; }
  setAttribute(k, v) { this.attrs[k] = v; }
  appendChild(c) { this.children.push(c); return c; }
  addEventListener(ev, f) { this['on' + ev] = f; }
  set innerHTML(_) { this.children.length = 0; }
  get innerHTML() { return ''; }
  all() { return [this, ...this.children.flatMap((c) => c.all())]; }
  text() { return this.textContent + this.children.map((c) => c.text()).join(' '); }
}
const mkSelect = (vals) => { const s = new El('select'); let v = ''; Object.defineProperty(s, 'value', { get: () => v, set: (x) => { v = s.children.some((o) => o.value === x) ? x : (s.children[0]?.value ?? ''); }, configurable: true });
  for (const w of vals) { const o = new El('option'); o.value = w; s.appendChild(o); } return s; };
const els = { bgProfiles: new El('div'), hwBg: new El('div'), bgOn: new El('input'), bgWait: mkSelect(['1', '2', '5', '10', '15', '30', '60']),
              hwSatPass: new El('div'), spOn: new El('input'), spMinEl: mkSelect(['10', '20', '30', '45']), spBirds: new El('div'), spLockedNote: new El('p') };
const document = { createElement: (t) => (t === 'select' ? mkSelect([]) : new El(t)) };
const $ = (id) => els[id];
const cfg = { serverUtcOffsetMin: 0 };
const api = new Function('$', 'document', 'cfg', `${src.slice(a, b)}\nreturn { bgLoad, bgCollect, sp: () => spModel, list: SAT_PASS_DOWNLINKS };`)($, document, cfg);
const boxes = () => els.spBirds.all().filter((e) => e.tag === 'input');

api.bgLoad({ driver: 'rtlsdr', mode: 'open', bgDecode: '' });
ok(api.bgCollect() === '', 'untouched (background off, passes off): stores nothing');
ok(!els.hwSatPass.classList.contains('hide') && boxes().length === api.list.length && boxes().every((c) => c.disabled), `${api.list.length} downlinks drawn, disabled while the switch is off`);
els.spOn.checked = true; els.spOn.onchange();
ok(boxes().every((c) => !c.disabled), 'switched on: the ticks are live');
const meteor = api.list.findIndex((d) => d.norad === 57166), ao73 = api.list.findIndex((d) => d.norad === 39444);
boxes()[meteor].checked = true; boxes()[meteor].onchange();
boxes()[ao73].checked = true; boxes()[ao73].onchange();
els.spMinEl.value = '30'; els.spMinEl.onchange();
let j = JSON.parse(api.bgCollect() || '{}');
ok(j.on === false && j.satPasses && j.satPasses.on === true && j.satPasses.minEl === 30
   && JSON.stringify(j.satPasses.birds) === JSON.stringify([{ norad: 57166, label: 'LRPT' }, { norad: 39444, label: 'Telemetry' }]),
   `its own switch: background off, passes on, stored (${api.bgCollect()})`);
boxes()[meteor].checked = false; boxes()[meteor].onchange();
j = JSON.parse(api.bgCollect());
ok(j.satPasses.birds.length === 1 && j.satPasses.birds[0].norad === 39444, 'unticked: removed');
// round trip with a background profile beside it
const BOTH = { on: true, waitMin: 5, profiles: [{ centre: 500000, from: '00:00', to: '00:00', vfos: [{ hz: 517500, mode: 'usb', dec: 'navtex' }] }],
               satPasses: { on: true, minEl: 20, birds: [{ norad: 59051, label: 'LRPT' }] } };
api.bgLoad({ driver: 'airspyhf', mode: 'open', bgDecode: JSON.stringify(BOTH) });
ok(els.spOn.checked && els.spMinEl.value === '20' && boxes()[api.list.findIndex((d) => d.norad === 59051)].checked, 'loads: on, 20°, Meteor-M2-4 ticked');
{ // ★ the profile editor itself is test-setup-bgdecode.mjs's; here: satPasses rides beside the profile untouched
  const back = JSON.parse(api.bgCollect());
  ok(JSON.stringify(back.satPasses) === JSON.stringify(BOTH.satPasses) && back.on === true && back.profiles.length === 1 && back.profiles[0].centre === 500000,
     `saved back beside a profile: ${JSON.stringify(back.satPasses)}`); }
api.bgLoad({ driver: 'icom', mode: 'open', bgDecode: JSON.stringify(BOTH) });
ok(els.hwSatPass.classList.contains('hide'), 'hidden on an Icom');
api.bgLoad({ driver: 'rtlsdr', mode: 'locked', bgDecode: JSON.stringify(BOTH) });
ok(!els.spLockedNote.classList.contains('hide'), 'a locked range: the note says passes are not followed there');
api.bgLoad({ driver: 'rtlsdr', mode: 'open', bgDecode: JSON.stringify(BOTH) });
ok(els.spLockedNote.classList.contains('hide'), 'an open radio: no note');
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
