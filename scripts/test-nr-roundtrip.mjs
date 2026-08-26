// test-nr-roundtrip.mjs — the NR slider's two conversions must be exact inverses.
//
// ★★★ WHY THIS EXISTS. The server echoes the strength it is using, onDspState renders it back
//     onto the slider AND dispatches an `input` event — which sends it again. That round trip is a
//     FEEDBACK LOOP, so a disagreement between the two directions is not an off-by-a-bit: it is a
//     RATCHET. On 2026-08-26 the forward map became `pct/80` while the reverse stayed `*100`, and
//     every echo multiplied the setting by 1.25 until it stuck at 100% on air:
//         30 -> 0.375 -> 38 -> 0.475 -> 48 -> 59 -> 73 -> 92 -> 100
//     The sweep that found four copies of the NR ceiling that morning missed this, because it
//     searched for the CLAMP being fixed and the reverse map contains no clamp to find.
// ★★ It also pins the RANGE. `strength > 1` is where audio_nr.cpp's over-subtraction begins; a
//    forward map that stops at 1.0 silently switches the aggressive half of the DSP off.
import assert from 'node:assert';
import fs from 'node:fs';

// ★★ READ THE REAL SOURCE, do not re-type the maths here. A test carrying its own copy of the
//    curve would agree with itself for ever while the shipped one drifted — the same trap as a
//    synthetic fixture that agrees with the bug. Bundling main.ts is not an option (it pulls in
//    the whole React Native app), so lift the two functions out of it verbatim and run those.
const src = fs.readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
function lift(name) {
  const i = src.indexOf(`function ${name}(`);
  assert.ok(i >= 0, `${name}() not found in main.ts — has it been renamed?`);
  let depth = 0, j = src.indexOf('{', i);
  for (let k = j; k < src.length; k++) {
    if (src[k] === '{') depth++;
    else if (src[k] === '}' && --depth === 0) { j = k; break; }
  }
  return src.slice(i, j + 1).replace(/: number/g, '');   // strip the TS annotations
}
const { nrStrength, nrPercent } = await import(
  'data:text/javascript;base64,' + Buffer.from(
    `${lift('nrStrength')}\n${lift('nrPercent')}\nexport { nrStrength, nrPercent };`
  ).toString('base64'));

let pass = 0;
const ok = (m) => { console.log('  ✓ ' + m); pass++; };

// 1. Exact round trip across the whole travel — this is the property that stops the ratchet.
for (let p = 0; p <= 100; p++) {
  const back = nrPercent(nrStrength(p));
  assert.ok(Math.abs(back - p) < 1e-9,
    `round trip drifted at ${p}%: -> ${nrStrength(p)} -> ${back}%`);
}
ok('nrPercent(nrStrength(p)) === p for every whole percent 0..100');

// 2. A settled value must not move when the server echoes it back, repeatedly.
for (const p of [1, 30, 50, 75, 100]) {
  let v = p;
  for (let i = 0; i < 20; i++) v = Math.max(1, Math.round(nrPercent(nrStrength(v))));
  assert.strictEqual(v, Math.max(1, p), `${p}% walked to ${v}% after 20 echoes`);
}
ok('20 consecutive echoes leave the setting where it was put');

// 3. The top of the slider must REACH over-subtraction, or the aggressive half is unreachable.
assert.ok(nrStrength(100) > 1.0, '100% must exceed strength 1.0 (over-subtraction)');
assert.ok(nrStrength(100) <= 1.4, '100% must not exceed AudioNR::setStrength clamp of 1.4');
assert.strictEqual(nrStrength(0), 0, '0% is off');
ok(`100% reaches strength ${nrStrength(100)} — past 1.0, within the engine's 1.4 clamp`);

console.log(`\nthe NR round trip holds (${pass} checks)`);
