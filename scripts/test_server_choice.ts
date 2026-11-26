// ★★★ beta96 "Use as a server" — the entry, VibeServer Simple, remote first-time setup, the device-only password / PIN /
// reset (docs/v12/SERVER-CHOICE.md). node --no-warnings scripts/test_server_choice.ts
//   A. the pure rules (src/utils/serverChoice.ts): choices, first open, remote setup offered, the .local label, Simple's
//      start config, merge (Full's settings KEPT) and view (LAN only), the status line, the password rule, the reset keys;
//   B. the TWINS in Kotlin agree (VibeServerBoot / VibeServerOps lists and hostSlug) — read from the source;
//   C. the native wiring that makes the rules true (LAN-only tunnel gates on every start path, no permanent remote access
//      from first-time setup, the code refused with a password, the reset's password check, remote refusals);
//   D. the screens: D-pad (every touchable in the new files carries a focus ring), START/STOP first in Full, the reset at
//      the bottom, Simple's four saved and sent, the remembered choice, Simple ↔ Full hand-over, Lite's entry.
import { readFileSync } from 'node:fs';
import * as SC from '../src/utils/serverChoice.ts';

let fails = 0, passes = 0;
const ok = (c: unknown, what: string) => { if (c) passes++; else { fails++; console.log('  FAIL ' + what); } };
const rd = (p: string) => readFileSync(new URL('../' + p, import.meta.url), 'utf8');

// ── A. the pure rules ───────────────────────────────────────────────────────────────────────────────────────────────
ok(SC.choicesFor({ lite: false, rtlTcpSupported: true, icom: false }).join() === 'simple,full,rtltcp', 'the app offers three');
ok(SC.choicesFor({ lite: true, rtlTcpSupported: true, icom: false }).join() === 'simple,full', 'Lite offers Simple and Full (no RTL-TCP)');
ok(SC.choicesFor({ lite: false, rtlTcpSupported: true, icom: true }).join() === 'simple,full', 'an Icom: no RTL-TCP (no raw IQ)');
ok(SC.routeFor('simple') === 'SimpleServer' && SC.routeFor('full') === 'ServerMode' && SC.routeFor('rtltcp') === 'RtlTcpServer', 'routes');
ok(SC.parseChoice('full') === 'full' && SC.parseChoice('nonsense') === null && SC.parseChoice(null) === null, 'the remembered choice parses');
ok(SC.isFirstOpen(null, false) && !SC.isFirstOpen('simple', false) && !SC.isFirstOpen(null, true), 'first open = never chosen and no admin password');
ok(SC.remoteSetupOffered(false, true) && !SC.remoteSetupOffered(true, true) && !SC.remoteSetupOffered(false, false),
   'REMOTE SETUP shown only with no admin password (first-time only) and a build that has it');
ok(SC.mdnsLabel('Stuey3D XCover4S!') === 'stuey3d-xcover4s' && SC.mdnsLabel('---') === 'vibesdr'
   && SC.mdnsLabel('x'.repeat(50)).length === 32, 'the .local label as the server makes it');

const st = SC.simpleStartConfig({ name: ' Kitchen TV ', mdnsName: ' kitchen ', rate: 0, topRate: 912_000, fps: 10 });
ok(st.simple === true && st.name === 'Kitchen TV' && st.mdnsName === 'kitchen' && st.sampleRate === 912_000
   && st.lockedRate === 0 && st.rateLock === false && st.maxFftRate === 10, 'Simple sends its four (client-controlled: the radio\'s top rate)');
const pinned = SC.simpleStartConfig({ name: '', mdnsName: '', rate: 1_200_000, topRate: 2_400_000, fps: 20 });
ok(pinned.name === 'VibeSDR' && pinned.sampleRate === 1_200_000 && pinned.lockedRate === 1_200_000 && pinned.rateLock, 'a chosen width pins');
ok(Object.keys(st).every((k) => (SC.SIMPLE_KEYS as readonly string[]).includes(k)), 'Simple sends ONLY its own keys');

const full = { name: 'Old', communityMap: true, pskrOn: true, wsprnetOn: true, dmrIdLookup: true, adsbFeed: '{"x":1}',
               bgDecode: '{"p":1}', dxScan: '{"d":1}', maxUsers: 10, advanced: true, pin: '1234', biasT: true, ppm: 3,
               adminPassword: 'hunter22', lockedCentre: 100e6, landingMessage: 'hi' };
