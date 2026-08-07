// ★★★ DOES RELEASING THE RADIO SURVIVE CONTROL CALLS LANDING MID-CLOSE?
//
// reopenDevice() sat unused for months with this note: "it closes and reopens `dev` while the
// HTTP/control threads are still calling rtlsdr_set_gain / tuneHw / setFftRate on that same
// pointer, with nothing serialising them ... it must be done with a test that hammers control
// calls while unplugging." This is that test, for release/reacquire instead of a replug.
//
// ★★ THE ASSERTION IS THAT THE SERVER IS STILL THERE. A use-after-free inside libusb is an
//    ABORT, not an exception — the process dies outright. So the probe hammers, then simply asks
//    whether anything is still answering. Checking gain values would miss the failure entirely.
//
//   usage:  HOST=<pi> node radio-release-hammer.mjs <port> [seconds]
import net from 'node:net'; import crypto from 'node:crypto';
const PORT = Number(process.argv[2] || 48000), SECS = Number(process.argv[3] || 40);
const HOST = process.env.HOST || '127.0.0.1';
function ws(path){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',()=>{}); s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j>=0){hs=true;}}buf=Buffer.alloc(0);});
 s.send=o=>{try{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));}catch{}};return s;}
const alive = async () => { try { const c=new AbortController(); const t=setTimeout(()=>c.abort(),5000);
  const r = await fetch(`http://${HOST}:${PORT}/vibeserver.json`,{signal:c.signal}); clearTimeout(t); return r.ok;
} catch { return false; } };

if (!await alive()) { console.log('server not answering before we started'); process.exit(2); }
console.log(`\nHammering ${HOST}:${PORT} for ${SECS}s — control calls across connect/disconnect cycles`);

let cycles = 0, sent = 0;
const deadline = Date.now() + SECS*1000;
while (Date.now() < deadline) {
  // A listener arrives (forces reacquire), is hammered, then leaves (arms the release).
  const spec = ws('/ws/user-spectrum?user_session_id=hammer&bins=1024');
  const hammer = setInterval(() => {
    // ★ Every control that touches the HARDWARE — the ones that raced the close.
    spec.send({type:'gain', value: 100 + (sent % 300)});
    spec.send({type:'tune', frequency: 7100000 + (sent % 50) * 1000, mode:'usb'});
    spec.send({type:'agc', on: (sent & 1) === 0});
    spec.send({type:'ppm', value: (sent % 5) - 2});
    sent += 4;
  }, 15);
  await new Promise(r => setTimeout(r, 2500));
  clearInterval(hammer);
  spec.destroy();
  cycles++;
  // Long enough for the idle grace to fire a release when one is configured.
  await new Promise(r => setTimeout(r, 1500));
  if (!(await alive())) {
    console.log(`\n  FAIL ★ the server DIED after ${cycles} cycles / ${sent} control messages`);
    process.exit(1);
  }
}
console.log(`  ${cycles} connect/disconnect cycles, ${sent} control messages`);
console.log(await alive() ? '  ok   ★ still serving — no abort, no wedge' : '  FAIL ★ gone');
process.exit(0);
