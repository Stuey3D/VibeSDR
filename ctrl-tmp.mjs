import WebSocket from 'ws';
const H='127.0.0.1:48500', sid='ctrltest';
const t0=Date.now(); const s=()=>String(Date.now()-t0).padStart(5)+'ms';
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
// ★ Exactly what the browser does: a throwaway PROBE socket, closed, then the real one.
const probe=new WebSocket(url);
probe.on('open',()=>{ console.log(s(),'probe open -> closing (as main.ts does)'); probe.close(); });
probe.on('close',()=>{
  const w=new WebSocket(url);
  w.on('open',()=>{ console.log(s(),'REAL socket open');
    setTimeout(()=>{ console.log(s(),'-> tune 7100 kHz'); w.send(JSON.stringify({type:'tune',freq:7100000,mode:'usb'})); },1500);
  });
  let cfgSeen=0;
  w.on('message',m=>{const t=m.toString();
    if(t[0]!=='{')return;
    const j=JSON.parse(t);
    if(j.type==='config'){ cfgSeen++; console.log(s(),`config #${cfgSeen} centre=${j.centerFreq}`); }
    if(j.type==='sig'&&cfgSeen&&!w.__done){ }
  });
  setTimeout(async()=>{
    const r=await fetch(`http://${H}/vibeserver.json`,{cache:'no-store'});
    const j=await r.json();
    console.log(s(),'server says centre/mode via json:', j.centerFreq ?? '(n/a)');
    process.exit(0);
  },5000);
});
