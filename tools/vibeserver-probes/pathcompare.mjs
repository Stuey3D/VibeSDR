// The same radio over three transports: LAN, port-forward, Cloudflare tunnel.
//
// ★★★ THE REPORTED SYMPTOM IS AUDIO DROPOUTS, so audio is the measurement. Every probe before this
//     one watched only the spectrum, which is bursty by nature and forgiving of a late frame — a
//     dropped waterfall row is a cosmetic stutter, a dropped audio packet is a HOLE YOU HEAR.
//     Audio is a constant-rate stream, so its gap distribution is the cleanest jitter measurement
//     on the wire: any gap materially above nominal IS the dropout, no interpretation needed.
//
// ★★ ASK FOR OPUS. The audio socket is refused outright when the owner has not allowed
//    uncompressed audio (`needs_codec`) — which is why an earlier run recorded 0 audio packets and
//    silently reported "no audio" as though the path were quiet. A probe that is refused must SAY
//    it was refused; one that reports zero looks like evidence and is not.
//
// Reports per path: DNS/TCP/TLS timings and the negotiated ALPN (h2/http1.1), then spectrum
// cadence and audio gaps side by side.
//
// Usage: RADIO=<id> node pathcompare.mjs
import crypto from 'node:crypto';
import { execSync } from 'node:child_process';

// ★★★ SAY WHO WE ARE. Node's WebSocket sends a bare `User-Agent: node`, so every probe run
//     against a server lands in its connection log looking like an unidentified client — 32 of
//     them on the public demo in one afternoon, filed under "other", inflating the connection
//     history and the client mix an owner uses to judge real usage (Stuart, 2026-08-11: "what is
//     a node connection?").
// ★★ Which is this repo's own lesson biting again: A PROBE IS PART OF THE SYSTEM IT MEASURES.
//    Last time it was asking for 4096 bins and loading the shared FFT for ten other people.
// ★ `headers` is undici's non-standard extension to the WHATWG WebSocket — verified reaching the
//   server, which is the only thing that matters here.

const RADIO = process.env.RADIO || '';
const FREQ = Number(process.env.FREQ || 198000);
const MODE = process.env.MODE || 'am';
const SECS = Number(process.env.SECS || 30);
const BINS = Number(process.env.BINS || 1024);

// ONLY=tunnel|lan|wan restricts the run — for isolating one variable (a mode, a codec) on a
// single path, where measuring the other two is just time spent not answering the question.
const ONLY = (process.env.ONLY || '').toLowerCase();
// CHANNELS=1 asks for mono. ★ wfm STEREO is the one thing that distinguishes the radio that
// dropped out from the two that did not, so it has to be switchable to be testable.
const CHANNELS = process.env.CHANNELS || '';
const PATHS_ALL = [
  { name: 'LAN direct',        base: 'ws://192.168.86.88:48000',      http: 'http://192.168.86.88:48000' },
  { name: 'port-forward (WAN)', base: 'ws://stuey3d.freemyip.com:48000', http: 'http://stuey3d.freemyip.com:48000' },
  { name: 'Cloudflare tunnel', base: 'wss://demo.vibesdr.net',        http: 'https://demo.vibesdr.net' },
];
const PATHS = PATHS_ALL.filter(p => !ONLY
  || (ONLY === 'tunnel' && p.name.startsWith('Cloud'))
  || (ONLY === 'lan' && p.name.startsWith('LAN'))
  || (ONLY === 'wan' && p.name.startsWith('port')));

const pct = (a, p) => a.length ? a.slice().sort((x, y) => x - y)[Math.min(a.length - 1, Math.floor(a.length * p))] : 0;
const mean = a => a.length ? a.reduce((s, v) => s + v, 0) / a.length : 0;
const sd = a => { const m = mean(a); return Math.sqrt(mean(a.map(v => (v - m) ** 2))); };
const wait = ms => new Promise(r => setTimeout(r, ms));

function handshake(httpBase) {
  // ★ curl, not a hand-rolled socket: it reports the NEGOTIATED ALPN, which is the "transport
  //   type" question. A tunnel that has quietly gone back to h3/QUIC would show up right here —
  //   and QUIC is what made the waterfall clump before it was pinned to http2.
  try {
    const fmt = '%{http_version} %{time_namelookup} %{time_connect} %{time_appconnect} %{time_starttransfer}';
    const out = execSync(`curl -s -o /dev/null -m 15 -w '${fmt}' '${httpBase}/vibeserver/radios'`,
      { encoding: 'utf8' }).trim().split(/\s+/).map((v, i) => i ? Number(v) * 1000 : v);
    const [ver, dns, tcp, tls, ttfb] = out;
    return { ver, dns, tcp: tcp - dns, tls: tls > 0 ? tls - tcp : 0, ttfb: ttfb - (tls > 0 ? tls : tcp) };
  } catch (e) { return { ver: 'unreachable', dns: 0, tcp: 0, tls: 0, ttfb: 0 }; }
}

