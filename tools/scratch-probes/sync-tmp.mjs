import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A';
const sid='Y'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{ const w=new WebSocket(url); let picked=false,prev=null,t0=0;
  const ev=[];
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;} if(j.type!=='dab')return;
    if(!picked&&j.services&&j.services.length){picked=true;
      const d=j.services.find(x=>x.codec==='DAB+')||j.services[0];
      w.send(JSON.stringify({type:'dab_service',sid:d.sid}));
      setTimeout(()=>{t0=Date.now(); prev=JSON.parse(JSON.stringify(j));},60000);
      return;}
    if(!t0||!prev)return;
    const dj=Number(j.syncJumps)-Number(prev.syncJumps);
    const ds=Number(j.sfTried)-Number(prev.sfOk===undefined?0:prev.sfOk);
    if(dj>0) ev.push({t:((Date.now()-t0)/1000).toFixed(1), jumps:dj,
        lost:Number(j.sfTried)-Number(prev.sfTried)-(Number(j.sfOk)-Number(prev.sfOk)),
        drop:Number(j.dropped)-Number(prev.dropped), prs:j.prs, nul:j.nullDepthDb});
    prev=JSON.parse(JSON.stringify(j));});
  setTimeout(()=>{
    const secs=(Date.now()-t0)/1000;
    const tot=ev.reduce((a,e)=>a+e.jumps,0);
    console.log(`over ${secs.toFixed(0)}s: ${tot} sync jumps in ${ev.length} events = ${(tot/secs).toFixed(2)}/s`);
    console.log('first 14 events (t, jumps, superframes lost, iq dropped, prs, null):');
    ev.slice(0,14).forEach(e=>console.log(`   ${e.t}s  jumps ${e.jumps}  sfLost ${e.lost}  drop ${e.drop}  prs ${e.prs}  null ${e.nul}`));
    process.exit(0);},150000);
});
