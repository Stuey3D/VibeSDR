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
  galleryMb: 100, batteryPauseAt: 0, pin: '', adminPassword: 'not-in-the-page' };
const raw = JSON.stringify({ settings, device: { batteryOptimisation: 'off', startOnPowerSupported: false, radioPresent: true,
  usbPermission: true, keepRadioAlive: false }, radio: { driver: 'rtl', model: 'RTL-SDR Blog V4' } });
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

// Change things on the server tab, then on the radio tab, and Apply.
doc.getElementById('landingMessage').value = 'Welcome to the allotment';
const tabs = [...doc.querySelectorAll('#radioTabs button')];
const radioTab = tabs.find((b) => !/server/i.test(b.textContent));
if (radioTab) radioTab.click();
await new Promise((r) => setTimeout(r, 300));
doc.getElementById('users').value = '5';
doc.getElementById('radioName').value = 'V4 on the mast';
doc.getElementById('saveBtn').click();
await new Promise((r) => setTimeout(r, 600));
ok(errors.length === 0, `Apply ran without a script error${errors.length ? ': ' + errors.slice(0, 3).join(' | ') : ''}`);
ok(!!posted, 'Apply posted the settings');
ok(/vs_admin_nonce=abc123&vs_admin_auth=[0-9a-f]{64}/.test(postedUrl), '…with the admin proof');
ok(posted && posted.restart === true, '…and restart:true');

if (posted) {
  const bodyFile = path.join(tmp, 'body.json');
  fs.writeFileSync(bodyFile, JSON.stringify(posted));
  const patch = JSON.parse(execFileSync(bin, ['--patch', bodyFile], { encoding: 'utf8' }));
  ok(patch.landingMessage === 'Welcome to the allotment', 'the patch carries the new landing message');
  ok(patch.maxUsers === 5, 'the patch carries the new listener limit');
  ok(patch.radioLabel === 'V4 on the mast', 'the patch carries the new radio name');
  ok(patch.name === 'Allotment Moto' && patch.centerFreq === 198000 && patch.mode === 'am', 'untouched settings come back as they were');
  // ★ The page posts its radio objects back whole (collect: radios = cfg.radios), so an untouched PIN comes back AS IT WAS.
  ok(!('pin' in patch) || patch.pin === settings.pin, 'an untouched PIN comes back unchanged');
  ok(!('adminPassword' in patch) && !('cpuGovernor' in patch) && !('port' in patch), 'nothing the phone does not have');
}
fs.rmSync(tmp, { recursive: true, force: true });
w.close();
console.log(fail ? `   ${fail} FAILED` : '   all passed');
process.exit(fail ? 1 : 0);
