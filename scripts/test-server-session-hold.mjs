// test-server-session-hold.mjs — THE HELD SESSION (V12, 2026-10-27, branch v12-session-hold — docs/v12/BACKGROUND-DECODING.md, "The held session").
//
// ★★★ STUART: "rather than a 60 second grace period … maybe a 3 minute grace period and when the OS suspends the app we
//     send a little notification … your session on the server is held for 3 minutes all decoders are still running for
//     this period and no data is missed." And: "obviously if the user hits a hard time limit within that grace period
//     then that is the hard rule and user gets disconnected as normal".
//
// ★★ WHAT THIS PINS, on a REAL vibeserver (fake rtl_tcp), the sockets simply vanishing as a suspended phone's do:
//     1. ADS-B (idle grace 300 → a 180 s hold): hwinfo says holdS 180; the sockets go → "HELD for 180 s"; at 100 s the
//        same session comes back to ADS-B still running, NOTHING re-entered, and the table counted messages decoded IN
//        THE GAP; gone again, the hold runs out at 180 s → released, the mode left, and mode_resume still gives it back.
//     2. A full radio (users 2: B listening, A held): C arrives and gets in — the held place is released for them;
//        a one-listener radio: C gets in at once; a listener who LEAVES on purpose (`leave`) is never held.
//     3. Decoders, dial and VFO B (CW skimmer clip, one-pipeline radio): all three sockets go; 40 s later the session
//        is back — the same dial, VFO B still open, the skimmer NOT restarted, and what it decoded in the gap REPLAYED.
//     4. ★ THE HARD LIMIT (Stuart's rule): --session-limit 1 from the LAN address (loopback is exempt): hwinfo states
//        holdHardS; the hold is cut to the limit, released AT it, and the returning listener is ended as normal.
// ★ SILENT: audio sockets are opened only so the listener has audio; their frames are counted, never played.
// ★ SLOW (~8 minutes: two 3-minute holds and a 1-minute limit) — run-tests.sh runs it with VIBESERVER_SLOW=1.
//
//   usage: VIBESERVER_BIN=… VIBE_ADSB_CLIP=~/VibeSDR-test-audio/adsb/flightaware-2026-10-24-2.4msps.u8 \
//          VIBE_CW_CLIP=…/clip.cu8 node scripts/test-server-session-hold.mjs
//   (the CW clip: scripts/test-cw.sh clip <file>; without it part 3 reports NOT RUN)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_ADSB_CLIP || '';
const CW_CLIP = process.env.VIBE_CW_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree and VIBE_ADSB_CLIP to a 2.4 MS/s 1090 MHz recording');
  process.exit(3);
}
const ONLY = (process.env.HOLD_PARTS || '1234').split('');
let lan = process.env.LAN_IP || '';
if (!lan) for (const list of Object.values(os.networkInterfaces())) for (const a of list || []) if (a.family === 'IPv4' && !a.internal && !lan) lan = a.address;

let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const LAND = 96100000;
const children = new Set();
const killAll = () => { for (const c of children) { try { c.kill('SIGTERM'); } catch {} } children.clear(); };
process.on('exit', killAll);
// ★ A deadline on the whole run: a hung server must never leave this waiting (or its children running).
const deadline = setTimeout(() => { console.log('   FAIL deadline — the run took over 14 minutes'); killAll(); process.exit(1); }, 14 * 60_000);
deadline.unref();

/** A minimal WebSocket client recording text frames (and 0x01 decoder-text binary frames). */
function ws(port, pathq, host = '127.0.0.1') {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, host);
  const st = { txt: [], bin: 0, open: false, closed: false, t0: Date.now() };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: ${host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x2) st.bin++;
      if (op === 0x1) st.txt.push({ t: Date.now() - st.t0, s: p.toString() });
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  // ★ Like a suspended phone: the socket just goes (no close frame) — the server must notice on its own.
  st.drop = () => { s.destroy(); };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const parsed = (st, type) => st.txt.filter((x) => x.s.includes(`"type":"${type}"`)).map((x) => { try { return { t: x.t, ...JSON.parse(x.s) }; } catch { return { t: x.t }; } });
const centreOf = (c) => c.center_freq ?? c.centerFreq ?? 0;
const waitFor = async (fn, ms) => { const t = Date.now(); while (Date.now() - t < ms) { if (fn()) return true; await sleep(200); } return fn(); };

