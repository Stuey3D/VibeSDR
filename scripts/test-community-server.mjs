#!/usr/bin/env node
// test-community-server.mjs — ★★ V12 beta94 THE COMMUNITY MAP, end to end on a vibeserver BUILT FROM THIS TREE
// (vibe_community.h; docs/v12/COMMUNITY-MAP.md). The fake dongle, a seeded gallery (a DSC call with a position, a NAVTEX
// message from Cullercoats, today's NDB day card naming WOD), and the owner's config. Asserts:
//   A. OFF BY DEFAULT: /vibeserver/community.json is a 404 {"on":false,"why":"off"} and vibeserver.json says nothing.
//   B. OWNER ONLY: a config POST from a remote listener without the admin proof is refused and changes nothing.
//   C. ON (the owner's POST): the endpoint answers from what the server STORES, with no decoder running — 61 REAL NAVTEX
//      messages (scripts/navtex-fixtures, the Lenovo's) moved into the last 24 h: stations (rx = null), warnings with their
//      points, forecasts (no text), messages one per id (the cleanest copy, a title, the text only from
//      /vibeserver/community/navtex?f=); a DSC call; the NDB day card; the ISM card's MeshCore nodes (MeshCore only, never
//      the receiver line); APRS and sondes REMEMBERED beside the gallery from an earlier run. Compact rows, bounded, gzip,
//      ETag → 304, the directory origin echoed; ?s= builds only those sections; vibeserver.json names the layers.
//   D. The owner leaves NAVTEX out (communityOff) → no NAVTEX sections. E. A PRIVATE GALLERY → none of the gallery's
//      sections. F. A PIN shares nothing ("why":"pin").
// ★ SILENT: no audio. usage: VIBESERVER_BIN=… node scripts/test-community-server.mjs [fixtureOut.json]   (exit 3 = not run)
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import http from 'node:http';
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
// ★ 61 REAL NAVTEX messages (the Lenovo's, scripts/navtex-fixtures) moved into the last 24 h, oldest first as the index is
const NFX = path.join(root, 'scripts/navtex-fixtures');
const nfx = fs.readFileSync(path.join(NFX, 'index.jsonl'), 'utf8').trim().split('\n').map((l) => JSON.parse(l)).reverse();
const nfxT0 = nfx[0].t, nfxSpan = nfx[nfx.length - 1].t - nfxT0 || 1;
nfx.forEach((it, i) => add({ ...it, t: now - 80000 + Math.round((it.t - nfxT0) / nfxSpan * 78000) }, fs.readFileSync(path.join(NFX, 'files', it.f))));
// ★ the same message twice more: a copy with errors after the clean one (the cleanest copy must win)
const ga = 'ZCZC GA98\n251200 UTC NOV\nNAVAREA I 123/25\nWIGHT. BUOY UNLIT\nNNNN\n';
add({ f: `${stamp(now - 7000)}-navtex-518k0-0a10.txt`, k: 'navtex', o: 'background', t: now - 7000, hz: 518000, m: 'usb', d: 'navtex', p: 'Profile 1', r: 'Test radio',
      title: 'G · Cullercoats, United Kingdom 🇬🇧 · Nav warning · #98' }, ga);
add({ f: `${stamp(now - 900)}-navtex-518k0-0a11.txt`, k: 'navtex', o: 'background', t: now - 900, hz: 518000, m: 'usb', d: 'navtex', p: 'Profile 1', r: 'Test radio',
      title: 'G · Cullercoats, United Kingdom 🇬🇧 · Nav warning · #98 · 3 lost' }, ga.replace('UNLIT', 'U**IT').replace('123', '1*3'));
const hms = (t) => new Date(t * 1000).toISOString().slice(11, 19);
const D = ' · ';
const card = `NDB heard log${D}${day} (UTC)\nHeard by every radio on this server — background decoding and listeners in NDB mode.\n` +
  `Updated 12:00 UTC.\n\n2 beacons identified\n\nEach beacon, most recently heard first (last heard${D}first heard${D}ident${D}kHz${D}best SNR${D}distance${D}bearing${D}country${D}station):\n` +
  `${hms(now - 120)}${D}${hms(now - 130)}${D}WOD${D}352.000 kHz${D}26 dB${D}94 km${D}181° S${D}GB${D}Woodley\n` +
  `${hms(now - 200)}${D}${hms(now - 210)}${D}QQX${D}399.000 kHz${D}12 dB${D}-${D}-${D}-${D}(not in the published list)\n`;
add({ f: `${stamp(now - (now % 86400))}-ndb-362k5-0b01.txt`, k: 'ndb', o: 'background', t: now - (now % 86400), hz: 362500, m: '', d: 'ndb', p: '', r: 'Test radio',
      s: `ndbsum-${day}`, title: `NDB${D}${day}${D}2 beacons heard` }, card);
