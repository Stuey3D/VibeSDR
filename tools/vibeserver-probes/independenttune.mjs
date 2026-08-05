// TWO listeners, TWO different frequencies, ONE radio. Each must hear its OWN station.
// fake-rtl-tcp puts AM tones at fixed offsets, so "which station am I on" is audible as a
// different audio pitch — the same test the DSP spike used, now end to end over the wire.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
function ws(path, onBin, onTxt){
  const key=crypto.randomBytes(16).toString('base64'); const s=net.connect(PORT,HOST);
  let hs=false,buf=Buffer.alloc(0);
  s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error',e=>console.log('  err',e.code));
  s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
  for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
  if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
  if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
  if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
  if(op===0x2&&onBin)onBin(p); if(op===0x1&&onTxt)onTxt(p.toString());}});
  s.send=(o)=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
    h[0]=0x81; if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
    s.write(Buffer.concat([h,b]));};
  return s;
}
// dominant audio frequency by zero crossings of the int16 PCM payload
function dom(chunks){
  const all=[]; for(const c of chunks){ if(c[1]!==0) continue;   // raw PCM only
    for(let i=6;i+1<c.length;i+=2) all.push(c.readInt16LE(i)); }
  if(all.length<2000) return {hz:0,n:all.length};
  const cut=all.slice(Math.floor(all.length/3));
  let mean=0; for(const v of cut) mean+=v; mean/=cut.length;
  let x=0; for(let i=1;i<cut.length;i++){const a=cut[i-1]-mean,b=cut[i]-mean; if((a<=0&&b>0)||(a>=0&&b<0))x++;}
  return {hz:(x/2)*48000/cut.length, n:all.length};
}
const A={aud:[]}, B={aud:[]};
const CENTRE=100e6;
// ★ OFFSET TUNING. The radio sits HW_OFFSET_HZ (15 kHz) ABOVE the logical centre so the DC spike
//   misses the channel, so a station sitting at capture-DC + X is at CENTRE + 15 kHz + X. Getting
//   this wrong put both listeners 15 kHz off-station and they heard noise — which reads exactly
//   like "they share one VFO", the very thing under test.
const HW_OFF=15000;
function join(name, st, freq){
  st.spec = ws(`/ws/user-spectrum?user_session_id=${name}&bins=1024`, null, null);
  setTimeout(()=>{ st.audio = ws(`/ws/audio?user_session_id=${name}`, p=>st.aud.push(p), null); }, 400);
  setTimeout(()=>{ st.spec.send({type:'tune', frequency:freq, mode:'am'}); }, 1600);
}
// fake-rtl-tcp stations sit at fixed offsets from centre; pick two of them
join('userA', A, CENTRE + HW_OFF - 120000);   // station at -120 kHz, 400 Hz tone
setTimeout(()=>join('userB', B, CENTRE + HW_OFF + 120000), 800);  // station at +120 kHz, 840 Hz tone
setTimeout(()=>{ A.aud.length=0; B.aud.length=0; }, 6000);   // settle, then measure
setTimeout(()=>{
  const a=dom(A.aud), b=dom(B.aud);
  console.log(`  A (-120 kHz, expect 400 Hz): ${a.n} samples, dominant ${a.hz.toFixed(0)} Hz`);
  console.log(`  B (+120 kHz, expect 840 Hz): ${b.n} samples, dominant ${b.hz.toFixed(0)} Hz`);
  const both = a.n>2000 && b.n>2000;
  const differ = both && Math.abs(a.hz-400) < 120 && Math.abs(b.hz-840) < 200;
  console.log(!both ? '\n  FAIL — one of them got no audio.'
            : differ ? '\n  PASS — two listeners, two different stations, one radio.'
                     : '\n  FAIL — not the expected tones (shared VFO, or tuned off-station).');
  process.exit(differ?0:1);
}, 14000);
