// A NEW SESSION must land on the owner's frequency/mode — even if the last listener left the
// radio somewhere else. But a listener JOINING an existing session must not move anyone.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
function join(name,onCfg){
  const key=crypto.randomBytes(16).toString('base64'); const s=net.connect(PORT,HOST);
  let hs=false,buf=Buffer.alloc(0);
  s.on('connect',()=>s.write(`GET /ws/user-spectrum?user_session_id=${name}&bins=1024 HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error',()=>{});
  s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
  for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
  if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
  if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
  if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
  if(op===0x1){const t=p.toString(); if(t.includes('"config"')) onCfg(JSON.parse(t));}}});
  s.send=(o)=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
    h[0]=0x81; if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
    s.write(Buffer.concat([h,b]));};
  return s;
}
const kHz = c => (c.vfo/1000).toFixed(1);   // the VFO, not the locked VIEW centre
let a=null;
const A=join('sessA', c=>{ if(!a){a=c; console.log(`  session 1 lands on ${kHz(c)} kHz, mode ${c.mode}`);} });
setTimeout(()=>{ console.log('  --- listener tunes away to 14074 ---'); A.send({type:'tune',frequency:14074000,mode:'usb'}); },3000);
setTimeout(()=>{ console.log('  --- listener leaves ---'); A.destroy(); },5000);
setTimeout(()=>{
  let b=null;
  join('sessB', c=>{ if(!b){b=c;
    const ok = Math.abs(c.vfo-7074000)<2000 && c.mode==='usb';
    console.log(`  session 2 lands on ${kHz(c)} kHz, mode ${c.mode}`);
    console.log(ok?'\n  PASS — a new session lands where the owner set it.'
                  :'\n  FAIL — inherited the previous listener\'s frequency.');
    process.exit(ok?0:1);}});
}, 8000);
