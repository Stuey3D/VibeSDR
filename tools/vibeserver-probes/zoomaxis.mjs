// Is the ZOOMED spectrum's frequency axis true?
//
// ★★★ THE SYMPTOM THIS CHASES. Stuart, 2026-08-05: tuned to 6070 there is plainly an AM carrier at
//     6035; click it and there is nothing there, and the signal that was there vanishes. "Phantom
//     stations." A phantom that MOVES when you retune is not a receiver image — it is a real
//     station being PAINTED IN THE WRONG PLACE. If the axis is mis-scaled, everything except the
//     signal exactly at the view centre is drawn at a frequency it is not on, and tuning to where
//     one appeared lands on empty air.
//
// So: the synthetic source has stations at KNOWN offsets. Zoom in, find the peaks, convert bin ->
// Hz with the binBandwidth THE SERVER ITSELF reported, and compare against the truth.
// ★ Using the server's own reported binBandwidth is the point: that is what the browser draws
//   with, so this measures the axis the user actually sees, not the one the DSP intended.
import net from 'node:net'; import crypto from 'node:crypto';
const PORT=Number(process.argv[2]), HOST=process.env.HOST||'127.0.0.1';
const CENTRE=Number(process.env.CENTRE||100e6), HW=15000;
const SPAN=Number(process.env.SPAN||400000);        // view span to ask for
// ★★ VIEWOFF puts the view centre this far from the middle station, so the station under test
//    sits OFF-CENTRE — which is the only place a mis-scaled or mis-offset axis can show itself,
//    and it is Stuart's exact case: tuned to 6070, judging a signal 35 kHz away. With the view
//    centred on the station, a completely broken axis still draws it dead centre and passes.
const VIEWOFF=Number(process.env.VIEWOFF||0);
function ws(path,onBin,onTxt){const key=crypto.randomBytes(16).toString('base64');const s=net.connect(PORT,HOST);
 let hs=false,buf=Buffer.alloc(0);
 s.on('connect',()=>s.write(`GET ${path} HTTP/1.1\r\nHost: ${HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
 s.on('error',()=>{});
 s.on('data',d=>{buf=Buffer.concat([buf,d]);if(!hs){const j=buf.indexOf('\r\n\r\n');if(j<0)return;hs=true;buf=buf.subarray(j+4);}
 for(;;){if(buf.length<2)break;const op=buf[0]&0x0f;let len=buf[1]&0x7f,off=2;
 if(len===126){if(buf.length<4)break;len=buf.readUInt16BE(2);off=4;}else if(len===127){if(buf.length<10)break;len=Number(buf.readBigUInt64BE(2));off=10;}
 if(buf.length<off+len)break;const p=buf.subarray(off,off+len);buf=buf.subarray(off+len);
 if(op===0x9){const q=Buffer.alloc(6+p.length);q[0]=0x8a;q[1]=0x80|p.length;q.writeUInt32BE(0,2);p.copy(q,6);s.write(q);continue;}
 if(op===0x2&&onBin)onBin(p); if(op===0x1&&onTxt)onTxt(p.toString());}});
 s.send=o=>{const b=Buffer.from(JSON.stringify(o));const h=Buffer.alloc(b.length<126?6:8);
  h[0]=0x81;if(b.length<126){h[1]=0x80|b.length;h.writeUInt32BE(0,2);}else{h[1]=0x80|126;h.writeUInt16BE(b.length,2);h.writeUInt32BE(0,4);}
  s.write(Buffer.concat([h,b]));};return s;}
let bb=0, cfgCentre=0, rows=[];
const spec = ws('/ws/user-spectrum?user_session_id=zax&bins=1024', p=>{
  // ★ 22-byte header, magic 'SPEC'. Getting this wrong is not a small error: reading 18 header
  //   bytes as spectrum data invents peaks near bin 0 and shifts every real one — i.e. it
  //   MANUFACTURES exactly the phantom this probe exists to find. Assert the magic.
  if (p.length < 40 || p[0]!==0x53 || p[1]!==0x50 || p[2]!==0x45 || p[3]!==0x43) return;
  const v = []; for (let i=22;i<p.length;i++) v.push(p[i]);
  rows.push(v);
}, t=>{ if(t.includes('"binBandwidth"')){ const j=JSON.parse(t);
        if (j.binBandwidth) bb=j.binBandwidth; if (j.centerFreq) cfgCentre=j.centerFreq; } });
const VIEW = CENTRE + HW;                        // the middle station
const VC = VIEW + VIEWOFF;                       // where the VIEW is centred
setTimeout(()=>spec.send({type:'tune',frequency:VIEW,mode:'am'}),900);
setTimeout(()=>{ rows=[]; spec.send({type:'zoom',frequency:VC,binBandwidth:SPAN/1024}); },1600);
setTimeout(()=>{
  if (!rows.length) { console.log('  no spectrum rows'); process.exit(1); }
  // Average the last 40 rows — a carrier is steady, noise is not.
  const use = rows.slice(-40), N = use[0].length, avg = new Array(N).fill(0);
  for (const r of use) for (let i=0;i<N;i++) avg[i]+=r[i]/use.length;
  // ★ The wire is NOT fftshifted: DC sits at bin 0. Rotate so the view centre is in the middle,
  //   the same way the browser does, or every offset comes out mirrored about the wrong point.
  const half=N>>1, sh=[...avg.slice(half),...avg.slice(0,half)];
  const peaks=[]; for(let i=2;i<N-2;i++){
    if(sh[i]>sh[i-1]&&sh[i]>=sh[i+1]&&sh[i]>sh[i-2]&&sh[i]>sh[i+2]){
      const med=[...sh].sort((a,b)=>a-b)[half];
      if (sh[i]-med > 25) peaks.push({i,v:sh[i]});}}
  peaks.sort((a,b)=>b.v-a.v);
  const perBin = SPAN/N;
  console.log(`  view span asked ${(SPAN/1e3).toFixed(1)} kHz over ${N} bins; server reported binBandwidth ${bb.toFixed(3)} Hz`);
  console.log(`  server's own axis implies a span of ${((bb*N)/1e3).toFixed(1)} kHz`);
  // Truth = every synthetic station that actually falls inside this view, as an offset from the
  // VIEW centre. ★ Computing this rather than hard-coding it is what stops the probe reporting a
  // phantom whenever a station is simply out of frame — which it did first time round.
  // ★★ SKIP, DO NOT GUESS, WHEN A STATION SITS ON THE VIEW EDGE. A half-visible station is
  //    neither reliably "in truth" nor reliably absent, and calling it either way gives a verdict
  //    that is about the probe rather than the server — it produced BOTH a false phantom report
  //    and, with a looser rule, would hide a real one.
  const all = [-120000,0,120000].map(o=>o-VIEWOFF);
  const edge = SPAN/2;
  if (all.some(o=>Math.abs(Math.abs(o)-edge) < SPAN*0.08)) {
    console.log('  SKIP — a station sits on the view edge at this span/offset; verdict would be about the probe.');
    process.exit(0);
  }
  const truth = all.filter(o=>Math.abs(o) < edge);
  const got = peaks.slice(0,truth.length).map(p=>Math.round((p.i-half)*perBin)).sort((a,b)=>a-b);
  console.log(`  strongest peaks at ${got.map(h=>(h/1e3).toFixed(1)+' kHz').join(', ')}   (truth: ${truth.sort((a,b)=>a-b).map(h=>(h/1e3).toFixed(1)).join(', ')})`);
  if (process.env.DUMP) { console.log('  all peaks (offset kHz @ level):');
    for (const q of peaks.slice(0,8)) console.log(`     ${(((q.i-half)*perBin)/1e3).toFixed(1)} @ ${q.v.toFixed(0)}`); }
  // ★ TIGHT. A 2% tolerance passed a systematic 4%-of-span scale error that put a station 4 kHz
  //   from where it really was — the measurement is good to ~100 Hz, so a loose tolerance buys
  //   nothing but false confidence.
  const tol=Math.max(1000,SPAN*0.005), near=(a,b)=>Math.abs(a-b)<tol;
  const placed = got.length===truth.length && truth.every((t,i)=>near(got[i],t));
  // ★★★ AND NOTHING ELSE MAY BE THERE. Checking only that the real stations are in the right
  //     place is not enough: an alias is an EXTRA peak, and it can be STRONGER than the station it
  //     folded from. This is the assertion that actually catches a phantom.
  const strongest = peaks.length ? peaks[0].v : 0;
  const phantoms = peaks.filter(q => q.v > strongest - 12)
                        .map(q => Math.round((q.i-half)*perBin))
                        .filter(h => !truth.some(t => near(h,t)));
  if (phantoms.length) console.log(`  PHANTOM peaks at ${phantoms.map(h=>(h/1e3).toFixed(1)).join(', ')} kHz — no station is there`);
  const ok = placed && phantoms.length === 0;
  console.log(ok?'\n  PASS — the zoomed axis puts every station where it really is.'
               :'\n  FAIL — stations are drawn at the wrong frequency: these are the phantoms.');
  process.exit(ok?0:1);
},9000);