const merged = SC.simpleMerge(full, st);
ok(merged.communityMap === true && merged.pskrOn === true && merged.maxUsers === 10 && merged.pin === '1234'
   && merged.landingMessage === 'hi', 'Full → Simple: every Full setting stays STORED');
ok(merged.name === 'Kitchen TV' && merged.simple === true, '…with Simple\'s four and the mark');
const view = SC.simpleView(merged);
ok(view.communityMap === false && view.pskrOn === false && view.wsprnetOn === false && view.dmrIdLookup === false
   && view.adsbFeed === '' && view.bgDecode === '' && view.dxScan === '', 'it RUNS local-network-only: no community map, no feeds, no background');
ok(view.maxUsers === 1 && view.advanced === false && view.pin === '' && !('lockedCentre' in view) && !('landingMessage' in view),
   '…one listener, no PIN (none typed — Full\'s 1234 is NOT used), Full\'s server behaviour left in the store');

// ── ★★★ beta96 (Stuart 11-26) the entry labels, Simple's PIN, LAN only, RAW IQ ──────────────────────────────────────────
ok(SC.ENTRY_LABELS.join(' | ') === 'VibeServer Simple (Local Network Only) | VibeServer Full | RTL-TCP (Compatibility)',
   `entry labels exactly as Stuart gave them (${SC.ENTRY_LABELS.join(' | ')})`);
ok(SC.pinProblem('') === '' && SC.pinProblem('1234') === '' && SC.pinProblem('123456789012') === ''
   && SC.pinProblem('1234567890123') !== '' && SC.pinProblem('12a4') !== '', 'PIN rule = the server PIN\'s: optional, digits only, up to 12');
ok(SC.pinTyped(' 12-34ab5678901234 ') === '123456789012', 'the PIN box keeps digits only, 12 at most');
const withPin = SC.simpleView(SC.simpleMerge(full, SC.simpleStartConfig({ name: 'k', mdnsName: '', rate: 0, topRate: 1, fps: 10, pin: '4321', rawIq: 1 })));
ok(withPin.pin === '4321', 'Simple\'s PIN IS ENFORCED: the view runs it as the server PIN');
ok(SC.simpleMerge(full, SC.simpleStartConfig({ name: 'k', mdnsName: '', rate: 0, topRate: 1, fps: 10, pin: '4321' })).pin === '1234',
   '…under its own key: Full\'s stored PIN is untouched');
ok(SC.simpleView({ ...merged, simplePin: '12x' }).pin === '', 'a stored PIN that breaks the rule runs as none (never a PIN nobody can type)');
ok(view.lanOnly === true && withPin.lanOnly === true, 'LAN ONLY ENFORCED: every Simple view carries lanOnly (the engine refuses non-local clients)');
ok(withPin.rawIq === 1 && withPin.rawIqMax === 1 && withPin.rawIqLanMaxHz === 0, 'RAW IQ OUT on Simple: local network');
ok(SC.simpleView({ ...merged, simpleRawIq: 2 }).rawIq === 0 && view.rawIq === 0, '…never public (a 2 runs as off), off by default');
ok(SC.SIMPLE_RAWIQ_CHOICES.map((c) => c[1]).join() === 'Off,Local only', 'Simple\'s RAW IQ choices: Off / Local only (no dead public choice)');
ok(!SC.simpleRawIqOffered({ lite: true, icom: false }) && !SC.simpleRawIqOffered({ lite: false, icom: true })
   && SC.simpleRawIqOffered({ lite: false, icom: false }), 'RAW IQ OUT not drawn on Lite or an Icom (as in Full)');
ok(view.biasT === true && view.ppm === 3 && view.adminPassword === 'hunter22', '…the radio\'s own settings and the admin password kept');
ok(SC.isSimpleConfig(merged) && !SC.isSimpleConfig(full), 'the mark');

ok(SC.remoteSettingsLine({ running: null, mdnsHost: '', hasAdminPassword: true, remoteOn: true }) === '', 'no line when not serving');
const on = SC.remoteSettingsLine({ running: { ip: '192.168.1.9', port: 48000 }, mdnsHost: 'tv', hasAdminPassword: true, remoteOn: true });
ok(on.includes('http://192.168.1.9:48000/setup') && on.includes('tv.local:48000/setup'), 'remote access on: the /setup address, both ways');
ok(/Remote settings: off/.test(SC.remoteSettingsLine({ running: { ip: '1.2.3.4', port: 1 }, mdnsHost: '', hasAdminPassword: true, remoteOn: false })),
   'remote access off: says so, and how to turn it on');

