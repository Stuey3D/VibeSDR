// fake-vibe-hwd.mjs — a stand-in for the R36S image's hardware service (vibe-hwd), for the handheld tests.
//
// ★ The interface is docs/v12/R36S-HANDHELD-IMAGE.md §0.2 — this file is ONE reading of it, kept deliberately literal
//   so a test that passes here is a test of the engine's half against the written contract, not against itself.
//   Unix socket, newline-delimited JSON, one reply per request, events after {"subscribe":true}; the shutdown event
//   waits ≤ 5 s for {"ack":"shutdown"}.
//
//   node scripts/fake-vibe-hwd.mjs [--sock /tmp/vibe-hwd.sock]      (stand-alone: prints what it is asked)
//   import { startFakeHwd } from './fake-vibe-hwd.mjs'                (tests: emit events, read what was asked)
//
import net from 'node:net';
import fs from 'node:fs';

export function startFakeHwd(sockPath) {
  try { fs.unlinkSync(sockPath); } catch { /* none */ }
  const st = {
    brightness: 120, maxBrightness: 255, volume: 60, muted: false, output: 'speaker',
    battery: { pct: 83, charging: false, status: 'Discharging' }, screen: 'on',
    timeSetTo: null, actions: [],
  };
  const requests = [];
  const subscribers = new Set();
  const acks = [];
  const server = net.createServer((sock) => {
    let buf = '';
    sock.setEncoding('utf8');
    sock.on('error', () => {});
    sock.on('close', () => subscribers.delete(sock));
    sock.on('data', (d) => {
      buf += d;
      let nl;
      while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl); buf = buf.slice(nl + 1);
        let m; try { m = JSON.parse(line); } catch { sock.write(JSON.stringify({ ok: false, error: 'not JSON' }) + '\n'); continue; }
        if (m.ack) { acks.push({ ack: m.ack, at: Date.now() }); continue; }   // ★ no reply to an ack (§0.2)
        requests.push(m);
        const r = answer(m, sock);
        if (r) sock.write(JSON.stringify(r) + '\n');
      }
    });
  });
  function answer(m, sock) {
    if (m.subscribe) { subscribers.add(sock); return { ok: true }; }
    if (m.get === 'state') {
      return { ok: true, brightness: st.brightness, maxBrightness: st.maxBrightness, volume: st.volume, muted: st.muted,
               output: st.output, battery: { ...st.battery }, screen: st.screen };
    }
    if (m.set === 'brightness') { if (typeof m.value !== 'number') return { ok: false, error: 'value' }; st.brightness = m.value; return { ok: true }; }
    if (m.set === 'volume') { if (typeof m.value !== 'number' || m.value < 0 || m.value > 100) return { ok: false, error: 'value' }; st.volume = m.value; return { ok: true }; }
    if (m.set === 'time') { if (typeof m.utcMs !== 'number') return { ok: false, error: 'utcMs' }; st.timeSetTo = m.utcMs; return { ok: true }; }
    if (['poweroff', 'reboot', 'screen-off', 'screen-on'].includes(m.action)) {
      st.actions.push(m.action);
      if (m.action === 'screen-off') st.screen = 'off';
      if (m.action === 'screen-on') st.screen = 'on';
      return { ok: true };
    }
    return { ok: false, error: 'unknown request' };
  }
  function emit(ev) { const line = JSON.stringify(ev) + '\n'; for (const s of subscribers) s.write(line); }
  /** The shutdown event, and a promise of the ack (or null after `waitMs`, as the real service gives up). */
  function shutdown(reason = 'request', waitMs = 5000) {
    const before = acks.length, t0 = Date.now();
    emit({ event: 'shutdown', reason, inSec: 5 });
    return new Promise((res) => {
      const iv = setInterval(() => {
        if (acks.length > before) { clearInterval(iv); res(Date.now() - t0); }
        else if (Date.now() - t0 > waitMs) { clearInterval(iv); res(null); }
      }, 20);
    });
  }
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(sockPath, () => resolve({
      state: st, requests, acks, emit, shutdown,
      subscribed: () => subscribers.size,
      close: () => new Promise((r) => { for (const s of subscribers) s.destroy(); server.close(() => r()); }),
    }));
  });
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const i = process.argv.indexOf('--sock');
  const sock = i > 0 ? process.argv[i + 1] : '/tmp/vibe-hwd.sock';
  const h = await startFakeHwd(sock);
  console.log(`fake vibe-hwd on ${sock} — type an event as JSON (e.g. {"event":"volume","volume":40,"muted":false}) or "shutdown"`);
  process.stdin.setEncoding('utf8');
  process.stdin.on('data', async (d) => {
    for (const line of d.split('\n').map((s) => s.trim()).filter(Boolean)) {
      if (line === 'shutdown') { const ms = await h.shutdown(); console.log(ms === null ? 'no ack in 5 s' : `ack after ${ms} ms`); continue; }
      try { h.emit(JSON.parse(line)); } catch { console.log('not JSON'); }
    }
  });
  setInterval(() => { if (h.requests.length) console.log('asked:', JSON.stringify(h.requests.splice(0))); }, 500);
}
