// ★★★ HWINFO → CONTROLS, PER RSP MODEL (2026-10-22). Three things that must agree, checked together:
//   1. the setup page's OFFLINE copy (rspCaps in vibe_setup_page.h, used while a radio is not running) gives, for each
//      model's name, exactly the row vibe_rsp_caps.h gives — and for a LIVE radio it passes the hwinfo object through;
//   2. every capability key the server writes into hwinfo's `radio` object (radioCapsJsonCore) is a key the app panel
//      and the web client actually read — a key written and never read is a control nobody draws;
//   3. the app's own gating: each RSP control in LocalHardwarePanel is conditioned on its capability key.
import { readFileSync } from 'node:fs';
const root = new URL('..', import.meta.url).pathname;
const read = (p) => readFileSync(root + p, 'utf8');
let fails = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };

// ── the C++ table ──
const caps = read('android/app/src/main/cpp/vibe_rsp_caps.h');
const rows = {};
for (const m of caps.matchAll(/case k\w+:\s*return \{\s*"([^"]+)",\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*\{([^}]*)\}/g)) {
  const ports = [...m[8].matchAll(/"([^"]*)"/g)].map((x) => x[1].replace(/\\u03a9/g, 'Ω'));
  rows[m[1]] = { rfNotch: m[2] === 'true', dabNotch: m[3] === 'true', biasT: m[4] === 'true', hdr: m[5] === 'true',
                 amNotch: m[6] === 'true', extRef: m[7] === 'true', antennas: ports };
}
ok(Object.keys(rows).length === 7, `vibe_rsp_caps.h: 7 models read (${Object.keys(rows).join(', ')})`);

// ── 1. the setup page's copy ──
const page = read('android/app/src/main/cpp/vibe_setup_page.h');
const fn = page.match(/function rspCaps\(hw, r\) \{[\s\S]*?\n\}/);
ok(!!fn, 'vibe_setup_page.h has rspCaps()');
// eslint-disable-next-line no-new-func
const rspCaps = new Function(`${fn[0]}; return rspCaps;`)();
for (const [model, want] of Object.entries(rows)) {
  const got = rspCaps(null, { label: `SDRplay ${model} 2305012345` });
  const same = ['rfNotch', 'dabNotch', 'biasT', 'hdr', 'amNotch', 'extRef'].every((k) => got[k] === want[k])
            && JSON.stringify(got.antennas) === JSON.stringify(want.antennas) && got.known;
  ok(same, `setup page, ${model} not running: the same controls as the server's table`);
}
const live = rspCaps({ radio: { driver: 'sdrplay', model: 'RSP1A', hdr: true, antennas: ['X', 'Y'] } }, { label: 'SDRplay RSP1' });
ok(live.hdr === true && live.antennas.join() === 'X,Y', 'setup page, radio running: the radio\'s own hwinfo wins over its name');
const unk = rspCaps(null, { label: 'mystery' });
ok(!unk.known && unk.hdr && unk.amNotch, 'setup page, model unknown: every switch shown, the radio refuses what it lacks (as before)');

// ── 2. every hwinfo key written is read ──
const shim = read('android/app/src/main/cpp/local_sdr_shim.cpp');
const core = shim.slice(shim.indexOf('std::string LocalSdrShim::radioCapsJsonCore()'));
const rspAt = core.indexOf('"driver\\":\\"sdrplay\\"');
const rspPart = core.slice(rspAt, core.indexOf('return j;', rspAt));
const keys = ['rfNotch', 'dabNotch', 'biasT', 'biasTOn', 'antennas', 'antenna', 'antennaLocked', 'hdr', 'hdrOn', 'amNotch', 'amNotchOn', 'extRef', 'extRefOn'];
const panel = read('src/components/LocalHardwarePanel.tsx');
const web = read('web/client/src/main.ts') + read('web/client/src/spectrum.ts');
for (const k of keys) {
  ok(rspPart.includes(`\\"${k}\\":`), `server writes radio.${k}`);
  ok(new RegExp(`radio\\??\\.${k}\\b`).test(panel), `app panel reads radio.${k}`);
}
for (const k of ['rfNotch', 'dabNotch', 'biasT', 'hdr', 'amNotch', 'extRef', 'antennas'])
  ok(new RegExp(`\\.${k}\\b`).test(web), `web client reads .${k}`);

// ── 3. the app gates each RSP control on its capability ──
for (const [cap, label] of [['hdr', 'HDR (below 2 MHz)'], ['extRef', '24 MHz reference out'], ['biasT', 'Bias-T (antenna power)'], ['amNotch', 'AM notch (Hi-Z)']]) {
  const at = panel.indexOf(`>${label}<`);   // the drawn text, not a comment that mentions it
  const before = panel.slice(Math.max(0, at - 1500), at);
  ok(at > 0 && new RegExp(`radio\\??\\.${cap}\\b`).test(before), `app: "${label}" is drawn only when radio.${cap}`);
}
console.log(fails ? `\n${fails} failed` : '\nall passed');
process.exit(fails ? 1 : 0);
