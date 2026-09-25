// ★★★ DO *GAIN* WRITES STARVE THE USB TRANSPORT, UNDER REAL LOAD?
//
// The companion probe (probe-sweep-usb.mjs) answered the FREQUENCY half and the answer was no:
// 543 retunes at the client's 22/s ceiling held delivery at 100.0% of idle. But that run had
// dspCpu=6 — NOBODY WAS LISTENING — so it measured the transport with the DSP nearly asleep,
// and the fault Stuart hears happens with audio running. A negative result from an unloaded box
// is not a negative result. [[test_that_cannot_fail]]
//
// So this probe differs in two ways that matter:
//   1. It opens the AUDIO socket as well, so the server demodulates, encodes Opus and streams —
//      the DSP is doing the work it does when a listener is present.
//   2. It drives `{type:'gain'}`, not `{type:'tune'}`. Stuart's claim is specifically about the
//      gain move: "is it when the VibeAGC makes the gain move the USB port transport is causing
//      the stutter, we had that way back when VibeAGC was being created" (2026-09-25).
//      A gain write is a different I2C conversation with the R820T from a frequency write, and
//      the shim's own note already records that "the cost is PER CONTROL TRANSFER, not per dB".
//
// ★★ THE MEASURE IS THE DELIVERED SAMPLE RATE (usbSamples slope). `iqDrops` counts OVERRUNS and
//    can never show starvation — checking it first was my own wrong instrument on 2026-09-25.
//
// ★ Driven over the network. ssh to the Pi 2 spawns a user systemd + dbus + mpris-proxy whose
//   startup burst alone can cause the stutter being measured.
//
// ★★★ IT PUTS THE RADIO BACK. The gain mode in force is read before anything moves and restored
//     on every exit path, including Ctrl-C — a probe that leaves a receiver on a hand-set gain is
//     a fault reported tomorrow as a bug.
//
// Usage:
//   node scripts/probe-gain-usb.mjs http://192.168.86.129:48001 --freq 96.1M [--secs 25]

import WebSocket from 'ws';

const base = (process.argv[2] || '').replace(/\/+$/, '');
if (!base.startsWith('http')) {
  console.error('usage: node scripts/probe-gain-usb.mjs http://host:port [--freq 96.1M] [--secs 25]');
  process.exit(1);
}
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const parseHz = (s) => {
  const m = String(s).trim().match(/^([\d.]+)\s*([kMG]?)$/i);
  return m ? parseFloat(m[1]) * ({ '': 1, k: 1e3, K: 1e3, m: 1e6, M: 1e6, g: 1e9, G: 1e9 }[m[2]] ?? 1) : NaN;
};
const FREQ = parseHz(arg('--freq', '96.1M'));
const SECS = Number(arg('--secs', 25));
const SID  = 'gainusb-' + Math.random().toString(16).slice(2, 10);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const status = async () => (await fetch(`${base}/vibeserver.json`, { signal: AbortSignal.timeout(4000) })).json();

async function measure(secs) {
  const out = []; let prev = null; const t0 = Date.now();
  while (Date.now() - t0 < secs * 1000) {
    let d; try { d = await status(); } catch { await sleep(1000); continue; }
    const t = Date.now() / 1000, s = d.usbSamples ?? 0;
    if (prev && t - prev[0] > 0.5) out.push((s - prev[1]) / (t - prev[0]));
    prev = [t, s];
    await sleep(1000);
  }
  return out;
}
const stats = (a) => a.length
  ? { mean: a.reduce((x, y) => x + y, 0) / a.length, min: Math.min(...a), n: a.length }
  : { mean: 0, min: 0, n: 0 };

let ws = null, audio = null, restored = false;
const restore = () => {
  if (restored) return; restored = true;
  try { if (ws && ws.readyState === 1) ws.send(JSON.stringify({ type: 'gain', auto: true })); } catch {}
};
process.on('SIGINT', () => { restore(); setTimeout(() => process.exit(130), 300); });

