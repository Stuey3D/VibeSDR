// ★★ EDITING A SERVER BOOKMARK, END TO END (V12, GitHub discussion #59) — the real vibeserver's POST /bookmarks?was=…
// Gesiima could add and delete the receiver's bookmarks but not change one: a corrected frequency or passband meant
// stopping the service and hand-editing bookmarks-<serial>.json.
//   1. admin-only: a LAN visitor without the password is refused (401) and nothing changes;
//   2. edit in place — name, mode and passband change, the frequency stays, no duplicate;
//   3. a MOVE — the old frequency is gone, the new one is there, in the reply AND in the file on disk, once;
//   4. validation refuses with 4xx + {"error":"…"} in words: no name, frequency 0 / junk / past 100 GHz, an unknown
//      mode, one edge only, low ≥ high, an edge past ±500 kHz, the old row missing (404), moving onto another row (409) —
//      and a refused edit changes nothing;
//   5. "the mode's default" = no edges: an edit without them clears a saved passband;
//   6. a LEARNED row (RDS) that is edited becomes manual, marked as the owner's name (nameSrc 4), and survives
//      "Clear learned"; a learned DAB service keeps its service id when it stays DAB;
//   7. a later RDS learn for the same frequency does NOT rename it — with a control: the same station, unedited, IS
//      learned from the fake dongle's RDS (so the protection is not just learning being off);
//   8. the old one-row add is unchanged (same frequency replaces, no duplicate; no `was`, no new rules).
// ★ SILENT: no audio is played (the listener socket is the spectrum one). Loopback only.
// usage: VIBESERVER_BIN=… node scripts/test-bookmark-edit.mjs
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN)'); process.exit(3); }

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
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
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); }
    }
  });
  return {
    send: (o) => {
      const b = Buffer.from(JSON.stringify(o));
      const h = Buffer.alloc(b.length < 126 ? 6 : 8);
      h[0] = 0x81;
      if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
      s.write(Buffer.concat([h, b]));
    },
    close: () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); },
  };
}

const PASS = 'bm-edit-admin-pass';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bmedit-'));
const dataDir = path.join(dir, 'data');
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'Bookmark Edit Test', locator: 'IO92nh', adminPass: PASS,
  radios: [{ serial: 'bmedit-test', label: 'Test radio', enabled: true, configured: true, users: 3 }],
}));
const rtlPort = await freePort(), port = await freePort();
// ★ One RDS station on the dial (PI C201, PS "TESTFM") — the learn the edited row must withstand, and its control.
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000',
                                     '--fm-rds', '96.1:C201:TESTFM__'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: dataDir, VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const base = `http://127.0.0.1:${port}`;
