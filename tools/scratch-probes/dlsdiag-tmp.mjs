import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A', CODEC=process.env.CODEC||'MP2';
const sid='P'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{ const w=new WebSocket(url); let picked=false,last=null;
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;} if(j.type!=='dab')return; last=j;
    if(!picked&&j.services&&j.services.length){const d=j.services.filter(x=>x.codec===CODEC);
      if(d.length){picked=true;console.log('service:',JSON.stringify(d[0]));
        w.send(JSON.stringify({type:'dab_service',sid:d[0].sid}));}}});
  setTimeout(()=>{ if(!last){console.log('no data');process.exit(1);}
    console.log(`padFrames ${last.padFrames}`);
    console.log(`  X-PAD indicator:  none ${last.xNone}   short ${last.xShort}   variable ${last.xVar}`);
    console.log(`  app types seen:   DLS-start ${last.xApp2}  DLS-cont ${last.xApp3}  dataGroupLen ${last.xApp1}  MOT ${last.xApp12}`);
    console.log(`  DLS: "${last.dls}"  crc ok ${last.dlsCrcOk} fail ${last.dlsCrcFail}`);
    process.exit(0); },50000);
});
