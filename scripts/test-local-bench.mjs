// test-local-bench.mjs — WHAT THIS PHONE CAN RUN (Local Hardware, Android; src/services/localBench.ts). Stuart, 2026-11-20:
// "we should offer the benchmark to the local hardware mode too so people on weaker devices know exactly what they are
// getting". The same benchmark result as the server screen, read for someone using the phone as their own radio:
//   1. the verdict words come from the engine's grade alone (green / amber / red / none);
//   2. only what THIS radio can do here is listed — an Airspy HF+ is never told about DAB, ADS-B or AIS (no dead rows);
//   3. no benchmark row, no line (DRM has none, so it never appears); a group is judged on its heaviest part;
//   4. WFM is read at the radio's own rate; "measured when" and "about N minutes" come from the result;
//   5. the wiring: the card is gated to a radio plugged into this Android phone, the run pauses and always resumes,
//      the engine skips the listener refusal ONLY for its own loopback app, and the server screen's call is untouched.
// Pure: no app, no device, no radio. Silent.
import fs from 'node:fs';
import { build } from 'esbuild';

let fail = 0, pass = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (c) pass++; else fail++; };

const src = new URL('../src/services/localBench.ts', import.meta.url);
const out = await build({ entryPoints: [src.pathname], bundle: true, write: false, format: 'esm', platform: 'neutral', logLevel: 'silent' });
const lb = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));

// ── 1. the words ──────────────────────────────────────────────────────────────────────────────────────────────────
ok(lb.verdictWords(lb.verdictFor('green')) === 'Runs comfortably', 'green → "Runs comfortably"');
ok(lb.verdictWords(lb.verdictFor('amber')) === 'Runs, but uses a lot of this phone', 'amber → "Runs, but uses a lot of this phone"');
ok(lb.verdictWords(lb.verdictFor('red')) === 'Too heavy for this phone', 'red → "Too heavy for this phone"');
ok(lb.verdictFor('none') === 'unmeasured' && lb.verdictFor('') === 'unmeasured', 'none / unknown → not measured, never a verdict');

// ── a result: every row the lines can read ────────────────────────────────────────────────────────────────────────
const row = (id, pct) => ({ id, label: id, pct, grade: pct < 0 ? 'none' : pct < 70 ? 'green' : pct <= 85 ? 'amber' : 'red' });
const result = { v: 1, at: Date.UTC(2026, 10, 20, 9, 5) / 1000, benchSec: 130, rows: [
  row('wfm_2400', 61), row('wfm_2048', 52), row('wfm_1024', 30), row('rdsx_2048', 78), row('dab', 92),
  row('ssb_2048', 12), row('am_2048', 9), row('nfm_2048', 14),
  row('dec_rtty', 2), row('dec_navtex', 3), row('dec_cw', 4), row('dec_psk', 5),
  row('dec_cwskim', 40), row('dec_rttyskim', 88), row('dec_ft8band', 72),
  row('dec_freedv', 20), row('dec_rade', 140), row('dec_m17', 10),
  row('adsb_2400', 66), row('adsb_3200', 90), row('dec_ais', 33), row('dec_ism', 25), row('dec_wmbus', -1),
  row('dec_vdl2', 50), row('dec_acars', 20), row('dec_rs41', 15), row('dec_dfm', 18), row('dec_m10', 12),
  row('dec_ndb', 45), row('dec_hfdl', 35),
] };
const rtl = { sampleRate: 2_400_000, tunable: [[24e6, 1766e6]], dab: true, freedv: [], m17: true, adsb: true, ais: true,
              ismHz: [433.92e6, 868.3e6], ndb: false, avdata: true, sondes: true, hfdl: false, cwSkim: false, rttySkim: false,
              spotsBand: false };
const keys = (ls) => ls.map((l) => l.key).join(',');
const line = (ls, k) => ls.find((l) => l.key === k);

