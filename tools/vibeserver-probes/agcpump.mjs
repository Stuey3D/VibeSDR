// How much is the RSP's AGC actually moving? Reads the server's own `rspstat` telemetry, which
// carries the LIVE IF gain reduction the AGC has settled on.
//
// ★★★ WHY MEASURE RATHER THAN EYEBALL. "The AGC is erratic" is a picture (horizontal banding
//     across the whole waterfall — Stuart, 2026-08-05, on 4625 kHz). A picture cannot say whether
//     a change helped a little or a lot, and AGC dynamics are exactly the kind of thing where a
//     plausible tweak makes matters worse. gr-changes/min and total dB travelled can.
//
//   HOST=vibeserver.local node agcpump.mjs 48000 [seconds]
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]||48000), SECS=Number(process.argv[3]||120);
const HOST=process.env.HOST||'127.0.0.1';
const key=crypto.randomBytes(16).toString('base64');
const s=net.connect(PORT,HOST); let hs=false,buf=Buffer.alloc(0);
const gr=[], t0=Date.now();
s.on('connect',()=>s.write(`GET /ws/user-spectrum?user_session_id=agc&bins=256 HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x1){ const m=p.toString(); if(m.includes('rspstat')){ try{ const j=JSON.parse(m);
   gr.push({t:Date.now()-t0, gr:j.ifgr, settling:j.settling}); }catch{} } }}});
setTimeout(()=>{
  // ★ Drop the settling window — the startup AGC kick is a DELIBERATE 6-step sequence and
  //   counting it as pumping would flatter or damn any change at random.
  const g = gr.filter(x=>!x.settling);
  if (g.length < 20) { console.log(`  not enough telemetry (${g.length} samples)`); process.exit(1); }
  let changes=0, travel=0, lastGr=g[0].gr, min=g[0].gr, max=g[0].gr;
  for (const x of g) { if (x.gr!==lastGr){changes++; travel+=Math.abs(x.gr-lastGr); lastGr=x.gr;}
                       if(x.gr<min)min=x.gr; if(x.gr>max)max=x.gr; }
  const mins = (g[g.length-1].t - g[0].t)/60000;
  console.log(`  ${g.length} samples over ${mins.toFixed(1)} min`);
  console.log(`  IF gain reduction: ${min}..${max} dB  (range ${max-min} dB)`);
  console.log(`  moves: ${(changes/mins).toFixed(1)} /min     dB travelled: ${(travel/mins).toFixed(1)} /min`);
  console.log(`  ${travel/mins < 12 ? 'STEADY' : travel/mins < 40 ? 'ACTIVE' : 'PUMPING'}`);
  process.exit(0);
}, SECS*1000);
