// SWEEP THE TUNED FREQUENCY IN SMALL STEPS and check the recovered tone every time.
// ★★★ The channelizer's phase correction is EXACTLY ZERO when centreBin is a multiple of
//     OVERLAP_DIV, so a bug in it hides at one frequency in four. Sampling one frequency proves
//     nothing — this is the same trap the original channelizer tests fell into ("sweep, do not
//     sample"), and it is what let a stale phase reference reach the air as "7074 is broken but
//     7073.5 is fine".
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const CENTRE=100e6, HW=15000, STATION=CENTRE+HW-120000, TONE=400;   // fake-rtl-tcp: 400 Hz AM
function ws(path,onBin){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',()=>{});
 s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x2&&onBin)onBin(p);}});
 s.send=o=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));};return s;}
let pcm=[];
const spec=ws('/ws/user-spectrum?user_session_id=sweep&bins=1024',null);
setTimeout(()=>ws('/ws/audio?user_session_id=sweep',p=>{ if(p[1]!==0)return;
  for(let i=6;i+1<p.length;i+=2) pcm.push(p.readInt16LE(i)); }),300);
function dom(a){ if(a.length<4000) return 0;
  const cut=a.slice(Math.floor(a.length/3)); let m=0; for(const v of cut)m+=v; m/=cut.length;
  let x=0; for(let i=1;i<cut.length;i++){const p=cut[i-1]-m,q=cut[i]-m; if((p<=0&&q>0)||(p>=0&&q<0))x++;}
  return (x/2)*48000/cut.length; }
(async()=>{
  // ★ Let the sockets open and the first tune settle before measuring — the first reading was
  //   otherwise taken before the audio socket existed, and reported the bug it was looking for.
  await new Promise(r=>setTimeout(r,2500));
  const bad=[];
  // Steps of 61 Hz — a quarter of a bin at 2.4 MSPS, so centreBin lands on every residue mod 4.
  for(let k=0;k<12;k++){
    const f=Math.round(STATION+k*61);
    spec.send({type:'tune',frequency:f,mode:'am'});
    await new Promise(r=>setTimeout(r,1400));
    pcm=[];
    await new Promise(r=>setTimeout(r,1400));
    const hz=dom(pcm);
    const ok=Math.abs(hz-TONE)<80;
    if(!ok) bad.push(`+${k*61}Hz→${hz.toFixed(0)}`);
    console.log(`  offset +${String(k*61).padStart(3)} Hz : recovered ${hz.toFixed(0).padStart(4)} Hz  ${ok?'ok':'BROKEN'}`);
  }
  console.log(bad.length?`\n  FAIL — broken at: ${bad.join(', ')}`
                        :'\n  PASS — every tuning step recovers the tone.');
  process.exit(bad.length?1:0);
})();
