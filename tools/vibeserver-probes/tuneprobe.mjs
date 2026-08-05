// Tune to a given frequency and report what the audio actually looks like: level, clipping and
// how often it is railed. Used to chase "exactly on the signal is distorted, 500 Hz off is fine".
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const FREQ=Number(process.argv[3]), MODE=process.argv[4]||'usb';
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
const pcm=[];
const spec=ws(`/ws/user-spectrum?user_session_id=tp${FREQ}&bins=1024`,null);
// ★ RAW PCM, not Opus: we are judging the DEMODULATED WAVEFORM, and a codec would hide clipping.
setTimeout(()=>ws(`/ws/audio?user_session_id=tp${FREQ}`,p=>{ if(p[1]!==0) return;
  for(let i=6;i+1<p.length;i+=2) pcm.push(p.readInt16LE(i)); }),300);
setTimeout(()=>spec.send({type:'tune',frequency:FREQ,mode:MODE}),1000);
setTimeout(()=>{pcm.length=0;},5000);
setTimeout(()=>{
  if(pcm.length<4000){console.log(`  ${(FREQ/1000).toFixed(1)} kHz ${MODE}: no audio (${pcm.length} samples)`);process.exit(1);}
  let acc=0,clip=0,peak=0;
  for(const v of pcm){acc+=v*v; const a=Math.abs(v); if(a>peak)peak=a; if(a>=32700)clip++;}
  const rms=Math.sqrt(acc/pcm.length);
  console.log(`  ${(FREQ/1000).toFixed(1)} kHz ${MODE}: ${pcm.length} samples  RMS ${rms.toFixed(0)}  `
            + `peak ${peak}  clipped ${(clip/pcm.length*100).toFixed(2)}%  crest ${(peak/Math.max(1,rms)).toFixed(1)}`);
  process.exit(0);
},14000);
