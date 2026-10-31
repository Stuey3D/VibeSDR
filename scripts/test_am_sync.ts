/**
 * test_am_sync.ts — src/services/amSync.ts, the ONE RULE the app and the web client both read for the V12 synchronous
 * AM family (docs/v12/AM-SYNC.md).
 *   Run: node --no-warnings --max-old-space-size=512 scripts/test_am_sync.ts
 * ★ SILENT: no audio, no network.
 */
import { AM_SYNC_IDS, AM_SYNC_LABEL, AM_SYNC_NAME, AM_SYNC_PASSBAND, amSyncOffered, amSyncReadout, isAmFamily, isAmSync,
         modeLegend, parseAmSync } from '../src/services/amSync.ts';
import { MODE_BANDWIDTHS, MODE_MAX_BW_HZ } from '../src/services/sdrProtocol.ts';
import { modeDisplay } from '../src/constants/modeBox.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { (c ? pass++ : fail++); console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };

console.log('── offered only where the server lists them ──');
ok(amSyncOffered(undefined).length === 0 && amSyncOffered(null).length === 0, 'no list (an older server, another backend) → none');
ok(amSyncOffered(['wfm', 'am', 'usb', 'drm']).length === 0, 'a VibeServer without them → none (it played "sam" as plain AM)');
ok(JSON.stringify(amSyncOffered(['amst', 'usb', 'sam', 'ecssl'])) === JSON.stringify(['sam', 'ecssl', 'amst']), 'key order is ours, not the server\'s');
ok(JSON.stringify(amSyncOffered(['am', ...AM_SYNC_IDS])) === JSON.stringify([...AM_SYNC_IDS]), 'all six');

console.log('── names ──');
ok(AM_SYNC_NAME.amst === 'AM Stereo (C-QUAM compatible)', 'AM stereo is "C-QUAM compatible", never "C-QUAM"');
ok(!Object.values(AM_SYNC_NAME).some((n) => /C-QUAM(?! compatible)/.test(n)), 'no name claims to be C-QUAM');
ok(modeLegend('sau') === 'SAM-U' && modeLegend('ecssl') === 'ECSS-L' && modeLegend('usb') === 'USB', 'legends');
ok(modeDisplay('amst') === 'AM-ST' && modeDisplay('cwu') === 'CW' && modeDisplay('sam') === 'SAM', 'the app mode box agrees');
ok(isAmFamily('am') && isAmFamily('ecssu') && !isAmFamily('usb') && !isAmSync('am'), 'family membership');
for (const id of AM_SYNC_IDS) ok(AM_SYNC_LABEL[id].length <= 6, `${AM_SYNC_LABEL[id]} fits a mode key (≤ 6 characters)`);

console.log('── passbands ──');
ok(MODE_BANDWIDTHS.ecssu[0] > 0 && MODE_BANDWIDTHS.ecssl[1] < 0, 'ECSS is one-sided, like SSB');
ok(MODE_BANDWIDTHS.sau[0] === -MODE_BANDWIDTHS.sau[1] && MODE_BANDWIDTHS.amst[0] === -5000, 'SAM-U/L and AM stereo are AM-shaped');
ok(MODE_BANDWIDTHS.sam[0] === -5000 && MODE_BANDWIDTHS.am[1] === 5000, 'SAM and AM unchanged');
ok(MODE_MAX_BW_HZ.ecssu === 12000 && MODE_MAX_BW_HZ.sau === 48000, 'ceilings mirror RxPipeline::maxBwHz');
for (const id of AM_SYNC_IDS) ok(JSON.stringify(MODE_BANDWIDTHS[id]) === JSON.stringify(AM_SYNC_PASSBAND[id]), `${id}: one table`);

console.log('── the amsync message ──');
const a = parseAmSync({ type: 'amsync', lock: true, off: 3.24, q: 0.97, lo: 5000, hi: 3900, auto: true, cut: true, pilot: false, notch: 0 });
ok(!!a && a.lock && Math.abs(a.offHz - 3.24) < 1e-9 && a.hiHz === 3900, 'parsed');
ok(parseAmSync({ type: 'rds' }) === null && parseAmSync(null) === null, 'other messages are not it');
const bad = parseAmSync({ type: 'amsync', off: 'x', q: 7, lo: -3 });
ok(!!bad && bad.offHz === 0 && bad.q === 1 && bad.loHz === 0 && !bad.lock, 'garbage is clamped, never trusted');
ok(amSyncReadout(a, 'sam') === 'SYNC +3.2 Hz · AUTO BW L 5.0k · U 3.9k', `readout: "${amSyncReadout(a, 'sam')}"`);
ok(amSyncReadout(a, 'am') === 'AUTO BW L 5.0k · U 3.9k', 'plain AM has no SYNC part');
ok(amSyncReadout(a, 'usb') === '' && amSyncReadout(null, 'sam') === '', 'nothing outside the AM family');
ok(amSyncReadout({ ...a!, lock: false, cut: false }, 'sau') === 'SYNC …', 'acquiring');
ok(amSyncReadout({ ...a!, offHz: -1.06, cut: false }, 'ecssu') === 'SYNC −1.1 Hz', 'a negative offset');

console.log(`\n${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
