// check-remote-keys.mjs — "Remote access to server settings" (beta88; EVERY setting since 2026-11-20). The same settings
// live in several lists, and they must agree or a setting changed on the /setup page never reaches the phone's screen (or
// the other way round), or — worse — a new setting on the phone's screen is simply not on the page and nobody notices:
//   1. vibe_android_setup.h settingsPatch() — the Android keys a page save may set;
//   2. src/services/serverSettingsMirror.ts — the keys the phone's screen takes back from the server (REMOTE_KEYS plain,
//      SPECIAL_KEYS in the screen's own shape), and what is device-only or not a setting (DEVICE_ONLY, NOT_SETTINGS);
//   3. ServerModeScreen's K table (and the name / location / public-name keys it reads beside it);
//   4. nativeServerConfig (vibeServer.ts) — the config every Start writes WHOLE into the phone's settings store, so a
//      mirrored key it does not send would be wiped by the next Start;
//   5. the header's capture-width lists against the screen's RATE_OPTIONS_*.
// ★★★ AND THE GUARD STUART ASKED FOR: a key in the screen's K table that is neither remote nor listed (with why) as
//     device-only or not a setting FAILS here — a future setting cannot be forgotten on the page.
// node scripts/check-remote-keys.mjs
import fs from 'node:fs';
const rd = (p) => fs.readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const header = rd('android/app/src/main/cpp/vibe_android_setup.h');
const mirror = rd('src/services/serverSettingsMirror.ts');
const screen = rd('src/screens/ServerModeScreen.tsx');
const vibeServer = rd('src/services/vibeServer.ts');
const rtlTcp = rd('src/services/rtlTcpServer.ts');
const doc = rd('docs/v12/ANDROID-SERVER-PROCESS.md');
let fail = 0, pass = 0;
const ok = (c, what) => { if (!c || process.env.VERBOSE) console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); c ? pass++ : fail++; };

// ── the constants the files name by identifier ──
const constOf = (src, name) => (src.match(new RegExp(`export const ${name}\\s*=\\s*'([^']+)'`)) || [])[1];
const IDS = { NAME_KEY: constOf(rtlTcp, 'NAME_KEY'), PUBLIC_NAME_KEY: constOf(rtlTcp, 'PUBLIC_NAME_KEY'),
              LOC_KEY: constOf(vibeServer, 'LOC_KEY'), LOCMODE_KEY: constOf(vibeServer, 'LOCMODE_KEY'), LOC_MOVING_KEY: constOf(vibeServer, 'LOC_MOVING_KEY') };
ok(Object.values(IDS).every(Boolean), `the storage-key constants are found (${Object.entries(IDS).map(([k, v]) => k + '=' + v).join(' ')})`);

