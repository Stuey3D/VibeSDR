import WebSocket from 'ws';
const H='127.0.0.1:48099', sid='diag'+Math.floor(Math.random()*1e6);
const t0=Date.now(); const s=()=>String(Date.now()-t0).padStart(5)+'ms';
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
let first=null, sent=false;
w.on('open',()=>console.log(s(),'connected'));
w.on('message',(m)=>{const t=m.toString(); if(t[0]!=='{')return;
  const j=JSON.parse(t);
  if(j.type==='config'){
    if(first===null){ first=j.centerFreq; console.log(s(),'centre now',first);
      setTimeout(()=>{ const target=first+4_000_000; sent=true;
        console.log(s(),'-> tune',target,'wfm  (outside the window: must move the hardware)');
        w.send(JSON.stringify({type:'tune',frequency:target,mode:'wfm'})); },1200);
    } else { console.log(s(),'*** CONFIG AGAIN: centre =',j.centerFreq,'— THE COMMAND WORKED ***'); process.exit(0); }
  }
  if(j.type==='pong') console.log(s(),'<- pong (the server IS reading our socket)');
});
setTimeout(()=>{ console.log(s(),'-> ping (does the server read us at all?)'); w.send(JSON.stringify({type:'ping'})); }, 3500);
setTimeout(()=>{ console.log(s(), sent? 'no new config — the tune had NO effect':'never sent'); process.exit(0); }, 8000);