// ── 2. an RTL (24–1766 MHz, no HF) ───────────────────────────────────────────────────────────────────────────────
let ls = lb.localBenchLines(result, rtl);
ok(keys(ls) === 'wfm,rdsx,dab,narrow,m17,adsb,ais,ism433,ism868,avdata,sondes',
   `an RTL with no HF: FM, DAB, the VHF/UHF decoders — no HF text decoders, no FreeDV, no NDB/HFDL (${keys(ls)})`);
ok(line(ls, 'wfm').words === 'Runs comfortably' && /^61% of one processor core$/.test(line(ls, 'wfm').small),
   `WFM read at the radio's own rate (2.4 MS/s → wfm_2400, 61%): "${line(ls, 'wfm').small}"`);
ok(line(ls, 'rdsx').words === 'Runs, but uses a lot of this phone', 'Advanced RDS at 78% → runs, but uses a lot');
ok(line(ls, 'dab').words === 'Too heavy for this phone' && line(ls, 'dab').verdict === 'too-heavy', 'DAB at 92% → too heavy');
ok(line(ls, 'narrow').small === 'NFM, the heaviest: 14% of one processor core', `a group names its heaviest part: "${line(ls, 'narrow').small}"`);
ok(line(ls, 'adsb').small === '66% of one processor core', 'ADS-B judged at 2.4 MS/s (its ordinary rate), not the 3.2 opt-in');
ok(line(ls, 'ism868').verdict === 'unmeasured' && line(ls, 'ism868').small === '', 'a row the run could not measure says so, with no figure');
ok(!ls.some((l) => /drm/i.test(l.key + l.title)), 'DRM never appears — the benchmark has no DRM row');

// ── an RTL at 1.024 MS/s ───────────────────────────────────────────────────────────────────────────────────────────
ls = lb.localBenchLines(result, { ...rtl, sampleRate: 1_024_000 });
ok(line(ls, 'wfm').small === '30% of one processor core', 'at 1.024 MS/s WFM is read from wfm_1024');

// ── an Airspy HF+ (0.5 kHz–31 MHz and 60–260 MHz; no DAB, AIS, ADS-B or ISM answers from its engine) ─────────────────
const hfplus = { sampleRate: 912_000, tunable: [[500, 31e6], [60e6, 260e6]], dab: false, freedv: ['RADE', '700D', '700E', '1600'],
                 m17: false, adsb: false, ais: false, ismHz: [], ndb: true, avdata: false, sondes: false, hfdl: true,
                 cwSkim: true, rttySkim: true, spotsBand: true };
ls = lb.localBenchLines(result, hfplus);
ok(keys(ls) === 'wfm,rdsx,narrow,text,skim,spots,freedv,rade,ndb,hfdl',
   `an Airspy HF+: HF and FM only — never DAB, ADS-B, AIS or ISM (${keys(ls)})`);
ok(line(ls, 'wfm').small === '30% of one processor core', 'the HF+ at 912 kS/s → the nearest WFM row (1.024 MS/s)');
ok(line(ls, 'skim').verdict === 'too-heavy' && /RTTY skimmer, the heaviest: 88%/.test(line(ls, 'skim').small),
   'skimmers: the heavier of the two decides, and is named');
ok(line(ls, 'rade').words === 'Too heavy for this phone' && line(ls, 'freedv').words === 'Runs comfortably',
   'RADE and the codec2 FreeDV modes are judged apart');
ok(line(ls, 'text').small === 'PSK31, the heaviest: 5% of one processor core', 'the text decoders: one line, its heaviest named');

// ── only RADE offered → no codec2 FreeDV line; skimmer rows not offered → not read ───────────────────────────────────
ls = lb.localBenchLines(result, { ...hfplus, freedv: ['RADE'], rttySkim: false });
ok(!line(ls, 'freedv') && !!line(ls, 'rade'), 'an engine offering RADE alone gets no 700D/700E/1600 line');
ok(line(ls, 'skim').verdict === 'good' && line(ls, 'skim').small === '40% of one processor core',
   'with the RTTY skimmer not offered, only the CW skimmer is judged');

