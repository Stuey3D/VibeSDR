// Is the signal meter's data FRESH, or merely FREQUENT?
//
// ★★★ THE DISTINCTION THAT MATTERS. Raising 'sig' from 5 Hz to 20 Hz made the meter lag WORSE,
//     because it rode on the never-drop control class: the frames queued, and each reading the
//     client displayed was further behind the radio than the last. Rate alone cannot show that —
//     the messages arrive on time and say something old. So this measures BOTH: how often they
//     arrive, and whether the gap between arrivals stays even or drifts (a growing backlog shows
//     up as arrivals bunching while the values age).
//   HOST=vibeserver.local node sigrate.mjs 48000 [seconds]
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]||48000), SECS=Number(process.argv[3]||30);
const HOST=process.env.HOST||'127.0.0.1';
const key=crypto.randomBytes(16).toString('base64');
const s=net.connect(PORT,HOST); let hs=false,buf=Buffer.alloc(0);
const sig=[], rsp=[], t0=Date.now();
s.on('connect',()=>s.write(`GET /ws/user-spectrum?user_session_id=sigrate&bins=1024 HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x1){const m=p.toString(); const t=Date.now()-t0;
   if(m.includes('"sig"')) sig.push(t); if(m.includes('rspstat')) rsp.push(t);}}});
const stats=(a,name)=>{
  if(a.length<5){console.log(`  ${name}: only ${a.length} frames`);return;}
  const gaps=[]; for(let i=1;i<a.length;i++)gaps.push(a[i]-a[i-1]);
  const mean=gaps.reduce((x,y)=>x+y,0)/gaps.length;
  const sorted=[...gaps].sort((x,y)=>x-y);
  const p95=sorted[Math.floor(sorted.length*0.95)];
  // ★ Compare the FIRST and LAST thirds. A backlog that is building shows as the later gaps
  //   bunching up (the writer racing to drain) while the data itself ages.
  const third=Math.floor(gaps.length/3);
  const avg=g=>g.reduce((x,y)=>x+y,0)/g.length;
  const early=avg(gaps.slice(0,third)), late=avg(gaps.slice(-third));
  console.log(`  ${name}: ${a.length} frames, ${(1000/mean).toFixed(1)} Hz, gap mean ${mean.toFixed(0)} ms, p95 ${p95} ms`);
  console.log(`     early ${early.toFixed(0)} ms vs late ${late.toFixed(0)} ms  ${Math.abs(late-early) < mean*0.4 ? 'STEADY' : 'DRIFTING — backlog'}`);
};
setTimeout(()=>{ stats(sig,'sig    '); stats(rsp,'rspstat'); process.exit(0); }, SECS*1000);
