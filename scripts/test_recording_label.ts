// test_recording_label.ts — the recording's frequency + mode word (src/services/recordingLabel.ts). node --experimental-strip-types
import { recordingLabel, fileSafe } from '../src/services/recordingLabel.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, w: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${w}`); };
const dab = { channel: '11D', centreHz: 222064000, sid: 0xc0d8, services: [{ sid: 0xc0d8, label: 'talkSPORT', codec: 'aac', subch: 1 }, { sid: 1, label: 'Other', codec: 'aac', subch: 2 }] } as never;
let r = recordingLabel(96600000, 'wfm', null);
ok(r.hz === 96600000 && r.mode === 'WFM', `analogue: the dial and the mode (${r.mode})`);
r = recordingLabel(222064000, 'wfm', dab);
ok(r.hz === 222064000 && r.mode === 'DAB talkSPORT 11D', `DAB: the multiplex and the service (${r.mode})`);
r = recordingLabel(0, '', { channel: '12B', centreHz: 225648000, sid: 9, services: [] } as never);
ok(r.mode === 'DAB 12B' && r.hz === 225648000, `DAB with the service not known yet (${r.mode})`);
ok(fileSafe('Absolute 80s/90s: "Hits"_x') === 'Absolute 80s 90s Hits x', `air text made file-safe, no underscore (${fileSafe('Absolute 80s/90s: "Hits"_x')})`);
ok(fileSafe('A'.repeat(80)).length === 40, 'at most 40 characters');
console.log(`\n${pass} passed, ${fail} failed`); process.exit(fail ? 1 : 0);
