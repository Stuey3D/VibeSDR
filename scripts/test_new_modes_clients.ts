// test_new_modes_clients.ts — ★★★ beta96 the CLIENT half of the new-modes rule (docs/v12/NEW-MODES-OFF.md): the Setup
// page's real list functions (lifted out of vibe_setup_page.h) and the app's src/utils/newModes.ts, on the same lists as
// vibeserver/test-new-modes.cpp — Paul's ADS-B-only radio above all — plus SATELLITE's client gate (satCapFrom).
//   npx tsx scripts/test_new_modes_clients.ts      ★ silent: no server, no browser
import { readFileSync } from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import * as NM from '../src/utils/newModes';
import { satCapFrom } from '../src/services/satellites';

const root = path.resolve(__dirname, '..');
const read = (p: string) => readFileSync(path.join(root, p), 'utf8');
let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

// ── the server's table (vibe_decoder_reach.h modeGens) ──
const h = read('android/app/src/main/cpp/vibe_decoder_reach.h');
const GENS: Record<string, number> = {};
for (const m of h.matchAll(/\{"([a-z0-9+-]+)",(\d+),(true|false)\}/g)) if (m[3] === 'false') GENS[m[1]] = Number(m[2]);
const NOW = Number((h.match(/constexpr int kModeGenNow = (\d+);/) || [])[1]);
const MG: NM.ModeGens = { now: NOW, gens: GENS };
ok(NOW >= 96 && Object.keys(GENS).length > 60, `the server's table: generation ${NOW}, ${Object.keys(GENS).length} switches`);
const allOffAt = (g: number, except: string[]) => Object.keys(GENS).filter((id) => GENS[id] <= g && !except.includes(id)).join(',');
const PAUL = allOffAt(80, ['adsb', 'commb']);

// ── the Setup page's own functions ──
const page = read('android/app/src/main/cpp/vibe_setup_page.h');
const a = page.indexOf('function modeOwnerSet()'), b = page.indexOf('/** ★★★ CAN THIS MACHINE DECODE AAC AT ALL?');
ok(a > 0 && b > a, 'the page’s list functions found');
const ctx: any = { radio: () => ctx.R, R: { blockedModes: '' } };
vm.createContext(ctx);
vm.runInContext(page.slice(a, b) + `
  this.modeBlockSet = modeBlockSet; this.newModesState = newModesState; this.newModesStamp = newModesStamp;
  this.newModesFreeze = newModesFreeze; this.modeNoneLeft = modeNoneLeft; this.modeNewOff = modeNewOff; this.modeIsNew = modeIsNew;
  this.modeDefaultWhy = modeDefaultWhy; this.MODE_GENS = MODE_GENS; this.MODE_GEN_NOW = MODE_GEN_NOW; this.setRD = (o) => { REGION_DEFAULTS = o; };`, ctx);
const pageOff = (csv: string) => { ctx.R = { blockedModes: csv }; return ctx.modeBlockSet() as Set<string>; };
const appOff = (csv: string) => NM.enforcedOff(NM.ownSet(csv), {}, MG);
const onlyLeft = (off: Set<string>) => {
  if (!NM.LISTEN_DEMODS.every((d) => off.has(d))) return '';
  const left = NM.WHOLE_MODES.filter((w) => !off.has(w));
  return left.length === 1 ? left[0] : '';
};
{
  ok(ctx.MODE_GEN_NOW === NOW, 'page MODE_GEN_NOW = the server’s');
  ok(JSON.stringify(Object.entries(ctx.MODE_GENS).sort()) === JSON.stringify(Object.entries(GENS).sort()), 'page MODE_GENS = the server’s table');
}

