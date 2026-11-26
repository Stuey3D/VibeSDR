// ★★ V12 beta96 THE "ADS-B BEAST FEED (from another receiver)" RADIO through the REAL server (docs/v12/ADSB-FEED.md).
// A synthetic dump1090 here (a local TCP port sending Beast — never Stuart's Pi), then:
//   A. a server whose ONLY radio is a Beast feed: it starts with no SDR, a listener gets the ADS-B report with the aircraft
//      (positions, callsign) and onlyMode "adsb", a listener's EXIT is refused in words, the admin status shows the
//      input connected, line noise and a bad-CRC reply are dropped, the feeder going away is said in the owner's words.
//   B. a FULL server with a fake RTL (fake rtl_tcp) BESIDE a Beast feed radio: both radio processes run, the door lists
//      both, the Beast radio answers its admin status through the door, the RTL is an ordinary receiver.
// Silent: no audio device. Exit 3 = not run (no VIBESERVER_BIN).
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const up = async (port) => { for (let i = 0; i < 150; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) return true; await sleep(100); } return false; };
const until = async (f, ms = 8000) => { for (let t = 0; t < ms; t += 100) { if (await f()) return true; await sleep(100); } return f(); };
const kids = [];
process.on('exit', () => { for (const k of kids) { try { k.kill('SIGTERM'); } catch {} } });

// ── the synthetic dump1090: the Riddle's worked examples (CRC-good), re-sent with a rising 12 MHz counter ──
const FRAMES = ['8D4840D6202CC371C32CE0576098',                // KLM1023 identification (4840D6)
                '8D40621D58C382D690C8AC2863A7', '8D40621D58C386435CC412692AD6',   // 40621D even + odd: 52.2572, 3.9194
                '5D484FDEA248F5'];                              // a DF11 all-call
const BAD = '8D40621D58C382D690C8AC2863A8';                     // one bit off in the parity… plus a second: never believed
function beast(hex, ts, sig) {
  const m = Buffer.from(hex, 'hex');
  const t = m.length === 7 ? 0x32 : 0x33;
  const raw = [];
  for (let i = 5; i >= 0; --i) raw.push(Number((BigInt(ts) >> BigInt(8 * i)) & 0xffn));
  raw.push(sig, ...m);
  const out = [0x1a, t];
  for (const b of raw) { out.push(b); if (b === 0x1a) out.push(0x1a); }
  return Buffer.from(out);
}
function feeder(port) {
  let ts = 1000, k = 0;
  const socks = new Set();
  const srv = net.createServer((c) => {
    socks.add(c);
    c.on('error', () => {});
    const t = setInterval(() => {
      const bufs = [];
      for (let i = 0; i < 20; i++, k++) {
        ts += 12000 * 25;
        bufs.push(beast(FRAMES[k % FRAMES.length], ts, 0x1a));
        if (k % 37 === 0) bufs.push(Buffer.from([0x55, 0x1a, 0x99, 0x00]));        // line noise
        if (k % 41 === 0) { const b = Buffer.from(BAD, 'hex'); b[8] ^= 0x40; bufs.push(beast(b.toString('hex'), ts, 30)); }
      }
      c.write(Buffer.concat(bufs));
    }, 100);
    c.on('close', () => clearInterval(t));
  });
  srv.listen(port, '127.0.0.1');
  srv.kill = () => { srv.close(); for (const c of socks) c.destroy(); };
  return srv;
}

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], bin: 0 };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x2) st.bin++;
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(6); h[0] = 0x81; h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2);
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => s.destroy();
  return st;
}
const parsed = (st, type) => st.txt.filter((x) => x.includes(`"type":"${type}"`)).map((x) => { try { return JSON.parse(x); } catch { return {}; } });
const adminQ = async (H, pass) => {
  const nonce = (await fetch(`${H}/vibeserver/auth`).then((r) => r.json())).nonce;
  const q = `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', pass).update(nonce).digest('hex')}`;
  // ★ a radio behind the door takes the door's admin ticket (as admin.ts qCached does)
  const t = await (await fetch(`${H}/vibeserver/admin-ticket?${q}`)).json();
  return `vs_admin_ticket=${encodeURIComponent(t.ticket)}`;
};
function mkdir(tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-beast-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  return dir;
}

