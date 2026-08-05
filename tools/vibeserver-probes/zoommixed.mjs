// Zoom engaged (real DSP bins) with two listeners at DIFFERENT widths.
// The zoom row is produced at the WIDEST width; the narrow listener must be peak-held down from it.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
function join(name,bins,onFrame,onCfg){
  const key=crypto.randomBytes(16).toString('base64'); const s=net.connect(PORT,HOST);
  let hs=false,buf=Buffer.alloc(0);
  s.on('connect',()=>s.write(`GET /ws/user-spectrum?user_session_id=${name}&bins=${bins} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
    for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
    if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
    if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
    if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
    if(op===0x1){const t=p.toString(); if(t.includes('"config"')&&onCfg)onCfg(JSON.parse(t));}
    if(op===0x2)onFrame(len);}});
  s.send=(o)=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
    h[0]=0x81; if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);} else {h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
    s.write(Buffer.concat([h,b.map((c,i)=>c)]));};
  return s;
}
let A=0,B=0,cfgA=null;
const a=join('zA',4096,l=>A=l,c=>{if(!cfgA)cfgA=c;});
const b=join('zB',128,l=>B=l);
setTimeout(()=>{
  console.log(`  wide   : A=${A-22} bins  B=${B-22} bins`);
  // zoom in hard: ask for a span ~1/200 of the capture via binBandwidth
  const bw = (cfgA.totalBandwidth/200)/4096;
  console.log(`  --- A zooms in (binBandwidth ${bw.toFixed(3)} Hz => span ${(bw*4096/1000).toFixed(1)} kHz) ---`);
  a.send({type:'zoom', frequency: 100000000, binBandwidth: bw});
  A=0;B=0;
},6000);
setTimeout(()=>{
  console.log(`  zoomed : A=${A?A-22:0} bins  B=${B?B-22:0} bins`);
  const ok = (A-22===4096)&&(B-22===128);
  console.log(ok?'\n  PASS — zoom serves both widths.':'\n  FAIL');
  process.exit(ok?0:1);
},14000);