async function server(tag, args, { adsb = true, hf = null } = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-hold-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort();
  const rate = hf ? hf.rate : 2400000;
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Hold test', adminPass: 'hold-test-admin',
    radios: [{ serial: 'hold-test', label: 'Test radio', enabled: true, configured: true }] }));
  const fakeArgs = [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(rate)];
  if (adsb) fakeArgs.push('--adsb', CLIP);
  if (hf) fakeArgs.push('--hf', hf.clip, '--hf-centre', String(hf.centre));
  const rtl = spawn(process.execPath, fakeArgs, { stdio: 'ignore' });
  children.add(rtl);
  await sleep(600);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn('nice', ['-n', '19', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(rate), ...args],
                    { env, stdio: ['ignore', log, log] });
  children.add(srv);
  for (let i = 0; i < 150; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  return { port, dir, log: () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } },
           stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); children.delete(srv); children.delete(rtl); } };
}
const spectrum = (port, sid, host) => ws(port, `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256&proto=1`, host);
const audio = (port, sid, host) => ws(port, `/ws/audio?user_session_id=${sid}&codec=opus`, host);
const entries = (S) => (S.log().match(/\[ADS-B\] mode ON: 1090 MHz/g) || []).length;
const gaveBack = (S) => /\[ADS-B\] last listener left — the receiver goes back/.test(S.log());
const lastAdsbMsgs = (st) => { const r = parsed(st, 'adsb').filter((x) => typeof x.messages === 'number'); return r.length ? r.at(-1).messages : -1; };

