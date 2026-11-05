// check-android-settings-parity.mjs — every owner setting the Linux/Mac Setup page offers is either ON the Android
// server screen (ServerModeScreen, which the main app's server mode AND VibeServer Lite render) or listed here as not
// ported, with the reason. docs/v12/ANDROID-SETTINGS-PARITY.md is the table in words; this is the table as a test.
//
// ★★★ WHY IT EXISTS (Stuart, 2026-11-03: "does the android apps have the background decoder settings ported over?").
//     They had not: Background Decoding and the gallery shipped on the Setup page in beta3 and the phone's screen never
//     heard of them — nothing failed, so nobody knew. Now a NEW Setup-page control fails this test until it is
//     classified: ported (with the Android evidence) or not (with why — AGENTS.md: no dead controls).
// ★ Static, like check-setup-page.mjs and check-icom-servermode.mjs: the app and Lite cannot be driven headless here.
//   For a "ports" row, the Android evidence must be present: the screen's own words or key for it, and — for a setting
//   the engine must apply — the key carried to the engine (vibeServer.ts) and applied by VibeServerBoot (every start
//   path: start, crash restore, Lite).
import fs from 'node:fs';
const read = (p) => fs.readFileSync(new URL(`../${p}`, import.meta.url), 'utf8');
const PAGE = read('android/app/src/main/cpp/vibe_setup_page.h');
const SCREEN = read('src/screens/ServerModeScreen.tsx')
  + read('src/components/BackgroundDecodingSetup.tsx') + read('src/components/BandLimitEditor.tsx')
  + read('src/components/AntennaBandsEditor.tsx') + read('src/components/IcomSetupCard.tsx') + read('src/services/icomRadio.ts')
  + read('src/components/RspSetupCard.tsx') + read('src/components/DxScanSetup.tsx');
const VS = read('src/services/vibeServer.ts') + read('src/services/rspCard.ts');
const BOOT = read('android/app/src/main/java/com/vibesdr/app/VibeServerBoot.kt');
const KT = BOOT + read('android/app/src/main/java/com/vibesdr/app/VibeIcom.kt');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

/* The table. `ids`: the Setup page's control ids. ports: `screen` must appear in the screen (a key, a word);
 * `engine` (optional) must be in the config vibeServer.ts sends AND read by VibeServerBoot/VibeIcom (cfg.x("key")).
 * not: `why` — said in the doc too. */