// ★ the ISM decoder's remembered nodes card (vibe_ism_gallery.h writeNodes): two MeshCore nodes, a Meshtastic one, the receiver line
const ismDay = new Date((now - (now % 86400)) * 1000).toISOString().slice(0, 10);
const nodeRow = (f) => f.join(D) + '\n';
add({ f: `${stamp(now - (now % 86400))}-ism-868m0-0d01.txt`, k: 'ism', o: 'background', t: now - (now % 86400), hz: 868000000, m: '', d: 'ism', p: '', r: 'Test radio',
      s: `ismnodes-868-${ismDay}`, title: `ISM nodes heard${D}868${D}${ismDay}` },
    `ISM nodes heard${D}868 MHz${D}${ismDay}\nReceiver:     52.3456, -1.2345\n\nEach node, most recent first (last heard${D}first heard${D}times heard${D}heard by${D}network${D}type${D}name${D}id${D}position${D}hops${D}SNR dB${D}MHz):\n`
    + nodeRow([hms(now - 300), hms(now - 3000), '12×', 'background', 'MeshCore', 'repeater', 'Hilltop RPT', 'a1b2c3d4', '52.12345, -1.54321', '0', '7.5', '869.525'])
    + nodeRow([hms(now - 400), hms(now - 3000), '3×', 'live', 'MeshCore', 'companion', "Bob's phone", 'ffee0011', '52.98765, -1.11111', '1', '2.0', '869.525'])
    + nodeRow([hms(now - 500), hms(now - 3000), '5×', 'live', 'Meshtastic', 'router', 'MT node', '!abcd', '51.50000, -0.10000', '0', '3.0', '869.525']));
fs.writeFileSync(path.join(gdir, 'index.jsonl'), index);
// ★ APRS and a sonde REMEMBERED from an earlier run (vibe_community.h writeLive) — no decoder runs in this test
fs.mkdirSync(path.join(gdir, '.community'), { recursive: true });
fs.writeFileSync(path.join(gdir, '.community', 'live-p-earlier.json'),
  JSON.stringify([['G4ABC-9', 'G4ABC-9', 52.2, -1.3, now - 600, '/>', 'position', 'car 50 km/h'], ['OLD-1', 'OLD-1', 52, -1, now - 30000, '/-', 'position', 'too old']]));