ok(SC.passwordProblem('abc', 'abc') !== '' && SC.passwordProblem('abcdefgh', 'abcdefgx') !== '' && SC.passwordProblem('abcdefgh', 'abcdefgh') === '',
   'the device\'s password rule: 8+, typed twice');
const keys = ['vs_adminpw', 'vs_pin', 'vs_serverchoice', 'vs_learned_bookmarks', 'lsv_server_location', 'vsdr_rtltcp_server_name',
              'vibe.padProfile', 'favourites', 'lsv_local_place'];
const w0 = SC.serverKeysToWipe(keys, false), w1 = SC.serverKeysToWipe(keys, true);
ok(w0.includes('vs_adminpw') && w0.includes('vs_serverchoice') && w0.includes('lsv_server_location') && !w0.includes('vs_learned_bookmarks'),
   'reset wipes the server\'s keys (choice too → first run again); stored data kept by default');
ok(w1.includes('vs_learned_bookmarks') && !w1.includes('favourites') && !w1.includes('vibe.padProfile') && !w1.includes('lsv_local_place'),
   'the tick adds the stored data; the app\'s own things are never touched');

// ── B. the Kotlin twins ─────────────────────────────────────────────────────────────────────────────────────────────
const boot = rd('android/app/src/main/java/com/vibesdr/app/VibeServerBoot.kt');
const kList = (name: string) => { const m = boot.match(new RegExp(`val ${name} = listOf\\(([^)]*)\\)`)); return m ? [...m[1].matchAll(/"([A-Za-z0-9]+)"/g)].map((x) => x[1]) : []; };
ok(kList('SIMPLE_KEYS').join() === SC.SIMPLE_KEYS.join(), 'SIMPLE_KEYS: Kotlin = TypeScript');
ok(kList('RADIO_KEYS').join() === SC.RADIO_KEYS.join(), 'RADIO_KEYS: Kotlin = TypeScript');
ok([...kList('LAN_ONLY_OFF_FALSE'), ...kList('LAN_ONLY_OFF_EMPTY'), ...kList('LAN_ONLY_OFF_TRUE')].sort().join() === Object.keys(SC.LAN_ONLY_OFF).sort().join(),
   'LAN_ONLY_OFF: Kotlin = TypeScript');
const ops = rd('android/app/src/main/java/com/vibesdr/app/VibeServerOps.kt');
const slug = ops.match(/fun hostSlug\(name: String\): String \{[\s\S]*?\n    \}/)?.[0] ?? '';
ok(/\[\^a-z0-9\]\+/.test(slug) && /take\(32\)/.test(slug) && /"vibesdr"/.test(slug), 'hostSlug and mdnsLabel make the same label');

// ── C. the native wiring ────────────────────────────────────────────────────────────────────────────────────────────
const restore = rd('android/app/src/main/java/com/vibesdr/app/VibeServerRestore.kt');
const remote = rd('android/app/src/main/java/com/vibesdr/app/VibeRemoteSetup.kt');
const shim = rd('android/app/src/main/cpp/local_sdr_shim.cpp');
const page = rd('android/app/src/main/cpp/vibe_setup_page.h');
ok(/val cfg = effective\(cfgIn\)/.test(boot), 'every start path runs the Simple VIEW (applyAndStart)');
ok(/setVibeServerLanOnly\(cfg\.b\("lanOnly", false\)\)/.test(boot), 'LAN only reaches the engine on every start (absent = off: Full as before)');
ok(/out\.put\("pin", if \(simplePinOk\(sp\)\) sp else ""\)/.test(boot) && /"rawIq", ri/.test(boot), 'Kotlin view: Simple\'s PIN and RAW IQ (the TS twin)');
ok(/nativeSetVibeServerLanOnly/.test(rd('android/app/src/main/cpp/vibe_localsdr_jni.cpp')), 'the JNI entry exists');
{
  const h = shim.slice(shim.indexOf('void handleConnection('));
  const gate = h.slice(h.indexOf('g_vsLanOnly.load('), h.indexOf('g_vsLanOnly.load(') + 900);
  ok(/!isPrivateIp\(peer\) \|\| viaTunnel\(\*sock\)/.test(gate) && /socketPeerAddress\(\)/.test(gate),
     'the engine refuses a non-local client with the RTL-TCP door\'s own rule (isPrivateIp on the REAL peer), and the tunnel');
  ok(h.indexOf('g_vsLanOnly.load(') < h.indexOf('const std::string reqPath'), '…before any route answers');
}
ok(/!VibeServerBoot\.isSimple\(cfg\)\) VibeTunnel\.restoreIfWanted/.test(ops) && /!VibeServerBoot\.isSimple\(cfg\)\) VibeTunnel\.restoreIfWanted/.test(restore),
   'LAN only: no listing put back for a Simple server — Start AND the restore');