const P = (ids, screen, engine) => ({ ids, status: 'ports', screen, engine });
const N = (ids, why) => ({ ids, status: 'not', why });
const TABLE = [
  // ── Pocket / Linux machine maintenance: the phone IS the machine and has its own screens for these ──
  N(['claimPass', 'claimPass2', 'claimPin', 'pass'], 'the Setup page\'s own sign-in and first-run claim; the phone\'s screen is on the device itself'),
  N(['wifiIgnore', 'apSsid', 'apPsk', 'wifiCountry', 'sshPass', 'sshPass2'], 'Pocket (Pi image) Wi-Fi, hotspot and SSH — Android owns its own network'),
  N(['cpuGovernor'], 'Linux cpufreq governor — an Android app cannot set one'),
  N(['srvPort'], 'the phone takes the first free port of 48000-48049 itself (one VibeServer per device)'),
  N(['rtlSerialNow', 'serialA', 'serialB'], 'radio serials that tell several radios apart on one box — a phone serves one radio'),
  N(['radioPin', 'radioPinClear'], 'a per-radio PIN on a multi-radio box — the phone has the server PIN (PIN — WHO MAY CONNECT)'),
  N(['releaseWhenIdle'], '"Let another program use this radio" — on Android no other program can take a USB radio the app holds, so it would be a dead control'),
  N(['usrpRxBw', 'usrpImgName', 'usrpImgFile'], 'USRP — UHD is not built for Android (usrp_source.cpp is not in the app\'s CMakeLists); no USRP can be served from a phone'),
  // ★ the external antenna switch: ported (components/AntennaSwitchSetup) behind the SAME gate as the Setup page's
  //   (SW_SETUP there, ANT_SWITCH_SETUP here — both false since V11, Stuart 2026-10-10; a configured switch still shows)
  P(['swYes', 'swNo', 'swBroker', 'swUser', 'swPass', 'swLocked'], 'AntennaSwitchSetup', 'antennaSwitch'),
  N(['icomPort', 'icomTtyOk', 'icomAudio'], 'an Icom\'s serial port, tty permission and audio device are Linux\'s; Android finds the Icom on USB and its audio itself (ICOM_PHONE_KEYS)'),

  // ── SERVER ──
  P(['mdns', 'name'], 'K.advertise'),
  P(['place', 'country', 'locator', 'lat', 'lon', 'locFollow'], 'RECEIVER LOCATION'),
  P(['dirList', 'dirName', 'dirPublicUrl', 'tunnelResume', 'dirShareSec'], 'ADVERTISE ON VIBESDR.NET'),
  P(['batteryPauseAt', 'batteryResumeAt'], 'batteryPauseAt', 'batteryPauseAt'),
  P(['landingMessage', 'landingLinkUrl', 'landingLinkLabel'], 'LANDING SCREEN MESSAGE', 'landingMessage'),
  P(['trustedProxies'], 'BEHIND A REVERSE PROXY', 'trustedProxies'),
  P(['maxRadiosPerIp'], 'oneRadioPerIp', 'oneRadioPerIp'),
  P(['decoderMax'], 'DECODERS', 'decoderMax'),
  P(['galleryMb'], 'Gallery size', 'galleryMb'),
  P(['galleryPublic'], 'Everyone may see the gallery', 'galleryPublic'),
  P(['gallerySaveLive'], 'Keep listeners', 'gallerySaveLive'),
  P(['uncompressed'], 'UNCOMPRESSED AUDIO', 'uncompressedAudio'),
  // ── RADIO ──
  P(['radioName'], 'SDR DISPLAY NAME', 'radioLabel'),
  P(['users'], 'HOW MANY LISTENERS', 'maxUsers'),
  P(['askFirst'], 'askFirst', 'askBeforeTuning'),
  P(['lockFreq'], 'lockedCentre', 'lockedCentre'),
  P(['zoomSpectrum'], 'ZOOM DETAIL', 'zoomSpectrum'),
  P(['icomBaud', 'icomCiv', 'icomScopeSpeed', 'icomTuneLock', 'icomPowerOn', 'icomAutoOff', 'icomOffDelayMin'], 'ICOM_PHONE_KEYS', 'icomBaud'),
  P(['antenna'], 'ANTENNA', 'antennaIcon'),
  P(['antRangePreset', 'antFilterPreset'], 'AntennaBandsEditor', 'antennaRanges'),
  P(['adsbLandOn'], 'landingAdsb', 'landingAdsb'),
  P(['adsbKeep'], 'adsbKeep', 'adsbKeepRunning'),
  P(['dabLandOn', 'dabLandBlock', 'dabLandStation'], 'landingDabCh', 'landingDabChannel'),
  P(['landingFreq', 'demodMode'], 'WHERE LISTENERS START', { sent: 'centerFreq: landingHz', applied: 'setVibeServerLanding(centerFreq, mode)' }),
  P(['fmDeemph', 'fmStereo', 'fmNr', 'fmIms', 'fmCeq', 'fmAutoBw', 'fmNb', 'fmReturn', 'fmRemember'], 'BROADCAST FM DEFAULTS', { sent: 'fmDefaultsKeys(cfg.fmDefaults', applied: 'setVibeServerFmDefaults(cfg.toString())' }),
  P(['sessionLimit', 'sessionLimitMode'], 'TIME LIMIT PER LISTENER', 'sessionLimitSoft'),
  P(['idleKick'], 'IDLE LISTENERS', 'idleKickMin'),
  P(['rawIqOn', 'rawIqLan', 'rawIqStreams', 'rawIqLocalOnly'], 'RAW IQ OUT', 'rawIqLanMaxHz'),
  P(['vfoB'], 'K.vfoB', 'vfoB'),
  P(['gainRestSlider', 'gainRest', 'gainRestLock', 'rtlAgc', 'gainAgcLock'], 'restGain', 'restGain'),
  P(['gainPick', 'gainCurve', 'gainMaxSlider', 'gainMax', 'gainIfSlider', 'gainIfMax', 'gainSplitSlider', 'gainIfBw',
     'gainAspLock', 'gainLock', 'bandCopyFrom'], 'PER-BAND CEILINGS', 'gainLimits'),
  P(['allowPick', 'allowLo', 'allowHi', 'blockPick', 'blockLo', 'blockHi'], 'WHERE LISTENERS MAY TUNE', 'allowRanges'),
  P(['dabScanLabels'], 'dabScanLabels', 'dabScanLabels'),
  P(['rate', 'rateLock'], 'CAPTURE WIDTH', 'rateLock'),
  P(['maxFps'], 'WATERFALL RATE', 'maxFftRate'),
  P(['dabRateBoost'], 'dabRateBoost', 'dabRateBoost'),
  P(['adsb256'], 'adsb256', 'adsb256'),
  P(['biasT'], 'BIAS-T', 'biasT'),
  P(['ppm'], 'FREQUENCY CORRECTION', 'ppm'),
  P(['convLo', 'convDown', 'convHi'], 'CONVERTER IN FRONT OF THE RADIO', 'converterOffsetHz'),
  P(['autoDs', 'autoDsBelow'], 'DIRECT SAMPLING (HF)', 'autoDirectSampling'),
  P(['spectrogram'], 'spectrogram', 'spectrogram'),
  P(['idleGrace'], 'Power down the radio', 'idleGraceSec'),
  // ── ★★★ V12 BACKGROUND DECODING (the reason this test exists) ──
  P(['bgOn', 'bgWait', 'bgKeepInRange'], 'Background Decoding', 'bgDecode'),
  // ★★★ V12 the idle DX scanner (components/DxScanSetup) — one JSON blob, RadioConfig::dxScan; Lite too (the same engine)
  P(['dxOn', 'dxWait', 'dxBands', 'dxRaster', 'dxViewAdmin'], 'Only the owner may view the DX Log', 'dxScan'),
  P(['rspVibeAgc'], 'VibeAGC (SDRplay)', 'rspVibeAgc'),
  P(['nbWide'], 'IMPULSE NOISE BLANKER', 'nbWide'),
  // ── the SDRplay card (Stuart 2026-11-04: "all the ones currently in Linux go to android") and the HF+ calibration ──
  P(['rfNotch'], 'Broadcast notch', 'rfNotch'),
  P(['dabNotch'], 'DAB notch', 'dabNotch'),
  P(['rspAmNotch'], 'AM broadcast notch (RSPduo)', 'rspAmNotch'),
  P(['rspHdr'], 'HDR mode below 2 MHz (RSPdx family)', 'rspHdr'),
  P(['rspExtRef'], '24 MHz reference output (RSP2, RSPduo)', 'rspExtRef'),
  P(['rspDabDecim'], 'decimation (flat ensemble)', 'rspDabDecim'),
  P(['rfAgc'], 'Automatic RF gain', 'rfAgc'),
  P(['autoNotch'], 'Automatic notch filtering', 'autoNotch'),
  P(['userNotch'], 'Let listeners toggle the notches', 'userNotch'),
  P(['dabAgcOverride', 'dabAgcTarget'], 'Use a separate IF AGC target for DAB', 'dabAgcTarget'),
  P(['agcSet', 'agcSetLock'], 'Lock the AGC target', 'agcSetLock'),
  P(['antennaMap', 'antennaPortLocked'], 'Switch aerial by band', { sent: 'antennaLocked: live.current.rspCard.antennaPortLocked', applied: 'cfg.s("antennaMap")' }),
  P(['ppb'], 'CALIBRATION (PPB)', 'ppb'),
];

