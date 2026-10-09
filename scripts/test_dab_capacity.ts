// The DAB capacity-unit bar's segments (src/utils/dabCapacity.ts) — one copy for the app and the web client.
import { DAB_CU_TOTAL, cuPct, dabCuSegments } from '../src/utils/dabCapacity.ts';

let pass = 0, fail = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  const ok = JSON.stringify(got) === JSON.stringify(want); ok ? pass++ : fail++;
  if (!ok) console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const svcs = [
  { sid: 1, subch: 3, cuStart: 84, cuSize: 48 },
  { sid: 2, subch: 1, cuStart: 0, cuSize: 84 },
  { sid: 3, subch: 3, cuStart: 84, cuSize: 48 },          // a second service on the same sub-channel
  { sid: 4, subch: 7, cuStart: 840, cuSize: 60 },          // runs off the end: clipped
  { sid: 5, subch: 9 },                                    // no CU yet (FIG 0/1 not read): left out
  { sid: 6, subch: 11, cuStart: 900, cuSize: 10 },         // impossible start: left out
];
const segs = dabCuSegments(svcs, 3);
eq('one segment per sub-channel, in CU order', segs.map((s) => s.start), [0, 84, 840]);
eq('the playing sub-channel is lit', segs.map((s) => s.current), [false, true, false]);
eq('clipped to the 864 CU of a multiplex', segs[2].size, DAB_CU_TOTAL - 840);
eq('percentages', cuPct(segs[1]), { left: (84 / 864) * 100, width: (48 / 864) * 100 });
eq('nothing playing: nothing lit', dabCuSegments(svcs, undefined).some((s) => s.current), false);
eq('no services: no segments', dabCuSegments([], 1), []);
console.log(`dab capacity: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
