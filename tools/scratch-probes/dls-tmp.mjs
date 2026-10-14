import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A', CODEC=process.env.CODEC||'MP2';
const sid='T'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{ const w=new WebSocket(url); let picked=false,seen='',n=0;
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;} if(j.type!=='dab')return;
    if(!picked&&j.services&&j.services.length){
      const d=j.services.filter(x=>x.codec===CODEC);
      if(d.length){picked=true; console.log('service:',d[0].label,'('+CODEC+')');
        w.send(JSON.stringify({type:'dab_service',sid:d[0].sid}));}}
    if(picked&&j.dls!==undefined&&j.dls!==seen){seen=j.dls;
      console.log(`  DLS: "${j.dls}"   (changes ${j.dlsChanges}, crc ok ${j.dlsCrcOk} fail ${j.dlsCrcFail})`);}
    if(picked&&++n%40===0) console.log(`  ... crc ok ${j.dlsCrcOk} fail ${j.dlsCrcFail}`);
  });
  setTimeout(()=>process.exit(0),75000);
});
