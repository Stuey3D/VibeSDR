import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A';
const sid='F'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{ const w=new WebSocket(url); let picked=false,last=null,n=0;
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;} if(j.type!=='dab')return; last=j;
    if(!picked&&j.services){const d=j.services.filter(s=>s.codec==='DAB+');
      if(d.length){picked=true;w.send(JSON.stringify({type:'dab_service',sid:d[0].sid}));}}
    if(picked&&++n%15===0) console.log('pcmPushed',j.pcmPushed,'pcmFilled',j.pcmFilled,
      'pcmAvail',j.pcmAvail,'aus',j.aus,'sfOk',j.sfOk);
  });
  setTimeout(()=>process.exit(0),40000);
});
