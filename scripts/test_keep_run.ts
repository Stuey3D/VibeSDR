// test_keep_run.ts — the app's landing map + keep-running rule (src/utils/keepRun.ts, beta96): the SAME gate cases as
// vibeserver/test-keep-run.cpp §3/§4 and scripts/test-setup-keep-run.mjs, the local radio's facts, the ADS-B mirror and
// beta95's migration. Run: node --no-warnings scripts/test_keep_run.ts
import { keepGate, keepAvailLocal, keepMirror, keepMigrate, keepParse, keepRateWords, keepSatWords, keepName, KEEP_MODES } from '../src/utils/keepRun.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };
const all = () => KEEP_MODES.map((m) => ({ id: m.id, hw: true, why: '' }));
const find = (v: ReturnType<typeof keepGate>, id: string) => v.find((o) => o.id === id)!;

// the gate — the C++ cases
{
  const a = all(); a[0].why = 'ADS-B needs a capture of 2.4 MS/s';
  let g = keepGate(a, false, '', 2_048_000, []);
  ok(!find(g, 'adsb').enabled && find(g, 'vdl2').enabled, '2.048: ADS-B greyed, VDL2 offered');
  g = keepGate(all(), false, '', 2_400_000, ['vdl2']);
  ok(find(g, 'adsb').why === '2.4 MS/s: one mode at a time on this radio — VDL2 is ticked', '2.4: ' + find(g, 'adsb').why);
  ok(/6\.1 MS\/s/.test(find(g, 'acars').why), '2.4: ACARS names the combination\'s rate');
  g = keepGate(all(), false, '', 2_560_000, ['ais']);
  ok(/^2\.56 MS\/s: one mode at a time/.test(find(g, 'vdl2').why), '2.56: one at a time');
  g = keepGate(all(), true, '', 8_000_000, ['acars', 'vdl2']);
  ok(find(g, 'acars').on && find(g, 'vdl2').on && !find(g, 'adsb').enabled, '8: the pair, the rest greyed');
  g = keepGate(all(), false, '', 2_400_000, ['acars', 'vdl2']);
  ok(find(g, 'vdl2').on && !find(g, 'acars').on, '2.4: a stored pair keeps its first');
  ok(keepRateWords(2_048_000) === '2.048 MS/s' && keepRateWords(8_000_000) === '8 MS/s', 'rate words');
  ok(keepParse('acars+vdl2').join() === 'vdl2,acars' && keepName(['vdl2', 'acars']) === 'ACARS + VDL2', 'the pair\'s words');
}
// the local radio's facts
{
  const base = { driver: 'rtlsdr', isIcom: false, blockedModes: '', rate: 2_400_000, dabRateBoost: false, lockedOwnVfos: false };
  let f = keepAvailLocal(base);
  ok(f.avail.every((a) => a.hw && !a.why) && !f.comboOk, 'RTL at 2.4: every mode, no pair');
  f = keepAvailLocal({ ...base, rate: 2_048_000 });
  ok(/2\.4 MS\/s/.test(f.avail[0].why), 'RTL at 2.048: ADS-B says the rate');
  f = keepAvailLocal({ ...base, rate: 2_048_000, dabRateBoost: true });
  ok(!f.avail[0].why, '…the rate switch lets it');
  f = keepAvailLocal({ ...base, driver: 'airspyhf', rate: 912_000 });
  ok(!f.avail.find((a) => a.id === 'adsb')!.hw && !f.avail.find((a) => a.id === 'vor')!.hw && f.avail.find((a) => a.id === 'vdl2')!.hw, 'HF+: no ADS-B or VOR row');
  f = keepAvailLocal({ ...base, isIcom: true });
  ok(f.avail.every((a) => !a.hw), 'an Icom: nothing');
  f = keepAvailLocal({ ...base, driver: 'sdrplay', rate: 2_000_000, blockedModes: 'avdata+8m' });
  ok(f.comboOk, 'an RSP with the 8 MS/s switch: the pair is offered');
  f = keepAvailLocal({ ...base, driver: 'sdrplay', rate: 8_000_000 });
  ok(f.comboOk, 'an RSP at 8 MS/s: the pair');
  f = keepAvailLocal({ ...base, blockedModes: 'avdata' });
  ok(/switched off/.test(f.avail.find((a) => a.id === 'vdl2')!.why) && !f.comboOk, 'the datalink switched off: VDL2 greyed, no pair');
}
// the mirror and the migration
{
  ok(keepMirror(true, 'adsb', true).adsbKeepRunning && keepMirror(true, 'adsb', false).landingAdsb, 'ADS-B mirrored');
  ok(!keepMirror(true, 'vdl2', true).landingAdsb, 'VDL2: no ADS-B pair');
  const m = keepMigrate({ map: null, landingAdsb: true, adsbKeep: true })!;
  ok(m.map && m.modes === 'adsb' && m.keep, 'beta95 ADS-B + keep migrates unchanged');
  ok(keepMigrate({ map: '1', landingAdsb: true, adsbKeep: true }) === null, 'a stored map is never overwritten');
  ok(/SHARES its 137 MHz capture/.test(keepSatWords('vdl2', true)) && /ADS-B pauses/.test(keepSatWords('adsb', true)) && keepSatWords('vdl2', false) === '', 'the satellites words');
}
console.log(`keep run (app): ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
