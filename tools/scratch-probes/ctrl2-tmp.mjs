import WebSocket from 'ws';
const H='127.0.0.1:48500';
const t0=Date.now(); const s=()=>String(Date.now()-t0).padStart(5)+'ms';
const mk=(sid)=>`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const variant=process.argv[2];
if(variant==='noprobe'){
  const w=new WebSocket(mk('noprobe'));
  w.on('open',()=>{ console.log(s(),'single socket, NO probe'); setTimeout(()=>{console.log(s(),'-> tune 7100'); w.send(JSON.stringify({type:'tune',freq:7100000,mode:'usb'}));},1200); });
  setTimeout(()=>process.exit(0),4000);
} else {
  const p=new WebSocket(mk('withprobe'));
  p.on('open',()=>p.close());
  p.on('close',()=>{ const w=new WebSocket(mk('withprobe'));
    w.on('open',()=>{ console.log(s(),'real socket AFTER probe'); setTimeout(()=>{console.log(s(),'-> tune 7100'); w.send(JSON.stringify({type:'tune',freq:7100000,mode:'usb'}));},1200); });
    setTimeout(()=>process.exit(0),4000); });
}
