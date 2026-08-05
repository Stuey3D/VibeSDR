// Do this listener's audio effects actually reach this listener's audio?
//
// ★★★ WHAT THIS CATCHES. NR, the auto notch, de-emphasis and stereo were all set on the SHARED
//     pipeline, which in per-client mode feeds nobody — so the control moved, the server accepted
//     it, and the audio was untouched (Stuart, 2026-08-05: "the controls move but no audible
//     difference"). Nothing on the wire says anything is wrong: the message is accepted, no error
//     comes back, and the only evidence is the SOUND. So this measures the sound.
// ★★ NR is spectral subtraction on noise, so the honest assertion is that the noise floor DROPS
//    with it on. Asserting "the audio changed" would pass on any change at all, including a bug.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const CENTRE=Number(process.env.CENTRE||100e6), HW=15000;
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
const spec = ws('/ws/user-spectrum?user_session_id=fx&bins=1024');
setTimeout(()=>ws('/ws/audio?user_session_id=fx', p=>{ if(p[1]!==0)return;
  for(let i=6;i+1<p.length;i+=2) pcm.push(p.readInt16LE(i)); }),400);
// Park on EMPTY AIR: NR is measured on noise, and a strong carrier would dominate the RMS.
setTimeout(()=>spec.send({type:'tune',frequency:CENTRE+HW+60000,mode:'am'}),900);
const rms=a=>{ if(!a.length) return 0; let m=0; for(const v of a)m+=v; m/=a.length;
               let s=0; for(const v of a)s+=(v-m)*(v-m); return Math.sqrt(s/a.length); };
let off=0;
setTimeout(()=>{ pcm=[]; },3500);
setTimeout(()=>{ off = rms(pcm);
  spec.send({type:'nr', on:true, strength:0.9});
  pcm=[]; },6500);
setTimeout(()=>{
  const on = rms(pcm);
  console.log(`  noise RMS with NR off: ${off.toFixed(0)}`);
  console.log(`  noise RMS with NR on : ${on.toFixed(0)}`);
  if (!off || !on) { console.log('\n  FAIL — no audio in one of the windows'); process.exit(1); }
  const dropDb = 20*Math.log10(on/off);
  console.log(`  change: ${dropDb.toFixed(1)} dB`);
  const ok = dropDb < -2;
  console.log(ok ? '\n  PASS — noise reduction reaches this listener\'s audio.'
                 : '\n  FAIL — NR was accepted and changed nothing: it is not on this listener\'s pipeline.');
  process.exit(ok?0:1);
},11500);
