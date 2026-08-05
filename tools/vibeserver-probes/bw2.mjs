import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1', BINS=process.env.BINS||'4096';
let B=0, F=0, t0=Date.now();
const key=crypto.randomBytes(16).toString('base64'); const s=net.connect(PORT,HOST);
let hs=false, buf=Buffer.alloc(0);
s.on('connect',()=>s.write(`GET /ws/user-spectrum?user_session_id=bw1&bins=${BINS} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
s.on('data',d=>{ if(hs) B+=d.length;
  buf=Buffer.concat([buf,d]); if(!hs){const j=buf.indexOf('\r\n\r\n'); if(j<0)return; hs=true; buf=buf.subarray(j+4);}
  for(;;){ if(buf.length<2)break; const op=buf[0]&0x0f; let len=buf[1]&0x7f,off=2;
    if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;} else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
    if(buf.length<off+len)break; const p=buf.subarray(off,off+len); buf=buf.subarray(off+len);
    if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);continue;}
    if(op===0x2){F++; if(F===3) console.log('  binary frame size:', len, 'bytes');}
    if(op===0x1 && F<1) console.log('  text:', p.toString().slice(0,100)); }});
setTimeout(()=>{B=0;F=0;t0=Date.now();},5000);
setTimeout(()=>{ const sec=(Date.now()-t0)/1000;
  console.log(`\nbins=${BINS}: ${F} frames in ${sec.toFixed(0)}s = ${(F/sec).toFixed(1)} fps`);
  console.log(`  ${(B/1024/sec).toFixed(1)} KB/s = ${(B*8/1e6/sec).toFixed(2)} Mb/s per spectrum listener`);
  process.exit(0);},25000);
