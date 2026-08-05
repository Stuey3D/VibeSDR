// A zooms in hard; B does not touch anything. B's view must be UNCHANGED.
// Before per-client DSP, zoom was a global: one listener zooming rescaled everyone's waterfall.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
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
let aCfg=null,bCfg=null,bBefore=null;
const A=ws('/ws/user-spectrum?user_session_id=zA&bins=1024',c=>aCfg=c);
const B=ws('/ws/user-spectrum?user_session_id=zB&bins=1024',c=>bCfg=c);
setTimeout(()=>{ bBefore=bCfg && bCfg.totalBandwidth;
  console.log(`  before: A span ${(aCfg.totalBandwidth/1e3).toFixed(1)} kHz   B span ${(bBefore/1e3).toFixed(1)} kHz`);
  console.log('  --- A zooms to ~12 kHz; B does nothing ---');
  A.send({type:'zoom', frequency:100015000, binBandwidth: 12000/1024});
},4000);
// ★ A THIRD listener joining AFTER the zoom is the decisive check: it reads the server's CURRENT
//   state fresh, so if A's zoom had leaked into the global view it would arrive already zoomed.
let cCfg=null;
setTimeout(()=>{ ws('/ws/user-spectrum?user_session_id=zC&bins=1024',c=>{if(!cCfg)cCfg=c;}); },6500);
setTimeout(()=>{
  const aNow=aCfg.totalBandwidth, bNow=bCfg.totalBandwidth;
  console.log(`  after : A span ${(aNow/1e3).toFixed(1)} kHz   B span ${(bNow/1e3).toFixed(1)} kHz`);
  const cNow = cCfg ? cCfg.totalBandwidth : 0;
  console.log(`  new joiner C: span ${(cNow/1e3).toFixed(1)} kHz (must be full, not A's zoom)`);
  const aZoomed = aNow < bBefore/10;
  const bSame   = Math.abs(bNow - bBefore) < 1;
  const cFull   = Math.abs(cNow - bBefore) < 1;
  console.log(aZoomed && bSame && cFull
    ? '\n  PASS — A zoomed, B untouched. Views are per listener.'
    : `\n  FAIL — ${!aZoomed?'A did not zoom':!bSame?"B's view moved when A zoomed":"a new joiner inherited A's zoom (it leaked into the global view)"}`);
  process.exit(aZoomed&&bSame&&cFull?0:1);
},9000);
