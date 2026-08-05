// Bytes/sec a REAL listener costs: spectrum + audio sockets, as a browser opens them.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const BINS=process.env.BINS||'4096';
let specB=0, audioB=0, t0=Date.now();
function open(path, onBytes){
  const key=crypto.randomBytes(16).toString('base64'); const s=net.connect(PORT,HOST);
  let hs=false, buf=Buffer.alloc(0);
  s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error',e=>console.log('err',e.code));
  s.on('data',d=>{ if(hs) onBytes(d.length);
    buf=Buffer.concat([buf,d]); if(!hs){const j=buf.indexOf('\r\n\r\n'); if(j<0)return; hs=true; buf=buf.subarray(j+4);}
    for(;;){ if(buf.length<2)break; const op=buf[0]&0x0f; let len=buf[1]&0x7f,off=2;
      if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;} else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
      if(buf.length<off+len)break; const p=buf.subarray(off,off+len); buf=buf.subarray(off+len);
      if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);} }});
  return s;
}
open(`/ws/user-spectrum?user_session_id=bw&bins=${BINS}`, n=>specB+=n);
setTimeout(()=>open(`/ws/audio?user_session_id=bw&codec=opus`, n=>audioB+=n), 500);
setTimeout(()=>{ specB=0; audioB=0; t0=Date.now(); }, 5000);   // settle, then measure
setTimeout(()=>{
  const s=(Date.now()-t0)/1000;
  const sp=specB/s, au=audioB/s, tot=sp+au;
  console.log(`bins=${BINS}  over ${s.toFixed(0)}s`);
  console.log(`  spectrum : ${(sp/1024).toFixed(1)} KB/s   (${(sp*8/1e6).toFixed(2)} Mb/s)`);
  console.log(`  audio    : ${(au/1024).toFixed(1)} KB/s   (${(au*8/1e6).toFixed(2)} Mb/s)`);
  console.log(`  TOTAL    : ${(tot/1024).toFixed(1)} KB/s  (${(tot*8/1e6).toFixed(2)} Mb/s) per listener`);
  console.log(`  => 110 Mb/s uplink / ${(tot*8/1e6).toFixed(2)} = ${Math.floor(110/(tot*8/1e6))} listeners at 100% of the pipe`);
  process.exit(0);
}, 25000);
