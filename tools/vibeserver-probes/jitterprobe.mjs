// Why the waterfall STICKS: is it edge clumping, or stale frames after a retune?
//
// Two symptoms, and they are NOT the same bug:
//   (a) sitting still, "smooth then sticks then smooth" — frames arriving in clumps. The mean
//       cadence stays right, so nothing is lost; the eye reads the STALL, not the burst.
//   (b) sticking on tune/zoom — frames for the OLD centre still in flight, drawn as if they were
//       the new one, until the real ones arrive.
// A pacing buffer fixes (a) and makes (b) WORSE, so we must know which is which before building.
//
// ★★★ THE SPEC HEADER MAKES THIS MEASURABLE WITHOUT CLOCK SYNC. 22 bytes, magic "SPEC":
//     [6..13] server timestamp u64 LE ns, [14..21] frequency u64 LE Hz.
//     arrival - serverTs has an unknown but CONSTANT offset, so its VARIATION is the one-way
//     delay variation exactly. Subtract the run minimum and you have queue delay above best-case:
//     a flat line is a clean pipe, a sawtooth IS clumping. And the frequency field dates each
//     frame to a tune epoch, so "stale frames after a retune" is counted, not guessed.
//
// Usage:  node jitterprobe.mjs wss://demo.vibesdr.net        (through the tunnel)
//         node jitterprobe.mjs ws://host:48000               (direct — the control)
// Env:    SECS=12  TUNE_A=198000 TUNE_B=909000 MODE=am
import crypto from 'node:crypto';

const BASE = process.argv[2] || 'wss://demo.vibesdr.net';
const SECS = Number(process.env.SECS || 12);
const A = Number(process.env.TUNE_A || 198000);
const B = Number(process.env.TUNE_B || 909000);
const MODE = process.env.MODE || 'am';
const SID = 'jit' + crypto.randomBytes(4).toString('hex');

// ★★ Behind the multi-radio front door the RADIO IS A PATH PREFIX — `/r/<id>/ws/user-spectrum`.
//    Note this is NOT what BRIEF-vibeserver-multiradio-gui.md proposed (a `?radio=` query param
//    or a cookie); the shipped door routes on the prefix and the radio strips it
//    (local_sdr_shim.cpp ~5560). Get it wrong and you get a 503 front-door page, which as a
//    WebSocket handshake surfaces as a bare 1006 with no clue in it. Ask /vibeserver/radios for
//    the ids. Omit RADIO for a single-radio server.
const RADIO = process.env.RADIO || '';
// ★★ MEASURE THE SESSION THE USER ACTUALLY HAS. Two defaults made every earlier run LIGHTER than
//    a real listener, and both change the thing being measured:
//    - BINS: a browser on a wide screen asks for 4096, not 1024. Bytes per frame scale with this,
//      so a probe at 1024 is a quarter of the traffic and sees a quarter of the burst.
//    - AUDIO: a real session ALWAYS has an audio socket alongside, and on wfm it is the heaviest
//      audio the server sends. Spectrum measured on an otherwise idle link is not the link the
//      listener has.
const BINS = Number(process.env.BINS || 1024);
const WANT_AUDIO = process.env.AUDIO === '1';
const url = `${BASE}${RADIO ? `/r/${RADIO}` : ''}`
          + `/ws/user-spectrum?user_session_id=${SID}&bins=${BINS}&mode=binary8`;

const frames = [];   // {t, ts, freq}
const marks = [];    // {t, what}
let t0 = 0;

const ws = new WebSocket(url);
ws.binaryType = 'arraybuffer';

ws.onerror = e => { console.log('  ws error:', e.message || e.type); };
ws.onclose = e => { if (!done) { console.log(`  closed early: ${e.code} ${e.reason}`); report(); } };