console.log('Android server screen ↔ the Setup page\'s settings');

// ── 1. every control id the Setup page draws is classified ──
const pageIds = new Set([...PAGE.matchAll(/<(?:input|select|textarea)\b[^>]*\bid="([A-Za-z][\w-]*)"/g)].map((m) => m[1]));
const classified = new Map();
for (const row of TABLE) for (const id of row.ids) classified.set(id, row);
const unclassified = [...pageIds].filter((id) => !classified.has(id));
ok(unclassified.length === 0, `every Setup-page control is classified (${pageIds.size} ids)` +
   (unclassified.length ? ` — NEW, not in this table: ${unclassified.join(', ')}. Port it to ServerModeScreen (and say so here) or list it as not ported, with why.` : ''));
const stale = [...classified.keys()].filter((id) => !pageIds.has(id));
ok(stale.length === 0, 'no row names a control the Setup page no longer has' + (stale.length ? ` — gone: ${stale.join(', ')}` : ''));

// ── 2. every "ports" row is really on the phone: on the screen, carried to the engine, applied at boot ──
for (const row of TABLE.filter((r) => r.status === 'ports')) {
  const what = row.ids.join('/');
  ok(SCREEN.includes(row.screen), `${what}: on the server screen ("${row.screen}")`);
  if (row.engine && typeof row.engine === 'object') {
    ok((VS + SCREEN).includes(row.engine.sent) && KT.includes(row.engine.applied), `${what}: sent to the engine ("${row.engine.sent}") and applied by VibeServerBoot ("${row.engine.applied}")`);
  } else if (row.engine) {
    const sent = VS.includes(`${row.engine}:`) || VS.includes(`${row.engine}?:`) || SCREEN.includes(row.engine);
    const applied = KT.includes(`"${row.engine}"`) || KT.includes(row.engine);
    ok(sent && applied, `${what}: "${row.engine}" sent to the engine and applied by VibeServerBoot`);
  }
}
// ── 3. every "not" row says why ──
for (const row of TABLE.filter((r) => r.status === 'not')) ok(!!row.why && row.why.length > 20, `${row.ids.join('/')}: not ported, and says why`);

