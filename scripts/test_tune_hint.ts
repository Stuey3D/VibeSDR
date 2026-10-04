// test_tune_hint.ts — RTTY AUTO's tuning guide label (src/utils/tuneHint.ts): the arrow is the DIAL's way.
import { tuneHintLabel } from '../src/utils/tuneHint.ts';
let pass = 0, fail = 0;
const eq = (a: string, b: string, m: string) => { if (a === b) pass++; else { fail++; console.log(`  FAIL ${m}: "${a}" ≠ "${b}"`); } };
eq(tuneHintLabel(0, 'usb'), '', 'zero = no guide');
eq(tuneHintLabel(500, 'usb'), 'tune ◀ 500 Hz', 'USB, pitch up 500 → dial down');
eq(tuneHintLabel(-100, 'usb'), 'tune 100 Hz ▶', 'USB, pitch down 100 → dial up');
eq(tuneHintLabel(500, 'lsb'), 'tune 500 Hz ▶', 'LSB, pitch up → dial up');
eq(tuneHintLabel(-1400, 'cwu'), 'tune 1.4 kHz ▶', 'CW-U, 1400 → kHz');
eq(tuneHintLabel(900, 'CWL'), 'tune 900 Hz ▶', 'CW-L counts as lower');
console.log(`tuneHint: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
