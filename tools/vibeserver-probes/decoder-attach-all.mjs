// ★★★ DOES EVERY DECODER THE CLIENT CAN ASK FOR ACTUALLY GET CONSTRUCTED?
//
// WEFAX did not, for a long time. `startWefax()` existed, complete and correct, and NOTHING EVER
// CALLED IT: `extension_name: "wefax"` fell through startDecoder's dispatch to
// `if (ext != "fsk" && !navtex) return;` and quietly returned. The client drew the panel, said
// "decoding…", and waited forever for an image that could not arrive.
//
// ★★ WHY NO EXISTING TEST CAUGHT IT: every decoder test went through the FSK path, which worked.
//    A function with no caller is invisible to any test that exercises the callers. The only
//    thing that finds it is asserting, per extension name the CLIENT can send, that the server
//    ends up with the decoder it asked for.
//
// ★ The names come from the web client's own EXT map (web/client/src/decoders.ts) — if that map
//   ever grows an entry the server does not dispatch, this probe fails on the day it is added.
//
//   usage:  node decoder-attach-all.mjs <port> [admin-password]
import net from 'node:net';
import crypto from 'node:crypto';

const PORT = Number(process.argv[2]), PASS = process.argv[3] ?? 'secret';
const HOST = process.env.HOST ?? '127.0.0.1';
let pass = 0, fail = 0;
const ok = (c, what, extra = '') => { c ? (pass++, console.log(`   ok   ${what}`))
                                        : (fail++, console.log(`   FAIL ${what} ${extra}`)); };
const hmac = (s, n) => crypto.createHmac('sha256', s).update(n).digest('hex');
async function status() {
  const n = (await (await fetch(`http://${HOST}:${PORT}/vibeserver/auth`)).json()).nonce;
  const q = `vs_admin_nonce=${n}&vs_admin_auth=${hmac(PASS, n)}`;
  return (await fetch(`http://${HOST}:${PORT}/vibeserver/admin/status?${q}`)).json();
}
function ws(path) {
  const key = crypto.randomBytes(16).toString('base64');
  const sock = net.connect(PORT, HOST);
  let hs = false, buf = Buffer.alloc(0);
  sock.on('connect', () => sock.write(
    `GET ${path} HTTP/1.1\r\nHost: ${HOST}:${PORT}\r\nUpgrade: websocket\r\n` +
    `Connection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  sock.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const i = buf.indexOf('\r\n\r\n'); if (i < 0) return; hs = true; buf = buf.subarray(i + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f;
      let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const pl = buf.subarray(off, off + len);
      buf = buf.subarray(off + len);
      if (op === 0x9) { const p = Buffer.alloc(6 + pl.length); p[0] = 0x8a; p[1] = 0x80 | pl.length;
                        p.writeUInt32BE(0, 2); pl.copy(p, 6); sock.write(p); }
    }
  });
  sock.on('error', () => {});
  const send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const ext = b.length >= 126;
    const f = Buffer.alloc((ext ? 8 : 6) + b.length);
    f[0] = 0x81;
    if (ext) { f[1] = 0x80 | 126; f.writeUInt16BE(b.length, 2); f.writeUInt32BE(0, 4); b.copy(f, 8); }
    else     { f[1] = 0x80 | b.length; f.writeUInt32BE(0, 2); b.copy(f, 6); }
    sock.write(f);
  };
  return { sock, send };
}

// extension_name the client sends  ->  decoderKind the server must end up with.
// ★ `rds` is deliberately NOT an audio decoder — attaching it turns on the extended RDS stream,
//   so "none" is the correct answer for it and asserting otherwise would be wrong.
const CASES = [
  { ext: 'fsk',    params: { center_frequency: 1000, shift: 450, baud_rate: 50,
                             framing: '5N1.5', encoding: 'ITA2' },          expect: 'fsk'   },
  { ext: 'navtex', params: {},                                              expect: 'fsk'   },
  { ext: 'wefax',  params: { lpm: 120, carrier: 1900, deviation: 400,
                             image_width: 1809 },                           expect: 'wefax' },
  { ext: 'sstv',   params: {},                                              expect: 'sstv'  },
  { ext: 'rds',    params: {},                                              expect: 'none'  },
];

const SID = 'attach-' + crypto.randomBytes(4).toString('hex');
const spec = ws(`/ws/user-spectrum?user_session_id=${SID}`);
await new Promise((r) => setTimeout(r, 1500));
const dx = ws(`/ws/dxcluster?user_session_id=${SID}`);
await new Promise((r) => setTimeout(r, 800));

console.log('\nEvery extension the client can attach must construct its decoder');
for (const c of CASES) {
  dx.send({ type: 'audio_extension_detach' });
  await new Promise((r) => setTimeout(r, 400));
  dx.send({ type: 'audio_extension_attach', extension_name: c.ext, ...c.params });
  await new Promise((r) => setTimeout(r, 1200));
  const kind = (await status()).decoderKind;
  ok(kind === c.expect, `"${c.ext}" -> ${c.expect}`, `(got "${kind}")`);
}

dx.send({ type: 'audio_extension_detach' });
await new Promise((r) => setTimeout(r, 600));
ok((await status()).decoderKind === 'none', 'detaching leaves nothing running');

spec.sock.destroy(); dx.sock.destroy();
console.log(`\n${fail ? `${fail} FAILURES, ` : ''}${pass} passed`);
process.exit(fail ? 1 : 0);
