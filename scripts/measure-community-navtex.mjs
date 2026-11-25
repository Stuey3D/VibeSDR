#!/usr/bin/env node
// measure-community-navtex.mjs — ★ V12 beta94 the COMMUNITY MAP's NAVTEX cost on a REAL day: one full UTC day of Stuart's
// Lenovo gallery (~/VibeSDR-test-audio/navtex-lenovo, outside git) moved into the last 24 h, served by a vibeserver built
// from this tree; prints each section's size (raw and gzipped) and the cold and warm build times. Not a pass/fail test.
//   VIBESERVER_BIN=… node scripts/measure-community-navtex.mjs [lenovoDir]
import fs from 'node:fs'; import os from 'node:os'; import path from 'node:path'; import net from 'node:net'; import http from 'node:http';
import zlib from 'node:zlib'; import { spawn } from 'node:child_process'; import { fileURLToPath } from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN, SRC = process.argv[2] || path.join(os.homedir(), 'VibeSDR-test-audio/navtex-lenovo');
if (!BIN || !fs.existsSync(path.join(SRC, 'index.jsonl'))) { console.log('not run'); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-cm-measure-')); const procs = [];
process.on('exit', () => { for (const p of procs) try { p.kill('SIGTERM'); } catch {} fs.rmSync(T, { recursive: true, force: true }); });
const rows = fs.readFileSync(path.join(SRC, 'index.jsonl'), 'utf8').trim().split('\n').map((l) => JSON.parse(l));
const byDay = new Map(); for (const r of rows) { const d = Math.floor(r.t / 86400); if (!byDay.has(d)) byDay.set(d, []); byDay.get(d).push(r); }
const days = [...byDay.keys()].sort(); const day = days[days.length - 3]; const set = byDay.get(day).sort((a, b) => a.t - b.t);
const now = Math.floor(Date.now() / 1000), shift = now - 600 - (day * 86400 + 86400);
const g = path.join(T, 'data', 'gallery'); fs.mkdirSync(g, { recursive: true }); fs.mkdirSync(path.join(T, 'run'));
let idx = '';
for (const r of set) { const src = path.join(SRC, 'files', r.f); if (!fs.existsSync(src)) continue; fs.copyFileSync(src, path.join(g, r.f)); idx += JSON.stringify({ ...r, t: r.t + shift }) + '\n'; }
fs.writeFileSync(path.join(g, 'index.jsonl'), idx);
fs.writeFileSync(path.join(T, 'config.json'), JSON.stringify({ configured: true, name: 'M', locator: 'IO92nh', adminPass: 'x', galleryMb: 500, communityMap: true,
  radios: [{ serial: 'm', label: 'M', enabled: true, configured: true, users: 4, rate: 2400000 }] }));
const rp = await freePort(), sp = await freePort();
procs.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rp), '--rate', '2400000'], { stdio: 'ignore' }));
await sleep(400);
procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rp}`, '--port', String(sp), '--rate', '2400000', '--freq', '93200000', '--mode', 'wfm'],
  { env: { ...process.env, VIBESERVER_CONFIG: path.join(T, 'config.json'), VIBESERVER_DATA_DIR: path.join(T, 'data'), VIBESERVER_RUNTIME_DIR: path.join(T, 'run') }, stdio: 'ignore' }));
for (let i = 0; i < 100; i++) { if (await new Promise((r) => { const c = net.connect(sp, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break; await sleep(100); }
const get = (q) => new Promise((res) => { const t0 = Date.now(); http.get(`http://127.0.0.1:${sp}/vibeserver/community.json${q}`, { headers: { 'accept-encoding': 'gzip' } }, (r) => {
  const b = []; r.on('data', (c) => b.push(c)); r.on('end', () => res({ ms: Date.now() - t0, gz: Buffer.concat(b).length, body: zlib.gunzipSync(Buffer.concat(b)).toString() })); }); });
let a = await get('?s=navtex');
for (let i = 0; i < 20 && JSON.parse(a.body).nw === undefined; i++) { await sleep(3000); a = await get('?s=navtex'); }   // the warnings index backfills 400 a request
await sleep(121000);
const cold = await get('?s=navtex'), warm = await get('?s=navtex');
const j = JSON.parse(cold.body);
console.log(`a real day: ${set.length} receptions (UTC day ${new Date(day * 86400000).toISOString().slice(0, 10)})`);
for (const k of ['ns', 'nw', 'nm', 'nx']) if (j[k]) console.log(`  ${k}: ${JSON.stringify(j[k]).length} B raw, ${zlib.gzipSync(JSON.stringify(j[k])).length} B gzipped${k === 'nx' ? `, ${j.nx.length} messages` : k === 'nw' ? `, ${j.nw.warnings.length} warnings` : k === 'nm' ? `, ${j.nm.areas.length} areas` : `, ${j.ns.stations.length} stations`}`);
console.log(`  all NAVTEX: ${cold.body.length} B raw, ${cold.gz} B gzipped on the wire; a rebuild ${cold.ms} ms, from the cache ${warm.ms} ms`);
