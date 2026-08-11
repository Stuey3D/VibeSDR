// Does ONE client's fftRate change everyone else's frame rate?
//
// ★★★ THE HYPOTHESIS. `type:"fftRate"` lands on LocalSdrShim::setFftRate() — a PROCESS-GLOBAL
//     engine rate (local_sdr_shim.cpp ~5165 -> ~10488). Forty lines below that handler, `wantBins`
//     carries the warning this one lacks: "RECORD IT AGAINST THIS CLIENT, never in a global.
//     Storing it globally is what let a watch asking for 128 cut every browser on the server down
//     to 128 bins." If fps has the same shape, then one listener's idle-saver drops the waterfall
//     for EVERY listener on that radio — which is what "smooth, then sticks, then smooth with
//     nobody touching anything" looks like from the other side.
//
// A is a bystander and NEVER sends fftRate. B joins, asks for 5, leaves. If A's rate moves, the
// rate is not A's to lose. Watching A across B's whole life also separates a leak from a
// coincidence: the step must line up with B, and it must NOT come back when B goes.
//
// Usage: RADIO=<id> node fpsleak.mjs wss://demo.vibesdr.net
import crypto from 'node:crypto';

// ★★★ SAY WHO WE ARE. Node's WebSocket sends a bare `User-Agent: node`, so every probe run
//     against a server lands in its connection log looking like an unidentified client — 32 of
//     them on the public demo in one afternoon, filed under "other", inflating the connection
//     history and the client mix an owner uses to judge real usage (Stuart, 2026-08-11: "what is
//     a node connection?").
// ★★ Which is this repo's own lesson biting again: A PROBE IS PART OF THE SYSTEM IT MEASURES.
//    Last time it was asking for 4096 bins and loading the shared FFT for ten other people.
// ★ `headers` is undici's non-standard extension to the WHATWG WebSocket — verified reaching the
//   server, which is the only thing that matters here.
const BASE = process.argv[2] || 'wss://demo.vibesdr.net';
const RADIO = process.env.RADIO || '';
const ASK = Number(process.env.ASK || 5);
const FREQ = Number(process.env.FREQ || 198000), MODE = process.env.MODE || 'am';
const pre = `${BASE}${RADIO ? `/r/${RADIO}` : ''}/ws/user-spectrum`;

const isSpec = b => b.length >= 22 && b[0] === 0x53 && b[1] === 0x50 && b[2] === 0x45 && b[3] === 0x43;
const open = tag => {
  const sid = tag + crypto.randomBytes(4).toString('hex');
  const ws = new WebSocket(`${pre}?user_session_id=${sid}&bins=1024&mode=binary8`, { headers: { 'User-Agent': 'VibeSDR-probe/1.0 (fpsleak)' } });
  ws.binaryType = 'arraybuffer';
  ws.hits = [];
  ws.onmessage = m => { if (typeof m.data !== 'string' && isSpec(new Uint8Array(m.data))) ws.hits.push(performance.now()); };
  // ★ A spectrum socket that never tunes gets NO frames — it sat silent at 0 fps and the leak test
  //   "passed" by measuring nothing at all. Tune on open, both clients, same frequency.
  ws.onopen = () => ws.send(JSON.stringify({ type: 'tune', frequency: FREQ, mode: MODE }));
  ws.onerror = () => console.log(`  ${tag}: socket error`);
  return ws;
};
const wait = ms => new Promise(r => setTimeout(r, ms));
const rate = (ws, from, to) => (ws.hits.filter(t => t >= from && t < to).length / ((to - from) / 1000)).toFixed(1);

const A = open('bystanderA');
await wait(3000);
const t0 = performance.now();
await wait(6000);                                   // A alone
const t1 = performance.now();

const B = open('saverB');
await wait(1500);
B.send(JSON.stringify({ type: 'fftRate', value: ASK }));
await wait(8000);                                   // B has asked for ASK fps
const t2 = performance.now();

B.close();
await wait(8000);                                   // B is gone — does A recover on its own?
const t3 = performance.now();

console.log(`\n=== fps leak: ${BASE} radio=${RADIO || '(single)'} ===`);
console.log(`  bystander A, who never asked for anything:`);
console.log(`    before B joined      ${rate(A, t0, t1)} fps`);
console.log(`    while B asked ${String(ASK).padEnd(2)}     ${rate(A, t1 + 1500, t2)} fps`);
console.log(`    after B disconnected ${rate(A, t2, t3)} fps`);
// ★★ AND THE ASKER MUST STILL BE SERVED. "No leak" is half the test: a server that ignored the
//    request outright would also pass it, and would be a different bug wearing the same result —
//    the idle phone would keep burning the battery it asked to save.
console.log(`  asker B, which asked for ${ASK}:  ${rate(B, t1 + 1500, t2)} fps`);

const before = +rate(A, t0, t1), during = +rate(A, t1 + 1500, t2), after = +rate(A, t2, t3);
console.log(during < before * 0.6
  ? `\n  ★ LEAK: B's request cut A to ${(during / before * 100).toFixed(0)}% of its rate.`
    + (after < before * 0.6
      ? ` A did NOT recover when B left — the radio stays slow until someone asks for fast.`
      : ` A recovered when B left.`)
  : `\n  no leak: A held its rate (${before} -> ${during} fps).`);
A.close(); process.exit(0);
