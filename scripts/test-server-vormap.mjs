// test-server-vormap.mjs — V12 the VOR / ILS MAP through the REAL server (docs/v12/VOR-MAP.md): a fake rtl_tcp at 2.4 MS/s
// with two VORs synthesised at their own frequencies (`--vor`: HON 113.65 MHz in window 3 of 5, DTY 116.40 MHz in window 5),
// a spectrum socket, and the contract: the capability (vormap, vormapWins, modes), entering, the radio on window 1, the sweep
// stepping window by window, both VORs found in their own windows with their radials and idents and named from the
// published list (DTY Daventry; IO92nh is 21 km from it), the radio held (a tune ignored — NDB's shape), the waterfall at
// 1 fps, the shared dial (a second listener on a shared radio with "ask before tuning" cannot enter it), the only mode
// (the owner's ticks leave only the VOR map: a listener lands in it, their "off" refused), and leaving with the receiver
// given back exactly as found. Also: an HF-only… no — a radio held off VHF by the owner's allowed ranges offers no VOR map at
// all. ★ The dwell is shortened for the test (VIBE_VORMAP_DWELL_S=20; the mode's own is 30 s). Silent: no audio socket is
// read aloud. Needs VIBESERVER_BIN.
import net from 'node:net';
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
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };
const angDiff = (a, b) => Math.abs(((a - b + 540) % 360) - 180);
// ★ magnetic radials towards IO92nh's centre: DTY 47.8° true − (−4.0°) variation = 51.8° (Stuart's Pi 500 read ~054°)
const DTY_RADIAL = 51.8, HON_RADIAL = 88.0;
const DWELL = 20;   // ★ the ident every 7 s: two whole readings in a window (the listed station read twice names it)

