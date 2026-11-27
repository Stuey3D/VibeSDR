// test-setup-android-e2e.mjs — the WHOLE /setup page, its own scripts running in jsdom, against an Android phone's
// /vibeserver/config (beta88 "Remote access to server settings"). Sign in with the admin password, change settings, press
// Apply — and the body the page posts goes through the phone's own translation (vibe_android_setup.h, via the compiled
// test-android-setup --patch) to the patch the phone stores. What must hold:
//   1. Sign-in works against the phone's config: no exception, the Android prune ran, the "This phone" card is there.
//   2. Apply posts with the admin proof (nonce + HMAC) and restart:true.
//   3. The patch carries exactly the changes made, and nothing the phone does not have.
// Needs the test binary: VIBE_ANDROID_SETUP_BIN=<path to a built vibeserver/test-android-setup> (exit 3 = not run).
//   node scripts/test-setup-android-e2e.mjs
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { JSDOM, VirtualConsole } from 'jsdom';

const bin = process.env.VIBE_ANDROID_SETUP_BIN;
if (!bin || !fs.existsSync(bin)) { console.log('   (not run — set VIBE_ANDROID_SETUP_BIN to a built vibeserver/test-android-setup)'); process.exit(3); }

const page = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const html = page.match(/kVibeSetupPage = R"HTML\(([\s\S]*?)\)HTML"/)[1];
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

// The phone's config, made by the phone's own code (pageConfig) from a phone's settings.
const settings = { name: 'Allotment Moto', advertise: true, landingMessage: 'Hello', uncompressedAudio: 0, maxUsers: 2,
  radioLabel: 'V4', antenna: 'Discone', centerFreq: 198000, mode: 'am', sessionLimitMin: 0, idleGraceSec: 300,
  galleryMb: 100, batteryPauseAt: 0, pin: '', adminPassword: 'not-in-the-page',
  // ★★ beta88 every setting remote — the new ones, at values that are not their defaults
  advanced: true, webServer: true, compressAudio: true, radioUse: 'single', rttySkimWorkers: 2,
  serverLocation: { mode: 'manual', lat: 52.25, lon: -0.9, label: 'Moulton', country: 'GB', moving: false },
  publicName: 'Moulton V4', sampleRate: 2400000, lockedRate: 0, rateLock: false, maxFftRate: 10,
  ppm: 3, tunerBwAuto: true, directSampling: 0, converterInputLoHz: 0, blockedModes: 'drm', fmDeemph: 50, fmStereo: true };
const raw = JSON.stringify({ settings, device: { batteryOptimisation: 'off', startOnPowerSupported: false, radioPresent: true,
  usbPermission: true, keepRadioAlive: false, tunnelSupported: true, listing: { wanted: false } },
  radio: { driver: 'rtl', model: 'RTL-SDR Blog V4' } });
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-android-setup-'));
const rawFile = path.join(tmp, 'raw.json');
fs.writeFileSync(rawFile, raw);
const cfgJson = execFileSync(bin, ['--page', rawFile], { encoding: 'utf8' });
ok(!cfgJson.includes('not-in-the-page'), 'the admin password never reaches the page');

const errors = [];
const vc = new VirtualConsole();
vc.on('jsdomError', (e) => errors.push(String(e && (e.detail || e.message || e))));
let posted = null, postedUrl = '', authAsked = 0;
const dom = new JSDOM(`<!doctype html>${html}`, {
  runScripts: 'dangerously', url: 'http://192.168.1.50:48000/setup', virtualConsole: vc, pretendToBeVisual: true,
  beforeParse(w) {
    w.fetch = async (url, opts = {}) => {
      const u = String(url);
      const json = (o, status = 200) => ({ ok: status < 400, status, json: async () => o, text: async () => JSON.stringify(o) });
      if (u.startsWith('/vibeserver/auth')) { authAsked++; return json({ nonce: 'abc123' }); }
      if (u.startsWith('/vibeserver/config')) {
        if (!/vs_admin_auth=[0-9a-f]{64}/.test(u)) return json({ error: 'admin password required' }, 401);
        if ((opts.method || 'GET') === 'POST') { posted = JSON.parse(opts.body); postedUrl = u; return json({ ok: true, restart: true }); }
        return json(JSON.parse(cfgJson));
      }
      if (u.startsWith('/vibeserver.json')) return json({ server: 'vibeserver', configured: true });
      return json({}, 404);
    };
    w.alert = () => {}; w.confirm = () => true; w.scrollTo = () => {};
    w.HTMLElement.prototype.scrollIntoView = function () {};
  },
});
const w = dom.window, doc = w.document;
await new Promise((r) => setTimeout(r, 200));

