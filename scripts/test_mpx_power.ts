// ★ MPX POWER AS BOTH CLIENTS DRAW IT (src/services/mpxPower.ts) — in dBr, because it is relative to BS.412's
// ±19 kHz reference: 0 is the limit and a figure below it is compliant, not a fault (Stuart, 2026-10-19, at −0.4).
import { mpxPowerParts } from '../src/services/mpxPower.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}

eq('under the limit',          mpxPowerParts(-0.4, 60), { value: '−0.4 dBr', settling: '' });
eq('over the limit',           mpxPowerParts(6.63, 60), { value: '+6.6 dBr', settling: '' });
eq('exactly at it',            mpxPowerParts(0, 60),    { value: '+0.0 dBr', settling: '' });
eq('settling counts down',     mpxPowerParts(1.2, 16),  { value: '+1.2 dBr', settling: 'settling 44 s' });
eq('nothing yet',              mpxPowerParts(0, 0),     { value: '—', settling: '' });
eq('a NaN is nothing',         mpxPowerParts(NaN, 30),  { value: '—', settling: '' });

console.log(`mpx power: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
