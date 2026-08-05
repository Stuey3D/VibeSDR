// The owner blocked wfm + rdsx. A client that asks for wfm anyway must be REFUSED —
// the UI hiding it is not enforcement, and a client can send whatever it likes.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const key=crypto.randomBytes(16).toString('base64'); const s=net.connect(PORT,HOST);
let hs=false,buf=Buffer.alloc(0); const seen=[];
function send(o){const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81; if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));}
s.on('connect',()=>s.write(`GET /ws/user-spectrum?user_session_id=blk&bins=1024 HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
if(op===0x1){const t=p.toString();
  if(t.includes('"hwinfo"')){const m=t.match(/"blocked":\[[^\]]*\]/); console.log('  published:', m?m[0]:'(none)');}
  if(t.includes('mode_blocked')) seen.push(t.trim());}}});
setTimeout(()=>{ console.log('  --- client asks for wfm via "mode" ---'); send({type:'mode',mode:'wfm'}); },3000);
setTimeout(()=>{ console.log('  --- client asks for wfm via "tune" (the sneaky one) ---'); send({type:'tune',mode:'wfm',frequency:6500000}); },4500);
setTimeout(()=>{ console.log('  --- client asks for am (allowed) ---'); send({type:'mode',mode:'am'}); },6000);
setTimeout(()=>{
  console.log('  refusals:', seen.length ? seen.join(' ') : 'NONE');
  const ok = seen.length===2 && seen.every(x=>x.includes('wfm'));
  console.log(ok?'\n  PASS — both routes refused, allowed mode untouched.':'\n  FAIL');
  process.exit(ok?0:1);},8000);
