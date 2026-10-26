#!/usr/bin/env node
// test-server-dab-scan-agc.mjs — THE DAB BAND SCAN AND THE GAIN (V12, 2026-10-27). Stuart: "the thing we gotta make sure with
// the DAB scan is that the AGC can work quick enough to allow the multiplex to be found but without taking forever to do a
// full scan either." Real server, the RTL front-end model (fake-rtl-tcp --dab-mux: the gain the server writes is the gain
// the samples get, with aerial noise, the tuner's own noise, 8-bit quantisation and the rails), no browser, silent.
//   1. a strong block then a weak one (direct tune): the weak one locks in seconds, not the 20 s the climb used to be
//      stuck for ("recovering … 15.2 dB in one move", refused every tick by the −15 dBFS gate it was not sized for);
//   2. a typical aerial — two strong locals, a medium, two weak (one RIGHT AFTER a strong one) — every ensemble found,
//      empty blocks left in ~2 s, the whole 41-block scan in under 2.5 minutes;
//   3. a weak ensemble at +3 dB on its own: found; and the radio gets back the gain it had before the scan;
//   4. per level from the noise gain (strong +45, medium +15, weak +4): each locks inside 2 s.
// Needs VIBESERVER_BIN and vibeserver/bench-clip/mux.cf32 (make-clip.sh) — exit 3 (NOT RUN) without either. ~9 min.
import fs from 'node:fs';
import path from 'node:path';
import { runProbe } from './probe-dab-scan-agc.mjs';

const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
if (!process.env.VIBESERVER_BIN || !fs.existsSync(process.env.VIBESERVER_BIN)
    || !fs.existsSync(process.env.DAB_MUX || path.join(ROOT, 'vibeserver/bench-clip/mux.cf32'))) {
  console.log('   (not run — set VIBESERVER_BIN and build vibeserver/bench-clip/mux.cf32)'); process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, w) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${w}`); c ? pass++ : fail++; };
const HZ = { '8A': 195936000, '10D': 215072000, '11B': 218640000, '11C': 220352000, '12B': 225648000 };

try {
  console.log('1. a strong block, then a weak one');
  const d = await runProbe({ ens: `${HZ['12B']}:45,${HZ['11C']}:5`, start: '12B', direct: '11C', quiet: true });
  ok(d.startGain !== null && d.startGain <= 5, `the strong block holds the gain low (${d.startGain} dB)`);
  ok(d.lockAt > 0 && d.lockAt < 4 && d.listAt > 0 && d.listAt < 10, `the weak block locks in ${d.lockAt} s and lists its stations by ${d.listAt} s (was 21 / 23 s)`);

  console.log('2. a typical aerial: 11B +45 and 12B +40 local, 10D +15, 8A and 11C +4 (11C right after 11B)');
  const s = await runProbe({ ens: `${HZ['11B']}:45,${HZ['12B']}:40,${HZ['10D']}:15,${HZ['11C']}:4,${HZ['8A']}:4`, start: '12B', quiet: true });
  const found = s.found.map((f) => f.split(':')[0]).sort().join(',');
  ok(found === '10D,11B,11C,12B,8A', `every ensemble found (${s.found.join(' ')})`);
  ok(s.found.every((f) => Number(f.split(':')[1]) === 12), 'each with all 12 stations');
  const empty = s.blocks.filter((b) => b.what === 'nothing');
  ok(empty.length === 36 && Math.max(...empty.map((b) => b.secs)) <= 2.6, `empty blocks are left in ≤ 2.6 s (${empty.length}, longest ${Math.max(...empty.map((b) => b.secs)).toFixed(1)} s)`);
  const c11 = s.blocks.find((b) => b.blk === '11C');
  ok(c11 && c11.lock >= 0 && c11.lock < 1.5, `11C, straight after the strong 11B, locks in ${c11?.lock} s`);
  ok(s.total > 0 && s.total <= 150, `the whole 41-block scan takes ${s.total} s (≤ 150)`);

  console.log('3. one weak ensemble at +3 dB; the gain afterwards');
  const w = await runProbe({ ens: `${HZ['12B']}:3`, start: '5A', quiet: true });
  ok(w.found.length === 1 && w.found[0].startsWith('12B:') && Number(w.found[0].split(':')[1]) >= 8, `found 12B (${w.found.join(' ') || 'nothing'})`);
  ok(w.startGain !== null && w.gainAfter === w.startGain, `the radio is back at the gain it had before the scan (${w.startGain} → ${w.gainAfter} dB)`);
  ok(w.total <= 120, `scan ${w.total} s`);

  console.log('4. per level, from the noise gain (cold, direct tune)');
  for (const [name, snr] of [['strong', 45], ['medium', 15], ['weak', 4]]) {
    const r = await runProbe({ ens: `${HZ['11C']}:${snr}`, start: '5A', direct: '11C', quiet: true });
    ok(r.lockAt > 0 && r.lockAt < 2 && r.listAt > 0 && r.listAt < 4, `${name} (+${snr} dB): lock ${r.lockAt} s, stations listed ${r.listAt} s`);
  }

  // ★ 5. THE IMPAIRED CLIP (bench-clip/weak-6.vbu8: a 20 µs echo, three 30 ms fades, a 1.8 kHz offset, its own noise at
  //    +6 dB) as every ensemble: a strong local at +45 — which, clipped and impaired, takes ~2 s to lock, and is caught
  //    only because the AGC's cut extends the window — then two weaker ones. Skipped (said so) without the clip.
  const W6 = path.join(ROOT, 'vibeserver/bench-clip/weak-6.vbu8');
  if (fs.existsSync(W6)) {
    console.log('5. the impaired clip (multipath, fades, offset): 11B +45, 11C +12, 12B +8');
    const raw = fs.readFileSync(W6).subarray(28), f = new Float32Array(raw.length);
    for (let i = 0; i < raw.length; i++) f[i] = raw[i] - 127.5;
    const tmp = path.join(fs.mkdtempSync(path.join((await import('node:os')).tmpdir(), 'vs-dabagc-w6-')), 'weak6.cf32');
    fs.writeFileSync(tmp, Buffer.from(f.buffer));
    process.env.DAB_MUX = tmp; process.env.DAB_MUX_FROM = '0';
    const m = await runProbe({ ens: `${HZ['11B']}:45,${HZ['11C']}:12,${HZ['12B']}:8`, start: '5A', quiet: true });
    delete process.env.DAB_MUX; delete process.env.DAB_MUX_FROM;
    fs.rmSync(path.dirname(tmp), { recursive: true, force: true });
    ok(m.found.map((x) => x.split(':')[0]).sort().join(',') === '11B,11C,12B', `all three found (${m.found.join(' ')}) in ${m.total} s`);
  } else console.log('5. (impaired clip not here — make-clip.sh — skipped)');
} catch (e) { console.error(e); fail++; }
console.log(`test-server-dab-scan-agc: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
