import WebSocket from 'ws';
const H='192.168.86.111:48000';
const SECS=Number(process.env.SOAK||120), SETTLE=Number(process.env.SETTLE||60), SVC=53041;
const sid='m'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{
  const w=new WebSocket(url); let last=null, base=null, hw=null;
  w.on('open',()=>{
    setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:'10C'})),1200);
    setTimeout(()=>w.send(JSON.stringify({type:'dab_service',sid:SVC})),5000);
    setTimeout(()=>{base=last;},SETTLE*1000);           // baseline AFTER the gain has settled
  });
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(j.type==='dab') last=j;
    if(j.type==='hwinfo') hw=j;
  });
  setTimeout(()=>{
    if(!last||!base){console.log('NO DATA');process.exit(1);}
    const d=k=>(last[k]||0)-(base[k]||0);
    const inN=d('mp2In');
    console.log(JSON.stringify({tunerBw:hw&&hw.tunerBw, gainNow:hw&&hw.gainNow, ovl:hw&&hw.ovlSteps,
      mp2In:inN, badPct:inN?+(100*d('mp2Bad')/inN).toFixed(2):null,
      fibRate:last.fibRate, syncJumps:d('syncJumps'), prs:last.prs,
      nullDepthDb:last.nullDepthDb, offsetHz:last.offsetHz}));
    process.exit(0);
  },(SETTLE+SECS)*1000);
});
