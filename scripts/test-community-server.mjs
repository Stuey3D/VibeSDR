#!/usr/bin/env node
// test-community-server.mjs — ★★ V12 beta94 THE COMMUNITY MAP, end to end on a vibeserver BUILT FROM THIS TREE
// (vibe_community.h; docs/v12/COMMUNITY-MAP.md). The fake dongle, a seeded gallery (a DSC call with a position, a NAVTEX
// message from Cullercoats, today's NDB day card naming WOD), and the owner's config. Asserts:
//   A. OFF BY DEFAULT: /vibeserver/community.json is a 404 {"on":false,"why":"off"} and vibeserver.json says nothing.
//   B. OWNER ONLY: a config POST from a remote listener without the admin proof is refused and changes nothing.
//   C. ON (the owner's POST): the endpoint answers — bounded JSON in the server's own formats: DSC from the gallery, the
//      NAVTEX station with rx = null (no receiver position or distances), NDB in the NdbLog shape WITHOUT km / brg; the
//      layers named; Cache-Control + ETag, If-None-Match → 304; the directory's origin echoed for CORS; vibeserver.json
//      says "communityMap":true with the layers (what directory.cpp publishes).
//   D. A PRIVATE GALLERY shares no gallery section; E. A PIN shares nothing ("why":"pin").
// ★ SILENT: no audio. usage: VIBESERVER_BIN=… node scripts/test-community-server.mjs [fixtureOut.json]   (exit 3 = not run)
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
const FIXTURE_OUT = process.argv[2] || '';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const procs = [];
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-community-'));
process.on('exit', () => { for (const p of procs) { try { p.kill('SIGTERM'); } catch {} } try { fs.rmSync(T, { recursive: true, force: true }); } catch {} });
const deadline = setTimeout(() => { console.log('   FAIL deadline (120 s)'); process.exit(1); }, 120000);

const ADMIN = 'cm-admin-pass';
const now = Math.floor(Date.now() / 1000);
const day = new Date(now * 1000).toISOString().slice(0, 10);
const dir = path.join(T, 'srv');
for (const d of ['data', 'run', 'data/gallery']) fs.mkdirSync(path.join(dir, d), { recursive: true });
const cfgPath = path.join(dir, 'config.json');
fs.writeFileSync(cfgPath, JSON.stringify({ configured: true, name: 'Community test', locator: 'IO92nh', adminPass: ADMIN, galleryMb: 50,
  radios: [{ serial: 'cm-test', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2400000 }] }));

// ── the gallery, as the server writes it ──
const gdir = path.join(dir, 'data', 'gallery');
let index = '';
const add = (it, body) => { fs.writeFileSync(path.join(gdir, it.f), body); index += JSON.stringify(it) + '\n'; };
const stamp = (t) => new Date(t * 1000).toISOString().replace(/[-:]/g, '').slice(0, 15).replace('T', '-');
add({ f: `${stamp(now - 600)}-dsc-2187k5-0c01.txt`, k: 'dsc', o: 'background', t: now - 600, hz: 2187500, m: 'usb', d: 'dsc', p: 'Profile 1', r: 'Test radio',
      title: 'DSC DISTRESS from 235012345 · fire, explosion · 50°45.00\'N 001°15.00\'W', lv: 3, plat: 50.75, plon: -1.25 }, 'DSC test call\n');
add({ f: `${stamp(now - 300)}-dsc-2187k5-0c02.txt`, k: 'dsc', o: 'background', t: now - 300, hz: 2187500, m: 'usb', d: 'dsc', p: 'Profile 1', r: 'Test radio',
      title: 'DSC routine from 232000111 (no position)', lv: 0 }, 'DSC test call\n');
add({ f: `${stamp(now - 900)}-navtex-518k0-0a02.txt`, k: 'navtex', o: 'background', t: now - 900, hz: 518000, m: 'usb', d: 'navtex', p: 'Profile 1', r: 'Test radio',
      title: 'G · Cullercoats, United Kingdom 🇬🇧 · Nav warning · #98' }, 'ZCZC GA98\nNAVAREA I TEST\nNNNN\n');
