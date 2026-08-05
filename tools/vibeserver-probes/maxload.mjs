// FULLY LOADED: N listeners each doing everything a real one does — spectrum AND audio, each
// tuned to a DIFFERENT frequency inside the locked window, each zoomed in so it earns its own
// view channel. That is two DSP chains and an Opus encoder per listener, which is what the
// server actually has to carry when the cap is full.
//
// ★ many.mjs is the FLOOR (spectrum only, zoomed out, no audio). This is the ceiling.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), N=Number(process.argv[3]||29);
const HOST=process.env.HOST||'127.0.0.1';
const CENTRE=Number(process.env.CENTRE||6500000), SPAN=Number(process.env.SPAN||8000000);
const HW=15000, BINS=Number(process.env.BINS||1024);
const st=[];
function ws(path,onBin){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',()=>{});
 s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x2&&onBin)onBin(p.length);}});
 s.send=o=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));};return s;}
for(let i=0;i<N;i++){
  const me={spec:0,audio:0,specB:0,audioB:0};
  st.push(me);
  setTimeout(()=>{
    // spread them across the locked window, like real people on different stations
    const f = CENTRE + HW + (i/(N-1) - 0.5) * SPAN * 0.8;
    me.s = ws(`/ws/user-spectrum?user_session_id=L${i}&bins=${BINS}`, n=>{me.spec++;me.specB+=n;});
    setTimeout(()=>{ me.a = ws(`/ws/audio?user_session_id=L${i}&codec=opus`, n=>{me.audio++;me.audioB+=n;}); },300);
    setTimeout(()=>{ me.s.send({type:'tune',frequency:Math.round(f),mode:'am'}); },900);
    // and each zoomed in, so every one of them earns a private view channel
    setTimeout(()=>{ me.s.send({type:'zoom',frequency:Math.round(f),binBandwidth:24000/BINS}); },1600);
  }, i*220);
}
setTimeout(()=>{ for(const m of st){m.spec=0;m.audio=0;m.specB=0;m.audioB=0;} }, N*220+6000);
setTimeout(()=>{
  const secs=12;
  const withSpec=st.filter(m=>m.spec>10).length, withAudio=st.filter(m=>m.audio>10).length;
  const sB=st.reduce((a,m)=>a+m.specB,0), aB=st.reduce((a,m)=>a+m.audioB,0);
  console.log(`  ${withSpec}/${N} receiving spectrum, ${withAudio}/${N} receiving audio`);
  console.log(`  spectrum ${(sB/secs/1024).toFixed(0)} KB/s   audio ${(aB/secs/1024).toFixed(0)} KB/s`
            + `   TOTAL ${((sB+aB)/secs*8/1e6).toFixed(2)} Mb/s`);
  console.log(`  per listener: ${((sB+aB)/secs*8/1e6/N).toFixed(3)} Mb/s`);
  process.exit(withSpec===N&&withAudio===N?0:1);
}, N*220+18000);
