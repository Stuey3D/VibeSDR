// test_keep_run.ts — the app's landing map + keep-running rule (src/utils/keepRun.ts, beta96): the SAME gate cases as
// vibeserver/test-keep-run.cpp §3/§4 and scripts/test-setup-keep-run.mjs, the local radio's facts, the ADS-B mirror and
// beta95's migration. Run: node --no-warnings scripts/test_keep_run.ts
import { singleOptionsLocal, singleFromTicks, singleWords, keepGate, keepAvailLocal, keepMirror, keepMigrate, keepParse, keepRateWords, keepSatWords, keepName, KEEP_MODES } from '../src/utils/keepRun.ts';
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
  ok(f.avail.filter((a) => a.id !== 'ism' && a.id !== 'aisdsc').every((a) => a.hw && !a.why) && !f.comboOk, 'RTL at 2.4: every aviation & maritime mode, no pair (ISM needs its bands; AIS + DSC its 6.2 MS/s)');
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
// ★★ the Amateur map's ISM (Stuart 11-26: 868 for MeshCore repeaters)
{
  ok(keepParse('ism:868,ism:433').join() === 'ism:868' && keepParse('ism').length === 0, 'ISM: one band, never without one');
  const base = { driver: 'rtlsdr', isIcom: false, blockedModes: '', rate: 2_400_000, dabRateBoost: false, lockedOwnVfos: false };
  const f = keepAvailLocal({ ...base, ismBands: ['433', '868'] });
  const g = keepGate(f.avail, f.comboOk, f.comboWhy, 2_400_000, ['ism:868'], 'amateur');
  ok(g.length === 1 && g[0].on && g[0].tok === 'ism:868', 'the amateur map: ISM only, ticked on 868');
  ok(keepGate(f.avail, f.comboOk, f.comboWhy, 2_400_000, ['ism:868']).every((o) => !o.on && o.id !== 'ism'), 'not an Aviation & Maritime mode');
  ok(!keepAvailLocal({ ...base, ismBands: [] }).avail.find((a) => a.id === 'ism')!.hw, 'no band in reach: no ISM row');
  ok(/switched off/.test(keepAvailLocal({ ...base, ismBands: ['868'], blockedModes: 'ism' }).avail.find((a) => a.id === 'ism')!.why), 'ISM switched off: greyed');
  ok(keepName(['ism:868'], { '868': '868 MHz SRD' }) === 'ISM 868 MHz SRD' && /ISM 868 MHz SRD pauses/.test(keepSatWords('ism:868', true, { '868': '868 MHz SRD' })), 'named from ISM\'s own words; pauses for a pass');
  ok(!keepMirror(true, 'ism:868', true).landingAdsb, 'no ADS-B pair');
}
// ★★★ the single-mode radio (the Setup page's same rules)
{
  const paul = 'wfm,nfm,am,sam,sau,sal,ecssu,ecssl,amst,usb,lsb,cwu,cwl,drm,freedv,m17,dvsig,dab,ism,ndb,ais,avdata,sondes,vormap,sat';
  ok(singleFromTicks(paul, '', []) === 'adsb' && singleFromTicks('dab', '', []) === '', "Paul's ticks read as ADS-B; ordinary ticks as nothing");
  const base = { driver: 'rtlsdr', isIcom: false, blockedModes: '', rate: 2_400_000, dabRateBoost: false, lockedOwnVfos: false, ismBands: ['868'] };
  const f = keepAvailLocal(base);
  const opts = singleOptionsLocal({ avail: f.avail, ismBands: ['868'], dab: true, ndb: false, shown: (id) => id !== 'drm', sat: false }).map((o) => o.tok);
  ok(['adsb', 'vdl2', 'acars', 'ais', 'vor', 'ism:868', 'dab', 'sondes', 'wfm'].every((t) => opts.includes(t)) && !opts.includes('sat') && !opts.includes('ndb') && !opts.includes('drm'),
     'the dropdown: what this radio can do, nothing else: ' + opts.join(' '));
  const hf = keepAvailLocal({ ...base, driver: 'airspyhf', ismBands: [] });
  ok(!singleOptionsLocal({ avail: hf.avail, ismBands: [], dab: false, ndb: true, shown: () => true, sat: true }).some((o) => o.tok === 'adsb' || o.tok.startsWith('ism')), 'HF+: no ADS-B, no ISM');
  ok(singleWords('vdl2') === 'VDL2' && singleWords('sat') === 'SATELLITES' && singleWords('ism:868', { '868': '868 MHz SRD' }) === 'ISM 868 MHz SRD', 'the words');
}
// ★★★ beta96 the MARITIME combination (AIS + DSC channel 70): drawn only on a wide radio whose rate holds both
{
  const base = { driver: 'rtlsdr', isIcom: false, blockedModes: '', rate: 2_400_000, dabRateBoost: false, lockedOwnVfos: false, ismBands: ['868'] };
  const hwOf = (f: ReturnType<typeof keepAvailLocal>) => f.avail.find((a) => a.id === 'aisdsc')!;
  ok(!hwOf(keepAvailLocal(base)).hw, 'RTL 2.4: AIS + DSC not drawn');
  ok(!hwOf(keepAvailLocal({ ...base, driver: 'sdrplay', rate: 6_000_000 })).hw, 'RSP 6 MS/s: not drawn (6.17 needed)');
  ok(!hwOf(keepAvailLocal({ ...base, driver: 'airspy', rate: 6_000_000 })).hw, 'Airspy Mini 6: not drawn');
  ok(hwOf(keepAvailLocal({ ...base, driver: 'sdrplay', rate: 8_000_000 })).hw, 'RSP 8 MS/s: drawn');
  ok(hwOf(keepAvailLocal({ ...base, driver: 'hackrf', rate: 20_000_000 })).hw && hwOf(keepAvailLocal({ ...base, driver: 'airspy', rate: 10_000_000 })).hw, 'HackRF 20, Airspy R2 10: drawn');
  ok(/DSC is switched off/.test(hwOf(keepAvailLocal({ ...base, driver: 'usrp', rate: 8_000_000, blockedModes: 'dsc' })).why), 'DSC switched off: greyed with the reason');
  const rsp = keepAvailLocal({ ...base, driver: 'sdrplay', rate: 8_000_000 });
  let g = keepGate(rsp.avail, rsp.comboOk, rsp.comboWhy, 8_000_000, ['aisdsc']);
  ok(find(g, 'aisdsc').on && !find(g, 'ais').enabled && /AIS \+ DSC \(channel 70\) is ticked/.test(find(g, 'ais').why), 'ticked: AIS greyed — ' + find(g, 'ais').why);
  g = keepGate(keepAvailLocal(base).avail, false, '', 2_400_000, []);
  ok(!find(g, 'aisdsc').drawn && find(g, 'ais').drawn, 'RTL: AIS drawn, AIS + DSC not');
  const opts = singleOptionsLocal({ avail: rsp.avail, ismBands: [], dab: false, ndb: false, shown: () => false, sat: false });
  ok(opts.some((o) => o.tok === 'aisdsc' && /MARITIME/.test(o.label)) && !singleOptionsLocal({ avail: keepAvailLocal(base).avail, ismBands: [], dab: false, ndb: false, shown: () => false, sat: false }).some((o) => o.tok === 'aisdsc'),
     'single mode lists MARITIME on the RSP at 8 only');
  ok(keepParse('aisdsc').join() === 'aisdsc' && singleWords('aisdsc') === 'MARITIME (AIS + DSC)', 'the token and its words');
}
console.log(`keep run (app): ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
