// test-rsp-notch-lr.mjs — an SDRplay's automatic notches in a wide DAB locked range (2026-11-17, the Lenovo 65-station run:
// the DAB notch stayed IN at 215.14 MHz, cutting Band III; the weak 10D lost every DAB+ station). Pins, in the source:
//   · the start-up write of the owner's notch states RECORDS them (the auto rule compares against the record, so a
//     hardware-only write left "notch ON" invisible to it);
//   · the periodic rule uses the capture centre where there is no listen frequency or in a wide DAB range, and treats a
//     range holding Band III blocks (dabLrEligible) as a DAB capture — the DAB notch out;
//   · the rule itself: DAB notch off in DAB / a DAB capture and across 158–240 MHz.
// The end-to-end proof needs an RSP (the Lenovo run). Silent.
import fs from 'node:fs';
const s = fs.readFileSync(new URL('../android/app/src/main/cpp/local_sdr_shim.cpp', import.meta.url), 'utf8');
let fail = 0, pass = 0;
const ok = (c, w) => { if (c) pass++; else { fail++; console.log('  FAIL ' + w); } };
ok(/LOGI\("RSP: DAB notch ON \(owner\)"\); \}[\s\S]{0,700}?vsRecordNotchChoice\(g_vsRfNotch\.load\(\), g_vsDabNotch\.load\(\)\);/.test(s),
   'start-up: the owner\'s notch states are recorded as well as written');
ok(/const bool lrDab = dabLrEligible\(\);\s*const double notchHz = \(g_dabMode\.load\(std::memory_order_relaxed\) \|\| lrDab \|\| lf <= 0\) \? rtlCenter\.load\(\) : lf;/.test(s),
   'the tick: the capture centre in DAB, a wide DAB range, or with no listen frequency');
ok(/vsApplyAutoNotch\(sdrp\.get\(\), notchHz, lrDab\)/.test(s), '…and tells the rule it is a DAB capture');
ok(/vsAutoNotchFor\(hz, g_dabMode\.load\(std::memory_order_relaxed\) \|\| dabCapture\)/.test(s), 'the rule takes a DAB capture as DAB');
const rule = s.slice(s.indexOf('static VsNotchWant vsAutoNotchFor'), s.indexOf('static float vsDesiredNrStrength'));
ok(/if \(dabMode\)\s+w\.dab = false;/.test(rule) && /if \(hz >= 158\.0e6 && hz <= 240\.0e6\)\s+w\.dab = false;/.test(rule),
   'the rule: DAB notch off in DAB and across 158–240 MHz');
console.log(`${fail ? 'FAIL' : 'ok'}  rsp notch in a wide DAB range: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
