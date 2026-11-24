// test-server-avdata.mjs — V12 AIRCRAFT DATALINK mode and the HFDL endpoints through the REAL server (docs/v12/AVIATION.md).
//   A fake rtl_tcp replays a synthetic 136.850 MHz VDL2 clip (`test-vdl2 --write-clip`: five aircraft, ACARS over AVLC on
//   three channels, 25 dB). Checked: the capability (avdata, avdataBands, modes, hfdl), entering the mode on VDL2, the
//   aircraft table (ICAO addresses, registrations, flights), NO message body without the owner's opt-in and — with it —
//   still none for a listener while the owner's socket gets it, a tune held, the band step to ACARS (131.675 MHz) and
//   back, the receiver given back exactly as found, the mode refused where the owner switched it off, and
//   /addon/hfdl/aircraft + /addon/hfdl/groundstations (the HFDL map's shape).
// ★★ V12 2026-11-24 ACARS + VDL2 TOGETHER and the owner's 8 MS/s switch ("avdata+8m"): an RTL never sees the combined band
//   or the switch (even ticked); a radio listing 8 MS/s (the fake rtl_tcp given an RSP's rate list, VIBE_TEST_TCP_RATES)
//   at 2 MS/s is NOT offered it with the switch off, and with it on goes to 8 MS/s on entering it and back to 2 MS/s on
//   leaving — and on stepping to a single band.
// Silent: no audio socket is opened.
//   VIBESERVER_BIN=…/vibeserver VIBE_VDL2_CLIP=…/vdl2.cu8 node scripts/test-server-avdata.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_VDL2_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_VDL2_CLIP (test-vdl2 --write-clip v.cu8 2400000)');
  process.exit(3);
}
const RATE = 2400000;
const PASS = 'avdata-test-admin';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
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
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const lastOf = (st, type) => { const a = jsons(st, type); return a[a.length - 1] || null; };

