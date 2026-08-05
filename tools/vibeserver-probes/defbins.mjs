import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const key=crypto.randomBytes(16).toString('base64'); const s=net.connect(PORT,HOST);
let hs=false,buf=Buffer.alloc(0),n=0;
s.on('connect',()=>s.write(`GET /ws/user-spectrum?user_session_id=def HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
if(op===0x1&&p.toString().includes('"config"'))console.log('  config:',p.toString().slice(0,95));
if(op===0x2&&++n===3)console.log(`  frame ${len} bytes = ${len-22} bins`);}});
setTimeout(()=>process.exit(0),9000);