ws.onmessage = m => {
  const t = performance.now();
  if (typeof m.data === 'string') return;               // config/status text
  const b = new Uint8Array(m.data);
  // ★ Assert the magic. Reading a header as data is how zoomaxis.mjs manufactured phantom peaks;
  //   here a mis-parse would invent a timestamp and quietly poison every number below.
  if (b.length < 22 || b[0] !== 0x53 || b[1] !== 0x50 || b[2] !== 0x45 || b[3] !== 0x43) return;
  const dv = new DataView(m.data);
  const ts = Number(dv.getBigUint64(6, true)) / 1e6;    // ns -> ms
  const freq = Number(dv.getBigUint64(14, true));
  if (!t0) t0 = t;
  frames.push({ t: t - t0, ts, freq, bytes: b.length });
};

// ★ The audio socket carries the SAME session id. Both sockets must, or single-occupancy refuses
//   the second one as "in use" — the trap that made the phone report itself busy on its own
//   connection. It is opened but not decoded: the point is that the bytes are on the link.
let audioBytes = 0, audioFrames = 0;
if (WANT_AUDIO) {
  const aurl = `${BASE}${RADIO ? `/r/${RADIO}` : ''}/ws/audio?user_session_id=${SID}`;
  const aws = new WebSocket(aurl);
  aws.binaryType = 'arraybuffer';
  aws.onmessage = m => { if (typeof m.data !== 'string') { audioBytes += m.data.byteLength; audioFrames++; } };
  aws.onerror = () => console.log('  (audio socket error)');
}

const send = o => ws.send(JSON.stringify(o));
const mark = what => marks.push({ t: performance.now() - t0, what });

let done = false;
ws.onopen = async () => {
  const wait = ms => new Promise(r => setTimeout(r, ms));
  await wait(600);
  send({ type: 'tune', frequency: A, mode: MODE });
  await wait(SECS * 1000);                              // (a) SIT STILL
  mark(`tune->${B}`); send({ type: 'tune', frequency: B, mode: MODE });
  await wait(4000);                                     // (b) after a RETUNE
  mark('zoom'); send({ type: 'zoom', frequency: B, binBandwidth: 100 });
  await wait(4000);                                     // (b) after a ZOOM
  done = true; ws.close(); report();
};

// ── numbers ───────────────────────────────────────────────────────────────────
const pct = (a, p) => a.length ? a.slice().sort((x, y) => x - y)[Math.min(a.length - 1, Math.floor(a.length * p))] : 0;
const mean = a => a.reduce((s, v) => s + v, 0) / (a.length || 1);

