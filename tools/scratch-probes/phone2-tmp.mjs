import WebSocket from 'ws';
const H='127.0.0.1:48099', sid='diag'+Math.floor(Math.random()*1e6);
const t0=Date.now(); const s=()=>String(Date.now()-t0).padStart(5)+'ms';
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
let bin=0, txt=0;
w.on('open',()=>console.log(s(),'OPEN'));
w.on('message',(m,isBin)=>{
  if(isBin||Buffer.isBuffer(m)&&m[0]!==0x7b){ bin++; if(bin<=2) console.log(s(),'binary frame',m.length,'bytes'); return; }
  txt++; const t=m.toString(); console.log(s(),'TEXT:',t.slice(0,120));
});
w.on('close',(c,r)=>{console.log(s(),'CLOSE',c,r.toString().slice(0,80)); process.exit(0);});
w.on('error',e=>{console.log(s(),'ERR',e.message); process.exit(1);});
setTimeout(()=>{console.log(s(),`summary: ${txt} text, ${bin} binary frames`); process.exit(0);},12000);