async function measure(p) {
  const sid = 'pc' + crypto.randomBytes(4).toString('hex');
  const pre = `${p.base}${RADIO ? `/r/${RADIO}` : ''}`;
  const spec = [], audio = [];
  let busy = null, refused = null, t0 = 0;

  // ★★ ROUND-TRIP, because ONE-WAY LATENCY IS NOT MEASURABLE without a shared clock. The SPEC
  //    header's timestamp is the server's clock, so `arrival - serverTs` carries an unknown
  //    constant offset: its VARIATION is meaningful (that is the jitter figure) but its ABSOLUTE
  //    value is not latency and must never be quoted as one. The server answers {type:"ping"}
  //    with a pong on the same socket, through the same proxies and the same queues as the data —
  //    so that round trip IS the path's latency, measured end to end.
  const rtts = []; let pingAt = 0, onPong = null;
  const ws = new WebSocket(`${pre}/ws/user-spectrum?user_session_id=${sid}&bins=${BINS}&mode=binary8`, { headers: { 'User-Agent': 'VibeSDR-probe/1.0 (pathcompare)' } });
  ws.binaryType = 'arraybuffer';
  ws.onmessage = m => {
    const t = performance.now();
    if (typeof m.data === 'string') {
      try { const j = JSON.parse(m.data);
        if (j.type === 'busy') busy = j;
        if (j.type === 'pong' && onPong) { rtts.push(t - pingAt); onPong(); onPong = null; }
      } catch {}
      return;
    }
    const b = new Uint8Array(m.data);
    if (b.length < 22 || b[0] !== 0x53 || b[1] !== 0x50) return;
    const dv = new DataView(m.data);
    if (!t0) t0 = t;
    spec.push({ t: t - t0, ts: Number(dv.getBigUint64(6, true)) / 1e6, bytes: b.length });
  };
  ws.onerror = () => {};
  await new Promise(r => { ws.onopen = r; setTimeout(r, 8000); });
  ws.send(JSON.stringify({ type: 'tune', frequency: FREQ, mode: MODE }));

  // ★ codec=opus, and the SAME session id as the spectrum socket — both are required, and the
  //   second one for a different reason: single-occupancy keys on it, so a mismatched id makes a
  //   client report itself "in use" on its own connection.
  const aws = new WebSocket(`${pre}/ws/audio?user_session_id=${sid}&codec=opus${CHANNELS ? `&channels=${CHANNELS}` : ''}`);
  aws.binaryType = 'arraybuffer';
  aws.onmessage = m => {
    const t = performance.now();
    if (typeof m.data === 'string') {
      try { const j = JSON.parse(m.data); if (j.type === 'needs_codec') refused = 'needs_codec'; } catch {}
      return;
    }
    audio.push({ t, bytes: m.data.byteLength });
  };
  aws.onerror = () => {};

  // ★ One ping in flight at a time, spaced out: a burst of them would queue behind each other and
  //   measure our own pipelining rather than the path.
  const pinger = (async () => {
    const end = performance.now() + SECS * 1000 - 500;
    while (performance.now() < end) {
      await wait(700);
      if (ws.readyState !== 1) break;
      await new Promise(res => {
        onPong = res; pingAt = performance.now();
        ws.send(JSON.stringify({ type: 'ping' }));
        setTimeout(() => { if (onPong) { onPong = null; res(); } }, 3000);   // lost pong
      });
    }
  })();

  await wait(SECS * 1000);
  await pinger.catch(() => {});
  try { ws.close(); aws.close(); } catch {}
  return { spec, audio, busy, refused, rtts };
}