doc.getElementById('pass').value = 'owner-typed-password';
await w.eval('signIn(false)');
await new Promise((r) => setTimeout(r, 400));
ok(errors.length === 0, `sign-in ran without a script error${errors.length ? ': ' + errors.slice(0, 3).join(' | ') : ''}`);
ok(authAsked > 0, 'the page asked for a nonce (the admin challenge)');
ok(!doc.getElementById('setup').classList.contains('hide'), 'the settings are shown after sign-in');
ok(!!doc.getElementById('androidDevCard'), 'the "This phone" card is drawn');
ok(doc.getElementById('name').value === 'Allotment Moto', 'the phone\'s name is in the box');
ok(doc.getElementById('landingMessage').value === 'Hello', 'the landing message is in the box');
ok(doc.getElementById('place').value === 'Moulton' && doc.getElementById('lat').value === '52.25000', 'the receiver\'s place and position are in the boxes');
ok(doc.getElementById('dirName').value === 'Moulton V4', 'the public name is in the box');
ok(!doc.getElementById('androidAdvanced') && doc.getElementById('androidWebServer').checked && doc.getElementById('androidCompress').checked,
   'the phone\'s own server card: web client and compression on, no Simple / Advanced choice (beta96)');
ok(doc.getElementById('rttySkimWorkers').value === '2', 'the RTTY skimmer workers');