// ── the same answers, page and app ──
const cases: [string, string][] = [
  ['', 'unrestricted, no stamp'],
  ['dab,drm', 'restricted, several modes on'],
  [PAUL, '★ Paul: ADS-B only, from generation 80'],
  [PAUL + ',newmodes@on', 'Paul + "switch them on"'],
  [PAUL + ',+vormap', 'Paul ticks VOR MAP'],
  ['seen@87', 'unrestricted, stamped 87'],
  ['seen@87,newmodes@off', 'stamped 87, "leave them off"'],
  ['dab,seen@87,newmodes@on', 'restricted, stamped 87, "switch them on"'],
  [allOffAt(63, ['dab']), 'DAB only from 63'],
];
for (const [csv, what] of cases) {
  const p = [...pageOff(csv)].sort().join(','), q = [...appOff(csv)].sort().join(',');
  ok(p === q, `${what}: page and app enforce the same list`);
}
{
  const off = pageOff(PAUL);
  for (const id of ['dvsig', 'vormap', 'orbcomm', 'sat']) ok(off.has(id), `Paul: new switch ${id} off`);
  ok(!off.has('adsb'), 'Paul: ADS-B on');
  ok(onlyLeft(off) === 'adsb', '★★★ Paul after the update: ADS-B only (the exit lock)');
  ok(onlyLeft(pageOff(PAUL + ',newmodes@on')) === 'adsb', 'single-mode wins over "switch them on"');
  ok(onlyLeft(appOff(PAUL)) === 'adsb', 'the app agrees: ADS-B only');
  ok(!pageOff('').has('sat') && !pageOff('').has('vormap'), 'unrestricted: new switches on');
  ok(pageOff('dab,drm').has('sat') && !pageOff('dab,drm,newmodes@on').has('sat'), 'restricted: SATELLITE off unless "switch them on"');
  ctx.R = { blockedModes: PAUL };
  ok(ctx.modeDefaultWhy('vormap') === 'NEW (off until you tick it)', 'the Setup page marks a new switch "NEW (off until you tick it)"');
  ctx.R = { blockedModes: '' };
  ok(ctx.modeDefaultWhy('vormap') === '', 'nothing marked on an unrestricted radio with nothing new');
}

// ── the stamp keeps the enforced list exactly, and moves "seen" to now ──
for (const [csv, what] of cases) {
  const before = [...pageOff(csv)].sort().join(',');
  const r = { blockedModes: csv };
  ctx.newModesStamp(r);
  const after = [...pageOff(r.blockedModes)].sort().join(',');
  ok(before === after, `stamp (page), ${what}: same list enforced after the save`);
  ok(r.blockedModes.split(',').includes('seen@' + NOW), `stamp (page), ${what}: seen@${NOW}`);
  const v = NM.stamp(csv, MG, {});
  ok([...appOff(v)].sort().join(',') === before && v === r.blockedModes, `stamp (app), ${what}: the page's result`);
  const f = { blockedModes: csv }; ctx.newModesFreeze(f);
  ok([...pageOff(f.blockedModes)].sort().join(',') === before, `freeze, ${what}: nothing changes`);
}
{   // after the stamp, the NEXT update's switch is judged from here: simulate generation 97 with the app's pure rule
  const later: NM.ModeGens = { now: NOW + 1, gens: { ...GENS, newmode: NOW + 1 } };
  const paulSaved = NM.stamp(PAUL, MG, {});
  ok(NM.enforcedOff(NM.ownSet(paulSaved), {}, later).has('newmode'), 'Paul saved in 96, a mode added in 97: off (still single-mode)');
  const open = NM.stamp('', MG, {});
  ok(!NM.enforcedOff(NM.ownSet(open), {}, later).has('newmode'), 'an unrestricted radio saved in 96, a mode added in 97: on');
  const leaveOff = NM.setPolicy(NM.stamp('', MG, {}), 'off');
  ok(NM.enforcedOff(NM.ownSet(leaveOff), {}, later).has('newmode'), '"leave them off": a mode added in 97 is off');
}

// ── ALL ON / ALL OFF and "never no modes" ──
{
  const ids = Object.keys(GENS);
  const allOff = NM.allSet('', false, ids, 'adsb', {}, MG)!;
  ok(onlyLeft(appOff(allOff)) === 'adsb', 'ALL OFF keeping the landing mode (ADS-B): ADS-B only, the exit lock applies');
  ok(NM.allSet('', false, ids, '', {}, MG) === null, 'ALL OFF with no landing mode it can name: refused');
  ok(NM.allSet('', false, ids, 'zzz', {}, MG) === null, 'ALL OFF with a landing mode not on the card: refused');
  const allOn = NM.allSet(PAUL, true, ids, 'adsb', {}, MG)!;
  ok([...appOff(allOn)].length === 0, 'ALL ON: nothing off (new switches ticked with "+id")');
  ok(!NM.allSet('avdata+text', true, ids, '', {}, MG)!.includes('avdata+text') === false, 'ALL ON never touches a "+" opt-in (message text stays as it was)');
  ok(!NM.allSet('', true, ids, '', {}, MG)!.includes('avdata+text'), '…and never switches one on');
  ok(NM.noneLeft(NM.allSet('', false, ids, 'wfm', {}, MG)!, {}, MG) === false, 'ALL OFF keeps one mode: not "none left"');
  ok(NM.noneLeft(allOffAt(NOW, []), {}, MG) === true && ctx.modeNoneLeft(allOffAt(NOW, [])) === true, 'every switch off = none left (page and app)');
}

