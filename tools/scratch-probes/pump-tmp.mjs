import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='P'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
let hw=null,picked=false;
w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:'11A'})),1500));
w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
  let j; try{j=JSON.parse(t);}catch(e){return;}
  if(j.type==='hwinfo'){hw=j;
    if(j.dabPumpMaxMs!==undefined&&picked)
      console.log(`pump: avg ${j.dabPumpAvgMs} ms, MAX ${j.dabPumpMaxMs} ms, calls over 60ms: ${j.dabPumpLate}`);}
  if(j.type==='dab'&&!picked&&j.services&&j.services.length){picked=true;
    const d=j.services.find(x=>x.codec==='DAB+')||j.services[0];
    console.log('service:',d.label);
    w.send(JSON.stringify({type:'dab_service',sid:d.sid}));}
});
setTimeout(()=>process.exit(0),60000);
