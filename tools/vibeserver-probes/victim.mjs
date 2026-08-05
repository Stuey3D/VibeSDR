// ONE listener over the network, exactly like a real user, while load runs on the server itself.
// Measures what a HUMAN notices: gaps in the spectrum, gaps in the audio, and whether the audio
// stream is CONTINUOUS. ★ Frame counts alone cannot tell "smooth" from "stuttered then caught up",
// which is the difference between a passing probe and a listener saying it broke up.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const CENTRE=Number(process.env.CENTRE||6500000), HW=15000;
function ws(path,onBin){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',e=>console.log(' err',e.code));
 s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x2&&onBin)onBin(p);}});
 s.send=o=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));};return s;}
const F=CENTRE+HW;
let sT=0,aT=0; const sGap=[],aGap=[]; let aSamples=0;
const spec=ws(`/ws/user-spectrum?user_session_id=victim&bins=1024`,()=>{const n=Date.now();if(sT)sGap.push(n-sT);sT=n;});
// ★ MUST ask for Opus: with uncompressed audio off, the server rightly refuses a networked
//   client that cannot decode it — my first run measured "0% audio" and blamed the server.
setTimeout(()=>ws('/ws/audio?user_session_id=victim&codec=opus',p=>{const n=Date.now();if(aT)aGap.push(n-aT);aT=n;
  if(p[1]===3) aSamples += 960;          // one Opus packet = 20 ms at 48 kHz
  else if(p[1]===0) aSamples += (p.length-6)/2;}),400);
setTimeout(()=>spec.send({type:'tune',frequency:F,mode:'am'}),1200);
let T0=0;
setTimeout(()=>{sGap.length=0;aGap.length=0;aSamples=0;T0=Date.now();},6000);
setTimeout(()=>{
  const secs=(Date.now()-T0)/1000;
  const med=a=>a.length?a.slice().sort((x,y)=>x-y)[Math.floor(a.length/2)]:0;
  const worst=a=>a.length?Math.max(...a):0;
  const over=(a,ms)=>a.filter(x=>x>ms).length;
  console.log(`  spectrum: ${sGap.length} frames  median gap ${med(sGap)} ms  worst ${worst(sGap)} ms  ${over(sGap,300)} gaps >300ms`);
  console.log(`  audio   : ${aGap.length} chunks  median gap ${med(aGap)} ms  worst ${worst(aGap)} ms  ${over(aGap,300)} gaps >300ms`);
  console.log(`  audio continuity: ${(aSamples/secs/48000*100).toFixed(0)}% of real time  (100% = unbroken)`);
  process.exit(0);
},24000);
