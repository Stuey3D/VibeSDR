import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='P'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
let picked=false;
w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:'11A'})),1500));
w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
  let j; try{j=JSON.parse(t);}catch(e){return;}
  if(j.type==='dab'&&!picked&&j.services&&j.services.length){picked=true;
    const d=j.services.find(x=>x.codec==='DAB+')||j.services[0];
    console.log('service:',d.label);
    w.send(JSON.stringify({type:'dab_service',sid:d.sid}));}});
// ★ fresh socket => hwinfo is sent at connect, with the values as they stand now
function snap(label){
  const s2='Q'+Date.now();
  const v=new WebSocket(`ws://${H}/ws/user-spectrum?client=${s2}&user_session_id=${s2}`);
  v.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(j.type==='hwinfo'&&!v.__d){v.__d=1;
      console.log(`${label}: pump avg ${j.dabPumpAvgMs} ms | MAX ${j.dabPumpMaxMs} ms | calls >60ms: ${j.dabPumpLate}`);
      v.close();}});
}
setTimeout(()=>snap('after 25s'),25000);
setTimeout(()=>snap('after 50s'),50000);
setTimeout(()=>snap('after 75s'),75000);
setTimeout(()=>process.exit(0),80000);