const hms = (t) => new Date(t * 1000).toISOString().slice(11, 19);
const D = ' · ';
const card = `NDB heard log${D}${day} (UTC)\nHeard by every radio on this server — background decoding and listeners in NDB mode.\n` +
  `Updated 12:00 UTC.\n\n2 beacons identified\n\nEach beacon, most recently heard first (last heard${D}first heard${D}ident${D}kHz${D}best SNR${D}distance${D}bearing${D}country${D}station):\n` +
  `${hms(now - 120)}${D}${hms(now - 130)}${D}WOD${D}352.000 kHz${D}26 dB${D}94 km${D}181° S${D}GB${D}Woodley\n` +
  `${hms(now - 200)}${D}${hms(now - 210)}${D}QQX${D}399.000 kHz${D}12 dB${D}-${D}-${D}-${D}(not in the published list)\n`;
add({ f: `${stamp(now - (now % 86400))}-ndb-362k5-0b01.txt`, k: 'ndb', o: 'background', t: now - (now % 86400), hz: 362500, m: '', d: 'ndb', p: '', r: 'Test radio',
      s: `ndbsum-${day}`, title: `NDB${D}${day}${D}2 beacons heard` }, card);
fs.writeFileSync(path.join(gdir, 'index.jsonl'), index);

const rtlPort = await freePort(), SP = await freePort();
procs.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' }));
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: cfgPath, VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const slog = fs.openSync(path.join(T, 'server.log'), 'w');
async function startServer() {
  procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(SP), '--rate', '2400000', '--freq', '93200000', '--mode', 'wfm', '--users', '4',
                         '--trusted-proxies', '127.0.0.1'], { env, stdio: ['ignore', slog, slog] }));
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(SP, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
}
await startServer();
const S = `http://127.0.0.1:${SP}`;
const REMOTE = { 'x-forwarded-for': '198.51.100.7' };   // ★ a listener elsewhere (the tunnel's header; 127.0.0.1 is a trusted proxy)
const adminQ = async () => {
  const nonce = (await fetch(`${S}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
};
const getC = (h = {}) => fetch(`${S}/vibeserver/community.json`, { headers: { ...REMOTE, ...h } });
const status = () => fetch(`${S}/vibeserver.json`, { headers: REMOTE }).then((r) => r.json());
const postCfg = async (patch) => {
  const got = await fetch(`${S}/vibeserver/config?${await adminQ()}`).then((r) => r.json());
  const r = await fetch(`${S}/vibeserver/config?${await adminQ()}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ ...got, ...patch }) });
  return r.status;
};
const until = async (f, ms = 15000) => { for (let t = 0; t < ms; t += 250) { if (await f()) return true; await sleep(250); } return f(); };

