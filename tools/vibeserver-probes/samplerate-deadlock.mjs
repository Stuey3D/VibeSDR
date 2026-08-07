// ★★★ DOES CHANGING THE CAPTURE RATE WEDGE THE WHOLE SERVER?
//
// It did. setSampleRate held clientMtx across sendConfig(), and sendConfig -> binsFor() locks
// clientMtx again — a plain std::mutex, so re-locking on the same thread hung forever. The DSP
// heartbeat stopped, HTTP stopped answering, and the PROCESS STAYED ALIVE, so it presented as a
// crash with no crash report and no log line.
//
// ★★ THE ASSERTION IS LIVENESS, NOT THE RATE. Whether the radio landed on the requested rate is a
//    different question; this asks only whether the server is still answering afterwards. A probe
//    that checked the rate would pass on a server that had already stopped serving everyone else.
//
//   usage:  node samplerate-deadlock.mjs [port]
import net from 'node:net'; import crypto from 'node:crypto';
const PORT = Number(process.argv[2] || 48000), HOST = process.env.HOST || '127.0.0.1';
function ws(path){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',()=>{}); s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j>=0){hs=true;buf=buf.subarray(j+4);}}buf=Buffer.alloc(0);});
 s.send=o=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));};return s;}

const alive = async () => {
  try { const c = new AbortController(); const t = setTimeout(()=>c.abort(), 4000);
        const r = await fetch(`http://${HOST}:${PORT}/vibeserver.json`, {signal:c.signal});
        clearTimeout(t); return r.ok; } catch { return false; }
};

console.log(`\nProbing ${HOST}:${PORT}`);
if (!await alive()) { console.log('  server was not answering before we started'); process.exit(2); }
console.log('  ok   answering before the rate change');

const spec = ws('/ws/user-spectrum?user_session_id=sr&bins=1024');
await new Promise(r => setTimeout(r, 1200));
console.log('  → asking for a different capture rate');
// ★ THE MESSAGE MUST BE THE ONE THE SERVER ACTUALLY HANDLES: type 'sampleRate', field 'value'.
// The first version of this probe sent {type:'samplerate', rate:...} — silently ignored — and so
// it PASSED against the deadlocking build. A probe that names the wire wrong tests nothing.
spec.send({ type: 'sampleRate', value: 1024000 });
await new Promise(r => setTimeout(r, 6000));

const ok = await alive();
console.log(ok ? '  ok   ★ still answering after the rate change'
               : '  FAIL ★ the server WEDGED — alive but answering nothing');
spec.destroy();
process.exit(ok ? 0 : 1);