// ── 1. the header: what a page save sets ──
const patchFn = header.slice(header.indexOf('inline bool settingsPatch(const std::string& body, const std::string& current'));
const patched = new Set([
  ...[...patchFn.matchAll(/p\.set\("([A-Za-z0-9]+)"/g)].map((m) => m[1]),
  ...[...patchFn.matchAll(/(?:str|num|flag|jsonText)\((?:&b|r|b), *"[A-Za-z0-9]+", *"([A-Za-z0-9]+)"/g)].map((m) => m[1]),
  ...[...patchFn.matchAll(/for \(const char\* k : \{([^}]*)\}\)\s*flag\(r, k, k\)/g)].flatMap((m) => [...m[1].matchAll(/"([A-Za-z0-9]+)"/g)].map((x) => x[1])),
]);
// Actions (not stored) and keys derived from another one.
const ACTIONS = new Set(['publicListed']);

// ── 2. the mirror ──
const remote = [...mirror.matchAll(/\{ native: '([A-Za-z0-9]+)',\s*storage: ([A-Za-z_]+|'[a-z_0-9]+'),\s*enc: '([a-z0-9]+)' \}/g)]
  .map((m) => ({ native: m[1], storage: m[2].startsWith("'") ? m[2].slice(1, -1) : IDS[m[2]], enc: m[3] }));
const specialBlock = mirror.slice(mirror.indexOf('export const SPECIAL_KEYS'), mirror.indexOf('};', mirror.indexOf('export const SPECIAL_KEYS')));
const special = new Map([...specialBlock.matchAll(/([A-Za-z0-9]+): \[([^\]]*)\]/g)].map((m) => [m[1], [...m[2].matchAll(/'([a-z_0-9]+)'/g)].map((x) => x[1])]));
const listOf = (name) => {
  const b = mirror.slice(mirror.indexOf(`export const ${name}`), mirror.indexOf('};', mirror.indexOf(`export const ${name}`)));
  return new Map([...b.matchAll(/(vs_[a-z0-9]+): '([^']+)'/g)].map((m) => [m[1], m[2]]));
};
const DEVICE_ONLY = listOf('DEVICE_ONLY'), NOT_SETTINGS = listOf('NOT_SETTINGS');
const mirroredNative = new Set([...remote.map((r) => r.native), ...special.keys()]);
console.log(`remote settings: ${patched.size} keys a page save sets, ${remote.length} plain + ${special.size} special the screen takes back`);
ok(remote.length >= 60 && special.size >= 40, 'the mirror is the whole screen, not the first 28');
for (const k of patched) if (!ACTIONS.has(k)) ok(mirroredNative.has(k), `${k}: set by the page AND taken back by the screen`);
for (const k of mirroredNative) ok(patched.has(k), `${k}: taken back by the screen AND settable from the page`);
ok([...ACTIONS].every((k) => patched.has(k)) && /p\.remove\("publicListed"\)/.test(rd('android/app/src/main/java/com/vibesdr/app/VibeRemoteSetup.kt')),
   'the listing switch is an action: VibeRemoteSetup takes it out and acts, never stores it');

// ── 3. the screen: every storage key the mirror writes is one the screen reads, and every screen key is accounted for ──
const kBlock = screen.slice(screen.indexOf('const K = {'), screen.indexOf('};', screen.indexOf('const K = {')));
const kValues = new Set([...kBlock.matchAll(/'(vs_[a-z0-9]+)'/g)].map((m) => m[1]));
if (/PUBLIC_NAME_KEY/.test(kBlock)) kValues.add(IDS.PUBLIC_NAME_KEY);
const screenReads = new Set([...kValues, IDS.NAME_KEY, IDS.LOC_KEY, IDS.LOCMODE_KEY, IDS.LOC_MOVING_KEY]);
ok(/readPrefs\(AsyncStorage, \[NAME_KEY, LOC_KEY, LOCMODE_KEY, LOC_MOVING_KEY, \.\.\.Object\.values\(K\)\]\)/.test(screen), 'the screen reads its K table, the name and the location in one read');
const mirrorStorage = new Set([...remote.map((r) => r.storage), ...[...special.values()].flat()]);
for (const k of mirrorStorage) ok(screenReads.has(k), `${k} (written by the mirror) is a key the screen reads`);
console.log(`the screen: ${screenReads.size} stored settings`);
const unaccounted = [...screenReads].filter((k) => !mirrorStorage.has(k) && !DEVICE_ONLY.has(k) && !NOT_SETTINGS.has(k));
ok(unaccounted.length === 0, `★ every ServerModeScreen setting is on the remote page, or listed as device-only / not a setting with why`
   + (unaccounted.length ? ` — NOT ACCOUNTED FOR: ${unaccounted.join(', ')}. Put it on the page (vibe_android_setup.h table + pageConfig + settingsPatch, the mirror) or list it in DEVICE_ONLY / NOT_SETTINGS with the reason.` : ''));
for (const [k, why] of [...DEVICE_ONLY, ...NOT_SETTINGS]) ok(why.length > 20 && screenReads.has(k), `${k}: not remote, and says why (and is still a key of the screen's)`);
for (const k of DEVICE_ONLY.keys()) ok(doc.includes(k), `${k}: the device-only list in docs/v12/ANDROID-SERVER-PROCESS.md names it`);
// Not every phone-only fact is a stored key: Android's own permission prompts and switches. The doc and the page's card list them.
for (const w of ['Battery optimisation', 'Start when power returns', 'USB permission', 'Keep radio alive', 'Microphone', 'Remote access to server settings', 'Admin password'])
  ok(rd('android/app/src/main/cpp/vibe_setup_page.h').includes(`"${w}`) && doc.toLowerCase().includes(w.toLowerCase()), `"${w}" is on the page's read-only card and in the doc`);

// ── 4. every mirrored key is in the config every Start writes whole ──
const nsc = vibeServer.slice(vibeServer.indexOf('export function nativeServerConfig('), vibeServer.indexOf('\n}\n', vibeServer.indexOf('export function nativeServerConfig(')));
const fmKeys = rd('src/services/fmDefaults.ts'), rspKeys = rd('src/services/rspCard.ts');
const sentByStart = (k) => new RegExp(`\\b${k}:`).test(nsc) || new RegExp(`\\b${k}:`).test(fmKeys.slice(fmKeys.indexOf('fmDefaultsKeys')))
  || new RegExp(`\\b${k}:`).test(rspKeys.slice(rspKeys.indexOf('rspConfigKeys'))) || /\.\.\.\(cfg\.ppm != null/.test(nsc) && k === 'ppm'
  || (k === 'antennaSwitch' || k === 'antennaMap' || k === 'antennaLocked') && new RegExp(`\\b${k}:`).test(nsc);
for (const k of mirroredNative) {
  if (k === 'publicShareSec') continue;   // ★ the page's own share length: the screen's comes from the running tunnel, so Start does not send it
  ok(sentByStart(k), `${k}: sent by every Start (nativeServerConfig), so a Start never wipes what the page set`);
}

// ── 5. the capture widths ──
const screenRates = (name) => { const b = screen.slice(screen.indexOf(`const ${name} = [`), screen.indexOf('];', screen.indexOf(`const ${name} = [`)));
  return [...b.matchAll(/value: ([0-9_]+)/g)].map((m) => Number(m[1].replace(/_/g, ''))).filter((v) => v > 0); };
const headerRates = (name) => { const m = header.match(new RegExp(`static const std::vector<double> ${name} = \\{([^}]*)\\}`));
  return m ? m[1].split(',').map((x) => Number(x.trim())) : []; };
for (const [scr, hdr] of [['RATE_OPTIONS_RTL', 'rtl'], ['RATE_OPTIONS_HRF', 'hrf'], ['RATE_OPTIONS_RSP', 'rsp'], ['RATE_OPTIONS_AHF', 'ahf'], ['RATE_OPTIONS_ASP', 'asp']]) {
  const a = screenRates(scr).sort((x, y) => x - y).join(','), b = headerRates(hdr).sort((x, y) => x - y).join(',');
  ok(a && a === b, `${scr} = the header's ${hdr} list (${a})`);
}

console.log(fail ? `   ${pass} passed, ${fail} FAILED` : `   all ${pass} passed`);
process.exit(fail ? 1 : 0);
