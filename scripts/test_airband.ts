/**
 * Airband channel naming + 8.33 kHz step arithmetic (src/utils/airband.ts).
 *
 * Reference: ICAO Annex 10 Vol V §4.1.2.4 Table 4-1 (bis) channel/frequency pairing, as restated by
 * the UK CAA/Ofcom ("Channel 126.855 / 126.8500 MHz"; "to communicate on 118.0333 MHz the pilot
 * will dial 118.035") and EUROCONTROL ("8.33 kHz channel 118.010 tunes 118.0083 MHz, 132.035 tunes
 * 132.0333 MHz").
 *
 * Run: npx tsx scripts/test_airband.ts
 */
import { STEP_FM_ODD } from '../src/utils/airband';
import {
  STEP_833, stepFrom, stepIndex, stepHz, snapToStep, channelAt, airbandChannel, channelNameToHz,
  airbandEntry, airbandStepFrom, airbandPassband, AIR_PB_25, AIR_PB_833,
} from '../src/utils/airband';
import { stepsForFreq } from '../src/services/sdrTypes';
import { bandTuneDefaults, bandJumpDefaults } from '../src/constants/bandPlan';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}

// ── A full 25 kHz block, both spacings ────────────────────────────────────────────────────────
// frequency (Hz)  →  [name in 8.33 mode, name in 25 kHz mode]
const block: Array<[number, string, string | null]> = [
  [118_000_000, '118.005', '118.000'],
  [118_008_333, '118.010', null],        // no 25 kHz name — the 8.33 name shows in any mode
  [118_016_667, '118.015', null],
  [118_025_000, '118.030', '118.025'],
  [118_033_333, '118.035', null],
  [118_041_667, '118.040', null],
  [118_050_000, '118.055', '118.050'],
  [118_058_333, '118.060', null],
  [118_066_667, '118.065', null],
  [118_075_000, '118.080', '118.075'],
  [118_083_333, '118.085', null],
  [118_091_667, '118.090', null],
  [118_100_000, '118.105', '118.100'],
];
for (const [hz, n833, n25] of block) {
  eq(`8.33 name @${hz}`, channelAt(hz, STEP_833)?.name, n833);
  eq(`25k-mode name @${hz}`, channelAt(hz, 25000)?.name, n25 ?? n833);
  eq(`spacing @${hz} 8.33 mode`, channelAt(hz, STEP_833)?.spacing, 833);
}
// The published worked examples.
eq('Ofcom 126.855', channelAt(126_850_000, STEP_833)?.name, '126.855');
eq('Ofcom 118.035 → 118.0333', channelNameToHz(118_035), { ok: true, hz: 118_033_333, spacing: 833, name: '118.035' });
eq('EUROCONTROL 132.035', channelNameToHz(132_035), { ok: true, hz: 132_033_333, spacing: 833, name: '132.035' });
eq('true text 118.010', channelAt(118_008_333, STEP_833)?.trueText, '118.0083');
eq('true text 118.015', channelAt(118_016_667, STEP_833)?.trueText, '118.0167');

// Names that never exist.
for (const bad of [118_020, 118_045, 118_070, 118_095, 136_995]) {
  const r = channelNameToHz(bad);
  eq(`unused name ${bad}`, r && !r.ok, true);
}
eq('118.020 neighbours', (() => { const r = channelNameToHz(118_020); return r && !r.ok ? [r.lower, r.upper] : null; })(),
   ['118.015', '118.025']);

// Every name in a block maps back to a channel that is named the same (round trip).
for (let k = 118_000; k < 118_100; k += 5) {
  const r = channelNameToHz(k);
  if (!r || !r.ok) continue;
  const ch = channelAt(r.hz, r.spacing === 833 ? STEP_833 : 25000, { hz: r.hz, spacing: r.spacing });
  eq(`round trip ${k}`, ch?.nameKhz, k);
}

