#!/usr/bin/env node
// ubersdr-rec — record any frequency off an UberSDR as a lossless WAV, for replaying through our decoders.
//
//   node tools/ubersdr-rec/ubersdr-rec.mjs <host:port> <freqHz> <mode> <seconds> <out.wav> [bwLow bwHigh]
//   e.g. MSF:   node tools/ubersdr-rec/ubersdr-rec.mjs 192.168.86.11:8080 59000 usb 900 msf.wav
//        DCF77: node tools/ubersdr-rec/ubersdr-rec.mjs 192.168.86.11:8080 76500 usb 900 dcf77.wav
//   (USB with the dial 1 kHz below the carrier: a 1 kHz tone whose level IS the time code.)
//
// ★★★ WHY (Stuart, 2026-10-05: "you have my full permission to do that any time you need to, I get the feeling
//     this could've saved us a lot of time and builds"). His own UberSDR (an RX888) gives every listener their own
//     channel, so recording never moves anything he is listening to. Real audio replayed through the shipping
//     decoder, before and after a change, is the test — not synthetic audio, and not "whatever was on air the night
//     Stuart tried it".
// ★ Format: UberSDR's `pcm-zstd` (pcm_binary.go in madpsy/ka9q_ubersdr): every WebSocket binary message is ONE
//   zstd frame holding a packet — FULL header "PC" (29 bytes at version 1: sample rate at 20, channels at 24) or
//   MINIMAL "PM" (13 bytes) — then int16 PCM. Lossless, unlike the Opus the apps use.
//   ★★ The doc says big-endian throughout; on the wire (2026-10-05) the HEADER is little-endian (magic bytes
//      43 50, rate LE) — see PCM_LE for the samples.
// ★ OTHER PEOPLE'S RECEIVERS (Stuart, 2026-10-07, the WEFAX world test): allowed only as memory/
//   third_party_receiver_etiquette.md says — honest UA on every request, ONE connection, a free slot, the operator's
//   max_session_time honoured (the recording is cut to it), and only for the scheduled chart: it disconnects at the
//   end of <seconds>. The `session_timeout` liveness check is answered with UberSDR's own `{"type":"ping"}` (what
//   their idle-detector.js and our UberSDRWsClient.noteActivity send) only WHILE recording — a running decoder is
//   real use — and never after; `max_session_time` is a fairness limit and is never answered.
//   host may be `https://host` / `host:443` for the tunnelled receivers; plain host:port stays http.
import { writeFileSync } from 'node:fs';
import { zstdDecompressSync } from 'node:zlib';
import { randomUUID } from 'node:crypto';