// Change things on the server tab, then on the radio tab, and Apply.
doc.getElementById('landingMessage').value = 'Welcome to the allotment';
doc.getElementById('androidCompress').checked = false;
doc.getElementById('place').value = 'Northampton';
doc.getElementById('lat').value = '52.2405'; doc.getElementById('lon').value = '-0.9027';
doc.getElementById('dirName').value = 'Allotment';
const tabs = [...doc.querySelectorAll('#radioTabs button')];
const radioTab = tabs.find((b) => !/server/i.test(b.textContent));
if (radioTab) radioTab.click();
await new Promise((r) => setTimeout(r, 300));
doc.getElementById('users').value = '5';
doc.getElementById('radioName').value = 'V4 on the mast';
ok(doc.getElementById('ppm').value === '3', 'the radio tab shows the phone\'s ppm');
ok(doc.getElementById('androidTunerBw').checked, '…and the phone\'s IF filter row');
const vis = (id) => { for (let e = doc.getElementById(id); e; e = e.parentElement) if (e.classList && e.classList.contains('androidOff')) return false; return !!doc.getElementById(id); };
ok(vis('ppm') && vis('androidTunerBw') && vis('adsb256') && !vis('ppb') && !vis('icomBaud'), 'an RTL\'s rows are offered and no other radio\'s (after the radio\'s own rows were drawn)');
// ★★ beta96 (Stuart 11-26: "on android devices on remote setup … the button to add range and filters dont work"): the rows
//    are drawn in #antRanges / #antFilters, whose generated inputs have no id — the containers must be kept, or a new row is
//    added inside a hidden box. Add a range and a filter, type in them, and they must be visible and posted.
{
  doc.getElementById('antRangeAdd').click();
  doc.getElementById('antFilterAdd').click();
  // ★ then the page re-runs its phone pruning (a tab switch, the radio's own rows arriving late) — the real-phone order
  const srvTab = tabs.find((b) => /server/i.test(b.textContent));
  if (srvTab) { srvTab.click(); await new Promise((r) => setTimeout(r, 200)); }
  if (radioTab) { radioTab.click(); await new Promise((r) => setTimeout(r, 300)); }
  doc.getElementById('antRangeAdd').click();
  const rIn = doc.querySelector('#antRanges [data-f="from"]'), fIn = doc.querySelector('#antFilters [data-f="from"]');
  const shown = (e) => { for (let x = e; x; x = x.parentElement) if (x.classList && x.classList.contains('androidOff')) return false; return !!e; };
  const fRow = doc.querySelector('#antFilters [data-l]');   // ★ a filter has no "from" until its type is ticked — the row itself
  ok(shown(rIn), `+ Add range: the new row is VISIBLE on a phone (it was added inside a hidden box) — input ${!!rIn}`);
  ok(shown(fRow), `+ Add filter: the new row is VISIBLE on a phone — row ${!!fRow}, from ${!!fIn}`);
  if (rIn) { rIn.value = '137'; rIn.dispatchEvent(new w.Event('input', { bubbles: true })); }
  const rTo = doc.querySelector('#antRanges [data-f="to"]');
  if (rTo) { rTo.value = '138'; rTo.dispatchEvent(new w.Event('input', { bubbles: true })); }
}
// ★★★ b96-2 (Stuart 11-27: "why cant i schedule satellite passes on the xcover?"): UNATTENDED SATELLITE PASSES (#hwSatPass)
//     were in no row of the phone's control table, so androidPrune hid the whole block — while the phone's server runs the
//     passes (the shim's satPassTick, every platform) and its own screen offers them. The block, its switch, the elevation,
//     the Meteor / ORBCOMM groups and the generated downlink ticks (#spBirds, no ids of their own) must be on the page, and a
//     save must carry satPasses inside bgDecode to the phone's settings.
{
  const shown = (id) => vis(id) && !doc.getElementById(id).classList.contains('hide');
  ok(shown('hwSatPass'), 'Unattended satellite passes: the block is on the phone\'s page');
  ok(vis('spOn') && vis('spMinEl') && vis('spMeteor') && vis('spOrbcomm'), '…its switch, elevation and the Meteor / ORBCOMM groups');
  const bird = doc.querySelector('#spBirds input[type="checkbox"]');
  ok(!!bird && vis('spBirds'), `…and the other downlinks' ticks (generated in #spBirds, no ids) — tick ${!!bird}`);
  // ★ the same class, found beside it: "When an update adds new modes" (radio tab) and "Not at this site" (server tab)
  ok(vis('newModesPolOn') && vis('newModesPolOff'), '"When an update adds new modes" is on the phone\'s page');
  ok(vis('feedOffsite'), '"Not at this site" (+ Add aircraft feed) is on the phone\'s page');
  const sp = doc.getElementById('spOn');
  sp.checked = true; sp.dispatchEvent(new w.Event('change', { bubbles: true }));
  const el = doc.getElementById('spMinEl');
  el.value = '20'; el.dispatchEvent(new w.Event('change', { bubbles: true }));
  const me = doc.getElementById('spMeteor');
  me.checked = true; me.dispatchEvent(new w.Event('change', { bubbles: true }));
}
doc.getElementById('ppm').value = '-4';
doc.getElementById('androidTunerBw').checked = false;
doc.getElementById('androidDsFixed').value = '2';
doc.getElementById('maxFps').value = '20';
doc.getElementById('maxFps').dispatchEvent(new w.Event('change', { bubbles: true }));
doc.getElementById('saveBtn').click();
await new Promise((r) => setTimeout(r, 600));
ok(errors.length === 0, `Apply ran without a script error${errors.length ? ': ' + errors.slice(0, 3).join(' | ') : ''}`);
ok(!!posted, 'Apply posted the settings');
ok(/vs_admin_nonce=abc123&vs_admin_auth=[0-9a-f]{64}/.test(postedUrl), '…with the admin proof');
ok(posted && posted.restart === true, '…and restart:true');