// ── Band edges ────────────────────────────────────────────────────────────────────────────────
eq('bottom 25k', channelAt(118_000_000, 25000)?.name, '118.000');
eq('top 25k', channelAt(136_975_000, 25000)?.name, '136.975');
eq('top 8.33', channelAt(136_991_667, STEP_833)?.name, '136.990');
eq('136.985', channelAt(136_983_333, STEP_833)?.name, '136.985');
eq('136.990 → hz', channelNameToHz(136_990), { ok: true, hz: 136_991_667, spacing: 833, name: '136.990' });
eq('137.000 not COM', channelAt(137_000_000, STEP_833), null);
eq('117.975 not COM', channelAt(117_975_000, 25000), null);
eq('off raster 128.5928', channelAt(128_592_800, STEP_833), null);
eq('only AM', airbandChannel(118_000_000, 'nfm', STEP_833), null);
eq('AM ok', airbandChannel(118_000_000, 'am', STEP_833)?.name, '118.005');
eq('121.5 emergency 25k', channelAt(121_500_000, 25000)?.name, '121.500');

// ── Pinned designation: the name the listener selected wins on a shared frequency ─────────────
eq('pinned 25 in 8.33 mode', channelAt(121_500_000, STEP_833, { hz: 121_500_000, spacing: 25 })?.name, '121.500');
eq('pinned 8.33 in 25 mode', channelAt(118_000_000, 25000, { hz: 118_000_000, spacing: 833 })?.name, '118.005');
eq('pin elsewhere ignored', channelAt(118_000_000, 25000, { hz: 118_025_000, spacing: 833 })?.name, '118.000');

// ── Entry: pilot-style names ──────────────────────────────────────────────────────────────────
eq('entry 118.005', airbandEntry(118_005_000), { ok: true, hz: 118_000_000, spacing: 833, name: '118.005' });
eq('entry 118.010', airbandEntry(118_010_000), { ok: true, hz: 118_008_333, spacing: 833, name: '118.010' });
eq('entry 121.5', airbandEntry(121_500_000), { ok: true, hz: 121_500_000, spacing: 25, name: '121.500' });
eq('entry 124.725', airbandEntry(124_725_000), { ok: true, hz: 124_725_000, spacing: 25, name: '124.725' });
eq('entry 118.020 refused', airbandEntry(118_020_000)?.ok, false);
eq('entry fine freq passes through', airbandEntry(118_008_300), null);
eq('entry VOLMET passes through', airbandEntry(128_592_800), null);
eq('entry outside COM', airbandEntry(145_005_000), null);
eq('entry 117.950 nav', airbandEntry(117_950_000), null);

// ── Step arithmetic: exact, driftless ─────────────────────────────────────────────────────────
eq('index of 118.00833 floors to itself', stepIndex(118_008_333, STEP_833, 'floor'), 14161);
eq('index of 118.01667 ceils to itself', stepIndex(118_016_667, STEP_833, 'ceil'), 14162);
eq('one up from 118.000', stepFrom(118_000_000, STEP_833, 1), 118_008_333);
eq('one up from 118.00833', stepFrom(118_008_333, STEP_833, 1), 118_016_667);
eq('one up from 118.01667', stepFrom(118_016_667, STEP_833, 1), 118_025_000);
eq('one down from 118.025', stepFrom(118_025_000, STEP_833, -1), 118_016_667);
{
  // 3000 single steps up from 118.000 = 1000 × 25 kHz: must land EXACTLY on 143.000 — no drift.
  let f = 118_000_000;
  for (let i = 0; i < 3000; i++) f = stepFrom(f, STEP_833, 1);
  eq('3000 steps no drift', f, 143_000_000);
  for (let i = 0; i < 3000; i++) f = stepFrom(f, STEP_833, -1);
  eq('and back', f, 118_000_000);
}
eq('off-grid up goes to next point', stepFrom(118_004_000, STEP_833, 1), 118_008_333);
eq('off-grid down goes to point below', stepFrom(118_004_000, STEP_833, -1), 118_000_000);
eq('snap 118.0081', snapToStep(118_008_100, STEP_833), 118_008_333);
eq('ordinary steps unchanged', stepFrom(7_153_437, 1000, 1), 7_154_000);
eq('ordinary round', stepFrom(7_153_437, 1000, 2, 'round'), 7_155_000);
eq('stepHz integer', Number.isInteger(stepHz(14161, STEP_833)), true);

