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
              hwSatPass: new El('div'), spOn: new El('input'), spMinEl: mkSelect(['10', '20', '30', '45']), spBirds: new El('div'), spLockedNote: new El('p'),
              // ★★★ beta94 the groups and their priority
              spMeteor: new El('input'), spOrbcomm: new El('input'), spBothNote: new El('p'), spFirstRow: new El('label'), spFirst: mkSelect(['meteor', 'orbcomm']),
              // ★★★ beta96 the linear transponders (Single mode: SATELLITES only)
              spXpdrWrap: new El('div'), spXpdr: new El('div'),
              // ★★★ beta97 the satellite digital-mode skimmer
              spSkim: new El('input'), spSkimOpts: new El('div'), spSkimMode: mkSelect(['auto', 'live', 'catchup']), spSkimIq: new El('input'), spXpdrHead: new El('p') };
const document = { createElement: (t) => (t === 'select' ? mkSelect([]) : new El(t)) };
const $ = (id) => els[id];
const cfg = { serverUtcOffsetMin: 0 };
const api = new Function('$', 'document', 'cfg', `${src.slice(a, b)}\nreturn { bgLoad, bgCollect, sp: () => spModel, list: SAT_PASS_DOWNLINKS, minRate: SAT_BOTH_MIN_RATE };`)($, document, cfg);
const boxes = () => els.spBirds.all().filter((e) => e.tag === 'input');

api.bgLoad({ driver: 'rtlsdr', mode: 'open', rate: 2400000, bgDecode: '' });
ok(api.bgCollect() === '', 'untouched (background off, passes off): stores nothing');
const others = api.list.filter((d) => d.label !== 'LRPT');
ok(!els.hwSatPass.classList.contains('hide') && boxes().length === others.length && boxes().every((c) => c.disabled) && els.spMeteor.disabled && els.spOrbcomm.disabled,
   `${others.length} other downlinks drawn (Meteor is its own group), all disabled while the switch is off`);
els.spOn.checked = true; els.spOn.onchange();
ok(boxes().every((c) => !c.disabled) && !els.spMeteor.disabled && els.spMeteor.checked && !els.spOrbcomm.checked, 'switched on: live, Meteor ticked to start with, ORBCOMM not');
const ao73 = others.findIndex((d) => d.norad === 39444);
boxes()[ao73].checked = true; boxes()[ao73].onchange();
els.spMinEl.value = '30'; els.spMinEl.onchange();
let j = JSON.parse(api.bgCollect() || '{}');
ok(j.on === false && j.satPasses && j.satPasses.on === true && j.satPasses.minEl === 30 && j.satPasses.meteor === true && !('orbcomm' in j.satPasses) && !('first' in j.satPasses)
   && JSON.stringify(j.satPasses.birds) === JSON.stringify([{ norad: 39444, label: 'Telemetry' }]),
   `its own switch: background off, passes on, Meteor as a group, stored (${api.bgCollect()})`);
// ★★★ beta94 ORBCOMM + the priority, per radio
els.spOrbcomm.checked = true; els.spOrbcomm.onchange();
ok(els.spBothNote.textContent.startsWith('On this radio both run together') && els.spFirstRow.classList.contains('hide'),
   `an RTL at 2.4 MS/s: "both run together", no priority drawn (${els.spBothNote.textContent})`);
j = JSON.parse(api.bgCollect());
ok(j.satPasses.meteor === true && j.satPasses.orbcomm === true && !('first' in j.satPasses), 'both groups stored, Meteor first (absent = the default)');
els.spMeteor.checked = false; els.spMeteor.onchange();
ok(els.spBothNote.classList.contains('hide') && els.spFirstRow.classList.contains('hide'), 'ORBCOMM alone: no note, no priority');
j = JSON.parse(api.bgCollect());
ok(!('meteor' in j.satPasses) && j.satPasses.orbcomm === true, `Meteor unticked: ${JSON.stringify(j.satPasses)}`);
boxes()[ao73].checked = false; boxes()[ao73].onchange();
j = JSON.parse(api.bgCollect());
ok(j.satPasses.birds.length === 0, 'unticked: removed');
// round trip with a background profile beside it — a beta90–93 Meteor tick becomes the Meteor group
const BOTH = { on: true, waitMin: 5, profiles: [{ centre: 500000, from: '00:00', to: '00:00', vfos: [{ hz: 517500, mode: 'usb', dec: 'navtex' }] }],
               satPasses: { on: true, minEl: 20, birds: [{ norad: 59051, label: 'LRPT' }] } };
api.bgLoad({ driver: 'airspyhf', mode: 'open', rate: 912000, bgDecode: JSON.stringify(BOTH) });
ok(els.spOn.checked && els.spMinEl.value === '20' && els.spMeteor.checked && !els.spOrbcomm.checked, 'loads: on, 20°, an old Meteor-M2-4 tick = the Meteor group');
{ // ★ the profile editor itself is test-setup-bgdecode.mjs's; here: satPasses rides beside the profile
  const back = JSON.parse(api.bgCollect());
  ok(JSON.stringify(back.satPasses) === JSON.stringify({ on: true, minEl: 20, birds: [], meteor: true }) && back.on === true && back.profiles.length === 1 && back.profiles[0].centre === 500000,
     `saved back beside a profile, the old tick as the group: ${JSON.stringify(back.satPasses)}`); }
