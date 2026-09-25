// ★★★ DOES SWEEP TUNING STARVE THE USB TRANSPORT?
//
// Stuart, 2026-09-25: "is it when the VibeAGC makes the gain move the USB port transport is
// causing the stutter, we had that way back when VibeAGC was being created" — and then
// "managed to break the dial with swep tuning just then".
//
// THE CLAIM UNDER TEST. Every retune on an RTL is several USB CONTROL transfers (I2C writes to
// the R820T through the RTL2832's bridge). The web client's hold-to-sweep ramps to 22 steps/s and
// paces itself on `rtt` — which is the PING round trip, and is floored at 60 ms, so on a LAN
// (~20 ms) no pacing happens at all. On a Pi 2 the bulk IQ stream, those control transfers AND
// the Ethernet all share one DWC OTG controller.
//
// ★★ SO THE MEASUREMENT IS THE DELIVERED SAMPLE RATE, not a drop counter. `iqDrops` counts
//    OVERRUNS (queue full); a transport stall is the opposite — STARVATION — and would never
//    appear there. usbSamples is a monotonic count of samples the dongle actually delivered, so
//    its slope IS the transport's health. A stall shows as a slope below nominal.
//
// ★ DRIVEN OVER THE NETWORK, NEVER OVER ssh. Each ssh login to the Pi 2 spawns a user systemd +
//   dbus-daemon + mpris-proxy, which on 900 MHz silicon costs enough CPU to cause the very
//   stutter being measured (observed 2026-09-25, and once before). The instrument must not be the
//   fault.
//
// Usage:
//   node scripts/probe-sweep-usb.mjs http://192.168.86.129:48001 --freq 96.1M [--secs 30]
//
// Prints, per tune-rate step, the mean/min delivered sample rate as a percentage of the rate
// observed while IDLE. A fall that tracks the tune rate is the finding.

import WebSocket from 'ws';

const base = (process.argv[2] || '').replace(/\/+$/, '');
if (!base.startsWith('http')) {
  console.error('usage: node scripts/probe-sweep-usb.mjs http://host:port [--freq 96.1M] [--secs 30]');
  process.exit(1);
}
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const parseHz = (s) => {
  const m = String(s).trim().match(/^([\d.]+)\s*([kMG]?)$/i);
  if (!m) return NaN;
  return parseFloat(m[1]) * ({ '': 1, k: 1e3, K: 1e3, m: 1e6, M: 1e6, g: 1e9, G: 1e9 }[m[2]] ?? 1);
};
const FREQ = parseHz(arg('--freq', '96.1M'));
const SECS = Number(arg('--secs', 30));
const SID  = 'sweepusb-' + Math.random().toString(16).slice(2, 10);

// ★ The step the sweep actually uses is the user's tuning step; 1 kHz is the common one and is
//   what makes each tick a REAL retune rather than a no-op the server can short-circuit.
const STEP_HZ = 1000;

const status = async () => {
  const r = await fetch(`${base}/vibeserver.json`, { signal: AbortSignal.timeout(4000) });
  return r.json();
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** Sample usbSamples for `secs`, returning samples/s figures. */
async function measure(secs) {
  const out = [];
  let prev = null;
  const t0 = Date.now();
  while (Date.now() - t0 < secs * 1000) {
    let d;
    try { d = await status(); } catch { await sleep(1000); continue; }
    const t = Date.now() / 1000, s = d.usbSamples ?? 0;
    if (prev) {
      const dt = t - prev[0];
      if (dt > 0.5) out.push((s - prev[1]) / dt);
    }
    prev = [t, s];
    await sleep(1000);
  }
  return out;
}

const stats = (a) => {
  if (!a.length) return { mean: 0, min: 0, n: 0 };
  const mean = a.reduce((x, y) => x + y, 0) / a.length;
  return { mean, min: Math.min(...a), n: a.length };
};

const main = async () => {
  const s0 = await status();
  console.log(`server ${s0.version}  listeners=${s0.listeners}  dspCpu=${s0.dspCpu}`);
  if (s0.listeners > 0) console.log('★ NOTE: someone is listening — the dial will move under them.');

  const ws = new WebSocket(`${base.replace(/^http/, 'ws')}/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=1024`);
  const send = (o) => { if (ws.readyState === 1) ws.send(JSON.stringify(o)); };
  await new Promise((res, rej) => { ws.once('open', res); ws.once('error', rej); });
  ws.on('message', () => {});                      // drain; we only care about the counter
  send({ type: 'tune', frequency: FREQ, mode: 'wfm' });
  await sleep(3000);

  // ★ IDLE FIRST, and it is the reference every other row is judged against — an absolute
  //   samples/s figure means nothing without knowing what this box does when undisturbed.
  console.log(`\nbaseline: no tuning, ${SECS}s ...`);
  const base0 = stats(await measure(SECS));
  console.log(`  idle   mean ${base0.mean.toFixed(0)}/s  min ${base0.min.toFixed(0)}/s  (n=${base0.n})`);

  const rates = [2, 5, 11, 22];                    // 22 = the client's own ceiling
  const rows = [];
  for (const r of rates) {
    let f = FREQ, dir = 1, n = 0;
    const gap = 1000 / r;
    const timer = setInterval(() => {
      // Walk up and down so we stay in the same part of the band rather than sweeping away.
      f += dir * STEP_HZ; n++;
      if (Math.abs(f - FREQ) > 200e3) dir = -dir;
      send({ type: 'tune', frequency: Math.round(f), mode: 'wfm' });
    }, gap);
    const st = stats(await measure(SECS));
    clearInterval(timer);
    const pct = base0.mean ? (100 * st.mean / base0.mean) : 0;
    const pctMin = base0.mean ? (100 * st.min / base0.mean) : 0;
    rows.push({ r, n, ...st, pct, pctMin });
    console.log(`  ${String(r).padStart(2)}/s  mean ${st.mean.toFixed(0)}/s (${pct.toFixed(1)}% of idle)`
              + `  min ${st.min.toFixed(0)}/s (${pctMin.toFixed(1)}%)  tunes sent ${n}`);
    // ★ Let it settle between steps, or the next row measures the previous row's recovery.
    send({ type: 'tune', frequency: FREQ, mode: 'wfm' });
    await sleep(4000);
  }

  console.log('\n★ VERDICT');
  const worst = rows.reduce((a, b) => (b.pct < a.pct ? b : a), rows[0]);
  if (worst.pct < 97) {
    console.log(`  Delivery FALLS with tune rate — worst ${worst.pct.toFixed(1)}% of idle at ${worst.r}/s.`);
    console.log('  The control transfers ARE disturbing the bulk stream: Stuart was right.');
  } else {
    console.log('  Delivery holds within 3% of idle at every rate up to the client ceiling.');
    console.log('  The transport is NOT starved by tune rate alone — look elsewhere (CPU, or the');
    console.log('  gain write specifically rather than the frequency write).');
  }
  send({ type: 'tune', frequency: FREQ, mode: 'wfm' });
  await sleep(500);
  ws.close();
  const s1 = await status();
  console.log(`\nleft at: dspCpu=${s1.dspCpu} iqDrops=${s1.iqDrops} (was ${s0.iqDrops})`);
};

main().catch((e) => { console.error('probe failed:', e.message); process.exit(1); });