ok(/isSimple\(cfg\)\) try \{ VibeTunnel\.pauseForServerStop/.test(ops), 'LAN only: a tunnel still up is closed');
ok(/listingAct == true && VibeServerBoot\.isSimple\(merged\)/.test(remote), 'LAN only: the /setup page cannot list a Simple server');
ok(/VibeServerBoot\.simpleMerge\(/.test(ops), 'a Simple start merges into the one store (Full kept)');
ok(/startMdns\(hostSlug\(VibeServerBoot\.mdnsName\(cfg\)\)/.test(ops), 'the address on this network reaches the mDNS responder');
const pwFn = remote.slice(remote.indexOf('fun setupPasswordSet'), remote.indexOf('fun setupPasswordSet') + 1600);
ok(pwFn.length > 100 && !/setRemoteAccess\(ctx, true\)/.test(pwFn), 'first-time setup does NOT switch on permanent remote access');
ok(/if \(!remote\) return ""/.test(remote) && /setRemoteAccess\(ctx, true\)/.test(remote.slice(remote.indexOf('fun setupFinish'))),
   '…only the owner\'s explicit tick at Finish does');
ok(/adminPassword", ""\)\.isNotEmpty\(\)\)\s*return VibeServerOps\.fail\("password_exists"/.test(remote), 'no code is issued once an admin password exists (app)');
ok(/issue\(vsAdminPasswordSet\(\)/.test(shim) && /redeem\([^;]*vsAdminPasswordSet\(\)/.test(shim), '…nor by the engine, at issue AND at use');
ok(/isPrivateIp\(sock->socketPeerAddress\(\)\) && !viaTunnel\(\*sock\)/.test(shim), 'codes are LAN only, never through the tunnel');
ok(!/LOGI\([^)]*code\.c_str\(\)/.test(shim), 'the code is never logged');
ok(/ctEq\(stored, typed\)/.test(remote) && /"wrong_password"/.test(remote), 'reset: the admin password is checked (constant time), a wrong one changes nothing');
ok(/"reset\.all"\s*->\s*VibeRemoteSetup\.resetAll/.test(ops) && !/reset\.all|resetAll/.test(shim), 'reset: device only — no HTTP route');
ok(/factoryReset/.test(rd('android/app/src/main/cpp/vibe_android_setup.h')), 'reset: refused if a /setup save names it');
ok(/id="codeCard"/.test(page) && /id="codePwCard"/.test(page) && /codePw2/.test(page), 'the page: code → the admin password typed twice');
ok(/PASS = a; CODE_SETUP = true;[\s\S]{0,200}await signIn\(true\)/.test(page), '…then the ordinary sign-in with that password (nothing else before it)');
ok(/codeRemote/.test(page) && /remote=" \+ \(\$\("codeRemote"\)\.checked/.test(page), 'Finish offers remote access as an explicit choice');

// ── D. the screens ──────────────────────────────────────────────────────────────────────────────────────────────────
const NEW = ['src/screens/ServerChoiceScreen.tsx', 'src/screens/SimpleServerScreen.tsx', 'src/screens/SetupElsewhereScreen.tsx',
             'src/components/ServerTopBar.tsx', 'src/components/DeviceCredentials.tsx', 'src/components/DeviceReset.tsx'];
for (const f of NEW) {
  const t = rd(f);
  const raw = (t.match(/<(TouchableOpacity|Pressable)\b/g) || []).length;
  ok(raw === 0, `${f}: no bare touchable — every key is a ServerKey (TV focus + pad ring)`);
  if (/screens\//.test(f) || /TopBar/.test(f)) ok(/useKeyList\(/.test(t) && /\bfirst\b/.test(t), `${f}: pad / keyboard list and a first focus`);
}
const key = rd('src/components/ServerKey.tsx');
ok(/hasTVPreferredFocus=\{!!p\.first\}/.test(key) && /onFocus=/.test(key) && /addKeyListener\('down'/.test(key), 'ServerKey: TV remote focus AND the pad bus');
const fullSrc = rd('src/screens/ServerModeScreen.tsx');
const runView = fullSrc.slice(fullSrc.indexOf('// ── Running view'), fullSrc.indexOf('// ── Config view'));
const cfgView = fullSrc.slice(fullSrc.indexOf('// ── Config view'));
ok(runView.indexOf('<ServerTopBar') > 0 && runView.indexOf('<ServerTopBar') < runView.indexOf('styles.h1'), 'Full running: STOP is the first thing');
ok(/const serverChoiceTop: React\.ReactNode = \(\s*<ServerTopBar/.test(fullSrc) && cfgView.indexOf('{serverChoiceTop}') > 0
   && cfgView.indexOf('{serverChoiceTop}') < cfgView.indexOf('secRender('), 'Full setup: START (the serverChoiceTop hook) comes before every section');
ok(/devicePassword: \{ key: 'devicePassword'[^}]*body: scDeviceCreds/.test(fullSrc), 'Full setup: the device-only password / PIN fills its section slot');
ok((fullSrc.match(/\{scCredentials\}/g) || []).length === 1 && runView.includes('{scCredentials}'), 'Full running: password / PIN and RESET at the bottom');
ok(cfgView.lastIndexOf('{scReset}') > cfgView.lastIndexOf('secRender('), 'Full setup: RESET at the very bottom, after every section');
ok(/mdnsName: mdnsName\.trim\(\)/.test(fullSrc), 'Full sends the address on this network (Simple → Full keeps it)');
const simple = rd('src/screens/SimpleServerScreen.tsx');
ok(/multiSet\(\[\[NAME_KEY, n\], \['vs_rate'/.test(simple) && /setMdnsName\(mdns\.trim\(\)\)/.test(simple) && /setChoice\('simple'\)/.test(simple),
   'Simple saves its four where Full reads them, and remembers the choice');
ok(simple.indexOf('testID="simple-start"') < simple.indexOf('>SERVER NAME<'), 'Simple: START first');
ok(/replace\('ServerMode', \{ vibeServerOnly: true, keepOnExit/.test(simple), 'Simple → Full hands the running server over');
ok(/<DeviceReset /.test(simple) && /<DeviceCredentials /.test(simple), 'Simple: password change and reset too');
ok(simple.indexOf('testID="simple-start"') < simple.indexOf('testID="simple-pin"') && /pinProblem\(pin\)/.test(simple)
   && /if \(pinWhy\) \{ setError/.test(simple), 'Simple: a PIN field under START, refused at Start if it breaks the rule');
ok(/SIMPLE_RAWIQ_CHOICES\.map/.test(simple) && /rawIq: rawIqOk \? rawIq : 0/.test(simple), 'Simple: RAW IQ OUT (Off / Local only), sent with Start');
ok(/CHOICE_WORDS\[c\]\.title/.test(rd('src/screens/ServerChoiceScreen.tsx')), 'the entry draws the labels from CHOICE_WORDS');
const choice = rd('src/screens/ServerChoiceScreen.tsx');
ok(/if \(choice\) \{ navigation\.replace\(routeFor\(choice\)/.test(choice), 'the remembered choice opens straight away');
ok(/'‹ Server type'|‹ Server type/.test(simple + rd('src/components/ServerTopBar.tsx') + rd('src/screens/RtlTcpServerScreen.tsx')),
   'every type has a way back to the list');
ok(/navigate\('ServerChoice'/.test(rd('src/screens/InstancePickerScreen.tsx')), '"Use as server" opens the entry');
const lite = rd('lite/app/index.js');
ok(/name: 'ServerChoice'/.test(lite) && /vibeServerOnly: true/.test(lite) && !/RtlTcpServer'?:/.test(lite), 'Lite opens on the same entry, no RTL-TCP');

console.log(fails ? `test_server_choice: ${passes} passed, ${fails} FAILED` : `test_server_choice: all ${passes} passed`);
process.exit(fails ? 1 : 0);