try {
  console.log('── A. off by default ──');
  {
    const r = await getC();
    const j = await r.json().catch(() => null);
    ok(r.status === 404 && j && j.on === false && j.why === 'off', `community.json is 404 {"on":false,"why":"off"} (${r.status} ${JSON.stringify(j)})`);
    const st = await status();
    ok(!('communityMap' in st) && !('communityLayers' in st), 'vibeserver.json says nothing about the community map');
  }

  console.log('── B. only the owner can switch it on ──');
  {
    const r = await fetch(`${S}/vibeserver/config`, { method: 'POST', headers: { 'content-type': 'application/json', ...REMOTE }, body: JSON.stringify({ communityMap: true }) });
    ok(r.status === 401 || r.status === 403, `a remote POST without the admin proof is refused (${r.status})`);
    await sleep(1500);
    ok(!JSON.parse(fs.readFileSync(cfgPath, 'utf8')).communityMap, 'the config file is unchanged');
    ok((await getC()).status === 404, 'still off');
  }

  console.log('── C. the owner switches it on ──');
  {
    const st = await postCfg({ communityMap: true });
    ok(st === 200, `the owner's save is accepted (${st})`);
    ok(JSON.parse(fs.readFileSync(cfgPath, 'utf8')).communityMap === true, 'saved as "communityMap": true');
    ok(await until(async () => (await getC()).status === 200), 'community.json answers 200 once the radio has re-read its config');
    const r = await getC({ origin: 'https://vibeserver.vibesdr.net' });
    const text = await r.text();
    const j = JSON.parse(text);
    if (FIXTURE_OUT) fs.writeFileSync(FIXTURE_OUT, text);
    ok(j.on === true && j.v === 1 && Math.abs(j.now - now) < 120, 'the envelope: v 1, on, now');
    ok(JSON.stringify(j.layers) === JSON.stringify(['dsc', 'navtex', 'ndb']), `the layers present, in order (${JSON.stringify(j.layers)})`);
    ok(text.length < 640 * 1024, `bounded (${text.length} bytes)`);
    ok(!('aircraft' in j) && !('ships' in j), 'no aircraft and no ships section (nothing heard) — no empty layer');
    const dsc = (j.dsc && j.dsc.items) || [];
    ok(dsc.some((i) => i.k === 'dsc' && i.plat === 50.75 && i.plon === -1.25 && i.lv === 3), 'DSC: the distress call with its position (gallery.json?kind=dsc format)');
    const nst = (j.navtex && j.navtex.stations) || [];
    ok(nst.length === 1 && nst[0].b1 === 'G' && /Cullercoats/.test(nst[0].name) && Number.isFinite(nst[0].lat), `NAVTEX: Cullercoats placed (${nst.map((s) => s.name)})`);
    ok(j.navtex && j.navtex.rx === null && !nst.some((s) => 'km' in s || 'near' in s), 'NAVTEX: rx null, no distance from the receiver');
    const log = (j.ndb && j.ndb.log) || [];
    ok(log.length === 1 && log[0].ident === 'WOD' && log[0].hz === 352000 && Math.abs(log[0].lat - 51.4528) < 0.01, `NDB: WOD placed from the list; the unlisted one left out (${log.map((b) => b.ident)})`);
    ok(log.every((b) => !('km' in b) && !('brg' in b)), 'NDB: no km / brg');
    ok(!/IO92nh|IO92NH|"rx":\{/.test(text), 'the receiver’s own locator and position appear nowhere');
    ok(/max-age=30/.test(r.headers.get('cache-control') || '') && /^"[0-9a-f]{16}"$/.test(r.headers.get('etag') || ''), `Cache-Control max-age=30 + ETag (${r.headers.get('etag')})`);
    ok(r.headers.get('access-control-allow-origin') === 'https://vibeserver.vibesdr.net', `the directory's origin is echoed (${r.headers.get('access-control-allow-origin')})`);
    const r304 = await getC({ 'if-none-match': r.headers.get('etag') });
    ok(r304.status === 304, `If-None-Match → 304 (${r304.status})`);
    const st2 = await status();
    ok(st2.communityMap === true && JSON.stringify(st2.communityLayers) === JSON.stringify(['dsc', 'navtex', 'ndb']), `vibeserver.json: communityMap + layers (${JSON.stringify(st2.communityLayers)})`);
    // ★ one build per 30 s, whoever asks: ten quick reads, one ETag
    const tags = new Set();
    for (let i = 0; i < 10; i++) tags.add((await getC()).headers.get('etag'));
    ok(tags.size === 1, 'ten reads inside the cache window = one body');
  }

  console.log('── D. a private gallery shares no gallery section ──');
  {
    ok(await postCfg({ communityMap: true, galleryPublic: false }) === 200, 'gallery made private');
    ok(await until(async () => { const j = await (await getC()).json().catch(() => ({})); return j.on === true && !j.dsc && !j.navtex && !j.ndb; }, 40000),
       'no DSC, NAVTEX or NDB once the gallery is private (after the cache window)');
  }

  console.log('── E. a PIN shares nothing (the server restarted with a PIN in its config) ──');
  {
    const c = JSON.parse(fs.readFileSync(cfgPath, 'utf8'));
    c.pin = '4321'; c.galleryPublic = true; c.communityMap = true;
    fs.writeFileSync(cfgPath, JSON.stringify(c));
    const srv = procs.pop(); srv.kill('SIGTERM');
    await new Promise((r) => srv.once('exit', r));
    await startServer();
    const r = await getC();
    const j = await r.json().catch(() => ({}));
    ok(r.status === 404 && j.why === 'pin', `community.json: 404 "why":"pin" (${r.status} ${JSON.stringify(j)})`);
    const st = await status();
    ok(!('communityMap' in st), 'vibeserver.json says nothing (the directory will not list it as a contributor)');
  }
} catch (e) {
  fail++; console.log('   FAIL ' + (e && e.stack || e));
}
clearTimeout(deadline);
if (fail) { console.log('--- server log (tail) ---'); console.log(fs.readFileSync(path.join(T, 'server.log'), 'utf8').split('\n').slice(-40).join('\n')); }
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