async function server(tag, radioExtra = {}, opt = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Aircraft datalink test', locator: 'IO92nh', adminPass: PASS,
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 4, ...radioExtra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rate = opt.rate || RATE;
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(rate),
                                       ...(opt.noClip ? [] : ['--hf', CLIP, '--hf-centre', '136850000'])], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...(opt.env || {}) };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(rate), '--freq', '96100000', '--mode', 'wfm',
                          '--users', '4', '--admin-pass', PASS], { env, stdio: ['ignore', log, log] });
  const getJson = async (p) => { try { const r = await fetch(`http://127.0.0.1:${port}${p}`, { signal: AbortSignal.timeout(3000) }); return { status: r.status, cors: r.headers.get('access-control-allow-origin'), j: await r.json() }; } catch { return null; } };
  const status = async () => (await getJson('/vibeserver.json'))?.j || null;
  await until(async () => !!(await status()), 15000);
  const L = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
  return { port, dir, status, getJson, L, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function signIn(S, sock) {
  const nonce = (await S.getJson('/vibeserver/auth'))?.j?.nonce || '';
  sock.send({ type: 'admin_unlock', nonce, token: crypto.createHmac('sha256', PASS).update(nonce).digest('hex') });
  return until(() => sock.txt.some((m) => m.includes('"type":"admin","ok":true')), 4000);
}

// ── 1. the mode, bodies off (the default) ─────────────────────────────────────────────────────────────────────────────
{
  console.log('── Aircraft datalink: VDL2 / ACARS bands, bodies off ──');
  const S = await server('avd');
  try {
    const st = await S.status();
    ok(st && st.avdata === true, `vibeserver.json: avdata ${JSON.stringify(st && st.avdata)} ${st && st.avdataWhy || ''}`);
    const ids = (st && st.avdataBands || []).map((b) => b.id);
    ok(ids.join(',') === 'acars,vdl2', `avdataBands ${JSON.stringify(st && st.avdataBands)} — ★ an RTL at 2.4 MS/s: no ACARS + VDL2`);
    ok(st && Array.isArray(st.modes) && st.modes.includes('avdata') && st.modes.includes('hfdl'), `modes lists avdata and hfdl (${st && st.modes && st.modes.join(' ')})`);
    ok(st && st.hfdl === true, `hfdl true on an RTL (direct sampling reaches HF) — ${JSON.stringify(st && st.hfdl)} ${st && st.hfdlWhy || ''}`);
    ok(st && !st.avdataText, 'no avdataText without the owner\'s opt-in');
    const sp = ws(S.port, '/ws/user-spectrum?user_session_id=avdtest01&bins=256&proto=1');
    await until(() => sp.open, 3000);
    await sleep(1200);
    const before = lastOf(sp, 'config');
    sp.send({ type: 'avdata', on: 1, band: 'vdl2' });
    ok(await until(() => !!lastOf(sp, 'avdata'), 6000), 'an avdata report arrives');
    const cfg = lastOf(sp, 'config');
    ok(cfg && Math.abs(cfg.centerFreq - 136.8625e6) < 1, `the radio on 136.8625 MHz (centre ${cfg && cfg.centerFreq})`);
    const five = () => { const r = lastOf(sp, 'avdata'); return r && r.aircraft.filter((a) => a.icao && a.reg).length >= 5 ? r : null; };
    ok(await until(() => !!five(), 30000), `five aircraft, by ICAO and registration: ${(lastOf(sp, 'avdata')?.aircraft || []).map((a) => `${a.icao}/${a.reg}/${a.flight}`).join(', ')}`);
    const r = lastOf(sp, 'avdata');
    const ei = r && r.aircraft.find((a) => a.icao === '4CA97B');
    ok(ei && ei.reg === 'EI-EST' && ei.flight === 'FR1234' && ei.label === 'H1' && /^VDL2 136\.\d{3}$/.test(ei.via), `a row: ${JSON.stringify(ei)}`);
    ok(r && r.band === 'vdl2' && r.bandName === 'VDL2' && r.msgs >= 5, `band vdl2, msgs ${r && r.msgs}`);
    // ★ V12 2026-11-16 the ACARS applications (vibe_acars_apps.h): three aircraft report a position, one an OOOI OUT
    const row = (ic) => r && r.aircraft.find((a) => a.icao === ic);
    const fms = row('4CA97B'), l16 = row('4070E8'), adsc = row('3C6548'), oooi = row('406B90');
    ok(fms && Math.abs(fms.lat - (53 + 21.4 / 60)) < 1e-3 && Math.abs(fms.lon + (2 + 16.4 / 60)) < 1e-3 && fms.alt === 37000 && fms.posSrc === 'FMS' && fms.posTime === '21:20:00',
       `FMS position on EI-EST: ${JSON.stringify(fms)}`);
    ok(l16 && l16.lat === 52.48 && l16.lon === -1.9 && l16.alt === 24000 && l16.posSrc === 'ACARS', `label 16 position on G-JZHU: ${JSON.stringify(l16)}`);
    ok(adsc && Math.abs(adsc.lat - 51.75) < 1e-3 && Math.abs(adsc.lon + 0.5) < 1e-3 && adsc.alt === 36000 && adsc.posSrc === 'ADS-C' && adsc.posTime === ':20:34' && adsc.app === 'ADS-C',
       `ADS-C position on D-AIJH: ${JSON.stringify(adsc)}`);
    ok(oooi && oooi.from === 'EGLL' && oooi.to === 'LEMD' && oooi.oooi === 'out' && oooi.oooiTime === '10:15' && oooi.lat == null, `OOOI on G-EZWK: ${JSON.stringify(oooi)}`);
    ok(r && r.apps && r.apps.pos >= 2 && r.apps.adsc >= 1 && r.apps.oooi >= 1 && r.apps.unparsed >= 1, `the counters: ${JSON.stringify(r && r.apps)}`);
    ok(!sp.txt.some((m) => m.includes('EGLLXYA') || m.includes('#M1BPOS')), '★ the messages themselves (ADS-C hex, FMS text) are bodies: not sent');
    ok(!sp.txt.some((m) => m.includes('TEST MESSAGE BODY')), '★ no message body anywhere without the owner\'s opt-in');
    sp.send({ type: 'tune', frequency: 145000000, mode: 'nfm' });
    await sleep(800);
    ok(Math.abs(lastOf(sp, 'config').centerFreq - 136.8625e6) < 1, 'a tune is ignored in the datalink mode');
    sp.send({ type: 'avdata', on: 1, step: -1 });
    ok(await until(() => { const c = lastOf(sp, 'config'); return c && Math.abs(c.centerFreq - 131.675e6) < 1 && lastOf(sp, 'avdata')?.band === 'acars'; }, 5000),
       `step −1: ACARS, the radio on 131.675 MHz (${lastOf(sp, 'config')?.centerFreq})`);
    sp.send({ type: 'avdata', on: 1, step: -1 });
    await sleep(800);
    ok(lastOf(sp, 'avdata')?.band === 'acars', 'clamped at the first band');
    sp.send({ type: 'avdata', on: 1, step: 1 });
    ok(await until(() => lastOf(sp, 'avdata')?.band === 'vdl2', 5000), 'step +1: back to VDL2');
    sp.send({ type: 'avdata', on: 0 });
    ok(await until(() => jsons(sp, 'avdata_off').length > 0, 4000), 'avdata_off');
    await sleep(800);
    const back = lastOf(sp, 'config');
    ok(back && before && Math.abs(back.vfo - before.vfo) < 1 && back.mode === before.mode, `the receiver given back (vfo ${back && back.vfo} ${back && back.mode} vs ${before && before.vfo} ${before && before.mode})`);
    ok(/\[AVDATA\] mode ON: VDL2/.test(S.L()) && /\[AVDATA\] mode OFF/.test(S.L()), 'the log says ON and OFF');
    // ── the HFDL map's endpoints (nobody has decoded HFDL here: every station, none heard; no aircraft) ──
    const gs = await S.getJson('/addon/hfdl/groundstations');
    ok(gs && gs.status === 200 && gs.cors === '*' && Array.isArray(gs.j) && gs.j.length === 16 && gs.j.some((g) => g.gs_id === 7 && g.location === 'Shannon' && Math.abs(g.lat - 52.7) < 0.2),
       `/addon/hfdl/groundstations: ${gs && gs.j && gs.j.length} stations (CORS ${gs && gs.cors})`);
    ok(gs && gs.j.every((g) => Array.isArray(g.frequencies) && g.frequencies.length === 0 && g.last_heard == null), 'none heard: no frequencies, no last_heard');
    const ac = await S.getJson('/addon/hfdl/aircraft');
    ok(ac && ac.status === 200 && Array.isArray(ac.j) && ac.j.length === 0, `/addon/hfdl/aircraft: [] (VDL2 rows are not HFDL rows) — ${ac && JSON.stringify(ac.j).slice(0, 80)}`);
    sp.close();
  } finally { S.stop(); }
}

// ── 2. the owner's opt-in: bodies to the OWNER only ──────────────────────────────────────────────────────────────────
{
  console.log('── the owner\'s message-body opt-in (avdata+text) ──');
  const S = await server('avdt', { blockedModes: 'avdata+text' });
  try {
    const st = await S.status();
    ok(st && st.avdataText === true, 'vibeserver.json: avdataText true');
    const owner = ws(S.port, '/ws/user-spectrum?user_session_id=avdown01&bins=256&proto=1');
    const guest = ws(S.port, '/ws/user-spectrum?user_session_id=avdgst01&bins=256&proto=1');
    await until(() => owner.open && guest.open, 3000);
    await sleep(800);
    ok(await signIn(S, owner), 'the owner signs in');
    owner.send({ type: 'avdata', on: 1, band: 'vdl2' });
    ok(await until(() => (lastOf(owner, 'avdata')?.aircraft || []).some((a) => /TEST MESSAGE BODY/.test(a.text || '')), 30000), 'the owner\'s report carries the message text');
    await sleep(1500);
    ok(jsons(guest, 'avdata').length > 0 && !guest.txt.some((m) => m.includes('TEST MESSAGE BODY')), `★ a listener never gets it (${jsons(guest, 'avdata').length} reports, none with text)`);
    owner.send({ type: 'avdata', on: 0 });
    owner.close(); guest.close();
  } finally { S.stop(); }
}

// ── 3. switched off by the owner ──────────────────────────────────────────────────────────────────────────────────────
{
  console.log('── the owner switched the aircraft datalink off ──');
  const S = await server('avdoff', { blockedModes: 'avdata' });
  try {
    const st = await S.status();
    ok(st && st.avdata === false && /switched the aircraft datalink off/.test(st.avdataWhy || ''), `avdata false with its reason: ${st && st.avdataWhy}`);
    ok(st && !st.modes.includes('avdata'), 'not in modes');
    const sp = ws(S.port, '/ws/user-spectrum?user_session_id=avdoff01&bins=256&proto=1');
    await until(() => sp.open, 3000);
    sp.send({ type: 'avdata', on: 1 });
    ok(await until(() => jsons(sp, 'avdata_error').length > 0, 4000), `refused: ${JSON.stringify(lastOf(sp, 'avdata_error'))}`);
    ok(!lastOf(sp, 'avdata'), 'no report');
    sp.close();
  } finally { S.stop(); }
}

// ── 4. ACARS + VDL2 together: who sees it, and the owner's 8 MS/s switch ─────────────────────────────────────────────
{
  console.log('── ACARS + VDL2 together: an RTL never; a wider radio only with the owner\'s 8 MS/s switch ──');
  {
    const S = await server('avd8rtl', { blockedModes: 'avdata+8m' });
    try {
      const st = await S.status();
      ok(st && st.avdata === true && (st.avdataBands || []).map((b) => b.id).join(',') === 'acars,vdl2',
         `an RTL with the switch ticked: still only ${JSON.stringify((st && st.avdataBands || []).map((b) => b.id))}`);
      const hw = await S.getJson('/vibeserver/hardware');
      const modes = hw && hw.j && hw.j.caps && hw.j.caps.modes || [];
      ok(modes.includes('avdata') && !modes.includes('avdata+8m'), `an RTL's caps: avdata, never the 8 MS/s switch (${modes.filter((m) => m.startsWith('avdata')).join(' ')})`);
    } finally { S.stop(); }
  }
  const RSP_RATES = '8000000,6000000,5000000,4000000,3000000,2048000,2000000';
  {
    const S = await server('avd8off', {}, { rate: 2000000, noClip: true, env: { VIBE_TEST_TCP_RATES: RSP_RATES } });
    try {
      const st = await S.status();
      ok(st && st.avdata === true && (st.avdataBands || []).map((b) => b.id).join(',') === 'acars,vdl2',
         `a radio listing 8 MS/s, at 2 MS/s, switch OFF: no ACARS + VDL2 (${JSON.stringify((st && st.avdataBands || []).map((b) => b.id))})`);
      const hw = await S.getJson('/vibeserver/hardware');
      ok((hw && hw.j && hw.j.caps && hw.j.caps.modes || []).includes('avdata+8m'), '…and its caps offer the owner the switch');
      const sp = ws(S.port, '/ws/user-spectrum?user_session_id=avd8off1&bins=256&proto=1');
      await until(() => sp.open, 3000);
      await sleep(800);
      sp.send({ type: 'avdata', on: 1, band: 'acars+vdl2' });
      ok(await until(() => /needs a capture of 6\.1 MS\/s or more .* runs at 2\.000 MS\/s \(its owner can let/.test(lastOf(sp, 'avdata_error')?.why || ''), 4000),
         `asking anyway is refused, with the numbers and the way out: ${JSON.stringify(lastOf(sp, 'avdata_error'))}`);
      sp.close();
    } finally { S.stop(); }
  }
  {
    const S = await server('avd8on', { blockedModes: 'avdata+8m' }, { rate: 2000000, noClip: true, env: { VIBE_TEST_TCP_RATES: RSP_RATES } });
    try {
      const st = await S.status();
      const both = (st && st.avdataBands || []).find((b) => b.id === 'acars+vdl2');
      ok(both && both.name === 'ACARS + VDL2' && both.channels === 16 && Math.abs(both.hz - 134.25e6) < 1,
         `switch ON at 2 MS/s: the third band offered — ${JSON.stringify(both)}`);
      const sp = ws(S.port, '/ws/user-spectrum?user_session_id=avd8on01&bins=256&proto=1');
      await until(() => sp.open, 3000);
      await sleep(1000);
      const before = lastOf(sp, 'config');
      ok(before && Math.abs((before.maxBandwidth || 0) - 2e6) < 5e4, `starts at 2 MS/s (${before && (before.maxBandwidth || before.span)})`);
      sp.send({ type: 'avdata', on: 1, band: 'acars+vdl2' });
      ok(await until(() => { const c = lastOf(sp, 'config'); return c && Math.abs(c.centerFreq - 134.25e6) < 1 && (c.maxBandwidth || 0) > 7.9e6; }, 10000),
         `entering ACARS + VDL2: the radio on 134.25 MHz at 8 MS/s (${lastOf(sp, 'config')?.centerFreq} @ ${lastOf(sp, 'config')?.maxBandwidth})`);
      ok(await until(() => /capture raised to 8 MS\/s for ACARS \+ VDL2/.test(lastOf(sp, 'avdata')?.rateNote || ''), 5000),
         `the report says so: "${lastOf(sp, 'avdata')?.rateNote}"`);
      ok(lastOf(sp, 'avdata')?.band === 'acars+vdl2', 'the report names the band');
      ok(/\[AVDATA\] rate 2\.000 -> 8\.000 MS\/s/.test(S.L()), 'the log says the rate went up');
      sp.send({ type: 'avdata', on: 1, band: 'vdl2' });
      ok(await until(() => { const c = lastOf(sp, 'config'); return c && Math.abs(c.centerFreq - 136.8625e6) < 1 && Math.abs((c.maxBandwidth || 0) - 2e6) < 5e4; }, 10000),
         `a single band (VDL2): the owner's 2 MS/s back (${lastOf(sp, 'config')?.maxBandwidth})`);
      ok(await until(() => { const r = lastOf(sp, 'avdata'); return r && r.band === 'vdl2' && !r.rateNote; }, 4000), '…and no rate note');
      sp.send({ type: 'avdata', on: 1, band: 'acars+vdl2' });
      ok(await until(() => (lastOf(sp, 'config')?.maxBandwidth || 0) > 7.9e6, 10000), 'back to ACARS + VDL2: 8 MS/s again');
      sp.send({ type: 'avdata', on: 0 });
      ok(await until(() => jsons(sp, 'avdata_off').length > 0, 4000), 'avdata_off');
      ok(await until(() => { const c = lastOf(sp, 'config'); return c && Math.abs((c.maxBandwidth || 0) - 2e6) < 5e4 && Math.abs(c.vfo - before.vfo) < 1; }, 10000),
         `leaving: 2 MS/s and the listener's dial back (${lastOf(sp, 'config')?.maxBandwidth}, vfo ${lastOf(sp, 'config')?.vfo})`);
      ok(/\[AVDATA\] rate back to the owner's 2\.000 MS\/s/.test(S.L()), 'the log says the rate came back');
      sp.close();
    } finally { S.stop(); }
  }
  {
    // ★ the owner PINNED the rate: never raised, so the combined band is not offered at 2 MS/s even with the switch
    const S = await server('avd8pin', { blockedModes: 'avdata+8m', rateLock: true }, { rate: 2000000, noClip: true, env: { VIBE_TEST_TCP_RATES: RSP_RATES } });
    try {
      const st = await S.status();
      ok(st && (st.avdataBands || []).map((b) => b.id).join(',') === 'acars,vdl2', `the rate pinned: no ACARS + VDL2 (${JSON.stringify((st && st.avdataBands || []).map((b) => b.id))})`);
    } finally { S.stop(); }
  }
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