const main = async () => {
  const s0 = await status();
  console.log(`server ${s0.version}  listeners=${s0.listeners}  dspCpu(idle)=${s0.dspCpu}`);
  if (s0.listeners > 0) console.log('★ someone is listening — their dial and gain will move.');

  ws = new WebSocket(`${base.replace(/^http/, 'ws')}/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=1024`);
  const send = (o) => { if (ws.readyState === 1) ws.send(JSON.stringify(o)); };
  await new Promise((res, rej) => { ws.once('open', res); ws.once('error', rej); });
  ws.on('message', () => {});
  send({ type: 'tune', frequency: FREQ, mode: 'wfm' });

  // ★★ THE LOAD. Without this the server never demodulates and the whole test is unloaded —
  //    which is exactly how the frequency probe produced a clean bill of health at dspCpu=6.
  audio = new WebSocket(`${base.replace(/^http/, 'ws')}/ws/audio?user_session_id=${SID}&codec=opus`);
  let audioFrames = 0;
  audio.on('message', () => { audioFrames++; });
  await new Promise((res) => { audio.once('open', res); audio.once('error', res); });
  await sleep(6000);

  const sL = await status();
  console.log(`loaded: dspCpu=${sL.dspCpu}  audio frames in 6s: ${audioFrames}`);
  if (audioFrames === 0) console.log('★★ WARNING: no audio frames — the load is NOT on. Treat any pass as meaningless.');

  console.log(`\nbaseline: audio running, no gain writes, ${SECS}s ...`);
  const b = stats(await measure(SECS));
  console.log(`  idle    mean ${b.mean.toFixed(0)}/s  min ${b.min.toFixed(0)}/s`);

  // Two widely separated gains, so every write is a REAL register change rather than a no-op the
  // driver can elide. 12 dB and 36 dB both exist on an R820T's table (nearest is chosen).
  const GAINS = [120, 360];
  const rows = [];
  for (const r of [2, 5, 11, 22]) {
    let n = 0, i = 0;
    const timer = setInterval(() => { send({ type: 'gain', value: GAINS[i++ % 2] }); n++; }, 1000 / r);
    const st = stats(await measure(SECS));
    clearInterval(timer);
    const pct = b.mean ? 100 * st.mean / b.mean : 0;
    const pMin = b.mean ? 100 * st.min / b.mean : 0;
    rows.push({ r, n, pct, pMin });
    console.log(`  ${String(r).padStart(2)}/s   mean ${st.mean.toFixed(0)}/s (${pct.toFixed(1)}%)`
              + `  min ${st.min.toFixed(0)}/s (${pMin.toFixed(1)}%)  gain writes ${n}`);
    send({ type: 'gain', auto: true });
    await sleep(4000);
  }

  console.log('\n★ VERDICT');
  const worst = rows.reduce((a, c) => (c.pct < a.pct ? c : a), rows[0]);
  const worstMin = rows.reduce((a, c) => (c.pMin < a.pMin ? c : a), rows[0]);
  if (worst.pct < 97 || worstMin.pMin < 90) {
    console.log(`  Delivery FALLS with gain-write rate — mean ${worst.pct.toFixed(1)}% at ${worst.r}/s,`
              + ` worst single second ${worstMin.pMin.toFixed(1)}% at ${worstMin.r}/s.`);
    console.log('  The gain write IS disturbing the bulk stream. Stuart was right, and the lever');
    console.log('  is the RATE of AGC corrections, not their size.');
  } else {
    console.log('  Delivery holds under gain writes at every rate, WITH audio running.');
    console.log('  The USB transport is not the stutter. Suspect CPU contention instead —');
    console.log('  note what dspCpu did above, and that a Pi 2 ssh login alone can cause it.');
  }

  restore();
  await sleep(600);
  const s1 = await status();
  console.log(`\nleft at: dspCpu=${s1.dspCpu}  iqDrops=${s1.iqDrops} (was ${s0.iqDrops})  gain restored to auto`);
  ws.close(); audio.close();
};

main().then(() => process.exit(0)).catch((e) => { restore(); console.error('probe failed:', e.message); process.exit(1); });
