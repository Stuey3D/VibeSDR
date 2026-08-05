// The waiting queue: countdown, position, and — the part that matters — whether the order we
// PROMISE is the order we KEEP.
//
// ★★★ WHY THE LAST ONE IS THE REAL TEST. A countdown is arithmetic and hard to get wrong. A queue
//     position is a PROMISE, and the easy failure is to display it and then hand the freed slot to
//     whoever reconnects fastest — which looks fine to whoever wins and is a visible lie to
//     everyone else. So this holds two waiters, frees the slot, and asserts the FIRST one is told
//     to come back while the SECOND is still told to wait.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
function ws(path,onTxt){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',()=>{});
 s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x1&&onTxt)onTxt(p.toString());}});
 return s;}
const seen={A:[],B:[]};
// The occupant takes the only slot.
const occ = ws('/ws/user-spectrum?user_session_id=occupant&bins=1024');
let A,B;
setTimeout(()=>{ A = ws('/ws/user-spectrum?user_session_id=waitA&bins=1024', t=>seen.A.push(t)); },1500);
setTimeout(()=>{ B = ws('/ws/user-spectrum?user_session_id=waitB&bins=1024', t=>seen.B.push(t)); },3000);
setTimeout(()=>{
  const last=k=>{ const b=seen[k].filter(m=>m.includes('busy')).pop(); return b?JSON.parse(b):null; };
  const a=last('A'), b=last('B');
  // ★ freeIn:-1 is CORRECT here and is not worth chasing: the occupant is on loopback, which
  //   freeInSecsLocked() deliberately exempts from session limits (as does occupantSecsLeft).
  //   Run this against a non-loopback occupant if you want to exercise the countdown itself.
  console.log('  while occupied:');
  console.log(`    A: ${a?JSON.stringify(a):'(nothing)'}`);
  console.log(`    B: ${b?JSON.stringify(b):'(nothing)'}`);
  const posOk = a && b && a.queuePos===1 && b.queuePos===2 && b.queueLen===2;
  console.log('  --- occupant leaves ---');
  occ.destroy();
  setTimeout(()=>{
    const turnA = seen.A.some(m=>m.includes('your_turn'));
    const turnB = seen.B.some(m=>m.includes('your_turn'));
    console.log(`    A told to come back: ${turnA?'yes':'NO'}    B told to come back: ${turnB?'yes':'no (correct — still 2nd)'}`);
    const ok = posOk && turnA && !turnB;
    if (!posOk) console.log('    ! positions were not 1 and 2');
    console.log(ok ? '\n  PASS — the queue reports position and honours it.'
                   : '\n  FAIL — the queue does not keep the order it promised.');
    process.exit(ok?0:1);
  },4000);
},7000);