// ── 4. the V12 background-decoding chain, link by link (the shape a future edit could quietly break) ──
const JNI = read('android/app/src/main/cpp/vibe_localsdr_jni.cpp');
const SHIM = read('android/app/src/main/cpp/local_sdr_shim.cpp');
const MOD = read('android/app/src/main/java/com/vibesdr/app/VibeLocalSdrModule.kt');
ok(/VibeLocalSDR\.setBackgroundDecoding\(cfg\.s\("bgDecode"\)\)/.test(BOOT), 'VibeServerBoot applies bgDecode on every start');
ok(/VibeLocalSDR\.setGallery\(galleryDir\(filesDir\)/.test(BOOT), 'VibeServerBoot points the gallery at files/gallery on every start');
{
  const priv = BOOT.slice(BOOT.indexOf('fun privateScanConfig'), BOOT.indexOf('fun privateScanConfig') + 1700);
  ok(/"bgDecode",?/.test(priv) && /\.remove\(k\)/.test(priv), 'the private DAB scan engine never runs the owner\'s profiles');
  ok(/"dxScan"\)\)/.test(priv), '…nor the owner\'s DX scanner (V12)');
  ok(/VibeLocalSDR\.setDxScan\(cfg\.s\("dxScan"\)\)/.test(BOOT) && /VibeLocalSDR\.setDxLogFile\(/.test(BOOT), 'VibeServerBoot applies dxScan and the DX log file on every start');
}
ok(JNI.includes('nativeSetBackgroundDecoding') && JNI.includes('nativeSetGallery') && JNI.includes('nativeBackgroundCaps') && JNI.includes('nativeValidateBackgroundDecoding'), 'JNI: background decoding, gallery, caps and the save rule');
ok(/@ReactMethod\s+fun backgroundCaps/.test(MOD) && /@ReactMethod\s+fun validateBackgroundDecoding/.test(MOD) && /@ReactMethod\s+fun galleryDisk/.test(MOD), 'the bridge exports backgroundCaps / validateBackgroundDecoding / galleryDisk (@ReactMethod)');
ok(SHIM.includes('LocalSdrShim::backgroundCapsJson') && /vibe::bg::decoders\(\)/.test(SHIM.slice(SHIM.indexOf('LocalSdrShim::backgroundCapsJson'))), 'the editor\'s decoders are the ENGINE\'s table (vibe::bg::decoders), not a list in the app');
ok(!/id: 'navtex',\s*label:/.test(read('src/services/bgDecode.ts')), 'services/bgDecode.ts keeps no copy of the decoder table');
const S0 = read('src/screens/ServerModeScreen.tsx');
ok(/bgDecode: live\.current\.bgJson/.test(S0), 'Start sends the profiles read through the live ref (never a stale closure)');
ok(/\[K\.bgDecode, live\.current\.bgJson\]/.test(S0), 'Start persists the profiles it sends');
ok(/live\.current\.bgProblem/.test(S0), 'Start refuses profiles the engine would refuse');
ok(/!isIcom && !!bgCaps && \(/.test(S0), 'not drawn on an Icom, nor on an engine without it (never drawn empty)');
ok(/NativeModules as any\)\.VibeLocalSDR\b/.test(S0) && !/NativeModules as any\)\.VibeLocalSdrModule/.test(S0), 'the screen calls the native module by its real name, VibeLocalSDR');