if (posted) {
  const bodyFile = path.join(tmp, 'body.json');
  fs.writeFileSync(bodyFile, JSON.stringify(posted));
  const patch = JSON.parse(execFileSync(bin, ['--patch', bodyFile, rawFile], { encoding: 'utf8' }));
  ok(patch.landingMessage === 'Welcome to the allotment', 'the patch carries the new landing message');
  ok(patch.maxUsers === 5, 'the patch carries the new listener limit');
  ok(patch.radioLabel === 'V4 on the mast', 'the patch carries the new radio name');
  ok(patch.name === 'Allotment Moto' && patch.centerFreq === 198000 && patch.mode === 'am', 'untouched settings come back as they were');
  // ★ The page posts its radio objects back whole (collect: radios = cfg.radios), so an untouched PIN comes back AS IT WAS.
  ok(!('pin' in patch) || patch.pin === settings.pin, 'an untouched PIN comes back unchanged');
  ok(!('adminPassword' in patch) && !('cpuGovernor' in patch) && !('port' in patch), 'nothing the phone does not have');
  // ★★ beta88 — the new settings, through the phone's own translation
  ok(patch.compressAudio === false && patch.advanced === true && patch.webServer === true, 'the phone\'s own server card: compression off, Advanced kept');
  ok(patch.serverLocation && patch.serverLocation.mode === 'manual' && patch.serverLocation.label === 'Northampton'
     && Math.abs(patch.serverLocation.lat - 52.2405) < 1e-9, 'the new place and position');
  ok(patch.publicName === 'Allotment' && patch.publicListed === false, 'the public name; the listing left off');
  ok(patch.ppm === -4 && patch.tunerBwAuto === false && patch.directSampling === 2, 'ppm, the IF filter and the fixed branch');
  ok(patch.maxFftRate === 20, 'the waterfall rate');
  ok(patch.lockedRate === 0 && patch.rateLock === false, 'the capture width stays the listener\'s choice');
  // ★★★ beta96 NEW MODES OFF (docs/v12/NEW-MODES-OFF.md): an UNSTAMPED list that blocks anything is a restricted radio, so the
  //     page stamps it "seen@<now>" on opening and the switch added since (SATELLITE, generation 96) comes back OFF by name —
  //     the owner's "drm" kept exactly. Nothing else is touched.
  const reach = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_decoder_reach.h', import.meta.url), 'utf8');
  const genNow = Number((reach.match(/constexpr int kModeGenNow = (\d+);/) || [])[1]);
  const bm = String(patch.blockedModes || '').split(',');
  ok(patch.rttySkimWorkers === 2 && patch.fmDeemph === 50 && patch.radioUse === 'single'
     && bm.includes('drm') && bm.includes('seen@' + genNow) && bm.includes('sat') && !bm.includes('wfm') && !bm.includes('am'),
     `untouched new settings come back as they were (blocked: "drm" kept, + the seen@${genNow} stamp and SATELLITE off as new — ${patch.blockedModes})`);
  ok(!('ppb' in patch) && !('rfNotch' in patch) && !('icomBaud' in patch), 'no other radio\'s settings');
  {
    let bg = null; try { bg = JSON.parse(patch.bgDecode || 'null'); } catch (e) { /* stays null */ }
    const sp = bg && bg.satPasses;
    ok(!!sp && sp.on === true && sp.minEl === 20 && sp.meteor === true,
       `unattended satellite passes reach the phone's settings inside bgDecode (on, 20°, Meteor): ${JSON.stringify(patch.bgDecode)}`);
    // ★ the phone's own reading of it: the page config hands the same pass settings back to the page
    const raw2 = JSON.parse(raw); raw2.settings.bgDecode = patch.bgDecode;
    const raw2File = path.join(tmp, 'raw2.json'); fs.writeFileSync(raw2File, JSON.stringify(raw2));
    const cfg2 = JSON.parse(execFileSync(bin, ['--page', raw2File], { encoding: 'utf8' }));
    let back = null; try { back = JSON.parse(cfg2.radios[0].bgDecode).satPasses; } catch (e) { /* stays null */ }
    ok(!!back && back.on === true && back.minEl === 20 && back.meteor === true, '…and come back to the page as saved (the round trip)');
  }
  ok(typeof patch.antennaRanges === 'string' && /137/.test(patch.antennaRanges) && /138/.test(patch.antennaRanges), `the range typed on the page reaches the phone's settings: ${JSON.stringify(patch.antennaRanges)}`);
}
fs.rmSync(tmp, { recursive: true, force: true });
w.close();
console.log(fail ? `   ${fail} FAILED` : '   all passed');
process.exit(fail ? 1 : 0);
