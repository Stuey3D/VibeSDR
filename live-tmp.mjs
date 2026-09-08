import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='V'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
let b=null,last=null;                       // ★ NO tune command — read whatever is already playing
w.on('message',m=>{const t=m.toString();
  if(t[0]!=='{'||!t.includes('"type":"dab"'))return;
  let j; try{j=JSON.parse(t);}catch(e){return;} last=j;
  if(!b) { b=JSON.parse(JSON.stringify(j));
    console.log(`listening to: ${j.channel} sid=${j.sid} ${j.bitrate}k ${j.protection} locked=${j.locked}`); }
});
setTimeout(()=>{
  if(!b||!last){console.log('no DAB running');process.exit(1);}
  const d=k=>Number(last[k]||0)-Number(b[k]||0);
  const inN=d('mp2In');
  console.log(`over 60s on ${last.channel}:`);
  console.log(`  MP2   in ${inN} bad ${d('mp2Bad')} = ${inN?(100*d('mp2Bad')/inN).toFixed(1):'-'}%`);
  console.log(`    ★ hdrBad ${d('mp2HdrBad')} (not a frame = FRAMING fault)  crcBad ${d('mp2CrcBad')} (bad bits = RF)`);
  console.log(`  DAB+  sf ${d('sfOk')}/${d('sfTried')}  RS fixed ${d('rsFixed')} lost ${d('rsLost')}  aus ${d('aus')}`);
  console.log(`  audio silence inserted ${(d('pcmFilled')/48).toFixed(0)} ms   pcmAvail ${last.pcmAvail}`);
  console.log(`  erased ${d('erased')}  syncJumps ${d('syncJumps')}  dropped ${d('dropped')}`);
  console.log(`  fib ${last.fibRate}  null ${last.nullDepthDb} dB  prsRatio ${last.prsRatio}  offset ${last.offsetHz} Hz`);
  process.exit(0);},60000);
