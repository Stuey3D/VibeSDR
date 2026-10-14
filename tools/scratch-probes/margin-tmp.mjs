import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'12B', WANT=process.env.CODEC||'MP2';
const sid='M'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
let picked=false,b=null,last=null,lbl='';
w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
  let j; try{j=JSON.parse(t);}catch(e){return;} if(j.type!=='dab')return; last=j;
  if(!picked&&j.services&&j.services.length&&j.locked&&j.label){picked=true;
    const d=j.services.find(x=>x.codec===WANT)||j.services[0]; lbl=d.label+' '+d.codec;
    w.send(JSON.stringify({type:'dab_service',sid:d.sid}));
    setTimeout(()=>{b=JSON.parse(JSON.stringify(last));},20000);}});
setTimeout(()=>{ if(!b){console.log('no data');process.exit(1);}
  const d=k=>Number(last[k]||0)-Number(b[k]||0); const i=d('mp2In');
  console.log(`${CH} ${lbl}  bitrate ${last.bitrate}k ${last.protection}`);
  console.log(`  MP2  in ${i} bad ${d('mp2Bad')} = ${i?(100*d('mp2Bad')/i).toFixed(1):'-'}%`);
  console.log(`  DAB+ sf ${d('sfOk')}/${d('sfTried')}  RS fixed ${d('rsFixed')} bytes, LOST ${d('rsLost')} codewords`);
  console.log(`  fib ${last.fibRate}  erased ${d('erased')}  null ${last.nullDepthDb} dB  prsRatio ${last.prsRatio}`);
  process.exit(0);},70000);
