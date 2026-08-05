// ZOOMING MUST NOT TOUCH THE AUDIO. Measure RMS before and after a zoom on the same listener.
// ★ The bug this guards: the channel was sized by max(audio, view), so changing the zoom changed
//   the channel WIDTH, which rebuilt the pipeline and restarted its AGC — the audio ducked.
//   Tuning never did, because tuning keeps the same width. Same shape as
//   memory/tuning_attenuates_agc_reset.md.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const CENTRE=Number(process.env.CENTRE||100e6), HW=15000;
function ws(path,onBin){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',e=>console.log('  err',e.code));
 s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x2&&onBin)onBin(p);}});
 s.send=o=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));};return s;}
let aud=[];
function samples(chunks){const out=[];
 for(const c of chunks){if(c[1]!==0)continue;
  for(let i=6;i+1<c.length;i+=2) out.push(c.readInt16LE(i));}
 return out;}
function rmsOf(a){if(!a.length)return 0;let acc=0;for(const v of a)acc+=v*v;return Math.sqrt(acc/a.length);}
function rms(){return rmsOf(samples(aud));}
// ★★ THE DIP IS A TRANSIENT, so an average over the whole window hides it — a rebuilt chain goes
//    quiet for a moment and recovers. Look at the WORST 100 ms block instead: that is what a
//    listener actually hears as the audio ducking.
function worstBlock(chunks){const a=samples(chunks); const N=4800; // 100 ms at 48 kHz
 if(a.length<N*2) return 0; let worst=Infinity;
 for(let i=0;i+N<=a.length;i+=N) worst=Math.min(worst, rmsOf(a.slice(i,i+N)));
 return worst===Infinity?0:worst;}
const F=CENTRE+HW-120000;
const spec=ws(`/ws/user-spectrum?user_session_id=zd&bins=1024`,null);
setTimeout(()=>ws('/ws/audio?user_session_id=zd',p=>aud.push(p)),400);
setTimeout(()=>spec.send({type:'tune',frequency:F,mode:'am'}),1500);
let before=0, postWindow=null;
setTimeout(()=>{aud=[];},4000);
setTimeout(()=>{before=rms(); aud=[];
  console.log(`  before zoom: RMS ${before.toFixed(0)}`);
  // ★★ THE SPAN MATTERS. A 10 kHz view needs the same channel width as 10 kHz of AM audio, so
  //    nothing was rebuilt and this test PASSED on the broken code. Pick a span that crosses a
  //    channel-width boundary — that is the case that used to rebuild the pipeline.
  console.log('  --- zoom to ~60 kHz (crosses a channel-width boundary) ---');
  spec.send({type:'zoom',frequency:F,binBandwidth:60000/1024});},7000);
setTimeout(()=>{ postWindow = aud.slice(); },8300);   // ~1.3 s of audio straight after the zoom
// ★★ MEASURE THE FIRST SECOND AFTER THE ZOOM, not four seconds later. A rebuilt chain restarts
//    its AGC, so the level DIPS and then recovers — by the time the old window sampled, the dip
//    had gone and the test passed on broken code. The transient IS the symptom the user hears.
setTimeout(()=>{
  const after=worstBlock(postWindow||aud);
  console.log(`  after  zoom: worst 100 ms block RMS ${after.toFixed(0)}`);
  const ratio = before>0 ? after/before : 0;
  console.log(`  ratio ${ratio.toFixed(2)}`);
  const ok = before>200 && ratio>0.85 && ratio<1.18;
  console.log(ok?'\n  PASS — zooming did not touch the audio.'
               :'\n  FAIL — the audio level changed when zooming (chain rebuilt?).');
  process.exit(ok?0:1);},11000);
