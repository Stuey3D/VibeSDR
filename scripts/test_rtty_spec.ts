// test_rtty_spec.ts — the full manual RTTY spec (src/utils/rttySpec.ts): what each setting sends the server.
import { rttyFraming, rttyStops } from '../src/utils/rttySpec.ts';
let pass = 0, fail = 0;
const eq = (a: unknown, b: unknown, m: string) => { if (JSON.stringify(a) === JSON.stringify(b)) pass++; else { fail++; console.log(`  FAIL ${m}: ${JSON.stringify(a)} ≠ ${JSON.stringify(b)}`); } };
eq(rttyFraming({ encoding: 'ITA2' }), '5N1.5', 'ITA2 default 1.5 stop (DWD, amateur)');
eq(rttyFraming({ encoding: 'ITA2', stop: 1 }), '5N1', 'ITA2 1 stop (PBB)');
eq(rttyFraming({ encoding: 'ITA2', stop: 2 }), '5N2', 'ITA2 2 stop');
eq(rttyFraming({ encoding: 'ASCII' }), '7N1', 'ASCII default 7N1');
eq(rttyFraming({ encoding: 'ASCII', dataBits: 7, parity: 'E', stop: 1 }), '7E1', 'ASCII 7E1');
eq(rttyFraming({ encoding: 'ASCII', dataBits: 8, parity: 'N', stop: 2 }), '8N2', 'ASCII 8N2');
eq(rttyFraming({ encoding: 'ASCII', stop: 1.5 }), '7N1', 'ASCII never sends 1.5 stop');
eq(rttyFraming({ encoding: 'CCIR476', stop: 2 }), '4/7', 'SITOR-B is always 4/7');
eq(rttyStops('ITA2'), [1, 1.5, 2], 'ITA2 offers 1, 1.5, 2');
eq(rttyStops('ASCII'), [1, 2], 'ASCII offers 1, 2');
eq(rttyStops('CCIR476'), [], 'SITOR-B offers no stop row');
console.log(`rttySpec: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
