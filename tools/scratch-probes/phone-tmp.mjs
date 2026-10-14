import WebSocket from 'ws';
const H='127.0.0.1:48099', sid='diag'+Math.floor(Math.random()*1e6);
const t0=Date.now(); const s=()=>String(Date.now()-t0).padStart(5)+'ms';
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
let centre=null;
w.on('open',()=>console.log(s(),'connected to the PHONE server'));
w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
  const j=JSON.parse(t);
  if(j.type==='config'){ console.log(s(),'config centre=',j.centerFreq,'span=',j.totalBandwidth);
    if(centre===null){ centre=j.centerFreq;
      const target = centre + 4_000_000;      // far outside the window -> must move the hardware
      setTimeout(()=>{ console.log(s(),'-> tune',target,'(forces a hardware retune)');
        w.send(JSON.stringify({type:'tune',frequency:target,mode:'wfm'})); },1500);
      setTimeout(()=>{ console.log(s(),'no further config seen — control had NO effect'); process.exit(0); },6000);
    } else { console.log(s(),'*** CENTRE MOVED — the control DID work ***'); process.exit(0); }
  }});
w.on('error',e=>{console.log(s(),'err',e.message); process.exit(1);});