fs.writeFileSync(path.join(gdir, '.community', 'live-z-earlier.json'),
  JSON.stringify([['T1234567', 'RS41-SGP', 52.5, -1.9, 12000, -15.2, now - 900, 28500, -40.1, [[52.4, -2.0], [52.45, -1.95], [52.5, -1.9]]]]));

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
const getC = (h = {}, q = '') => fetch(`${S}/vibeserver/community.json${q}`, { headers: { ...REMOTE, ...h } });
const SIZES = {};
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
    ok(j.on === true && j.v === 2 && Math.abs(j.now - now) < 120, 'the envelope: v 2, on, now');
    ok(JSON.stringify(j.layers) === JSON.stringify(['ndb', 'dsc', 'navtex', 'sondes', 'aprs', 'mesh']), `the layers stored on this server, with no decoder running (${JSON.stringify(j.layers)})`);
    ok(text.length < 640 * 1024, `bounded (${text.length} bytes)`);
    ok(!('a' in j) && !('s' in j) && !('o' in j), 'no aircraft, ships or VOR (nothing stored) — no empty layer');
    ok(Array.isArray(j.d) && j.d.some((x) => x[1] === 50.75 && x[2] === -1.25 && x[3] === 3), 'DSC: the distress call with its position (compact row)');
    const nst = (j.ns && j.ns.stations) || [];
    ok(nst.length >= 5 && nst.some((x) => /Cullercoats/.test(x.name)) && j.ns.rx === null && !nst.some((x) => 'km' in x || 'near' in x),
       `NAVTEX stations from the stored log: ${nst.length} (rx null, no distances)`);
    const nw = (j.nw && j.nw.warnings) || [];
    ok(nw.length >= 1 && nw.every((w) => Array.isArray(w.pts) && w.pts.length >= 1 && /^[A-Z]{2}\d{2}$/.test(w.id)), `NAVTEX warnings with their points: ${nw.length} (${nw.filter((w) => w.shape).length} with an area or route)`);
    const nm = (j.nm && j.nm.areas) || [];
    ok(nm.length >= 1 && nm.every((a) => a.fc.length === 1 && !('txt' in a.fc[0])), `NAVTEX forecasts: ${nm.length} sea areas, the newest only, no text in the poll`);
    const nx = j.nx || [];
    const ga98 = nx.filter((m) => m[0] === 'GA98');
    ok(nx.length >= 30 && ga98.length === 1 && ga98[0][4] === 3 && ga98[0][5] === 0 && !/0a11/.test(ga98[0][9]), `NAVTEX messages: ${nx.length}, GA98 once (heard 3 times: the real one + ours twice) with a CLEAN copy (0 lost), never the one with errors`);
    ok(nx.every((m) => m.length === 11 && typeof m[10] === 'string' && m[10].length <= 80) && !/WIGHT\. BUOY/.test(text), 'titles only — no message text in the poll');
    const one = await fetch(`${S}/vibeserver/community/navtex?f=${encodeURIComponent(ga98[0][9])}`, { headers: REMOTE });
    const oj = await one.json();
    ok(one.status === 200 && /ZCZC GA98/.test(oj.txt) && !/\*/.test(oj.txt) && /max-age=3600/.test(one.headers.get('cache-control') || ''), 'the text when a message is opened (cached an hour)');
    ok((await fetch(`${S}/vibeserver/community/navtex?f=..%2Fconfig.json`, { headers: REMOTE })).status === 404
       && (await fetch(`${S}/vibeserver/community/navtex?f=${encodeURIComponent(j.d[0][5])}`, { headers: REMOTE })).status === 404, 'only a NAVTEX item of the gallery: a path or a DSC item is refused');
    const b = j.b || [];
    ok(b.length === 1 && b[0][0] === 'WOD' && b[0][1] === 352000 && Math.abs(b[0][2] - 51.4528) < 0.01 && b[0].length === 7, `NDB: WOD placed from the list (${b.map((x) => x[0])}), no km/brg`);
    const m = j.m || [];
    ok(m.length === 2 && m.every((x) => x[0] !== '!abcd') && m.some((x) => x[0] === 'ffee0011' && x[3] === 52.99 && x[7] === 1), 'MeshCore: two nodes from the remembered card (not the Meshtastic one), the companion rounded');
    ok(!/52\.3456/.test(text), 'the ISM card’s receiver line is never shared');
    ok((j.p || []).length === 1 && j.p[0][0] === 'G4ABC-9', 'APRS: the remembered station, the one past its 6 h window left out');
    ok((j.z || []).length === 1 && j.z[0][0] === 'T1234567' && j.z[0][9].length === 3, 'sondes: the remembered flight with its track');
    ok(!/IO92nh|IO92NH|"rx":\{/.test(text), 'the receiver’s own locator and position appear nowhere');
    ok(/max-age=30/.test(r.headers.get('cache-control') || '') && /^"[0-9a-f]{16}"$/.test(r.headers.get('etag') || ''), `Cache-Control (the fastest section's TTL) + ETag (${r.headers.get('etag')})`);
    ok(r.headers.get('access-control-allow-origin') === 'https://vibeserver.vibesdr.net', `the directory's origin is echoed (${r.headers.get('access-control-allow-origin')})`);
    const r304 = await getC({ 'if-none-match': r.headers.get('etag') });
    ok(r304.status === 304, `If-None-Match → 304 (${r304.status})`);
    // ★ gzip on the wire (raw bytes, not fetch's decoded body)
    const raw = await new Promise((res) => http.get(`${S}/vibeserver/community.json`, { headers: { ...REMOTE, 'accept-encoding': 'gzip' } }, (rr) => {
      const parts = []; rr.on('data', (c) => parts.push(c)); rr.on('end', () => res({ enc: rr.headers['content-encoding'], n: Buffer.concat(parts).length }));
    }));
    ok(raw.enc === 'gzip' && raw.n < text.length / 3, `gzip on the wire: ${raw.n} bytes for ${text.length} (${(100 * raw.n / text.length).toFixed(0)} %)`);
    SIZES.full = { raw: text.length, gz: raw.n };
    for (const k of Object.keys(j).filter((k) => !['v', 'on', 'now', 'layers'].includes(k))) SIZES[k] = JSON.stringify(j[k]).length;
    const only = await (await getC({}, '?s=navtex')).json();
    ok(Object.keys(only).filter((k) => !['v', 'on', 'now', 'layers'].includes(k)).sort().join() === 'nm,ns,nw,nx', '?s=navtex builds only the NAVTEX sections');
    const st2 = await status();
    ok(st2.communityMap === true && JSON.stringify(st2.communityLayers) === JSON.stringify(j.layers), `vibeserver.json: communityMap + layers (${JSON.stringify(st2.communityLayers)})`);
    const tags = new Set();
    for (let i = 0; i < 10; i++) tags.add((await getC()).headers.get('etag'));
    ok(tags.size === 1, 'ten reads inside the cache window = one body');
  }

  console.log('── D. the owner leaves NAVTEX out ──');
  {
    ok(await postCfg({ communityMap: true, communityOff: 'navtex' }) === 200, 'communityOff: navtex');
    ok(await until(async () => { const j = await (await getC()).json().catch(() => ({})); return j.on === true && !j.ns && !j.nw && !j.nx && !!j.b; }, 20000),
       'no NAVTEX section; the rest still shared');
    ok(await postCfg({ communityMap: true, communityOff: '' }) === 200, 'back to every mode');
  }

  console.log('── E. a private gallery shares no gallery section ──');
  {
    ok(await postCfg({ communityMap: true, galleryPublic: false }) === 200, 'gallery made private');
    ok(await until(async () => { const j = await (await getC()).json().catch(() => ({})); return j.on === true && !j.d && !j.ns && !j.nx && !j.b && !j.m && !j.p && !j.z; }, 20000),
       'no gallery section (DSC, NAVTEX, NDB, MeshCore, the remembered APRS / sondes) once the gallery is private');
  }

  console.log('── F. a PIN shares nothing (the server restarted with a PIN in its config) ──');
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
console.log('   (sizes, bytes: ' + JSON.stringify(SIZES) + ')');
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