// ── Knob walk in 8.33 mode: names in order, both spacings on the shared frequency ─────────────
{
  const names: string[] = [];
  let hz = 118_000_000;
  let d = { hz, spacing: 25 as const } as { hz: number; spacing: 25 | 833 } | null;
  names.push(channelAt(hz, STEP_833, d)!.name);
  for (let i = 0; i < 9; i++) {
    const r = airbandStepFrom(hz, STEP_833, 1, 'dir', 'am', d);
    hz = r.hz; d = r.desig;
    names.push(channelAt(hz, STEP_833, d)!.name);
  }
  eq('walk up', names, ['118.000', '118.005', '118.010', '118.015', '118.025', '118.030', '118.035', '118.040', '118.050', '118.055']);
  const back: string[] = [];
  for (let i = 0; i < 9; i++) {
    const r = airbandStepFrom(hz, STEP_833, -1, 'dir', 'am', d);
    hz = r.hz; d = r.desig;
    back.push(channelAt(hz, STEP_833, d)!.name);
  }
  eq('walk down', back, ['118.050', '118.040', '118.035', '118.030', '118.025', '118.015', '118.010', '118.005', '118.000']);
  eq('walk ends at 118.000', airbandStepFrom(118_000_000, STEP_833, -1, 'dir', 'am', { hz: 118_000_000, spacing: 25 }).hz, 118_000_000);
  eq('multi-step walk', channelAt(airbandStepFrom(118_000_000, STEP_833, 4, 'round', 'am', { hz: 118_000_000, spacing: 25 }).hz, STEP_833)?.name, '118.030');
  eq('walk top end', airbandStepFrom(136_991_667, STEP_833, 1, 'dir', 'am', null).hz, 136_991_667);
  eq('not AM = plain raster', airbandStepFrom(118_000_000, STEP_833, 1, 'dir', 'nfm', null).hz, 118_008_333);
  eq('25k mode = plain 25k', airbandStepFrom(118_000_000, 25000, 1, 'dir', 'am', null).hz, 118_025_000);
}

// ── Passband defaults ─────────────────────────────────────────────────────────────────────────
{
  const ch833 = channelAt(118_008_333, STEP_833);
  const ch25 = channelAt(118_000_000, 25000);
  eq('AM default → 8.33', airbandPassband(118_008_333, 'am', ch833, -5000, 5000), [-AIR_PB_833, AIR_PB_833]);
  eq('AM default → 25', airbandPassband(118_000_000, 'am', ch25, -5000, 5000), [-AIR_PB_25, AIR_PB_25]);
  eq('8.33 default → 25', airbandPassband(118_000_000, 'am', ch25, -2800, 2800), [-8500, 8500]);
  eq('already right', airbandPassband(118_008_333, 'am', ch833, -2800, 2800), null);
  eq('bookmark ±3k untouched', airbandPassband(118_008_333, 'am', ch833, -3000, 3000), null);
  eq('off raster in COM: unchanged', airbandPassband(128_592_800, 'am', null, -2800, 2800), null);
  eq('leaving the airband restores AM', airbandPassband(648_000, 'am', null, -2800, 2800), [-5000, 5000]);
  eq('elsewhere untouched', airbandPassband(648_000, 'am', null, -3000, 3000), null);
  eq('not AM: nothing', airbandPassband(118_000_000, 'usb', ch25, -5000, 5000), null);
  eq('mil UHF: AM default → 25', airbandPassband(243_000_000, 'am', null, -5000, 5000), [-AIR_PB_25, AIR_PB_25]);
  eq('mil UHF: 8.33 leftover → 25', airbandPassband(300_025_000, 'am', null, -2800, 2800), [-AIR_PB_25, AIR_PB_25]);
  eq('mil UHF: already right', airbandPassband(243_000_000, 'am', null, -8500, 8500), null);
  eq('mil UHF: bookmark ±3k untouched', airbandPassband(243_000_000, 'am', null, -3000, 3000), null);
  eq('mil UHF: NFM untouched', airbandPassband(243_000_000, 'nfm', null, -5000, 5000), null);
  eq('above 400: restores AM', airbandPassband(406_025_000, 'am', null, -8500, 8500), [-5000, 5000]);
  eq('mil UHF: no channel name', airbandChannel(243_000_000, 'am', 25000, null), null);
}

