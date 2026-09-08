import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'10D', sid='Z'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
const t0=Date.now(); const s=()=>((Date.now()-t0)/1000).toFixed(1)+'s';
w.on('open',()=>{console.log(s(),'open'); setTimeout(()=>{console.log(s(),'-> dab on',CH); w.send(JSON.stringify({type:'dab',on:1,channel:CH}));},2000);});
w.on('close',(c,r)=>console.log(s(),'CLOSED code',c,String(r)));
w.on('error',e=>console.log(s(),'ERROR',e.message));
const seen=new Set();
w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
  let j; try{j=JSON.parse(t);}catch(e){return;}
  if(j.type==='dab'||j.type==='sig'||j.type==='lx')return;
  const k=j.type; if(seen.has(k)&&k!=='notice'&&k!=='error'&&k!=='dab_off')return; seen.add(k);
  console.log(s(),'<-',t.slice(0,260));});
setTimeout(()=>process.exit(0),25000);