async function server(extra = {}, users = 1, args = []) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-vormap-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'VOR map test', locator: 'IO92nh', adminPass: 'vormap-test-admin',
    radios: [{ serial: 'vormap-test', label: 'Test radio', enabled: true, configured: true, users, ...extra }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--tones', '0',
                                       '--vor', `113.65:HON:${HON_RADIAL}:0.22,116.40:DTY:${DTY_RADIAL}:0.22`], { stdio: 'ignore' });
  await sleep(400);
  const e = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'),
              VIBE_VORMAP_DWELL_S: String(DWELL) };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', String(users), ...args],
                    { env: e, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  const stop = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill(); } catch {} };
  return { dir, port, status, stop, log: () => fs.readFileSync(path.join(dir, 'server.log'), 'utf8') };
}
function listener(port, tag) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${tag}${Math.random().toString(16).slice(2, 8)}&bins=256`);
  ws.binaryType = 'arraybuffer';
  const c = { last: null, cfg: null, off: false, err: null, msgs: [], hw: {} };
  c.opened = new Promise((r) => { ws.onopen = r; });
  ws.onmessage = (e) => {
    if (typeof e.data !== 'string') return;
    const j = JSON.parse(e.data);
    c.msgs.push(j.type);
    if (j.type === 'vormap') c.last = j;
    if (j.type === 'vormap_off') c.off = true;
    if (j.type === 'vormap_error') c.err = j.why;
    if (j.type === 'config') c.cfg = j;
    if (j.type === 'hwinfo') c.hw = j;
  };
  c.send = (o) => ws.send(JSON.stringify(o));
  c.close = () => { try { ws.close(); } catch {} };
  return c;
}

// ── 1. a radio the owner holds off VHF offers no VOR map at all (no dead control) ──
let s = await server({ allowRanges: '87.5-107.9MHz' });
try {
  ok(await until(async () => !!(await s.status()), 15000), 'a radio held to 87.5–107.9 MHz: the server is up');
  const st = await s.status();
  ok(st.vormap === undefined && !(st.modes || []).includes('vormap'), `…and offers NO VOR map at all (vormap ${JSON.stringify(st.vormap)})`);
} finally { s.stop(); await sleep(600); fs.rmSync(s.dir, { recursive: true, force: true }); }

// ── 2. the mode end to end ──
s = await server();
try {
  ok(await until(async () => !!(await s.status()), 15000), 'the server is up');
  const st = await s.status();
  ok(st.vormap === true, `vibeserver.json: vormap ${JSON.stringify(st.vormap)} ${st.vormapWhy || ''}`);
  ok(st.vormapWins === 5 && st.vormapDwell === 30, `5 windows at 2.4 MS/s, 30 s each (the mode's own dwell): ${st.vormapWins} × ${st.vormapDwell}`);
  ok(Array.isArray(st.modes) && st.modes.includes('vormap'), 'modes lists vormap');

  const L = listener(s.port, 'vor');
  await L.opened;
  await sleep(1500);
  const before = L.cfg && { c: L.cfg.centerFreq, v: L.cfg.vfo, m: L.cfg.mode };
  L.send({ type: 'vormap', on: 1 });
  ok(await until(() => !!L.last, 5000), 'a vormap report arrives');
  ok(L.last && L.last.windows === 5 && Array.isArray(L.last.wins) && L.last.wins.length === 5, `the windows: ${L.last && JSON.stringify(L.last.wins)}`);
  ok(await until(() => L.cfg && Math.abs(L.cfg.centerFreq - 108975000) < 50000, 5000), `the radio is on window 1 (108.975 MHz): centre ${L.cfg && L.cfg.centerFreq}`);
  const t0 = Date.now();
  ok(await until(() => L.last && L.last.window === 1, (DWELL + 6) * 1000), `the sweep steps to window 2 after the dwell (${((Date.now() - t0) / 1000).toFixed(0)} s)`);
  ok(await until(() => L.cfg && Math.abs(L.cfg.centerFreq - 110975000) < 50000, 3000), `…the radio moved with it: centre ${L.cfg && L.cfg.centerFreq}`);
  const find = (hz) => L.last && L.last.beacons.find((b) => b.hz === hz);
  ok(await until(() => { const b = find(113650000); return b && b.ident === 'HON' && b.radial != null; }, (2 * DWELL + 12) * 1000),
     `HON 113.65 found in window 3: ${JSON.stringify(find(113650000))}`);
  const hon = find(113650000);
  ok(hon && hon.win === 2 && angDiff(hon.radial, HON_RADIAL) <= 2 && hon.station && hon.station.name === 'Honiley' && hon.kind === 'vor',
     `…its radial ${hon && hon.radial}° (want ${HON_RADIAL} ±2), named ${hon && hon.station && hon.station.name}, window ${hon && hon.win}`);
  // ★ ADS-B's / NDB's shape: the receiver is the mode's — a tune is ignored
  L.send({ type: 'tune', frequency: 96100000 });
  await sleep(1200);
  ok(L.cfg && L.cfg.centerFreq > 108e6 && L.cfg.centerFreq < 118e6, `a tune is ignored: radio still ${L.cfg && L.cfg.centerFreq}`);
  ok(/tune ignored — the VOR map owns the receiver/.test(s.log()), 'the server says why the tune was ignored');
  ok(/engine fft rate: 1\.0 fps/.test(s.log()) || /fft rate [\d.]+ -> 1\.0 fps/.test(s.log()), 'the waterfall is throttled to 1 fps behind the map');
  ok(await until(() => { const b = find(116400000); return b && b.ident === 'DTY' && b.radial != null; }, (3 * DWELL + 15) * 1000),
     `DTY 116.40 found in window 5: ${JSON.stringify(find(116400000))}`);
  const dty = find(116400000);
  ok(dty && dty.win === 4 && angDiff(dty.radial, DTY_RADIAL) <= 2 && dty.station && dty.station.ident === 'DTY' && dty.station.name === 'Daventry'
     && dty.station.km10 === 20 && Math.abs(dty.trueBrg - (DTY_RADIAL - 4.0)) < 0.2,
     `…radial ${dty && dty.radial}° magnetic (want ${DTY_RADIAL} ±2), true ${dty && dty.trueBrg}°, Daventry ~${dty && dty.station && dty.station.km10} km`);
  // ★★★ beta94 the report says WHY it was trusted. The server knows its place (IO92nh), and the fake sends DTY's radial from
  //    there, so once "DTY" has been read the bearing is a second witness: idBy is 'radial' (one read + the radial, the
  //    expected 051.8°), or 'reads' / 'votes' when a second reading came first — every way carries its count of reads.
  ok(dty && ['radial', 'reads', 'votes'].includes(dty.idBy) && (dty.reads >= 1 || dty.votes >= 3)
     && (dty.idBy !== 'radial' || (dty.reads === 1 && Math.abs(dty.expRadial - DTY_RADIAL) < 0.3 && angDiff(dty.idRadial, DTY_RADIAL) <= 2)),
     `★ beta94 why DTY was trusted: idBy ${dty && dty.idBy}, reads ${dty && dty.reads}, expected ${dty && dty.expRadial}°, measured ${dty && dty.idRadial}°`);
  const seen = [...new Set((s.log().match(/\[VORMAP\] window (\d) of 5/g) || []))].join(', ');
  ok(/window 2 of 5/.test(seen) && /window 3 of 5/.test(seen) && /window 4 of 5/.test(seen) && /window 5 of 5/.test(seen), `every window visited: ${seen}`);
  ok(L.last && L.last.heard === 2 && L.last.beacons.length === 2, `nothing else heard on 200 channels (dropped ${L.last && L.last.dropped}; ${L.last && L.last.heard}: ${L.last && L.last.beacons.map((b) => `${b.hz}/${b.ident || b.reading}`).join(' ')})`);
  ok(L.last.beacons.every((b) => b.fresh === true), 'both bright (heard this sweep)');

  L.send({ type: 'vormap', on: 0 });
  ok(await until(() => L.off, 4000), 'vormap_off');
  ok(await until(() => before && L.cfg && L.cfg.centerFreq === before.c && L.cfg.vfo === before.v && L.cfg.mode === before.m, 5000),
     `the receiver is back exactly as found: ${JSON.stringify(before)} → ${L.cfg && JSON.stringify({ c: L.cfg.centerFreq, v: L.cfg.vfo, m: L.cfg.mode })}`);
  // ★★ 11-24 (Stuart, the XCover: "sweep not starting this time" — "it did have an updating status before"): the
  //    report clock counted stream time from the FIRST entry, so after EXIT + re-entry no report was built until the new
  //    stream caught up with the old count — the page sat at "Window 1 · 0 of 30 s · 0 heard" while the radio swept
  L.last = null; L.off = false;
  L.send({ type: 'vormap', on: 1 });
  ok(await until(() => L.last && !L.last.starting && L.last.inWin >= 2, 8000),
     `★ re-entered: the report runs again at once (inWin ${L.last && L.last.inWin}, starting ${L.last && L.last.starting})`);
  L.send({ type: 'vormap', on: 0 });
  ok(await until(() => L.off, 4000), 'vormap_off again');
  L.close();
} finally {
  s.stop(); await sleep(800);
  if (fail) console.log(`   (server log kept: ${path.join(s.dir, 'server.log')})`); else fs.rmSync(s.dir, { recursive: true, force: true });
}

