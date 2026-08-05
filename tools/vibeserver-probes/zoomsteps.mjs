// The zoom must be CONTINUOUS, not two positions. Walk a listener through a ladder of spans and
// check the server reports each one — before this, anything wider than a private channel fell
// back to the shared row at the GLOBAL zoom, so the waterfall jumped 8 MHz -> 20 kHz with nothing
// in between.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const CENTRE=Number(process.env.CENTRE||100e6);
function ws(path,onTxt){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',e=>console.log('  err',e.code));
 s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x1&&onTxt){const t=p.toString(); if(t.includes('"config"'))onTxt(JSON.parse(t));}}});
 s.send=o=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));};return s;}
let cfg=null;
const BINS=1024;
const s=ws(`/ws/user-spectrum?user_session_id=steps&bins=${BINS}`,c=>cfg=c);
const LADDER=[2000000,800000,300000,120000,50000,20000,8000];
const got=[];
(async()=>{
  await new Promise(r=>setTimeout(r,3000));
  for(const span of LADDER){
    s.send({type:'zoom',frequency:CENTRE,binBandwidth:span/BINS});
    await new Promise(r=>setTimeout(r,900));
    got.push({want:span, saw:cfg?cfg.totalBandwidth:0});
  }
  let ok=true;
  for(const g of got){
    const near=Math.abs(g.saw-g.want)/g.want < 0.05;
    if(!near) ok=false;
    console.log(`  asked ${(g.want/1e3).toFixed(0).padStart(5)} kHz -> got ${(g.saw/1e3).toFixed(1).padStart(7)} kHz  ${near?'ok':'WRONG'}`);
  }
  const distinct=new Set(got.map(g=>Math.round(g.saw))).size;
  console.log(`\n  ${distinct} distinct spans out of ${LADDER.length} asked`);
  console.log(ok&&distinct===LADDER.length
    ? '  PASS — zoom is continuous.'
    : '  FAIL — zoom collapses to a few positions.');
  process.exit(ok&&distinct===LADDER.length?0:1);
})();
