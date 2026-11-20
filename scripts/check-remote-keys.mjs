// check-remote-keys.mjs — "Remote access to server settings" (beta88) has THREE lists of the same settings, and they must
// agree or a setting changed on the /setup page never reaches the phone's screen (or the other way round):
//   1. vibe_android_setup.h settingsPatch() — the Android keys a page save may set;
//   2. src/services/serverSettingsMirror.ts REMOTE_KEYS — the keys the phone's screen takes back from the server;
//   3. ServerModeScreen's K table — the storage keys the screen actually reads.
// node scripts/check-remote-keys.mjs
import fs from 'node:fs';
const rd = (p) => fs.readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const header = rd('android/app/src/main/cpp/vibe_android_setup.h');
const mirror = rd('src/services/serverSettingsMirror.ts');
const screen = rd('src/screens/ServerModeScreen.tsx');
let fail = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fail++; };

const patchFn = header.slice(header.indexOf('inline bool settingsPatch('));
const patched = new Set([...patchFn.matchAll(/p\.set\("([A-Za-z]+)"/g), ...patchFn.matchAll(/(?:str|num|flag)\((?:&b|r|&r|b), *"[A-Za-z]+", *"([A-Za-z]+)"/g),
  ...patchFn.matchAll(/str\("[A-Za-z]+", *"([A-Za-z]+)"/g)].map((m) => m[1]));
// Handled specially by the mirror (landing, PIN) or derived (resume = pause + 20).
const special = new Set(['centerFreq', 'mode', 'pin', 'batteryResumeAt']);
const mirrored = new Set([...mirror.matchAll(/native: '([A-Za-z]+)'/g)].map((m) => m[1]));
console.log(`remote settings: ${patched.size} keys a page save sets, ${mirrored.size} the screen takes back`);
for (const k of patched) if (!special.has(k)) ok(mirrored.has(k), `${k}: set by the page AND taken back by the screen`);
for (const k of mirrored) ok(patched.has(k), `${k}: taken back by the screen AND settable from the page`);
ok(/'vs_landinghz'/.test(mirror) && /'vs_landingmode'/.test(mirror) && /'vs_pinmode'/.test(mirror), 'landing and PIN mirrored specially');

const kBlock = screen.slice(screen.indexOf('const K = {'), screen.indexOf('};', screen.indexOf('const K = {')));
const kValues = new Set([...kBlock.matchAll(/'(vs_[a-z0-9]+)'/g)].map((m) => m[1]));
for (const m of mirror.matchAll(/storage: '(vs_[a-z0-9]+)'/g)) ok(kValues.has(m[1]), `${m[1]} is a key the screen reads`);
for (const k of ['vs_landinghz', 'vs_landingmode', 'vs_pin', 'vs_pinmode']) ok(kValues.has(k), `${k} is a key the screen reads`);
console.log(fail ? `   ${fail} FAILED` : '   all passed');
process.exit(fail ? 1 : 0);
