import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='s'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{
  const w=new WebSocket(url); const t0=Date.now();
  const s=()=>((Date.now()-t0)/1000).toFixed(0).padStart(4)+'s';
  let key=null;
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:'10C'})),1200));
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(j.type==='hwinfo'){
      const k=`${j.gainNow}|${j.tunerBw}|${j.ovlSteps}`;
      if(k!==key){ key=k; console.log(s(),'gainNow=',j.gainNow,'tunerBw=',j.tunerBw,
        'ovlSteps=',j.ovlSteps,'adcPeak=',j.adcPeak,'agc=',j.agc); }
    }
    if(j.type==='dab' && !w.__d && j.locked){ w.__d=1; console.log(s(),'locked, rfCentre',j.rfCentreHz); }
  });
  setTimeout(()=>process.exit(0),100000);
});