function report(p, hs, r) {
  console.log(`\n━━ ${p.name}  (${p.base})`);
  console.log(`   transport ${hs.ver}   dns ${hs.dns.toFixed(0)}ms  tcp ${hs.tcp.toFixed(0)}ms`
    + `  tls ${hs.tls ? hs.tls.toFixed(0) + 'ms' : '—'}  ttfb ${hs.ttfb.toFixed(0)}ms`);
  if (r.busy) { console.log(`   RADIO BUSY (queue ${r.busy.queuePos}) — no stream to measure`); return; }
  if (r.rtts?.length) {
    const q = r.rtts;
    // ★ MIN is the honest "latency" of the path — the best it can do with nothing in the way.
    //   Everything above it is queueing, and the SPREAD (min→p95) is the jitter that matters.
    console.log(`   LATENCY   rtt min ${Math.min(...q).toFixed(1)}  median ${pct(q, .5).toFixed(1)}`
      + `  p95 ${pct(q, .95).toFixed(1)}  MAX ${Math.max(...q).toFixed(1)} ms   (${q.length} pings)`);
    console.log(`   JITTER    rtt sd ${sd(q).toFixed(1)} ms   queueing above best ${(pct(q, .95) - Math.min(...q)).toFixed(1)} ms at p95`);
  }
  if (!r.spec.length) { console.log('   no spectrum frames'); return; }

  const g = []; for (let i = 1; i < r.spec.length; i++) g.push(r.spec[i].t - r.spec[i - 1].t);
  const dly = r.spec.map(f => f.t - (f.ts - r.spec[0].ts));
  const base = Math.min(...dly), excess = dly.map(d => d - base);
  const kbs = r.spec.reduce((s, f) => s + f.bytes, 0) / (r.spec[r.spec.length - 1].t / 1000) / 1024;
  console.log(`   SPECTRUM  ${r.spec.length} frames, ${kbs.toFixed(1)} KB/s`);
  console.log(`     cadence mean ${mean(g).toFixed(1)}  p95 ${pct(g, .95).toFixed(1)}  MAX ${Math.max(...g).toFixed(1)} ms   (sd ${sd(g).toFixed(1)})`);
  console.log(`     delay above best  p95 ${pct(excess, .95).toFixed(1)}  MAX ${Math.max(...excess).toFixed(1)} ms`);

  if (r.refused) { console.log(`   AUDIO refused: ${r.refused}`); return; }
  if (r.audio.length < 5) { console.log(`   AUDIO ${r.audio.length} packets — not enough to judge`); return; }
  const ag = []; for (let i = 1; i < r.audio.length; i++) ag.push(r.audio[i].t - r.audio[i - 1].t);
  const am = mean(ag);
  const akbs = r.audio.reduce((s, f) => s + f.bytes, 0) / ((r.audio[r.audio.length - 1].t - r.audio[0].t) / 1000) / 1024;
  // ★★ A DROPOUT IS A HOLE, and the threshold has to come from the stream's OWN nominal spacing —
  //    a 60 ms gap is nothing at 20 fps video and a dead silence in a 20 ms audio stream.
  const holes = ag.filter(x => x > am * 3);
  const lost = holes.reduce((s, x) => s + x, 0);
  console.log(`   AUDIO     ${r.audio.length} packets, ${akbs.toFixed(1)} KB/s, nominal ${am.toFixed(1)} ms apart`);
  console.log(`     gaps p95 ${pct(ag, .95).toFixed(1)}  MAX ${Math.max(...ag).toFixed(1)} ms   (sd ${sd(ag).toFixed(1)})`);
  console.log(`     ★ DROPOUTS (>3x nominal): ${holes.length}`
    + (holes.length ? `  worst ${Math.max(...holes).toFixed(0)} ms, ${(lost / 1000).toFixed(2)}s of audio missing` : ''));

  // ★★★ IS IT LATE, OR IS IT GONE? This decides whether a jitter buffer can help at all, and the
  //     answer over a WEBSOCKET is structural: WS runs on TCP, so nothing is ever dropped at the
  //     application layer. A gap means the stream was held up — head-of-line blocking behind a
  //     retransmit — and the packets behind it arrive in a BURST the moment it clears. Confirm it
  //     rather than assume it: count the packets that arrive back-to-back after each hole.
  //     (Contrast the fps bug, where frames were never sent at all. A buffer holds late data; it
  //     cannot invent data that was never produced. Same-looking symptom, opposite fix.)
  let caught = 0, deepest = 0;
  for (let i = 1; i < ag.length; i++) {
    if (ag[i] <= am * 3) continue;
    let burst = 0;
    for (let j = i + 1; j < ag.length && ag[j] < am * 0.5; j++) burst++;
    if (burst > 0) caught++;
    // How much audio arrived in that catch-up = how deep a buffer had to be to cover the hole.
    deepest = Math.max(deepest, ag[i]);
  }
  if (holes.length) {
    console.log(`     recovery: ${caught}/${holes.length} holes were followed by a catch-up burst`
      + `  ->  ${caught >= holes.length / 2 ? 'LATE, NOT LOST — a buffer absorbs this' : 'no catch-up seen'}`);
    // ★ Depth = the worst hole plus a margin, because a buffer that exactly equals the worst hole
    //   drains to empty at that moment and clicks anyway.
    console.log(`     ★ BUFFER DEPTH to have covered this run: ${Math.ceil(deepest * 1.5 / 20) * 20} ms`
      + `  (worst hole ${deepest.toFixed(0)} ms + 50% margin, rounded to a 20 ms packet)`);
  }
}

console.log(`Same radio (${RADIO || 'single'}) at ${FREQ / 1000} kHz ${MODE}, ${SECS}s per path, ${BINS} bins.`);
console.log('★ Sequentially, never in parallel — three streams at once would measure each other.');
for (const p of PATHS) {
  const hs = handshake(p.http);
  const r = await measure(p);
  report(p, hs, r);
}
// ★★ THE CAVEAT THAT DECIDES HOW MUCH THIS IS WORTH.
console.log(`\n★★ NOTE: run from a machine on the SAME LAN as the Pi, "port-forward (WAN)" may hairpin`);
console.log(`   through the router and never leave the building — which is NOT what a remote`);
console.log(`   listener experiences. Compare that row against a run from outside before trusting it.`);
process.exit(0);