// ── Ladder + band plan ────────────────────────────────────────────────────────────────────────
eq('airband ladder has 5k and 8.33', [stepsForFreq(125_000_000).includes(5000), stepsForFreq(125_000_000).includes(STEP_833)], [true, true]);
eq('no 8.33 at 145 MHz', stepsForFreq(145_000_000).includes(STEP_833), false);
eq('no 8.33 on VOR/ILS', stepsForFreq(113_000_000).includes(STEP_833), false);
eq('no 8.33 on HF', stepsForFreq(7_000_000).includes(STEP_833), false);
eq('R1 airband default step', bandTuneDefaults(125_000_000, 1), { mode: 'am', step: STEP_833 });
eq('R2 airband default step', bandTuneDefaults(125_000_000, 2), { mode: 'am', step: 25000 });
eq('mil UHF in a satcom segment lands AM', bandTuneDefaults(243_000_000, 1), { mode: 'am', step: 25000 });
eq('mil UHF outside satcom lands AM', bandTuneDefaults(275_000_000, 2), { mode: 'am', step: 25000 });
eq('1.25m ham still wins in R2', bandTuneDefaults(223_500_000, 2), { mode: 'nfm', step: 12500 });
eq('unknown region airband', bandTuneDefaults(125_000_000, 0).step, 25000);
eq('VOR/ILS unchanged', bandTuneDefaults(113_000_000, 1), { mode: 'am', step: 25000 });

// ★★ A JUMP (typed / bookmark / band plan — never the drums) takes the landing band's step, and its mode on a band
//    change (Stuart, 2026-10-03). One rule for the app and the web client: bandJumpDefaults.
eq('HF USB → FM typed: WFM + 100 kHz', bandJumpDefaults(14_200_000, 96_600_000, 'usb', 1), { mode: 'wfm', step: 100000 });
eq('already WFM → FM: step only', bandJumpDefaults(14_200_000, 96_600_000, 'wfm', 1), { step: 100000 });
eq('FM → airband R1: AM + 8.33', bandJumpDefaults(96_600_000, 121_500_000, 'wfm', 1), { mode: 'am', step: STEP_833 });
eq('FM → airband R2: AM + 25k', bandJumpDefaults(96_600_000, 121_500_000, 'wfm', 2), { mode: 'am', step: 25000 });
eq('within airband: NFM kept, step follows', bandJumpDefaults(121_500_000, 125_000_000, 'nfm', 1), { step: STEP_833 });
eq('in no band: nothing', bandJumpDefaults(96_600_000, 3_000_000_000, 'wfm', 1), {});

// ★★ THE AMERICAS' 200 kHz FM RASTER (RC30): odd tenths, offered in Region 2's FM band only.
eq('R2 FM ladder has 200k', stepsForFreq(98_100_000, 2).includes(STEP_FM_ODD), true);
eq('R1 FM ladder has no 200k', stepsForFreq(98_100_000, 1).includes(STEP_FM_ODD), false);
eq('unknown region: no 200k', stepsForFreq(98_100_000).includes(STEP_FM_ODD), false);
eq('R2 airband is not the FM ladder', stepsForFreq(125_000_000, 2).includes(STEP_FM_ODD), false);
eq('snap 98.0 → 98.1 (ties go up)', snapToStep(98_000_000, STEP_FM_ODD), 98_100_000);
eq('snap 98.05 → 98.1', snapToStep(98_050_000, STEP_FM_ODD), 98_100_000);
eq('snap 98.25 → 98.3', snapToStep(98_250_000, STEP_FM_ODD), 98_300_000);
eq('step up from 98.1 → 98.3', stepFrom(98_100_000, STEP_FM_ODD, 1), 98_300_000);
eq('step down from 98.1 → 97.9', stepFrom(98_100_000, STEP_FM_ODD, -1), 97_900_000);
eq('step up from 98.0 (off grid) → 98.1', stepFrom(98_000_000, STEP_FM_ODD, 1), 98_100_000);
eq('step down from 98.2 (off grid) → 98.1', stepFrom(98_200_000, STEP_FM_ODD, -1), 98_100_000);
eq('R2 FM landing step = 200k', bandTuneDefaults(98_100_000, 2).step, STEP_FM_ODD);
eq('R1 FM landing step = 100k', bandTuneDefaults(98_100_000, 1).step, 100000);
eq('unknown region FM = 100k', bandTuneDefaults(98_100_000, 0).step, 100000);
eq('other steps unoffset', snapToStep(98_049_000, 100000), 98_000_000);

console.log(`${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
