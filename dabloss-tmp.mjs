import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A';
const sid='L'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{ const w=new WebSocket(url); let picked=false,b=null,last=null;
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;} if(j.type!=='dab')return; last=j;
    if(!picked&&j.services){const d=j.services.filter(s=>s.codec==='DAB+');
      if(d.length){picked=true; w.send(JSON.stringify({type:'dab_service',sid:d[0].sid}));
        setTimeout(()=>{b=JSON.parse(JSON.stringify(last));},12000);}}});
  setTimeout(()=>{ if(!b){console.log('no baseline');process.exit(1);}
    const d=k=>Number(last[k]||0)-Number(b[k]||0);
    const tried=d('sfTried'), ok=d('sfOk');
    console.log(`superframes tried ${tried} ok ${ok} -> ${(100*ok/tried).toFixed(2)}% good, ${(100*(1-ok/tried)).toFixed(2)}% LOST`);
    console.log(`aus ${d('aus')} decoded ${d('aacDecoded')}  pcm ${d('pcmPushed')} frames  pcmAvail ${last.pcmAvail}`);
    process.exit(0); },45000);
});