// ── 3. the shared dial: ask before tuning — a second listener cannot move the room into it ──
s = await server({ askBeforeTuning: true }, 4, ['--ask-before-tuning']);
try {
  ok(await until(async () => !!(await s.status()), 15000), 'a shared radio with "ask before tuning": the server is up');
  const A = listener(s.port, 'one'); await A.opened; await sleep(1500);
  const B = listener(s.port, 'two'); await B.opened; await sleep(1500);
  B.send({ type: 'vormap', on: 1 });
  await sleep(2500);
  ok(!B.last && !A.last && !/\[VORMAP\] mode ON/.test(s.log()), '★ the second listener\'s "vormap on" moves nothing (gated as NDB\'s is)');
  ok(/vormap from session \[[^\]]*\] refused/.test(s.log()), '…refused at the socket, and logged');
  A.close(); B.close();
} finally { s.stop(); await sleep(600); if (!fail) fs.rmSync(s.dir, { recursive: true, force: true }); }

// ── 4. the only mode: the owner's ticks leave only the VOR map ──
const DEMODS = ['wfm', 'nfm', 'am', 'sam', 'sau', 'sal', 'ecssu', 'ecssl', 'amst', 'usb', 'lsb', 'cwu', 'cwl', 'drm', 'freedv', 'm17', 'dvsig'];
const WHOLE = ['dab', 'adsb', 'ism', 'ndb', 'ais', 'avdata', 'sondes'];
s = await server({ blockedModes: [...DEMODS, ...WHOLE].join(','), mode: 'single' }, 3);
try {
  ok(await until(async () => !!(await s.status()), 15000), 'the VOR map the only mode left: the server is up');
  const L = listener(s.port, 'only'); await L.opened;
  ok(await until(() => !!L.last, 20000), '★ a new listener LANDS in the VOR map without asking');
  ok(/\[ONLY\] vormap only: a new listener lands in it/.test(s.log()), '…and the server says so');
  ok(await until(() => L.hw.onlyMode === 'vormap', 5000), `hwinfo: onlyMode "vormap" (${JSON.stringify(L.hw.onlyMode)})`);
  L.send({ type: 'vormap', on: 0 });
  ok(await until(() => L.err && /this receiver is VOR \/ ILS map only/.test(L.err), 5000), `★ "VOR map off" refused with the reason (${L.err})`);
  await sleep(1200);
  ok(!L.off, '…and the VOR map stays on');
  L.close();
} finally { s.stop(); await sleep(600); if (!fail) fs.rmSync(s.dir, { recursive: true, force: true }); }

console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
