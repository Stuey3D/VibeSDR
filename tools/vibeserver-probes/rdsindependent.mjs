// Two listeners, two FM stations, two DIFFERENT RDS identities.
//
// ★★★ WHY COMPARATIVE. RDS state used to be ONE set of fields on the server, so every listener saw
//     whichever station the shared pipeline (or, briefly, a designated "owner") was on. A single
//     listener seeing a plausible station name proves nothing at all — it was seeing SOMEBODY's
//     station, just not necessarily its own. Only two listeners on two carriers can tell the
//     difference (Stuart, 2026-08-05: "what if user 1 is listening to Heart and user 2 is
//     listening to Radio 1? The RDS needs to be independent").
//   HOST=vibeserver.local node rdsindependent.mjs 48000 <freqA> <freqB>
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]||48000);
const A_HZ=Number(process.argv[3]||96600000), B_HZ=Number(process.argv[4]||98800000);
const HOST=process.env.HOST||'127.0.0.1';
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
 s.send=o=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));};return s;}
const got={A:[],B:[]};
const vfo={A:0,B:0};
// ★ Read each listener's OWN vfo back from its config, so a FAIL cannot be blamed on a tune that
//   never landed — "both saw the same station" and "both were ON the same station" look identical
//   in the result and have completely different causes.
const grab=k=>t=>{ if(t.includes('"rds"')){ try{ got[k].push(JSON.parse(t)); }catch{} }
  if(t.includes('"config"')){ try{ const j=JSON.parse(t); if(j.vfo) vfo[k]=j.vfo; }catch{} } };
const A=ws('/ws/user-spectrum?user_session_id=rdsA&bins=1024', grab('A'));
const B=ws('/ws/user-spectrum?user_session_id=rdsB&bins=1024', grab('B'));
setTimeout(()=>ws('/ws/audio?user_session_id=rdsA'),300);
setTimeout(()=>ws('/ws/audio?user_session_id=rdsB'),400);
setTimeout(()=>{ A.send({type:'tune',frequency:A_HZ,mode:'wfm'});
                 B.send({type:'tune',frequency:B_HZ,mode:'wfm'}); },1200);
setTimeout(()=>{
  const last=k=>{ const withPi=got[k].filter(m=>m.pi>0); return withPi.length?withPi[withPi.length-1]:(got[k].slice(-1)[0]||null); };
  const a=last('A'), b=last('B');
  const show=(k,f,m)=>console.log(`  ${k} on ${(f/1e6).toFixed(1)} MHz: ` +
    (m ? `ps="${m.ps}" pi=${m.pi>0?m.pi.toString(16).toUpperCase():'-'} stereo=${m.stereo} ber=${m.ber}` : '(no rds message)'));
  show('A',A_HZ,a); show('B',B_HZ,b);
  // ★★ NO vfo COMPARISON HERE, deliberately. The config carrying `vfo` is sent at CONNECT, before
  //    either listener has tuned, so both report the landing frequency and comparing them
  //    "proves" a failure that has not happened — it produced exactly that false verdict once.
  //    The identities below are the evidence; a stale config is not.
  if (!a || !b) { console.log('\n  FAIL — one listener received no RDS at all'); process.exit(1); }
  if (a.pi <= 0 && b.pi <= 0) {
    console.log('\n  INCONCLUSIVE — neither frequency decoded a PI. Pick two stations this\n' +
                '  receiver actually hears; this test cannot distinguish "shared" from "silent".');
    process.exit(2);
  }
  // ★ Require BOTH to have decoded something of their own. One listener with a PI and one with
  //   nothing cannot distinguish "independent" from "the second one is simply not decoding".
  if (a.pi <= 0 || b.pi <= 0) {
    console.log('\n  INCONCLUSIVE — one listener decoded no PI. Choose two stations this receiver\n' +
                '  hears well; a silent listener looks the same as a shared one.');
    process.exit(2);
  }
  const ok = a.pi !== b.pi;
  console.log(ok ? '\n  PASS — the two listeners report DIFFERENT stations.'
                 : '\n  FAIL — both listeners report the same identity: RDS is still shared.');
  process.exit(ok?0:1);
},30000);