// ★★★ the HF+: too narrow for both — the priority drawn and saved
els.spOrbcomm.checked = true; els.spOrbcomm.onchange();
ok(els.spBothNote.textContent.startsWith("This radio's capture is too narrow") && !els.spFirstRow.classList.contains('hide'),
   `an Airspy HF+: "too narrow", the priority drawn (${els.spBothNote.textContent})`);
els.spFirst.value = 'orbcomm'; els.spFirst.onchange();
j = JSON.parse(api.bgCollect());
ok(j.satPasses.first === 'orbcomm', 'ORBCOMM first stored');
api.bgLoad({ driver: 'airspyhf', mode: 'open', rate: 912000, bgDecode: JSON.stringify({ ...BOTH, satPasses: { ...j.satPasses } }) });
ok(els.spFirst.value === 'orbcomm' && JSON.stringify(JSON.parse(api.bgCollect()).satPasses) === JSON.stringify(j.satPasses), 'ORBCOMM first loads and saves back unchanged');
api.bgLoad({ driver: 'rtlsdr', mode: 'open', rate: 900000, bgDecode: JSON.stringify({ ...BOTH, satPasses: { on: true, minEl: 20, birds: [], meteor: true, orbcomm: true } }) });
ok(!els.spFirstRow.classList.contains('hide') && els.spBothNote.textContent.includes('at its sample rate'), `an RTL at 900 kS/s (under ${api.minRate}): the priority decides`);
api.bgLoad({ driver: 'icom', mode: 'open', bgDecode: JSON.stringify(BOTH) });
ok(els.hwSatPass.classList.contains('hide'), 'hidden on an Icom');
api.bgLoad({ driver: 'rtlsdr', mode: 'locked', bgDecode: JSON.stringify(BOTH) });
ok(!els.spLockedNote.classList.contains('hide'), 'a locked range: the note says passes are not followed there');
api.bgLoad({ driver: 'rtlsdr', mode: 'open', bgDecode: JSON.stringify(BOTH) });
ok(els.spLockedNote.classList.contains('hide'), 'an open radio: no note');
// ★★★ beta96 SINGLE MODE: SATELLITES — the linear transponders, drawn only on a SATELLITES radio; a tick stores norad + label
api.bgLoad({ driver: 'rtlsdr', mode: 'open', rate: 2400000, bgDecode: JSON.stringify({ on: false, satPasses: { on: true, minEl: 10, birds: [] } }) });
ok(els.spXpdrWrap.classList.contains('hide') && els.spXpdr.all().filter((e) => e.tag === 'input').length === 0, 'General radio: no transponder rows (a control that only works on a SATELLITES radio is not drawn)');
api.bgLoad({ driver: 'rtlsdr', mode: 'open', rate: 2400000, singleMode: 'sat', bgDecode: JSON.stringify({ on: false, satPasses: { on: true, minEl: 10, birds: [] } }) });
const xb = () => els.spXpdr.all().filter((e) => e.tag === 'input');
ok(!els.spXpdrWrap.classList.contains('hide') && xb().length === 8, `SATELLITES radio: the transponders drawn (${xb().length})`);
ok(/RS-44 · Transponder · 435\.610–435\.670 MHz/.test(els.spXpdr.text()), 'RS-44 named with its passband');
xb()[0].checked = true; xb()[0].onchange();
ok(JSON.stringify(api.sp().birds) === '[{"norad":44909,"label":"Transponder"}]', `a tick stores the transponder (${JSON.stringify(api.sp().birds)})`);
ok(/"birds":\[\{"norad":44909,"label":"Transponder"\}\]/.test(api.bgCollect() || ''), 'saved inside satPasses');
// ★★★ beta97 "Skim transponder digital modes": on ANY radio it draws the transponder ticks; its speed and keep-the-IQ under it
api.bgLoad({ driver: 'rtlsdr', mode: 'open', rate: 2400000, bgDecode: JSON.stringify({ on: false, satPasses: { on: true, minEl: 10, birds: [] } }) });
ok(!els.spSkim.checked && els.spSkimOpts.classList.contains('hide') && els.spXpdrWrap.classList.contains('hide'), 'skim off: its options hidden, no transponder rows on a general radio');
els.spSkim.checked = true; els.spSkim.onchange();
ok(!els.spSkimOpts.classList.contains('hide') && !els.spXpdrWrap.classList.contains('hide') && xb().length === 8, 'skim on: its options and the 8 transponders drawn on a general radio');
xb()[5].checked = true; xb()[5].onchange();
els.spSkimMode.value = 'catchup'; els.spSkimMode.onchange();
els.spSkimIq.checked = true; els.spSkimIq.onchange();
const saved = api.bgCollect() || '';
ok(/"skim":true/.test(saved) && /"skimMode":"catchup"/.test(saved) && /"skimIq":true/.test(saved) && /"norad":43803,"label":"Transponder"/.test(saved),
   'saved inside satPasses: skim, its speed, keep-the-IQ and JO-97\'s tick (' + saved.slice(saved.indexOf('"satPasses"'), saved.indexOf('"satPasses"') + 160) + ')');
api.bgLoad({ driver: 'rtlsdr', mode: 'open', rate: 2400000, bgDecode: saved });
ok(els.spSkim.checked && els.spSkimMode.value === 'catchup' && els.spSkimIq.checked, 'read back as saved');
api.bgLoad({ driver: 'rtlsdr', mode: 'open', rate: 2400000, bgDecode: JSON.stringify({ on: false, satPasses: { on: true, minEl: 10, birds: [] } }) });
ok(!/skim/.test(api.bgCollect() || ''), 'untouched, an older setting is written back without the skimmer\'s keys');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
