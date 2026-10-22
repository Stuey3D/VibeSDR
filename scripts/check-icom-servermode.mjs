// check-icom-servermode.mjs — the app's OWN server screen (ServerModeScreen, which VibeServer Lite renders too) serves
// an Icom receive only, with the same five settings Android's server reads, and draws none of the IQ radio's cards
// for it (docs/ICOM-SUPPORT-PLAN.md "Phase 4 built", AGENTS.md "a control that only works on one radio should not be
// there"). A static check: each line below is one expression a later edit could quietly drop, and neither the app
// nor Lite can be driven headless here. The pure logic (icomPhoneFields, icomPresence) is in test_icom_radio.ts.
import fs from 'node:fs';
const read = (p) => fs.readFileSync(new URL(`../${p}`, import.meta.url), 'utf8');
const S = read('src/screens/ServerModeScreen.tsx');
const V = read('src/services/vibeServer.ts');
const R = read('src/services/icomRadio.ts');
const C = read('src/components/IcomSetupCard.tsx');
const A = read('src/services/androidIcom.ts');
const KT = read('android/app/src/main/java/com/vibesdr/app/VibeIcom.kt');
const MOD = read('android/app/src/main/java/com/vibesdr/app/VibeLocalSdrModule.kt');
const PAGE = read('android/app/src/main/cpp/vibe_setup_page.h');
const MODELS = read('android/app/src/main/cpp/icom_models.h');
const LITE = read('lite/app/index.js');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const has = (src, needle, what) => ok(src.includes(needle), what);

console.log('Icom on the app\'s own server screen');

