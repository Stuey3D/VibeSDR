import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='B'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{ const w=new WebSocket(url); let seen=0;
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(j.type==='hwinfo'){ const k=`${j.tunerBw}|${j.tunerBwAuto}`;
      if(k!==w.__k){w.__k=k; console.log(`  tunerBw=${j.tunerBw} tunerBwAuto=${j.tunerBwAuto}  -> status bar would read "IF ${(j.tunerBw/1e3).toFixed(0)} kHz${j.tunerBwAuto?' auto':''}"`);} }});
  console.log('BEFORE DAB:');
  setTimeout(()=>{console.log('ENTERING DAB 11A:'); w.send(JSON.stringify({type:'dab',on:1,channel:'11A'}));},6000);
  setTimeout(()=>{console.log('LEAVING DAB:'); w.send(JSON.stringify({type:'dab',on:0}));},22000);
  setTimeout(()=>process.exit(0),34000);
});
