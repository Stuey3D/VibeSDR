import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='p'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{ const w=new WebSocket(url); let n=0;
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:'10C'})),1200));
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(j.type==='hwinfo') console.log('HW  gain',j.gainNow,'tunerBw',j.tunerBw,'rfCentre',j.rfCentre,'ovl',j.ovlSteps);
    if(j.type==='dab' && ++n%8===1) console.log('DAB samplesIn',j.samplesIn,'pushOk',j.pushOk,'dropped',j.dropped,
       'rfRate',j.rfRateHz,'rfCentre',j.rfCentreHz,'locked',j.locked,'null',j.nullDepthDb,'prs',j.prs,'fib',j.fibRate);
  });
  setTimeout(()=>process.exit(0),30000);
});
