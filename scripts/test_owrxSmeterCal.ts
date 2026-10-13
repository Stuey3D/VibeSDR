/**
 * OWRX S-METER CALIBRATION (src/services/owrxSmeterCal.ts). OWRX's `smeter` is uncalibrated dB below
 * the server's own full scale; we printed it as dBm and every OWRX server read S9+ on noise (2026-10-13).
 *
 * Proves:
 *   • an owner's smeter-plugin calibration is read from real init.js shapes (comments, URLs in strings);
 *   • a commented-out config, or one left at the defaults, is NOT a calibration (→ the S2 anchor);
 *   • nonsense figures are refused;
 *   • the floor tracker falls at once and rises only slowly, so a carrier cannot become the floor;
 *   • the noise floor lands on S3 (−109) and squelch converts both ways exactly.
 *
 * Run: node --no-warnings scripts/test_owrxSmeterCal.ts
 */
import { parseOwnerCal, FloorTracker, ownerMap, anchorMap, OWRX_NOISE_ANCHOR_DBM } from '../src/services/owrxSmeterCal.ts';

let fails = 0;
const ok = (c: boolean, m: string) => { if (c) console.log('  ✓ ' + m); else { fails++; console.log('  ✗ ' + m); } };

// Shapes taken from public servers' init.js (2026-10-13).
const active = `// Plugins
Plugins.load('https://0xaf.github.io/openwebrxplus-plugins/receiver/smeter/smeter.js');
window.smeter_config_global = {
    calibration_offset_hf: -55, // Calibration for HF (<30MHz) in dB
    calibration_offset_vhf: -72, // Calibration for VHF/UHF (>30MHz) in dB
    linearity_hf: 1.0,         // Linearity correction factor for HF
    linearity_vhf: 1.2222222222222223,
    hide_original: true, show_text: true
};`;
const c1 = parseOwnerCal(active);
ok(c1?.hf?.cal === -55 && c1?.hf?.lin === 1, 'HF offset/linearity read');
ok(c1?.vhf?.cal === -72 && Math.abs((c1?.vhf?.lin ?? 0) - 1.2222) < 1e-3, 'VHF offset/linearity read');

const commented = `window.smeter_config_global = {
    // calibration_offset_hf: -42, // Calibration for HF
    // calibration_offset_vhf: -70,
    hide_original: true
};
Plugins.load('smeter');`;
ok(parseOwnerCal(commented) === null, 'commented-out figures are not a calibration');

const defaults = `window.smeter_config_global = { calibration_offset_hf: 0, calibration_offset_vhf: 0, linearity_hf: 1.0, linearity_vhf: 1.0 };`;
ok(parseOwnerCal(defaults) === null, 'plugin defaults are not a calibration');

const hfOnly = `window.smeter_config_global = { calibration_offset_hf: -23, calibration_offset_vhf: 0, linearity_hf: 1.0, linearity_vhf: 1.0 };`;
const c2 = parseOwnerCal(hfOnly);
ok(c2?.hf?.cal === -23 && c2?.vhf === null, 'one band measured, the other falls through to the anchor');

const blockComment = `/* window.smeter_config_global = { calibration_offset_hf: -40 }; */ Plugins.load('smeter');`;
ok(parseOwnerCal(blockComment) === null, 'block-commented config ignored');
ok(parseOwnerCal(`window.smeter_config_global = { calibration_offset_hf: 419, linearity_hf: 6.2 };`) === null,
   'absurd figures refused');
ok(parseOwnerCal(`Plugins.load('listeners');`) === null, 'no smeter config → null');

// Floor tracker: noise around −60 with a −30 carrier for long stretches.
const ft = new FloorTracker();
let seed = 1; const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
for (let i = 0; i < 200; i++) ft.push(-60 + (rnd() - 0.5) * 4);
const f0 = ft.value!;
ok(f0 > -63 && f0 < -59, `floor learned from noise (${f0.toFixed(1)})`);
for (let i = 0; i < 300; i++) ft.push(-30 + (rnd() - 0.5) * 2);    // ~1 min of carrier (OWRX sends ~4–5 a second)
ok(ft.value! < f0 + 3, `a carrier does not become the floor (${ft.value!.toFixed(1)} after 300 readings)`);
for (let i = 0; i < 100; i++) ft.push(-70 + (rnd() - 0.5) * 4);
ok(ft.value! < -70, `a quieter floor is taken at once (${ft.value!.toFixed(1)})`);

// Maps.
const a = anchorMap(-43);
ok(a.toDisplay(-43) === OWRX_NOISE_ANCHOR_DBM && OWRX_NOISE_ANCHOR_DBM === -109, 'noise floor reads S3 (−109)');
ok(a.toDisplay(-13) === -79, 'a signal 30 dB up reads 30 dB above S3');
ok(a.toRaw(a.toDisplay(-50)) === -50, 'anchor squelch round-trips');
const o = ownerMap({ cal: -34, lin: 1.2222222222222223 });
ok(Math.abs(o.toRaw(o.toDisplay(-61.3)) + 61.3) < 1e-9, 'owner squelch round-trips');

console.log(fails ? `\n${fails} FAILED` : '\nall passed');
process.exit(fails ? 1 : 0);
