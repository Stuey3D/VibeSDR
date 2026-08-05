// Listener A asks for 4096 bins. Listener B then joins asking for 128 (as Jr does).
// Does A's waterfall change width underneath it?
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
function join(name,bins,onFrame){
  const key=crypto.randomBytes(16).toString('base64'); const s=net.connect(PORT,HOST);
  let hs=false,buf=Buffer.alloc(0);
  s.on('connect',()=>s.write(`GET /ws/user-spectrum?user_session_id=${name}&bins=${bins} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
    for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
    if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
    if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
    if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
    if(op===0x2)onFrame(len);}});
  return s;
}
let aSize=0,bSize=0;
join('deskA',4096,l=>{aSize=l;});
setTimeout(()=>console.log(`  A alone            : frame ${aSize} bytes  (~${aSize-22} bins)`),4000);
setTimeout(()=>{ console.log('  --- B joins asking for 128 bins (a watch) ---'); join('jrB',128,l=>{bSize=l;}); },5000);
setTimeout(()=>{console.log(`  A after B joined   : frame ${aSize} bytes  (~${aSize-22} bins)`);console.log(`  B (asked for 128)  : frame ${bSize} bytes  (~${bSize-22} bins)`);},11000);
setTimeout(()=>{console.log(`  A 5s later         : ${aSize-22} bins   B: ${bSize-22} bins`);const ok=(aSize-22===4096)&&(bSize-22===128);console.log(ok?'\n  PASS — each listener keeps its own width.':'\n  FAIL');process.exit(ok?0:1);},16000);
