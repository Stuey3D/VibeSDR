import WebSocket from 'ws';
import { createHmac } from 'node:crypto';
const H='127.0.0.1:48500';
const t0=Date.now(); const s=()=>String(Date.now()-t0).padStart(5)+'ms';
const vfo = async (tag) => {
  const j = await (await fetch(`http://${H}/vibeserver/auth`,{cache:'no-store'})).json();
  const tok = createHmac('sha256','Test123').update(j.nonce,'utf8').digest('hex');
  const q=`vs_admin_nonce=${encodeURIComponent(j.nonce)}&vs_admin_auth=${tok}`;
  const r = await (await fetch(`http://${H}/vibeserver/admin/sessions?${q}`,{cache:'no-store'})).json();
  console.log(s(), tag, (r.sessions||[]).map(x=>`vfo=${x.vfoHz} mode=${x.mode}`).join(' | ') || '(no sessions)');
};
const p=new WebSocket(`ws://${H}/ws/user-spectrum?client=t&user_session_id=t`);
p.on('open',()=>p.close());
p.on('close',()=>{
  const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=t&user_session_id=t`);
  w.on('open',async ()=>{ console.log(s(),'real socket open'); await vfo('before ->');
    setTimeout(async ()=>{ console.log(s(),'-> tune 7100 usb'); w.send(JSON.stringify({type:"tune",frequency:7100000,mode:'usb'}));
      setTimeout(async()=>{ await vfo('after  ->'); process.exit(0); },1500); },1200); });
});
