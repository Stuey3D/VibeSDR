#!/usr/bin/env node
// check-gain-write-path.mjs — THE AUGUST 2026 RTL STUTTER FIXES MUST STAY FIXED (rtl_agc_stutter_solved).
//
// A gain change on an RTL stuttered the audio three separate ways, and each has come back to look
// at once already (2026-10-21, the Portable stuttering on every VibeAGC move):
//   1. the write held modeMtx — the lock the DSP thread takes per buffer — across USB control transfers;
//   2. a second transfer (rtlsdr_set_tuner_gain_mode re-assert) on every move;
//   3. the web client rebuilt the radio controls on EVERY hwinfo, and hwinfo comes on every gain move.
// This is a source check, because all three are shapes of code, not numbers:
//   · every per-move rtlsdr_set_tuner_gain() is under devMtx, never VIBE_HW_LOCK / modeMtx, and has no
//     gain-mode re-assert beside it (the open/restore paths, which set manual mode once, are exempt);
//   · the DSP loop and the gain-step smoother never call into the radio;
//   · the web client's hwinfo handler still returns early on an unchanged signature, and the RTL's
//     radio-caps object carries nothing that moves with the gain.
// The CPU cost of what the DSP does after a write is test-gain-step-cost.cpp's job.
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const shim = readFileSync(join(root, 'android/app/src/main/cpp/local_sdr_shim.cpp'), 'utf8').split('\n');
const main = readFileSync(join(root, 'web/client/src/main.ts'), 'utf8');
let fails = 0;
const check = (ok, what) => { console.log(`   ${ok ? 'ok  ' : 'FAIL'}   ${what}`); if (!ok) fails++; };
const code = (l) => l.replace(/\/\/.*$/, '').replace(/\/\*.*?\*\//g, '');

// ── 1 + 2: the per-move writes ──
let perMove = 0;
shim.forEach((raw, i) => {
  const l = code(raw);
  if (!/rtlsdr_set_tuner_gain\(/.test(l) || /int setTunerGain\(/.test(l)) return;
  const before = shim.slice(Math.max(0, i - 4), i).map(code).join('\n');
  // the open/restore paths set manual mode ONCE a few lines up, then the starting gain, before streaming
  const isOpenPath = /rtlsdr_set_tuner_gain_mode\(/.test(shim.slice(Math.max(0, i - 12), i).map(code).join('\n'));
  if (isOpenPath) return;
  perMove++;
  check(/devMtx/.test(before), `line ${i + 1}: the gain write holds devMtx`);
  check(!/VIBE_HW_LOCK\(\)|modeMtx/.test(before + '\n' + l), `line ${i + 1}: …and not modeMtx (the DSP's lock)`);
});
check(perMove >= 1, `found the per-move gain write (${perMove})`);

// ── the DSP loop and the smoother never touch the radio ──
const fnBody = (sig) => {
  const at = shim.findIndex((l) => l.includes(sig));
  if (at < 0) return null;
  let depth = 0, started = false, out = [];
  for (let i = at; i < shim.length; i++) {
    const l = code(shim[i]);
    for (const ch of l) { if (ch === '{') { depth++; started = true; } else if (ch === '}') depth--; }
    out.push(l);
    if (started && depth <= 0) break;
  }
  return out.join('\n');
};
for (const sig of ['void smoothGainSteps(', 'void dspLoop() {']) {
  const body = fnBody(sig);
  check(body !== null, `${sig} found`);
  if (body) check(!/rtlsdr_set_tuner_gain|queueHwGain\(|setTunerGain\(/.test(body),
                  `${sig}…) does not write the tuner gain`);
}

// ── 3: the client ──
check(/if \(hwSig === lastHwSig\) return;/.test(main), 'web client: hwinfo returns early on an unchanged signature');
const rtlCaps = shim.filter((l) => /\\"radio\\":\{\\"driver\\":\\"rtl\\"/.test(l));
check(rtlCaps.length > 0 && rtlCaps.every((l) => !/gain|agc|comp/i.test(l.replace(/noHwGain/, ''))),
      "the RTL's radio caps carry nothing that moves with the gain (it is in the client's signature)");

console.log(fails ? `\n${fails} FAILED` : '\nall good');
process.exit(fails ? 1 : 0);
