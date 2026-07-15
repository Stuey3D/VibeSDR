// Runnable with: node tools/tests/piCallsign.test.ts   (Node >= 22 strips types)
// Verifies the NRSC-4-B PI<->callsign logic (addendum §6).
import { piToCallsign, callsignToPi } from '../../src/services/piCallsign.ts';

let fail = 0;
const eq = (name: string, got: unknown, want: unknown) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  console.log((ok ? 'ok   ' : 'FAIL ') + name +
    (ok ? '' : ` — got ${JSON.stringify(got)} want ${JSON.stringify(want)}`));
  if (!ok) fail++;
};

// Spec worked example: KBBB -> 4799.
eq('KBBB->PI', callsignToPi('KBBB'), 4799);
eq('PI 4799->KBBB', piToCallsign(4799).callsign, 'KBBB');

// Round-trip across both blocks and the extremes.
for (const cs of ['KBBB', 'WABC', 'WXYZ', 'KROQ', 'WKRP', 'KQED', 'KAAA', 'WZZZ', 'WAAA', 'KZZZ']) {
  const pi = callsignToPi(cs)!;
  eq(`roundtrip ${cs}`, piToCallsign(pi).callsign, cs);
}

// Block boundaries.
eq('K base 4096 -> KAAA', piToCallsign(4096).callsign, 'KAAA');
eq('W base 21672 -> WAAA', piToCallsign(21672).callsign, 'WAAA');

// -FM suffix is stripped before encoding.
eq('WABC-FM == WABC', callsignToPi('WABC-FM'), callsignToPi('WABC'));

// Caveats degrade to a null callsign (never a wrong guess), addendum §6.
eq('PI 0 -> null', piToCallsign(0).callsign, null);
eq('PI 0xFFFF -> null', piToCallsign(0xffff).callsign, null);
eq('below K block -> null', piToCallsign(1000).callsign, null);
eq('A-block -> null', piToCallsign(0xa123).callsign, null);
eq('3-letter (no Annex D table) -> null', callsignToPi('KOB'), null);
eq('bad-length -> null', callsignToPi('KAB'), null);

console.log(fail === 0 ? '\nPI: ALL PASS' : `\nPI: ${fail} FAILURES`);
if (fail) process.exit(1);