// ── the page wires it (source contracts) ──
{
  ok(/function collect\(\) \{\n  stashRadio\(\);\n  newModesStampDrawn\(\);/.test(page), 'collect() stamps the drawn radios before every save');
  ok((page.match(/if \(modesStop\(\)\) return;/g) || []).length === 2, 'both save buttons refuse a radio with no modes');
  ok(/\$\("modeAllOn"\)\.addEventListener\("click"/.test(page) && /\$\("modeAllOff"\)\.addEventListener\("click"/.test(page), 'All on / All off wired');
  ok(/<button type="button" class="ghost" id="modeAllOn">All on<\/button>/.test(page), 'All on is a real <button> (keyboard / pad reachable)');
  ok(!/function modeAllSet[\s\S]{0,2500}fetch\(/.test(page.slice(page.indexOf('function modeAllSet'), page.indexOf('function modeAllSet') + 2600)), 'All on / All off save nothing (no fetch)');
  // ★★★ (B) the locked radio's card is drawn: renderBands draws the modes BEFORE its locked-range return
  const rb = page.slice(page.indexOf('function renderBands() {'), page.indexOf('function renderBands() {') + 2500);
  ok(rb.indexOf('modeBlockRender(); modeBlockSummary();') > 0 && rb.indexOf('modeBlockRender(); modeBlockSummary();') < rb.indexOf('if (!single) return;'),
     '★★★ renderBands draws the Modes card for a LOCKED radio too (Paul: "all modes were ticked again")');
  ok(/if \(capsSig\(\) !== MODE_CAPS_DRAWN\) \{ modeBlockRender\(\); modeBlockSummary\(\); \}/.test(page), 'capsApply redraws a locked radio’s list too');
  ok(/curRadio = keepSerial \? radioList\(\)\.findIndex\(x => x\.serial === keepSerial\) : -1;/.test(page), 'sdrReloadRadios keeps the open radio by serial');
  ok(/\{ id: "sat", label: "SATELLITE mode/.test(page), 'SATELLITE is a switch on the Setup page');
  const sm = read('src/screens/ServerModeScreen.tsx');
  ok(/\{ id: 'sat', label: 'SATELLITE mode/.test(sm), '…and on the app’s server screen');
  ok(/NM\.allSet\(/.test(sm) && /'Undo'/.test(sm) && /NM\.setPolicy\(blockedModes, 'off'\)/.test(sm), 'the app: All on / All off with Undo, the policy row');
  ok(/setModeGens\(NM\.parseModeGens\(j\)\)/.test(sm), 'the app reads the engine’s table');
}

// ── SATELLITE on the clients (satCapFrom) — Paul's exact radio shape ──
{
  const paulHw = { doppler: 1, blocked: PAUL.split(','), onlyMode: 'adsb' };
  ok(satCapFrom(paulHw) === null, '★★★ Paul’s ADS-B radio: no SATELLITE (blocked, onlyMode)');
  ok(satCapFrom({ doppler: 1, blocked: ['sat'] }) === null, '"sat" blocked: none');
  ok(satCapFrom({ doppler: 1, blocked: ['nfm', 'usb', 'lsb'] }) === null, 'NFM, USB and LSB all off: none');
  ok(satCapFrom({ doppler: 1, onlyMode: 'dab' }) === null, 'a single-mode radio: none');
  ok(!!satCapFrom({ doppler: 1, blocked: ['nfm'] }), 'one of the three still on: offered');
  ok(!!satCapFrom({ doppler: 1 }), 'an ordinary radio: offered');
}

console.log(`\ntest_new_modes_clients: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
