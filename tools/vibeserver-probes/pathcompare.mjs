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

const RADIO = process.env.RADIO || '';
const FREQ = Number(process.env.FREQ || 198000);
const MODE = process.env.MODE || 'am';
const SECS = Number(process.env.SECS || 30);
const BINS = Number(process.env.BINS || 1024);

const PATHS = [
  { name: 'LAN direct',        base: 'ws://192.168.86.88:48000',      http: 'http://192.168.86.88:48000' },
  { name: 'port-forward (WAN)', base: 'ws://stuey3d.freemyip.com:48000', http: 'http://stuey3d.freemyip.com:48000' },
  { name: 'Cloudflare tunnel', base: 'wss://demo.vibesdr.net',        http: 'https://demo.vibesdr.net' },
];

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
  const ws = new WebSocket(`${pre}/ws/user-spectrum?user_session_id=${sid}&bins=${BINS}&mode=binary8`);
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
  const aws = new WebSocket(`${pre}/ws/audio?user_session_id=${sid}&codec=opus`);
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