function report() {
  if (done && report.ran) return; report.ran = true;
  console.log(`\n=== ${BASE} — ${frames.length} frames ===`);
  if (frames.length < 20) { console.log('  too few frames to judge'); process.exit(1); }

  const settle = marks.length ? marks[0].t : frames[frames.length - 1].t;
  const sit = frames.filter(f => f.t < settle);

  // (a) SITTING STILL — cadence and delay variation
  const gaps = [];
  for (let i = 1; i < sit.length; i++) gaps.push(sit[i].t - sit[i - 1].t);
  const m = mean(gaps);
  const sd = Math.sqrt(mean(gaps.map(g => (g - m) ** 2)));
  console.log(`\n  SITTING STILL (${sit.length} frames over ${(settle / 1000).toFixed(1)}s)`);
  // ★ Bytes, because a burst is a quantity of DATA arriving at once, not a count of frames. Two
  //   radios at the same fps are not the same load if one paints four times the spectrum.
  const sb = sit.reduce((s, f) => s + f.bytes, 0) / (settle / 1000);
  console.log(`    payload   ${BINS} bins, ${(sit[0]?.bytes || 0)} B/frame`
    + `   spectrum ${(sb / 1024).toFixed(1)} KB/s`
    + (WANT_AUDIO ? `   audio ${(audioBytes / (settle / 1000) / 1024).toFixed(1)} KB/s (${audioFrames} pkts)` : '   (no audio socket)'));
  console.log(`    cadence   mean ${m.toFixed(1)} ms   p50 ${pct(gaps, .5).toFixed(1)}   p95 ${pct(gaps, .95).toFixed(1)}   MAX ${Math.max(...gaps).toFixed(1)} ms`);
  console.log(`    jitter    sd ${sd.toFixed(1)} ms`);

  // ★ Delay variation above best-case. This is the clumping fingerprint: if the pipe were clean
  //   it would sit near 0 and never climb, because every frame would take the same time.
  const dly = sit.map(f => f.t - (f.ts - sit[0].ts));
  const base = Math.min(...dly);
  const excess = dly.map(d => d - base);
  console.log(`    delay above best   p50 ${pct(excess, .5).toFixed(1)}   p95 ${pct(excess, .95).toFixed(1)}   MAX ${Math.max(...excess).toFixed(1)} ms`);

  // Stalls, and whether a burst FOLLOWS them — the signature of clumping rather than loss.
  const stalls = [];
  for (let i = 1; i < gaps.length; i++) if (gaps[i] > 2 * m) {
    let burst = 0; for (let j = i + 1; j < gaps.length && gaps[j] < m * 0.5; j++) burst++;
    stalls.push({ at: sit[i].t, gap: gaps[i], burst });
  }
  console.log(`    stalls >2x mean: ${stalls.length}` + (stalls.length ? ` (worst ${Math.max(...stalls.map(s => s.gap)).toFixed(0)} ms)` : ''));
  for (const s of stalls.slice(0, 8))
    console.log(`      t=${(s.at / 1000).toFixed(1)}s  gap ${s.gap.toFixed(0)} ms  then ${s.burst} frame(s) back-to-back`);
  const caught = stalls.filter(s => s.burst > 0).length;
  console.log(`    ${caught}/${stalls.length} stalls were followed by a catch-up burst`
    + `  ->  ${caught > stalls.length / 2 ? 'CLUMPING (a buffer absorbs this)' : 'frames genuinely LATE or LOST (a buffer will not help)'}`);

  // ★★ FRAME RATE PER SECOND. A pacing buffer assumes the frames EXIST and merely arrive
  //    unevenly. If the rate itself steps down mid-run, no buffer can invent the missing ones —
  //    it would drain and stall exactly as before, one buffer-depth later. So print the timeline
  //    before reaching for a fix: a flat line means jitter, a STEP means the server stopped
  //    sending, and those two want opposite work.
  const secs = Math.ceil(frames[frames.length - 1].t / 1000);
  const hist = new Array(secs).fill(0);
  for (const f of frames) hist[Math.min(secs - 1, Math.floor(f.t / 1000))]++;
  console.log(`\n    frames/sec: ${hist.join(' ')}`);
  for (const mk of marks) console.log(`      ^ ${mk.what} at t=${(mk.t / 1000).toFixed(1)}s`);

  // (b) AFTER A RETUNE — stale frames
  for (const mk of marks) {
    const after = frames.filter(f => f.t >= mk.t);
    if (!after.length) { console.log(`\n  ${mk.what}: no frames after`); continue; }
    const before = frames.filter(f => f.t < mk.t).pop();
    const oldF = before ? before.freq : 0;
    const firstNew = after.find(f => f.freq !== oldF);
    const stale = firstNew ? after.filter(f => f.t < firstNew.t).length : after.length;
    console.log(`\n  ${mk.what} (old centre ${(oldF / 1000).toFixed(0)} kHz)`);
    if (!firstNew) {
      console.log(`    frequency in the header NEVER changed — this event does not re-stamp frames,`);
      console.log(`    so a stale frame is INDISTINGUISHABLE from a fresh one at the client. ${after.length} frames seen.`);
    } else {
      console.log(`    first frame at the new centre: +${(firstNew.t - mk.t).toFixed(0)} ms`);
      console.log(`    stale frames drawn in the meantime: ${stale}` +
        (stale ? `  (~${(stale * m).toFixed(0)} ms of wrong-centre waterfall)` : ''));
    }
    const g2 = []; for (let i = 1; i < after.length; i++) g2.push(after[i].t - after[i - 1].t);
    if (g2.length) console.log(`    cadence after: mean ${mean(g2).toFixed(1)} ms  MAX ${Math.max(...g2).toFixed(1)} ms`);
  }
  process.exit(0);
}