// ── 5. the owner's DAB band scan ("Scan all DAB blocks", a button — not an input, so not in the id table) ──
// ★ 2026-11-04 the Linux Setup page's scan moved to SERVER ADMIN → DAB SCAN (it ran only on an idle radio and failed
//   "radio not running"); the page now points there. The Android server screen keeps its own — the radio is on the device.
ok(!PAGE.includes('id="dabBandScan"') && PAGE.includes('DAB SCAN') && S0.includes('Scan all DAB blocks') && S0.includes('Find the DAB multiplexes this aerial hears'),
   'Scan all DAB blocks: on the Android server screen; the Setup page points to SERVER ADMIN → DAB SCAN');
ok(/@ReactMethod\s+fun dabBandScanStart/.test(MOD) && /@ReactMethod\s+fun dabBandScanState/.test(MOD) && /@ReactMethod\s+fun dabBandScanCancel/.test(MOD),
   'the bridge exports dabBandScanStart / State / Cancel (@ReactMethod)');
ok(/\/vibeserver\/dab-bandscan/.test(MOD) && /privateScanConfig\(owner\)/.test(MOD.slice(MOD.indexOf('fun dabBandScanStart'))),
   'the band scan is the ENGINE\'s (GET /vibeserver/dab-bandscan), on the owner\'s radio in a private engine when stopped');
ok(/bandScanRef\.current\) \{\s*Alert\.alert\('A DAB scan is running'/.test(S0), 'Start waits for (or cancels) a running band scan');
// ── 5b. the SDRplay card: only for an RSP, each row only where its model has it, applied through one JNI call ──
ok(/radio\?\.driver === 'sdrplay' && \(<>\s*<Text[^>]*>SDRPLAY<\/Text>/.test(S0), 'the SDRplay card is drawn only for an RSP');
const RC = read('src/components/RspSetupCard.tsx');
ok(/caps\.hdr && /.test(RC) && /caps\.amNotch && /.test(RC) && /caps\.extRef && /.test(RC) && /caps\.rfNotch && /.test(RC) && /caps\.dabNotch && /.test(RC),
   'each model-only row is gated on the radio\'s capabilities (rspCaps)');
ok(/rspCaps\(/.test(S0) && /caps\.driver === 'sdrplay'/.test(S0), 'the capabilities come from hwinfo (remembered from serving), else the model');
ok(JNI.includes('nativeSetRspFrontEnd') && JNI.includes('nativeSetAhfPpb') && /VibeLocalSDR\.setRspFrontEnd\(/.test(BOOT) && /VibeLocalSDR\.setAhfPpb\(/.test(BOOT),
   'VibeServerBoot applies the card and the HF+ ppb through JNI on every start');
ok(!TABLE.some((r) => r.status === 'not' && /GAP/.test(r.why)), 'no GAP rows left in the table — every row is ported or cannot work on Android');

// ── 6. numbers from the app's map are read as numbers (toHashMap gives Doubles; optString(1.0) is "1.0") ──
ok(!/cfg\.s\("[A-Za-z]+"\)\.toIntOrNull\(\)/.test(BOOT), 'VibeServerBoot never reads a number through optString().toIntOrNull()');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