try {
  // ── 1. ADS-B: held 180 s, back at 100 s, then the hold runs out ─────────────────────────────────────────────────
  if (ONLY.includes('1')) {
    console.log('\n1. ADS-B — the hold (idle grace 300 → 180 s)');
    const S = await server('adsb', ['--users', '3', '--freq', String(LAND), '--mode', 'wfm', '--idle-grace', '300']);
    try {
      const sid = crypto.randomUUID();
      const A = spectrum(S.port, sid);
      await waitFor(() => parsed(A, 'hwinfo').length > 0, 4000);
      const hw = parsed(A, 'hwinfo').at(-1) || {};
      ok(hw.holdS === 180 && hw.holdHardS === -1, `hwinfo states the terms: holdS ${hw.holdS}, holdHardS ${hw.holdHardS}`);
      A.send({ type: 'adsb', on: 1 });
      await waitFor(() => lastAdsbMsgs(A) > 20, 15_000);
      const m0 = lastAdsbMsgs(A);
      ok(m0 > 0, `in ADS-B, ${m0} messages decoded`);
      const t0 = Date.now();
      A.drop();
      await waitFor(() => /HELD for 180 s/.test(S.log()), 5000);
      ok(/HELD for 180 s/.test(S.log()), '★ the sockets went → "HELD for 180 s"');
      await sleep(100_000 - (Date.now() - t0));
      ok(!gaveBack(S), '★ 100 s on: still in ADS-B (the old grace had given it back at 60 s)');
      const A2 = spectrum(S.port, sid);
      await waitFor(() => parsed(A2, 'adsb').length > 0, 3000);
      ok(parsed(A2, 'adsb').length > 0, 'the same session back: ADS-B reports at once');
      const m1 = lastAdsbMsgs(A2);
      ok(m1 > m0 + 50, `★ the table counted the gap: ${m0} → ${m1} messages (decoded while the phone was away)`);
      ok(entries(S) === 1, `★ nothing re-entered (${entries(S)} entry)`);
      ok(/held session \S+ returned after 1\d\d s/.test(S.log()), '…"returned after 1xx s" logged');
      const cfg = parsed(A2, 'config').at(-1) || {};
      ok(Math.abs(centreOf(cfg) - 1090e6) < 2e6, `the dial is still 1090 MHz (${centreOf(cfg)})`);
      // ── gone again: the hold runs out ──
      const t1 = Date.now();
      A2.drop();
      await waitFor(() => /hold ran out/.test(S.log()), 190_000);
      const el = (Date.now() - t1) / 1000;
      ok(/hold ran out/.test(S.log()) && el >= 178 && el < 188, `★ the hold ran out at ${el.toFixed(0)} s → released`);
      await waitFor(() => gaveBack(S), 6000);
      ok(gaveBack(S), '★ …and then ADS-B was left, as a live last listener leaving would have');
      const A3 = spectrum(S.port, sid);
      await waitFor(() => parsed(A3, 'config').length > 0, 3000);
      A3.send({ type: 'mode_resume', mode: 'adsb' });
      await waitFor(() => parsed(A3, 'mode_resume').length > 0, 4000);
      ok((parsed(A3, 'mode_resume')[0] || {}).ok === 1, '★ after the hold, mode_resume still gives it back (the 8b63720b9 fallback)');
      A3.close(); await sleep(300);
    } finally { S.stop(); }
  }

  // ── 2. a held place gives way; `leave` is never held ────────────────────────────────────────────────────────────
  if (ONLY.includes('2')) {
    console.log('\n2. A held place gives way to somebody who is here');
    {
      const S = await server('full', ['--users', '2', '--freq', String(LAND), '--mode', 'wfm', '--idle-grace', '300']);
      try {
        const a = crypto.randomUUID(), b = crypto.randomUUID(), c = crypto.randomUUID();
        const A = spectrum(S.port, a);
        await waitFor(() => parsed(A, 'config').length > 0, 4000);
        A.send({ type: 'adsb', on: 1 });
        await waitFor(() => parsed(A, 'adsb').length > 0, 10_000);
        const B = spectrum(S.port, b);
        await waitFor(() => parsed(B, 'config').length > 0, 4000);
        A.drop();
        await waitFor(() => /HELD for/.test(S.log()), 5000);
        await sleep(1500);
        const users = parsed(B, 'users').at(-1) || {};
        ok(users.n === 2, `B is told 2 listeners — the held place counts (${JSON.stringify(users)})`);
        const C = spectrum(S.port, c);
        await waitFor(() => parsed(C, 'config').length > 0 || C.closed, 5000);
        ok(parsed(C, 'config').length > 0 && !C.closed, '★ C, arriving at a radio full with a held place, gets in');
        ok(/the held place is theirs/.test(S.log()), '…the held place released for them, logged');
        await waitFor(() => /held session \S+ released/.test(S.log()), 4000);
        ok(parsed(C, 'adsb').length > 0 || parsed(B, 'adsb').length > 0, 'the radio is still in ADS-B for the two who are here');
        B.close(); C.close(); await sleep(300);
      } finally { S.stop(); }
    }
    {
      const S = await server('one', ['--users', '1', '--freq', String(LAND), '--mode', 'wfm', '--idle-grace', '300']);
      try {
        const a = crypto.randomUUID();
        const A = spectrum(S.port, a);
        await waitFor(() => parsed(A, 'config').length > 0, 4000);
        A.drop();
        await waitFor(() => /HELD for/.test(S.log()), 5000);
        const C = spectrum(S.port, crypto.randomUUID());
        await waitFor(() => parsed(C, 'config').length > 0 || C.closed, 5000);
        ok(parsed(C, 'config').length > 0 && !C.closed, '★ one-listener radio: somebody else arriving gets in at once');
        ok(/one-listener radio/.test(S.log()), '…the hold released for them, logged');
        // ★ leaving on purpose: never held
        const n0 = (S.log().match(/HELD for/g) || []).length;
        C.send({ type: 'leave' });
        await sleep(300);
        C.drop();
        await sleep(2500);
        ok((S.log().match(/HELD for/g) || []).length === n0, '★ a listener who sent `leave` is NOT held');
      } finally { S.stop(); }
    }
  }

  // ── 3. decoders, dial and VFO B through a hold (CW skimmer) ─────────────────────────────────────────────────────
  if (ONLY.includes('3')) {
    console.log('\n3. Decoders, dial and VFO B (CW skimmer clip, one-pipeline radio)');
    if (!CW_CLIP || !fs.existsSync(CW_CLIP)) console.log('   not run — set VIBE_CW_CLIP (scripts/test-cw.sh clip <file>)');
    else {
      const CENTRE = 7030000;
      const S = await server('cw', ['--users', '1', '--freq', String(CENTRE), '--mode', 'cwu', '--idle-grace', '300'],
                             { adsb: false, hf: { clip: CW_CLIP, centre: CENTRE, rate: 960000 } });
      try {
        const sid = crypto.randomUUID();
        const L = { spec: spectrum(S.port, sid) };
        await sleep(400);
        L.aud = audio(S.port, sid); L.dx = ws(S.port, `/ws/dxcluster?user_session_id=${sid}`);
        await sleep(500);
        L.spec.send({ type: 'tune', frequency: CENTRE, mode: 'cwu' });
        await sleep(600);
        L.dx.send({ type: 'audio_extension_attach', extension_name: 'cwskim', width: 48000 });
        await waitFor(() => parsed(L.dx, 'cw_spot').length > 0, 45_000);
        ok(parsed(L.dx, 'cw_spot').length > 0, `the skimmer spots (${parsed(L.dx, 'cw_spot').length})`);
        L.spec.send({ type: 'watch', id: 1, frequency: CENTRE + 9800, mode: 'cwu' });
        await waitFor(() => (parsed(L.spec, 'watch').at(-1)?.chans || []).length === 1, 4000);
        ok((parsed(L.spec, 'watch').at(-1)?.chans || []).length === 1, 'VFO B open at 7.0398 MHz');
        const starts0 = (S.log().match(/decoder attached: cw/g) || []).length;
        const lands0 = (S.log().match(/new session — landing on/g) || []).length;
        for (const k of ['spec', 'aud', 'dx']) L[k].drop();
        await waitFor(() => /HELD for/.test(S.log()), 6000);
        ok(/HELD for 180 s/.test(S.log()), '★ all three sockets gone → held');
        ok(/decoder kept running/.test(S.log()), '★ the skimmer is KEPT running for the held session');
        await sleep(40_000);
        const R = { spec: spectrum(S.port, sid) };
        await sleep(400);
        R.aud = audio(S.port, sid); R.dx = ws(S.port, `/ws/dxcluster?user_session_id=${sid}`);
        await waitFor(() => parsed(R.spec, 'config').length > 0, 4000);
        await sleep(600);
        R.dx.send({ type: 'audio_extension_attach', extension_name: 'cwskim', width: 48000 });
        await waitFor(() => /replaying \d+ decoder messages sent while the listener was away/.test(S.log()), 4000);
        const rep = (S.log().match(/replaying (\d+) decoder messages sent while the listener was away/) || [])[1];
        ok(Number(rep) > 10, `★ what the skimmer decoded in the gap is replayed (${rep} messages)`);
        await sleep(1500);
        ok(parsed(R.dx, 'cw_signals').length >= 10 || parsed(R.dx, 'cw_spot').length > 0,
           `…and arrives on the returning socket (${parsed(R.dx, 'cw_signals').length} lists, ${parsed(R.dx, 'cw_spot').length} spots)`);
        ok((S.log().match(/decoder attached: cw/g) || []).length === starts0, '★ the skimmer was NOT restarted (Joined, not rebuilt)');
        const cfg = parsed(R.spec, 'config').at(-1) || {};
        ok(Math.abs((cfg.vfo ?? 0) - CENTRE) < 2 && cfg.mode === 'cwu', `★ the dial is where it was (${cfg.vfo} ${cfg.mode})`);
        ok((S.log().match(/new session — landing on/g) || []).length === lands0, '…and not landed on return');
        R.spec.send({ type: 'watch', stereo: true });
        await waitFor(() => parsed(R.spec, 'watch').length > 0, 3000);
        const w = parsed(R.spec, 'watch').at(-1) || {};
        ok((w.chans || []).length === 1 && Math.abs(w.chans[0].freq - (CENTRE + 9800)) < 2, `★ VFO B is still open (${JSON.stringify((w.chans || [])[0] || null)})`);
        for (const k of ['spec', 'aud', 'dx']) R[k].close();
        await sleep(300);
      } finally { S.stop(); }
    }
  }

  // ── 4. the hard limit ends a hold ───────────────────────────────────────────────────────────────────────────────
  if (ONLY.includes('4')) {
    console.log('\n4. ★ The session time limit ends a hold, as it ends a live session');
    if (!lan) console.log('   not run — no non-loopback IPv4 address (loopback is exempt from the limit)');
    else {
      const S = await server('limit', ['--users', '1', '--freq', String(LAND), '--mode', 'wfm', '--idle-grace', '300', '--session-limit', '1'],
                             { adsb: false });
      try {
        const sid = crypto.randomUUID();
        const A = spectrum(S.port, sid, lan);
        await sleep(300);
        const Aa = audio(S.port, sid, lan);
        await waitFor(() => parsed(A, 'hwinfo').length > 0, 4000);
        const hw = parsed(A, 'hwinfo').at(-1) || {};
        ok(hw.holdS === 180 && hw.holdHardS > 0 && hw.holdHardS <= 60, `hwinfo: holdS ${hw.holdS}, holdHardS ${hw.holdHardS} (the 1-minute limit)`);
        await sleep(15_000);
        const t0 = Date.now();
        A.drop(); Aa.drop();
        await waitFor(() => /HELD for \d+ s — until the session time limit/.test(S.log()), 5000);
        const held = (S.log().match(/HELD for (\d+) s — until the session time limit/) || [])[1];
        ok(Number(held) > 30 && Number(held) <= 46, `★ the hold is cut to the limit: ${held} s, not 180`);
        await waitFor(() => /reached the session time limit/.test(S.log()), 60_000);
        const el = (Date.now() - t0) / 1000;
        ok(/reached the session time limit/.test(S.log()) && Math.abs(el - Number(held)) < 4, `★ released AT the limit (${el.toFixed(0)} s after the sockets went)`);
        const B = spectrum(S.port, sid, lan);
        await sleep(300);
        const Ba = audio(S.port, sid, lan);
        await waitFor(() => parsed(B, 'cooldown').length > 0 || parsed(B, 'session_expired').length > 0, 8000);
        ok((parsed(B, 'cooldown').length > 0 || parsed(B, 'session_expired').length > 0) && parsed(B, 'hwinfo').length === 0,
           `★ coming back after it: refused exactly as after a live limit (${(parsed(B, 'cooldown')[0] || parsed(B, 'session_expired')[0] || {}).type})`);
        B.close(); Ba.close(); await sleep(300);
      } finally { S.stop(); }
    }
  }
} finally {
  killAll();
}

console.log(`\n${fail ? '✗' : '✓'} test-server-session-hold: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
