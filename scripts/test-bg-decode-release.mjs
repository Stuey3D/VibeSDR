// test-bg-decode-release.mjs — BACKGROUND DECODING and "Let another program use this radio" SHARE THE IDLE RADIO BY TIME
// (Stuart, 2026-10-22: "You cannot release a radio and also hand it over to another app").
//   · outside every profile's hours the release works exactly as configured (Let go after);
//   · at a profile's start a released radio is taken back and decoding runs;
//   · during the hours it is never released;
//   · at the end it stops, gives the dial back, and the release follows after Let go after;
//   · if another program holds the radio at the start: said plainly, retried every 60 s — never a tight loop.
// ★ TEST-ONLY knobs: VIBESERVER_TEST_TCP_RELEASE=1 lets the fake rtl_tcp radio be "released"; while the file named by
//   VIBESERVER_TEST_RADIO_BUSY exists, taking it back fails as if another program held it. The clock is a file (HH:MM).
// SILENT. usage: VIBESERVER_BIN=… node scripts/test-bg-decode-release.mjs   (~2 min — the real 60 s retry is waited out)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bgrel-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const BG = { on: true, waitMin: 5, profiles: [{ centre: 500000, from: '06:00', to: '12:00',
  vfos: [{ hz: 489500, mode: 'usb', dec: 'navtex' }, { hz: 517500, mode: 'usb', dec: 'navtex' }] }] };
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'Release test', adminPass: 'x',
  radios: [{ serial: 'bgrel', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2048000,
             releaseWhenIdle: true, idleGrace: 0, bgDecode: JSON.stringify(BG) }],
}));
const clock = path.join(dir, 'clock'), busy = path.join(dir, 'busy');
fs.writeFileSync(clock, '13:00\n');
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_TEST_TCP_RELEASE: '1', VIBESERVER_TEST_RADIO_BUSY: busy, VIBESERVER_BG_WAIT_SEC: '3',
              VIBESERVER_BG_TEST_CLOCK: clock, VIBESERVER_CONFIG: path.join(dir, 'config.json'),
              VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '7074000', '--mode', 'usb', '--users', '4'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const stop = () => { for (const p of [srv, rtl]) { try { p.kill('SIGTERM'); } catch {} } };
process.on('exit', stop);
const L = () => fs.readFileSync(logPath, 'utf8');
const count = (re, t = L()) => (t.match(new RegExp(re.source, 'g')) || []).length;
const until = async (f, ms = 10000) => { for (let t = 0; t < ms; t += 100) { if (f()) return true; await sleep(100); } return f(); };
const after = (marker) => { const t = L(); const i = t.lastIndexOf(marker); return i < 0 ? '' : t.slice(i); };

try {
  console.log('── outside the profile hours (13:00 UTC): the release works as configured ──');
  ok(await until(() => count(/radio RELEASED/) === 1, 20000), '★ the idle radio is RELEASED (Let go after), as if background decoding were off');
  ok(count(/background decoding: nobody here — profile/) === 0 && count(/background decoding: profile \d, RF centre/) === 0, '...and background decoding did not claim it');

  console.log('── the profile starts (07:00): the released radio is taken back, decoding runs ──');
  fs.writeFileSync(clock, '07:00\n');
  ok(await until(() => count(/radio taken back/) === 1 && count(/profile 1, RF centre 500\.0 kHz, 2 VFO\(s\), 2 decoder\(s\) running/) === 1, 10000),
     '★ taken back at once (nobody has been here for longer than the wait) and Nick\'s profile runs');
  const relInside = count(/radio RELEASED/);
  await sleep(8000);
  ok(count(/radio RELEASED/) === relInside && !/idle park in/.test(after('profile 1, RF centre 500.0 kHz')), '★ during the hours it is never released, and no park is armed');

  console.log('── the profile ends (13:00): stop, the dial back, then the release after Let go after ──');
  fs.writeFileSync(clock, '13:00\n');
  ok(await until(() => /background decoding stopped — no profile covers this time of day/.test(L()) && /the radio is back on 7074\.000 kHz/.test(L()), 6000),
     '★ stopped at the boundary, and the dial is back on 7074.000');
  const endedAt = Date.now();
  ok(await until(() => count(/radio RELEASED/) === relInside + 1, 15000), '★ ...and the release follows, as configured');
  ok((Date.now() - endedAt) >= 2500, `...counted from the profile's end (Let go after: ${((Date.now() - endedAt) / 1000).toFixed(1)} s ≥ the 3 s floor)`);

  console.log('── another program holds the radio when the profile is due: said plainly, retried every 60 s ──');
  fs.writeFileSync(busy, 'held');
  fs.writeFileSync(clock, '08:00\n');
  ok(await until(() => count(/profile 1 is due but another program has the radio — trying again in 60 s/) === 1, 8000),
     `★ "${(L().match(/background decoding: profile 1 is due[^\n]*/) || [''])[0]}"`);
  await sleep(15000);
  ok(count(/profile 1 is due but another program has the radio/) === 1, '★ ...and no tight loop: one attempt in 15 s');
  fs.unlinkSync(busy);
  ok(await until(() => count(/profile 1, RF centre 500\.0 kHz, 2 VFO\(s\)/) === 2, 60000), '★ the other program lets go: the next try (within 60 s) takes it back and runs');
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stop();
  await sleep(300);
}
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) console.log(`   (server log: ${logPath})`);
process.exit(fail ? 1 : 0);
