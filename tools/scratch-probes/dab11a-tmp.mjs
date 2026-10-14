import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A';
const sid='y'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{
  const w=new WebSocket(url); let picked=false,n=0;
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(j.type!=='dab')return;
    if(!picked && j.services && j.services.length){
      const d=j.services.filter(s=>s.codec==='DAB+');
      console.log('DAB+ services:',d.map(s=>s.label).join(', '));
      if(d.length){ picked=true; console.log('-> selecting',d[0].label,d[0].sid);
        w.send(JSON.stringify({type:'dab_service',sid:d[0].sid})); }
    }
    if(picked && ++n%12===0)
      console.log('svc',j.sid,j.bitrate+'k','| aacRateHz',j.aacRateHz,'| aacCh',j.aacCh,
                  '| pcmFramesPerAU',j.aacPcmPerAu,'| decoded',j.aacDecoded);
  });
  setTimeout(()=>process.exit(0),60000);
});