// ── A. only a Beast feed radio ──
console.log('\nA. a server whose only radio is an ADS-B Beast feed');
{
  const fport = await freePort();
  const fsrv = feeder(fport);
  const dir = mkdir('only'), port = await freePort(), ADMIN = 'beast-only-test';
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, adminPass: ADMIN, port,
    radios: [{ serial: 'beast-1', driver: 'beast', label: 'PiAware Pi 3', beastIn: `127.0.0.1:${fport}`, enabled: true, configured: true, users: 4 }],
  }));
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logf = path.join(dir, 'server.log');
  const srv = spawn('nice', ['-n', '19', BIN, '--port', String(port), '--admin-pass', ADMIN], { env, stdio: ['ignore', fs.openSync(logf, 'w'), fs.openSync(logf, 'a')] });
  kids.push(srv);
  const L = () => { try { return fs.readFileSync(logf, 'utf8'); } catch { return ''; } };
  try {
    ok(await up(port), `the server starts with no SDR at all (port ${port})`);
    ok(await until(() => /Beast feed radio started: ADS-B from 127\.0\.0\.1:\d+/.test(L()), 15000), 'as a Beast feed radio');
    ok(!/no radio with serial|no radios are detected/.test(L()), 'nothing looked for hardware');
    const A = ws(port, `/ws/user-spectrum?user_session_id=${crypto.randomUUID()}&mode=binary8&bins=256&proto=1`);
    await until(() => parsed(A, 'adsb').some((m) => (m.aircraft || []).some((a) => a.lat)), 15000);
    const rep = parsed(A, 'adsb').reverse().find((m) => (m.aircraft || []).length) || {};
    const ac = rep.aircraft || [];
    ok(ac.some((a) => a.hex === '4840d6' && /KLM1023/.test(a.flight || '')), `KLM1023 (4840d6) with its callsign (${ac.length} aircraft)`);
    const p = ac.find((a) => a.hex === '40621d');
    ok(!!p && Math.abs(p.lat - 52.2658) < 0.001 && Math.abs(p.lon - 3.9389) < 0.001, `40621d positioned from the feed (${p ? p.lat + ',' + p.lon : 'none'})`);
    const hw = parsed(A, 'hwinfo').pop() || {};
    ok(hw.onlyMode === 'adsb', `hwinfo onlyMode "adsb" — the listener lands on the map with no EXIT (${hw.onlyMode})`);
    A.send({ type: 'adsb', on: 0 });
    await until(() => parsed(A, 'adsb_error').length > 0, 3000);
    ok(/ADS-B feed from another receiver/.test((parsed(A, 'adsb_error')[0] || {}).why || '') || /only/.test((parsed(A, 'adsb_error')[0] || {}).why || ''),
       `EXIT refused in words: ${(parsed(A, 'adsb_error')[0] || {}).why}`);
    A.send({ type: 'tune', freq: 96100000 });
    await sleep(600);
    ok(!/mode ON: 1090|tune.*96100000/.test(L()) && parsed(A, 'adsb_off').length === 0, 'a tune changes nothing (still the map)');
    const st = await (await fetch(`http://127.0.0.1:${port}/r/beast-1/vibeserver/admin/adsbfeed?${await adminQ(`http://127.0.0.1:${port}`, ADMIN)}`)).json();
    if (!st.input) console.log('   status was: ' + JSON.stringify(st).slice(0, 300));
    ok(st.input && st.input.state === 'connected' && st.input.msgs > 0, `the admin status: the input connected (${st.input && st.input.line})`);
    ok(st.input && st.input.bad > 0, `line noise resynced and counted (${st.input && st.input.bad})`);
    ok(/Beast feed from 127\.0\.0\.1:\d+: connected/.test(st.input ? st.input.line : ''), 'its line for the tab and the admin page');
    A.close();
    // the feeder goes away (a Pi rebooting)
    fsrv.kill();
    await until(async () => {
      const s2 = await (await fetch(`http://127.0.0.1:${port}/r/beast-1/vibeserver/admin/adsbfeed?${await adminQ(`http://127.0.0.1:${port}`, ADMIN)}`)).json();
      return s2.input && s2.input.state === 'waiting';
    }, 6000);
    const s3 = await (await fetch(`http://127.0.0.1:${port}/r/beast-1/vibeserver/admin/adsbfeed?${await adminQ(`http://127.0.0.1:${port}`, ADMIN)}`)).json();
    ok(s3.input && /is dump1090's Beast output on\?|closed the connection/.test(s3.input.error || ''), `gone: said in the owner's words (${s3.input && s3.input.error})`);
    ok(s3.input && s3.input.state === 'reconnecting' && s3.input.downSince > 0 && s3.input.attempts >= 1,
       `the tab's status: reconnecting, down since, attempts (${s3.input && s3.input.state}, ${s3.input && s3.input.attempts})`);
    // ★ the listeners' banner while it is down, and it clears itself when the receiver is back
    const W = ws(port, `/ws/user-spectrum?user_session_id=${crypto.randomUUID()}&mode=binary8&bins=256&proto=1`);
    await until(() => parsed(W, 'adsb').some((m) => /Connection to the receiver lost/.test(m.feedWarn || '')), 6000);
    const lost = parsed(W, 'adsb').reverse().find((m) => m.feedWarn) || {};
    ok(/Connection to the receiver lost — reconnecting… \(lost \d+ s ago\)/.test(lost.feedWarn || ''), `listeners see the banner: ${lost.feedWarn}`);
    const back = feeder(fport);
    await until(() => { const a = parsed(W, 'adsb'); const l = a[a.length - 1] || {}; return !l.feedWarn && (l.aircraft || []).length > 0; }, 12000);
    const last = parsed(W, 'adsb').pop() || {};
    ok(!last.feedWarn && (last.aircraft || []).length > 0, 'the receiver back: the banner clears itself and aircraft return');
    ok(/\[ADS-B\] Beast feed 127\.0\.0\.1:\d+: lost:/.test(L()) && /\[ADS-B\] Beast feed 127\.0\.0\.1:\d+: back after/.test(L()),
       'logged once at the loss and once on recovery');
    ok((L().match(/Beast feed 127\.0\.0\.1:\d+: (lost|still reconnecting)/g) || []).length === 1, 'no log flood while down');
    W.close(); back.kill();
  } finally { srv.kill('SIGTERM'); fsrv.kill(); }
}

// ── B. beside an SDR ──
console.log('\nB. a full server: a fake RTL beside a Beast feed radio');
{
  const fport = await freePort();
  const fsrv = feeder(fport);
  let P = 0;
  for (let i = 0; i < 20 && !P; i++) {
    const p = await freePort();
    const free = await Promise.all([p + 1, p + 2].map((q) => new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(q, '127.0.0.1', () => s.close(() => r(true))); })));
    if (free.every(Boolean)) P = p;
  }
  const ADMIN = 'beast-full-test', dir = mkdir('full');
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Beast beside an SDR', adminPass: ADMIN, port: P,
    radios: [
      { serial: 'bfx-rtl', driver: 'rtlsdr', label: 'RTL', enabled: true, configured: true, users: 1, rate: 2048000 },
      { serial: 'beast-1', driver: 'beast', label: 'PiAware', beastIn: `127.0.0.1:${fport}`, enabled: true, configured: true, users: 4 },
    ],
  }));
  let rtlPort = 0;
  while (!rtlPort || (rtlPort >= P && rtlPort <= P + 2)) rtlPort = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
  kids.push(rtl);
  await sleep(300);
  const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: path.join(dir, 'config.json'),
                VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logs = ['door', 'rtl', 'beast'].map((n) => path.join(dir, `${n}.log`));
  const out = (i) => ['ignore', fs.openSync(logs[i], 'w'), fs.openSync(logs[i], 'a')];
  const door = spawn(BIN, [], { env, stdio: out(0) });
  const r1 = spawn(BIN, ['--radio-serial', 'bfx-rtl', '--tcp', `127.0.0.1:${rtlPort}`, '--freq', '96100000', '--mode', 'wfm'], { env, stdio: out(1) });
  const r2 = spawn(BIN, ['--radio-serial', 'beast-1'], { env, stdio: out(2) });   // ★ exactly as vibeserver@beast-1 starts it
  kids.push(door, r1, r2);
  const LG = (i) => { try { return fs.readFileSync(logs[i], 'utf8'); } catch { return ''; } };
  try {
    ok(await up(P) && await up(P + 1) && await up(P + 2), `the door (${P}), the RTL (${P + 1}) and the Beast feed radio (${P + 2}) are up`);
    ok(/Beast feed radio started/.test(LG(2)), 'the Beast radio process started with no SDR');
    const H = `http://127.0.0.1:${P}`;
    const q = await adminQ(H, ADMIN);
    const st = await until(async () => {
      try { const j = await (await fetch(`${H}/r/beast-1/vibeserver/admin/adsbfeed?${q}`)).json(); return j.input && j.input.state === 'connected'; } catch { return false; }
    }, 8000);
    ok(st, 'its admin status through the door: the input connected');
    const B = ws(P + 2, `/ws/user-spectrum?user_session_id=${crypto.randomUUID()}&mode=binary8&bins=256&proto=1`);
    await until(() => parsed(B, 'adsb').some((m) => (m.aircraft || []).length), 6000);
    ok(parsed(B, 'adsb').some((m) => (m.aircraft || []).some((a) => a.hex === '4840d6')), 'a listener on the Beast radio sees the aircraft');
    B.close();
    const C = ws(P + 1, `/ws/user-spectrum?user_session_id=${crypto.randomUUID()}&mode=binary8&bins=256&proto=1`);
    await until(() => C.bin > 3, 6000);
    ok(C.bin > 3 && !(parsed(C, 'hwinfo').pop() || {}).onlyMode, `the RTL beside it is an ordinary receiver (waterfall frames ${C.bin}, no only mode)`);
    C.close();
  } finally { for (const k of [door, r1, r2, rtl]) { try { k.kill('SIGTERM'); } catch {} } fsrv.kill(); }
}
// ── C. ADD and REMOVE through the Setup page's own request ──
console.log('\nC. adding and removing a Beast feed radio (POST /vibeserver/sdr-change)');
{
  const dir = mkdir('add'), port = await freePort(), ADMIN = 'beast-add-test';
  const cfgPath = path.join(dir, 'config.json');
  fs.writeFileSync(cfgPath, JSON.stringify({ configured: true, adminPass: ADMIN, port,
    radios: [{ serial: 'bfa-rtl', driver: 'rtlsdr', label: 'RTL', enabled: true, configured: true }] }));
  const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: cfgPath, VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const door = spawn(BIN, [], { env, stdio: 'ignore' });
  kids.push(door);
  try {
    ok(await up(port), 'the door is up');
    const H = `http://127.0.0.1:${port}`;
    const post = async (body) => {
      const nonce = (await fetch(`${H}/vibeserver/auth`).then((r) => r.json())).nonce;
      const q = `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
      const r = await fetch(`${H}/vibeserver/sdr-change?${q}`, { method: 'POST', body: JSON.stringify(body) });
      return { status: r.status, j: await r.json().catch(() => ({})) };
    };
    let r = await post({ action: 'addbeast', beastIn: '192.168.86:30005', label: 'x' });
    ok(r.status === 400 && /IPv4/.test(r.j.error || ''), `a bad address is refused in words (${r.j.error})`);
    r = await post({ action: 'addbeast', beastIn: '192.168.86.12:30005', label: 'PiAware in the loft' });
    ok(r.status === 200 && r.j.ok, 'added');
    const c1 = JSON.parse(fs.readFileSync(cfgPath, 'utf8'));
    const b = (c1.radios || []).find((x) => x.driver === 'beast');
    ok(!!b && b.serial === 'beast-1' && b.beastIn === '192.168.86.12:30005' && b.label === 'PiAware in the loft' && b.enabled && b.configured,
       `a radio entry of its own: ${b ? JSON.stringify({ serial: b.serial, beastIn: b.beastIn, label: b.label }) : 'none'}`);
    ok((c1.radios || []).some((x) => x.serial === 'bfa-rtl'), 'the RTL is untouched');
    await sleep(2500);   // ★ the restart it asked for
    await up(port);
    r = await post({ action: 'remove', serial: 'beast-1' });
    const c2 = JSON.parse(fs.readFileSync(cfgPath, 'utf8'));
    ok(r.status === 200 && !(c2.radios || []).some((x) => x.driver === 'beast'), `removed like any radio (${r.status} ${r.j.error || ''})`);
  } finally { door.kill('SIGTERM'); }
}
console.log(`\ntest-server-beast-feed: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