// ── the five keys: one list, three readers ──
const keys = JSON.parse(R.match(/ICOM_PHONE_KEYS = (\[[^\]]*\])/)[1].replace(/'/g, '"'));
ok(keys.length === 5, `ICOM_PHONE_KEYS has five keys (${keys.join(', ')})`);
const prep = KT.slice(KT.indexOf('fun prepareStart'), KT.indexOf('fun onServerStarted'));
const ktKeys = [...prep.matchAll(/cfg\.opt(?:Int|Boolean)\("(icom\w+)"/g)].map((m) => m[1]);
ok(JSON.stringify([...ktKeys].sort()) === JSON.stringify([...keys].sort()),
   `VibeIcom.prepareStart reads exactly ICOM_PHONE_KEYS (Kotlin: ${ktKeys.join(', ')})`);
// ★ Defaults agree: the Kotlin optX default = what icomPhoneFields(null) gives.
const ktDefaults = Object.fromEntries([...prep.matchAll(/cfg\.opt(?:Int|Boolean)\("(icom\w+)", (\w+)\)/g)].map((m) => [m[1], m[2]]));
const want = { icomBaud: '115200', icomCiv: '0', icomScopeSpeed: '0', icomOwnerLock: 'true', icomRfOpen: 'false' };
ok(JSON.stringify(ktDefaults) === JSON.stringify(Object.fromEntries(Object.keys(ktDefaults).map((k) => [k, want[k]]))),
   `Kotlin defaults = the setup page's (owner lock ON): ${JSON.stringify(ktDefaults)}`);
for (const k of keys) {
  has(V, `${k}?: `, `VibeServerConfig declares ${k}`);
  ok(new RegExp(`\\b${k}: cfg\\.${k} \\?\\? `).test(V), `nativeServerConfig sends ${k} at a definite value`);
  has(S, `${k}: icomSet.${k}`, `the screen's start config carries ${k}`);
}
has(MOD, 'if (isIcom(dev)) VibeIcom.prepareStart(cfg, dev)', 'the start path hands the whole config to prepareStart');

// ── option lists from the radio's menu, not invented here ──
const pageSel = PAGE.slice(PAGE.indexOf('<select id="icomBaud">'), PAGE.indexOf('</select>', PAGE.indexOf('<select id="icomBaud">')));
const pageBauds = [...pageSel.matchAll(/value="(\d+)"/g)].map((m) => Number(m[1]));
const tsBauds = JSON.parse(R.match(/ICOM_BAUDS = (\[[^\]]*\])/)[1]);
ok(pageBauds.length > 0 && JSON.stringify(pageBauds) === JSON.stringify(tsBauds),
   `ICOM_BAUDS = the setup page's baud list, in order (${tsBauds.join(', ')})`);
const pageSpeeds = PAGE.match(/<select id="icomScopeSpeed">(.*?)<\/select>/s)[1];
ok(/value="0">Fast.*value="1">Mid.*value="2">Slow/s.test(pageSpeeds) && /\['Fast', 'Mid', 'Slow'\]/.test(R),
   'scope speed names and values = the setup page\'s');

// ── the copy names every supported Icom, and only those ──
const known = [...MODELS.matchAll(/\{0x[0-9A-Fa-f]+, "([^"]+)",\s*true,/g)].map((m) => m[1]);
const servable = S.match(/const SERVABLE_RADIOS = ([\s\S]*?);\n/)[1];
ok(known.length > 0 && known.every((m) => servable.includes(m)), `"what can be served" names each supported Icom (${known.join(', ')})`);
const unknown = [...MODELS.matchAll(/\{0x[0-9A-Fa-f]+, "([^"]+)",\s*false,/g)].map((m) => m[1]);
ok(unknown.every((m) => !new RegExp(`${m.replace(/[-]/g, '\\-')}(?![0-9A-Za-z])`).test(servable)),
   `…and none it refuses (${unknown.join(', ')})`);
has(servable, 'receive only', 'and says receive only');
ok((S.match(/SERVABLE_RADIOS/g) || []).length >= 3, 'said where no radio is found AND where a Start fails');

// ── the Icom branch comes first, and the IQ cards are behind it ──
ok(S.indexOf('const isIcom = icomHere !== ') > 0 && S.indexOf('const isIcom = icomHere !== ') < S.indexOf('const hasHwSetup = !isAhf && !isIcom;'),
   'isIcom is decided before the hardware cards (else means dongle)');
has(S, 'const hasHwSetup = !isAhf && !isIcom;', 'no converter / hardware cards (and so no bias-T) for an Icom');
has(S, "const radioUseEff: 'single' | 'locked' = isIcom ? 'single' : radioUse;", 'no locked centre for an Icom');
has(S, "const protoEff: Proto = isIcom ? 'vibeserver' : proto;", 'VibeServer only (no RTL-TCP) for an Icom');
has(S, "{isIcom ? (\n          <Text", 'the RTL-TCP card is replaced by a sentence for an Icom');
has(S, '{advanced && !isIcom && (<>', 'HOW WILL IT BE USED hidden');
has(S, "{!isIcom && (<>\n            <Text style={[styles.section, { color: C.section, fontFamily: F }]}>WATERFALL RATE</Text>", 'WATERFALL RATE hidden');
has(S, "{!isIcom && (<>\n            <Text style={[styles.section, { color: C.section, fontFamily: F }]}>WHERE LISTENERS START</Text>", 'WHERE LISTENERS START hidden');
has(S, "!!radio && radio.driver !== 'airspyhf' && !isIcom && dabBlocks.length > 0", 'no DAB landing');
has(S, '{isIcom ? icomSetupBlock : isRtl ? (<>', 'the Icom card in place of the gain / VibeAGC / IF filter / DS / ppm block');
has(S, "{radioUseEff !== 'locked' && !isIcom && rateBlock}", 'CAPTURE WIDTH hidden');
has(S, '{!isIcom && (<>\n                {/* ★★★ REAL BINS AT DEEP ZOOM.', 'ZOOM DETAIL hidden');
has(S, '{!isLite && !isIcom && (<>', 'RAW IQ OUT hidden');
has(S, "{radioUseEff === 'single' && !isIcom && (<>\n                <Text style={[styles.section, { color: C.section, fontFamily: F }]}>PER-BAND CEILINGS</Text>", 'PER-BAND CEILINGS hidden');
has(S, '{!isIcom && <Row C={C} F={F} k="SAMPLE RATE"', 'no sample-rate row while serving an Icom');
has(S, 'BLOCKABLE.filter((m) => !(isIcom && ICOM_NEVER_MODES.includes(m.id)))', 'no WFM / DAB / Adv RDS block chips');

// ── what IS drawn ──
has(S, 'androidIcomFacts()', 'the facts come from androidIcomFacts()');
has(S, "if (!isIcom) { setIcomFacts(null); return; }", '…and only for an Icom');
has(S, 'onPress={() => icomAskForAudio()}', 'the permission is offered again while serving');
has(S, '{icomMicMissing && icomFacts?.audioSupported !== false && (', '…only when it is missing and could help');
has(S, "icomHere === 'maybe'", 'an unnamed CP210x is said to be "maybe an Icom"');
has(S, 'An Icom? Press Start and allow it.', '…in the words the plan gives');
has(S, '<IcomSettingsFields phone ', 'the shared settings, phone variant');
has(S, 'ink={icomInk} Input={TextInput} />', '…with the TV-safe text field (a remote can move past it)');
has(S, '<IcomFacts icom={icomRadioNow}', 'the shared facts while serving');
has(S, 'if (isIcom && parseCivHex(icomCivTxt) === null)', 'Start refuses a CI-V address the radio could not have');
const fieldsFn = C.slice(C.indexOf('export function IcomSettingsFields'));
ok(fieldsFn.indexOf('{!phone && (<>') > 0 && fieldsFn.indexOf('{!phone && (<>') < fieldsFn.indexOf('icomAudio'),
   'the phone form draws no audio / port / serial-device (not settings on Android)');
has(LITE, "import ServerModeScreen from '../../src/screens/ServerModeScreen'", 'Lite renders this same screen');

// ★★★ receive only: nothing on this path builds a transmit message.
for (const [name, src] of [['ServerModeScreen', S], ['IcomSetupCard', C], ['androidIcom', A]]) {
  const bad = /type:\s*['"](ptt|tx|transmit)['"]|\bptt\s*:/i.test(src);
  ok(!bad, `${name}: no transmit message anywhere`);
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