let srv = null;
const up = () => new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
async function start() {
  const log = fs.openSync(path.join(dir, 'server.log'), 'a');
  srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '3'],
              { env, stdio: ['ignore', log, log] });
  for (let i = 0; i < 150; i++) { if (await up()) return true; await sleep(100); }
  return false;
}
async function stop() {
  if (!srv) return;
  const s = srv; srv = null;
  const gone = new Promise((r) => s.once('exit', r));
  s.kill('SIGTERM');
  await Promise.race([gone, sleep(10_000)]);
  for (let i = 0; i < 50 && await up(); i++) await sleep(100);
}
const auth = async () => {
  const nonce = (await fetch(`${base}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', PASS).update(nonce).digest('hex')}`;
};
const lanIp = Object.values(os.networkInterfaces()).flat().find((a) => a && a.family === 'IPv4' && !a.internal)?.address;
const list = () => fetch(`${base}/bookmarks`, { cache: 'no-store' }).then((r) => r.json());
const bmFile = () => fs.readdirSync(dataDir).filter((f) => /^bookmarks.*\.json$/.test(f)).map((f) => path.join(dataDir, f))[0];
const onDisk = () => JSON.parse(fs.readFileSync(bmFile(), 'utf8'));
/** POST /bookmarks?<q> as the owner (or as a LAN visitor without the password). */
const post = async (q, visitor = false) => {
  const host = visitor ? lanIp : '127.0.0.1';
  const r = await fetch(`http://${host}:${port}/bookmarks?${visitor ? '' : (await auth()) + '&'}${q}`, { method: 'POST' });
  const text = await r.text();
  return { status: r.status, text, json: (() => { try { return JSON.parse(text); } catch { return null; } })() };
};
const at = (l, hz) => l.filter((b) => Math.abs(b.frequency - hz) < 500);
const now = Math.floor(Date.now() / 1000);

try {
  ok(await start(), 'the server starts');
  await fetch(`${base}/bookmarks/import?${await auth()}`, { method: 'POST', body: JSON.stringify([{ name: 'SEED', frequency: 99_000_000, mode: 'wfm' }]), headers: { 'Content-Type': 'application/json' } });
  await stop();
  const file = bmFile();
  ok(!!file, `the bookmark file exists (${file && path.basename(file)})`);
  fs.writeFileSync(file, JSON.stringify([
    { frequency: 7_150_000, name: 'NET 40M', pi: -1, lastHeard: now, manual: true, mode: 'lsb', source: 'server' },
    { frequency: 14_074_000, name: 'FT8 20M', pi: -1, lastHeard: now, manual: true, mode: 'usb', bandwidth_low: 0, bandwidth_high: 3000, source: 'server' },
    { frequency: 198_000, name: 'RADIO 4 LW', pi: -1, lastHeard: now, manual: true, mode: 'am', source: 'server' },
    { frequency: 97_700_000, name: 'LEARNT 97.7', pi: 0xC777, lastHeard: now, manual: false, mode: 'wfm', source: 'server' },
    { frequency: 222_064_000, name: 'DAB SERVICE', pi: -1, lastHeard: now, manual: false, mode: 'dab', sid: 0xC221, eid: 0xC1CE, ecc: 0xE1, source: 'server' },
  ]));
  ok(await start(), 'restart with manual, learned FM and learned DAB rows');

  // ── 1. admin-only ──
  if (lanIp) {
    const v = await post('was=7150000&frequency=7151000&name=HIJACK&mode=lsb', true);
    ok(v.status === 401, `a LAN visitor without the password is refused (${v.status})`);
    ok(at(await list(), 7_150_000)[0]?.name === 'NET 40M', '…and the row is unchanged');
  } else console.log('   (no LAN address — the admin refusal is not checked)');

  // ── 2. edit in place ──
  {
    const r = await post('was=7150000&frequency=7150000&name=' + encodeURIComponent('Net 40m (Sunday)') + '&mode=lsb&bandwidth_low=-2700&bandwidth_high=-300');
    const l = await list();
    const rows = at(l, 7_150_000);
    ok(r.status === 200 && Array.isArray(r.json), `an in-place edit answers 200 with the whole list (${r.status})`);
    ok(rows.length === 1 && rows[0].name === 'Net 40m (Sunday)' && rows[0].bandwidth_low === -2700 && rows[0].bandwidth_high === -300,
       `name and passband changed, no duplicate: ${JSON.stringify(rows)}`);
  }

  // ── 3. a move ──
  {
    const before = (await list()).length;
    const r = await post('was=14074000&frequency=14075500&name=FT8%2020M&mode=usb&bandwidth_low=100&bandwidth_high=2900');
    const l = await list(), d = onDisk();
    ok(r.status === 200 && at(l, 14_074_000).length === 0, 'moved: the old frequency is gone');
    ok(at(l, 14_075_500).length === 1 && at(l, 14_075_500)[0].bandwidth_high === 2900, 'the new frequency is there, with its passband');
    ok(l.length === before, `the list did not grow (${before} → ${l.length})`);
    ok(at(d, 14_074_000).length === 0 && at(d, 14_075_500).length === 1, 'the file on disk says the same (one save, old gone + new present)');
  }

  // ── 4. validation ──
  {
    const snap = JSON.stringify(await list());
    const cases = [
      ['no name', 'was=198000&frequency=198000&name=&mode=am', 400, /name/i],
      ['a name of only spaces', 'was=198000&frequency=198000&name=%20%20&mode=am', 400, /name/i],
      ['frequency 0', 'was=198000&frequency=0&name=X&mode=am', 400, /frequency/i],
      ['frequency junk', 'was=198000&frequency=abc&name=X&mode=am', 400, /frequency/i],
      ['negative frequency', 'was=198000&frequency=-5&name=X&mode=am', 400, /frequency/i],
      ['past 100 GHz', 'was=198000&frequency=200000000000&name=X&mode=am', 400, /100 GHz/],
      ['an unknown mode', 'was=198000&frequency=198000&name=X&mode=wobble', 400, /mode/i],
      ['no mode', 'was=198000&frequency=198000&name=X', 400, /mode/i],
      ['one edge only', 'was=198000&frequency=198000&name=X&mode=am&bandwidth_low=-4000', 400, /both/i],
      ['low ≥ high', 'was=198000&frequency=198000&name=X&mode=am&bandwidth_low=4000&bandwidth_high=-4000', 400, /low edge/i],
      ['low = high', 'was=198000&frequency=198000&name=X&mode=am&bandwidth_low=0&bandwidth_high=0', 400, /low edge/i],
      ['an edge past 500 kHz', 'was=198000&frequency=198000&name=X&mode=am&bandwidth_low=-600000&bandwidth_high=4000', 400, /500000/],
      ['a non-integer edge', 'was=198000&frequency=198000&name=X&mode=am&bandwidth_low=-4k&bandwidth_high=4000', 400, /whole hertz/i],
      ['the old row missing', 'was=5000000&frequency=5000000&name=X&mode=am', 404, /no longer/i],
      ['moving onto another row', 'was=198000&frequency=7150000&name=X&mode=am', 409, /already a bookmark.*Net 40m/],
    ];
    for (const [what, q, code, re] of cases) {
      const r = await post(q);
      ok(r.status === code && typeof r.json?.error === 'string' && re.test(r.json.error), `${what}: ${r.status} "${r.json?.error ?? r.text}"`);
    }
    ok(JSON.stringify(await list()) === snap, 'a refused edit changes nothing');
  }

  // ── 5. the mode's default ──
  {
    const r = await post('was=14075500&frequency=14075500&name=FT8%2020M&mode=usb');
    const row = at(await list(), 14_075_500)[0];
    ok(r.status === 200 && row && row.bandwidth_low === undefined && row.bandwidth_high === undefined, `no edges = the mode's default: ${JSON.stringify(row)}`);
  }

  // ── 6. learned → manual ──
  {
    const r = await post('was=97700000&frequency=97700000&name=' + encodeURIComponent('My 97.7') + '&mode=wfm');
    const row = at(await list(), 97_700_000)[0];
    ok(r.status === 200 && row?.manual === true && row?.nameSrc === 4 && row?.name === 'My 97.7', `an edited LEARNED row is the owner's now: ${JSON.stringify(row)}`);
    const rd = await post('was=222064000&was_sid=' + 0xC221 + '&frequency=222064000&name=' + encodeURIComponent('My DAB') + '&mode=dab');
    const dab = (await list()).find((b) => b.sid === 0xC221);
    ok(rd.status === 200 && dab?.manual === true && dab?.name === 'My DAB' && dab?.eid === 0xC1CE, `a learned DAB service edited, still DAB, keeps its service id: ${JSON.stringify(dab)}`);
    const c = await fetch(`${base}/vibeserver/admin/clear?what=learned&${await auth()}`, { method: 'POST' });
    const l = await list();
    ok(c.ok && at(l, 97_700_000)[0]?.name === 'My 97.7' && l.some((b) => b.name === 'My DAB'), `"Clear learned" (${c.status}) leaves both edited rows`);
  }

  // ── 7. a later learn does not clobber the edit (with a control) ──
  {
    // Control: listen on 96.1 with no bookmark there — the fake dongle's RDS is learned.
    const a = ws(port, '/ws/user-spectrum?user_session_id=bmedit01&bins=256');
    await sleep(500);
    a.send({ type: 'tune', frequency: 96_100_000, mode: 'wfm' });
    let learned = null;
    for (let t = 0; t < 90 && !learned; t++) {
      await sleep(1000);
      if (t % 5 === 4) a.send({ type: 'tune', frequency: 96_100_000, mode: 'wfm' });
      learned = at(await list(), 96_100_000).find((b) => b.manual === false && /TESTFM/.test(b.name)) ?? null;
    }
    if (!learned) {
      console.log('   (control: the fake RDS was not learned within 90 s — the clobber check below cannot prove anything; skipped)');
      const prov = at(await list(), 96_100_000);
      console.log(`   (what is at 96.1: ${JSON.stringify(prov)})`);
    } else {
      ok(true, `control: an unedited 96.1 IS learned from RDS ("${learned.name}")`);
      const r = await post('was=96100000&frequency=96100000&name=' + encodeURIComponent('Owner name') + '&mode=wfm');
      ok(r.status === 200, 'the owner edits the learned row');
      // Keep listening: the station keeps sending "TESTFM" — enough for another full learn cycle (≥ 30 s).
      for (let t = 0; t < 40; t++) { await sleep(1000); if (t % 5 === 4) a.send({ type: 'tune', frequency: 96_100_000, mode: 'wfm' }); }
      const rows = at(await list(), 96_100_000);
      ok(rows.length === 1 && rows[0].name === 'Owner name' && rows[0].manual === true, `40 s more of the same RDS: still the owner's name, one row: ${JSON.stringify(rows)}`);
    }
    a.close();
  }

  // ── 8. the old add is unchanged ──
  {
    const r = await post('frequency=198000&name=RADIO%204&mode=am');
    const rows = at(await list(), 198_000);
    ok(r.status === 200 && rows.length === 1 && rows[0].name === 'RADIO 4', `a plain POST on an existing frequency replaces it, no duplicate: ${JSON.stringify(rows)}`);
    const legacy = await fetch(`${base}/bookmarks?${await auth()}`, { method: 'POST' });
    ok(legacy.status === 400 && (await legacy.text()) === '', 'and an add with no frequency is still the EMPTY 400 an older client knows');
  }

  // A restart keeps the owner's mark.
  await stop();
  ok(await start(), 'restart');
  const after = at(await list(), 97_700_000)[0];
  ok(after?.manual === true && after?.nameSrc === 4 && after?.name === 'My 97.7', `the edit survives a restart: ${JSON.stringify(after)}`);
} finally {
  await stop().catch(() => {});
  try { rtl.kill('SIGTERM'); } catch {}
}
console.log(`\n   bookmark edit: ${pass} passed, ${fail} failed`);
if (fail) console.log(`   server log: ${path.join(dir, 'server.log')}`);
else fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