const [hostPort, freq, mode, secs, out, bwl, bwh] = process.argv.slice(2);
const BE = process.env.PCM_LE !== '1';   // samples: big-endian per pcm_binary.go — checked against a known tone
if (!out) { console.error('usage: ubersdr-rec.mjs <host:port> <freqHz> <mode> <seconds> <out.wav> [bwLow bwHigh]'); process.exit(2); }
const UA = 'VibeSDR/11.0 (+https://vibesdr.net)';   // ★ honest, as src/constants/version.ts USER_AGENT — never a browser
const tls = /^https:\/\//.test(hostPort) || /:443$/.test(hostPort);
const hp = hostPort.replace(/^https?:\/\//, '').replace(/\/$/, '');
const base = `${tls ? 'https' : 'http'}://${hp}`;
const sid = randomUUID();
const reg = await fetch(`${base}/connection`, { method: 'POST', headers: { 'Content-Type': 'application/json', 'User-Agent': UA },
  body: JSON.stringify({ user_session_id: sid }) }).then((r) => r.json()).catch((e) => ({ error: String(e) }));
if (reg.allowed !== true) { console.error('connection refused:', JSON.stringify(reg)); process.exit(1); }
const idleSecs = typeof reg.session_timeout === 'number' ? reg.session_timeout : 300;   // 0 = no idle check
const maxSecs = Number(reg.max_session_time) || 0;
let durSecs = +secs;
if (maxSecs > 0 && durSecs > maxSecs - 30) durSecs = Math.max(30, maxSecs - 30);      // ★ fairness cap: never overrun it
console.error(`policy: idle=${idleSecs}s maxSession=${maxSecs}s → recording ${durSecs}s`);
let q = `user_session_id=${sid}&format=pcm-zstd&version=1&frequency=${freq}&mode=${mode}&ua=${encodeURIComponent(UA)}`;
if (bwl !== undefined && bwh !== undefined) q += `&bandwidthLow=${bwl}&bandwidthHigh=${bwh}`;
const ws = new WebSocket(`${tls ? 'wss' : 'ws'}://${hp}/ws?${q}`, { headers: { 'User-Agent': UA } });
ws.binaryType = 'arraybuffer';
const chunks = []; let rate = 0, ch = 1, samples = 0, bad = 0;
/* ★ THE FIRST SAMPLE'S UTC (2026-10-25, the FT8 CPU work): a slot decoder needs to know where the slot boundaries are
 *  in the file. Every packet's arrival minus the samples so far is a LATE estimate of it (a packet is never early), so
 *  the lowest is the best — the same lowest-latency anchor the server's FT8 decoder uses. Printed as start_utc=. */
let startUtc = Infinity;
const t0 = Date.now();
ws.onmessage = (ev) => {
  if (typeof ev.data === 'string') return;                    // status / signal JSON
  let p;
  try { p = zstdDecompressSync(Buffer.from(ev.data)); } catch { bad++; return; }
  const magic = p.readUInt16LE(0);                            // ★ the header is LITTLE-endian on the wire
  let off;
  if (magic === 0x5043) { const v = p[2]; rate = p.readUInt32LE(20); ch = p[24]; off = v >= 2 ? 37 : 29; }
  else if (magic === 0x504d) off = 13;
  else { bad++; return; }
  if (!rate) return;                                          // minimal packet before any full header
  const n = (p.length - off) >> 1;
  const le = Buffer.alloc(n * 2);
  for (let i = 0; i < n; i++) le.writeInt16LE(BE ? p.readInt16BE(off + i * 2) : p.readInt16LE(off + i * 2), i * 2);
  chunks.push(le); samples += n;
  startUtc = Math.min(startUtc, Date.now() / 1000 - samples / ch / rate);
};
ws.onerror = (e) => console.error('ws error', e.message ?? e);
// ★ Presence ping only while recording, at a third of the idle window (≥ their 10 s throttle); none if no idle check.
const pingMs = idleSecs > 0 ? Math.max(10, Math.floor(idleSecs / 3)) * 1000 : 0;
const keep = pingMs ? setInterval(() => { try { ws.send(JSON.stringify({ type: 'ping' })); } catch {} }, pingMs) : null;
ws.onmessage = ((orig) => (ev) => {
  if (typeof ev.data === 'string') { try { const m = JSON.parse(ev.data); if (m.type && !['status', 'signal', 'pong'].includes(m.type)) console.error('server:', ev.data.slice(0, 200)); } catch {} }
  orig(ev);
})(ws.onmessage);
let done = false;
const finish = () => {
  if (done) return; done = true;
  if (keep) clearInterval(keep); try { ws.close(); } catch {}
  const data = Buffer.concat(chunks);
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8); h.write('fmt ', 12);
  h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(ch, 22); h.writeUInt32LE(rate, 24);
  h.writeUInt32LE(rate * ch * 2, 28); h.writeUInt16LE(ch * 2, 32); h.writeUInt16LE(16, 34);
  h.write('data', 36); h.writeUInt32LE(data.length, 40);
  writeFileSync(out, Buffer.concat([h, data]));
  console.log(`${out}: ${(samples / ch / (rate || 1)).toFixed(1)} s at ${rate} Hz x${ch}, wall ${((Date.now() - t0) / 1000).toFixed(1)} s, bad packets ${bad}, start_utc=${startUtc.toFixed(3)}`);
  process.exit(0);
};
ws.onclose = () => finish();
setTimeout(finish, durSecs * 1000);
