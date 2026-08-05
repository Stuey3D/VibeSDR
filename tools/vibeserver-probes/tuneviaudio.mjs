// The iOS app sends {type:"tune"} over the AUDIO socket, not the spectrum one — its native audio
// path (VibePowerModule) owns that connection. The web client uses the spectrum socket. BOTH must
// move this listener's own VFO.
// ★ Missing this shipped as: on iPhone the waterfall follows the dial and the audio stays put.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const CENTRE=Number(process.env.CENTRE||100e6), HW=15000;
function ws(path,onBin,onTxt){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',e=>console.log(' err',e.code));
 s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x2&&onBin)onBin(p); if(op===0x1&&onTxt)onTxt(p.toString());}});
 s.send=o=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));};return s;}
function dom(a){ if(a.length<4000) return 0;
  const cut=a.slice(Math.floor(a.length/3)); let m=0; for(const v of cut)m+=v; m/=cut.length;
  let x=0; for(let i=1;i<cut.length;i++){const p=cut[i-1]-m,q=cut[i]-m; if((p<=0&&q>0)||(p>=0&&q<0))x++;}
  return (x/2)*48000/cut.length; }
let pcm=[]; let cfgVfo=0;
const spec=ws('/ws/user-spectrum?user_session_id=appish&bins=1024',null,t=>{
  if(t.includes('"config"')) cfgVfo=JSON.parse(t).vfo||0; });
let audio;
setTimeout(()=>{ audio=ws('/ws/audio?user_session_id=appish',p=>{ if(p[1]!==0)return;
  for(let i=6;i+1<p.length;i+=2) pcm.push(p.readInt16LE(i)); }); },400);
// station at -120 kHz = 400 Hz tone; at +120 kHz = 840 Hz
const A=CENTRE+HW-120000, B=CENTRE+HW+120000;
setTimeout(()=>{ spec.send({type:'tune',frequency:A,mode:'am'}); },1200);   // via SPECTRUM socket
setTimeout(()=>{pcm=[];},4000);
setTimeout(()=>{
  const viaSpec=dom(pcm);
  console.log(`  tune via SPECTRUM socket -> ${viaSpec.toFixed(0)} Hz (expect 400)`);
  console.log('  --- now tune via the AUDIO socket, as the iOS app does ---');
  audio.send({type:'tune',frequency:B,mode:'am'});
  pcm=[];
  setTimeout(()=>{
    const viaAudio=dom(pcm);
    console.log(`  tune via AUDIO socket    -> ${viaAudio.toFixed(0)} Hz (expect 840)`);
    const ok = Math.abs(viaSpec-400)<120 && Math.abs(viaAudio-840)<200;
    console.log(ok?'\n  PASS — a tune on either socket moves this listener\'s own VFO.'
                 :'\n  FAIL — the audio did not follow a tune sent on that socket.');
    process.exit(ok?0:1);
  },5000);
},7000);
