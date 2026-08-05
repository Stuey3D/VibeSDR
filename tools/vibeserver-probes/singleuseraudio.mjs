// REGRESSION GUARD: the single-user path (phone, Mac, personal server) must be untouched by the
// per-client DSP work. One listener, the SHARED pipeline, must still produce correct audio.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
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
function dom(chunks){const all=[];for(const c of chunks){if(c[1]!==0)continue;
 for(let i=6;i+1<c.length;i+=2)all.push(c.readInt16LE(i));}
 if(all.length<2000)return{hz:0,n:all.length};
 const cut=all.slice(Math.floor(all.length/3));let m=0;for(const v of cut)m+=v;m/=cut.length;
 let x=0;for(let i=1;i<cut.length;i++){const a=cut[i-1]-m,b=cut[i]-m;if((a<=0&&b>0)||(a>=0&&b<0))x++;}
 return{hz:(x/2)*48000/cut.length,n:all.length};}
const aud=[];
const FREQ=9410000+15000;                       // centre station, +HW_OFFSET (620 Hz tone)
const spec=ws('/ws/user-spectrum?user_session_id=solo&bins=1024',null);
setTimeout(()=>ws('/ws/audio?user_session_id=solo',p=>aud.push(p)),400);
setTimeout(()=>spec.send({type:'tune',frequency:FREQ,mode:'am'}),1500);
setTimeout(()=>aud.length=0,5000);
setTimeout(()=>{const d=dom(aud);
 console.log(`  single listener, shared pipeline: ${d.n} samples, dominant ${d.hz.toFixed(0)} Hz (expect 620)`);
 const ok=d.n>2000&&Math.abs(d.hz-620)<150;
 console.log(ok?'\n  PASS — the single-user path still demodulates correctly.':'\n  FAIL');
 process.exit(ok?0:1);},12000);
