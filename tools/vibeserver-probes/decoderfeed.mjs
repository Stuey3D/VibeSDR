// Do the decoders receive audio in per-client mode?
// ★★★ In per-client mode the shared pipeline's VFO is nobody's, so feeding the decoders from it
//     was disabled — but the replacement only fired when the decoder socket carried a
//     user_session_id, and no shipped client sends one. Result: RDS/WEFAX/SSTV/FT8 all silently
//     dead. This attaches a decoder exactly as a real client does (NO session id) and asserts the
//     server reports it decoding.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const CENTRE=Number(process.env.CENTRE||100e6), HW=15000;
function ws(path,onTxt,onBin){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',()=>{});
 s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x1&&onTxt)onTxt(p.toString());
 // ★ Decoded text arrives as a BINARY frame (type byte + timestamp + length + UTF-8), NOT as
 //   JSON. A probe that only reads text frames reports "decoder receives nothing" against a
 //   perfectly healthy server — which is exactly what this one did first time round.
 if(op===0x2&&onBin)onBin(p);}});
 s.send=o=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));};return s;}
const F = CENTRE+HW;                     // the centre station: 620 Hz AM tone
const spec = ws('/ws/user-spectrum?user_session_id=dec&bins=1024');
setTimeout(()=>ws('/ws/audio?user_session_id=dec'),300);
setTimeout(()=>spec.send({type:'tune',frequency:F,mode:'am'}),900);
let msgs=[];
// ★ NO user_session_id — exactly what every shipped client does.
let bin=0;
const dx = ws('/ws/dxcluster', t=>msgs.push(t), p=>{bin++;});
setTimeout(()=>dx.send({type:'audio_extension_attach',extension_name:'fsk',baud_rate:45.45}),1600);
setTimeout(()=>{
  const attached = msgs.some(m=>m.includes('audio_extension_attached'));
  const decoded  = bin > 0 || msgs.some(m=>!m.includes('audio_extension_attached'));
  console.log(`  attach ack: ${attached?'yes':'NO'}   decoder output frames: ${bin}`);
  for (const m of msgs.slice(0,4)) console.log('   ', m.slice(0,110));
  console.log(decoded ? '\n  PASS — a decoder attached without a session id is being fed audio.'
                      : '\n  FAIL — decoder attached but received nothing: it is not being fed.');
  process.exit(decoded?0:1);
},14000);