// ── 3. no rows, no lines ─────────────────────────────────────────────────────────────────────────────────────────
ok(lb.localBenchLines(null, rtl).length === 0 && lb.localBenchLines({ rows: [] }, rtl).length === 0, 'no result → no lines');
ls = lb.localBenchLines({ rows: [row('ssb_2048', 10)] }, rtl);
ok(keys(ls) === 'narrow', 'an older result with one row → that line only; nothing guessed for the rest');
ok(keys(lb.localBenchLines(result, { ...rtl, tunable: [] })).startsWith('wfm,rdsx,dab,narrow,text'),
   'a radio that has not stated where it tunes is not judged out of FM or HF');

// ── 4. when, and how long ────────────────────────────────────────────────────────────────────────────────────────
const at = new Date(2026, 10, 20, 9, 5).getTime() / 1000;
ok(lb.measuredWhen(at, new Date(2026, 10, 20, 18, 0)) === 'Measured today at 09:05', 'today → "Measured today at 09:05"');
ok(lb.measuredWhen(at, new Date(2026, 10, 21, 8, 0)) === 'Measured yesterday at 09:05', 'yesterday');
ok(lb.measuredWhen(at, new Date(2026, 11, 2, 8, 0)) === 'Measured 12 days ago', '12 days ago');
ok(lb.measuredWhen(undefined) === null && lb.measuredWhen(0) === null, 'a result without `at` says nothing about when');
ok(lb.pauseMinutes(result) === 3 && lb.pauseMinutes({ benchSec: 45 }) === 1 && lb.pauseMinutes({}) === null && lb.pauseMinutes(null) === null,
   'pause estimate: 130 s → about 3 minutes, 45 s → 1, never run → null ("a few minutes")');

// ── 5. the wiring (source) ───────────────────────────────────────────────────────────────────────────────────────
const read = (p) => fs.readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const sdr = read('src/screens/SDRScreen.tsx'), panel = read('src/components/LocalHardwarePanel.tsx');
const card = read('src/components/LocalBenchmarkCard.tsx'), run = read('src/services/localBenchRun.ts');
const jni = read('android/app/src/main/cpp/vibe_localsdr_jni.cpp'), mod = read('android/app/src/main/java/com/vibesdr/app/VibeLocalSdrModule.kt');
const srv = read('src/screens/ServerModeScreen.tsx');
ok(/const localBench = useMemo\(\(\) => \(pagingHere && localBenchAvailable\(\)/.test(sdr),
   'SDRScreen offers it only for a radio plugged into this Android phone (the pagingHere gate)');
ok(/bench=\{localBench\}/.test(sdr) && /Platform\.OS === 'android' && !!p\.bench/.test(panel), 'the hardware sheet draws it only when given, on Android');
ok(/<LocalBenchmarkCard/.test(panel) && !/LocalBenchmarkCard/.test(read('src/components/ControlsBar.tsx')),
   'it sits in the hardware sheet — not on the control deck');
ok(/finally \{[\s\S]*h\.resume\(\)/.test(run) && /h\.pause\(\)/.test(run), 'the run pauses first and resumes in finally — success or not');
ok(/The radio pauses for \$\{pause\}/.test(card), 'it says before starting that the radio pauses, and for how long');
ok(/RUN AGAIN/.test(card), 'RUN AGAIN once there is a result');
ok(/ownRadio && !vibe::LocalSdrShim::serveOnLan\(\)/.test(jni) && /jboolean ownRadio/.test(jni),
   'the engine skips the listener refusal only for its own loopback app — never while it serves the network');
ok(/fun runBenchmarkOwnRadio\(wantDab: Boolean/.test(mod) && /VibeBenchmark\.run\(reactContext, wantDab\)\n/.test(mod),
   'a separate bridge method; the server screen\'s runBenchmark is unchanged');
ok(/mod\.runBenchmark\(true\)/.test(srv) && !/runBenchmarkOwnRadio/.test(srv), 'ServerModeScreen still calls runBenchmark (server mode)');

console.log(`\n${fail ? 'FAIL' : 'PASS'} — ${pass}/${pass + fail}`);
process.exit(fail ? 1 : 0);
