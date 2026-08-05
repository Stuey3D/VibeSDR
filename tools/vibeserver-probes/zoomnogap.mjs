// ZOOMING MUST NOT STOP THE SPECTRUM. Walk down a zoom ladder and watch the gap between frames.
// ★ The bug: the view channel was sized per view, so every zoom that crossed a power-of-two
//   boundary rebuilt the pipeline — and a fresh ZoomSpectrum must refill its accumulator before
//   it emits, so the waterfall stopped for a moment on the way in.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const CENTRE=Number(process.env.CENTRE||100e6);
function ws(path,onBin){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',e=>console.log('  err',e.code));
 s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x2&&onBin)onBin();}});
 s.send=o=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));};return s;}
const BINS=1024;
let last=0; const gaps=[];
const s=ws(`/ws/user-spectrum?user_session_id=gap&bins=${BINS}`,()=>{
  const now=Date.now(); if(last) gaps.push({t:now, ms:now-last}); last=now;});
// A ladder that crosses several power-of-two channel boundaries on the way in.
const LADDER=[400000,200000,100000,50000,25000,12000,6000,3000];
(async()=>{
  await new Promise(r=>setTimeout(r,3500));
  const marks=[];
  for(const span of LADDER){
    const at=Date.now();
    s.send({type:'zoom',frequency:CENTRE,binBandwidth:span/BINS});
    marks.push({span,at});
    await new Promise(r=>setTimeout(r,1400));
  }
  await new Promise(r=>setTimeout(r,600));
  let worst=0, worstSpan=0;
  for(const m of marks){
    // the largest gap in the 1.2 s after each zoom
    let w=0; for(const g of gaps) if(g.t>=m.at && g.t<m.at+1200) w=Math.max(w,g.ms);
    console.log(`  -> ${(m.span/1e3).toFixed(0).padStart(4)} kHz : worst gap ${String(w).padStart(4)} ms`);
    if(w>worst){worst=w;worstSpan=m.span;}
  }
  const typical = gaps.length ? gaps.map(g=>g.ms).sort((a,b)=>a-b)[Math.floor(gaps.length/2)] : 0;
  console.log(`\n  median frame interval ${typical} ms, worst gap ${worst} ms (at ${(worstSpan/1e3).toFixed(0)} kHz)`);
  // ★★★ COMPARE AGAINST THE PHYSICAL FLOOR, NOT A FLAT NUMBER. A narrow view needs a long look:
  //     filling 2*BINS points of a `span`-wide stream takes 2*BINS/span seconds, so a 3 kHz view
  //     CANNOT produce frames faster than ~680 ms however the code is written. That is
  //     time-bandwidth, not a stall — and a test that called it a bug would push someone into
  //     "fixing" it by throwing away the resolution the zoom exists to provide.
  //     What this must catch is a gap that has no such excuse: a pipeline REBUILD.
  let bad = null;
  for (const m of marks) {
    let w = 0; for (const g of gaps) if (g.t >= m.at && g.t < m.at + 1200) w = Math.max(w, g.ms);
    const floorMs = (2 * BINS / m.span) * 1000;          // shortest possible frame at this span
    // ★ The time-bandwidth floor is NO LONGER AN EXCUSE. A listener stays on the shared row while
    //   its private view primes, so even a 3 kHz zoom keeps producing frames. Left in the
    //   calculation only so a change that removes that cover fails loudly here instead of being
    //   waved through as physics.
    void floorMs;
    const allowed = Math.max(250, typical * 3.5);
    if (w > allowed) bad = { span: m.span, w, allowed };
  }
  if (bad) console.log(`  worst offender: ${(bad.span/1e3).toFixed(0)} kHz gapped ${bad.w} ms, `
                     + `allowed ${bad.allowed.toFixed(0)} ms`);
  const ok = gaps.length>40 && !bad;
  console.log(ok?'  PASS — the spectrum never stopped while zooming.'
               :'  FAIL — the spectrum stalled on a zoom (pipeline rebuilt?).');
  process.exit(ok?0:1);
})();
