// Does each listener's signal meter measure THEIR OWN frequency?
//
// ★★★ WHAT THIS CATCHES. The meter was computed once from the SHARED VFO and broadcast to
//     everyone — and in per-client mode the shared VFO is nobody's, because a per-client tune goes
//     to that listener's ClientDsp and never touches it. Every listener's meter therefore read a
//     frequency no human had selected. It did not look like a wrong number; it looked like LAG,
//     and then like an INVERSION (with the AGC riding the whole band, a strong signal elsewhere
//     pulls your reading DOWN). Two sessions were spent on the draw rate before the frequency was
//     suspected.
// ★★ So the assertion is COMPARATIVE and cannot be satisfied by a plausible-looking number: two
//    listeners, one parked on a station and one on empty air, must disagree — and in the right
//    direction. A single listener reading "something" proves nothing at all.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const CENTRE=Number(process.env.CENTRE||100e6), HW=15000;
function ws(path,onTxt){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',()=>{});
 s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x1&&onTxt)onTxt(p.toString());}});
 s.send=o=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));};return s;}
const chan={A:[],B:[]};
const grab=(k)=>(t)=>{ if(t.includes('"sig"')){ try{ chan[k].push(JSON.parse(t).chan); }catch{} } };
const ON  = CENTRE + HW;              // the centre synthetic station
const OFF = CENTRE + HW + 60000;      // empty air, well clear of any station
const A = ws('/ws/user-spectrum?user_session_id=sigA&bins=1024', grab('A'));
const B = ws('/ws/user-spectrum?user_session_id=sigB&bins=1024', grab('B'));
setTimeout(()=>{ A.send({type:'tune',frequency:ON, mode:'am'});
                 B.send({type:'tune',frequency:OFF,mode:'am'}); },1200);
setTimeout(()=>{ chan.A=[]; chan.B=[]; },3000);
setTimeout(()=>{
  const med=a=>{const s=[...a].sort((x,y)=>x-y);return s[Math.floor(s.length/2)];};
  if (chan.A.length<5||chan.B.length<5){console.log(`  too few readings (A ${chan.A.length}, B ${chan.B.length})`);process.exit(1);}
  const a=med(chan.A), b=med(chan.B);
  console.log(`  A on the station : ${a.toFixed(1)} dBFS  (${chan.A.length} readings)`);
  console.log(`  B on empty air   : ${b.toFixed(1)} dBFS  (${chan.B.length} readings)`);
  console.log(`  difference: ${(a-b).toFixed(1)} dB`);
  const ok = a - b > 10;
  console.log(ok ? '\n  PASS — each listener\'s meter measures their OWN frequency.'
                 : '\n  FAIL — both listeners read the same level: the meter is not per-listener.');
  process.exit(ok?0:1);
},9000);
