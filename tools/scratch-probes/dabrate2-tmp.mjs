import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A';
const sid='z'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{
  const w=new WebSocket(url); let picked=false, t0=0, p0=0, last=null;
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;} if(j.type!=='dab')return; last=j;
    if(!picked && j.services){ const d=j.services.filter(s=>s.codec===(process.env.CODEC||'DAB+'));
      if(d.length){picked=true; console.log('service:',JSON.stringify(d[0]));
        w.send(JSON.stringify({type:'dab_service',sid:d[0].sid}));
        setTimeout(()=>{t0=Date.now(); p0=Number(last.pcmPushed||0); console.log('measuring...');},12000);}}
  });
  setTimeout(()=>{
    const secs=(Date.now()-t0)/1000, d=Number(last.pcmPushed||0)-p0;
    console.log(`pcm frames ${d} in ${secs.toFixed(1)}s = ${(d/secs).toFixed(0)} Hz   (need 48000)`);
    console.log(`ratio ${(d/secs/48000).toFixed(3)}x   aacRateHz=${last.aacRateHz} aacCh=${last.aacCh} pcmAvail=${last.pcmAvail}`);
    process.exit(0);
  },52000);
});
