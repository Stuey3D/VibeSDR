import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'10C', SVC=Number(process.env.SVC||53041);
const SETTLE=Number(process.env.SETTLE||60), SECS=Number(process.env.SOAK||120);
const sid='c'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{
  const w=new WebSocket(url); let last=null,base=null,hw=null;
  w.on('open',()=>{
    setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500);
    setTimeout(()=>w.send(JSON.stringify({type:'dab_service',sid:SVC})),6000);
    setTimeout(()=>{base=last;},SETTLE*1000);
  });
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(j.type==='dab') last=j; if(j.type==='hwinfo') hw=j;});
  setTimeout(()=>{
    if(!last){console.log('NO DATA');process.exit(1);}
    if(!base){console.log('no baseline; svc=',last.sid,'services=',(last.services||[]).length);process.exit(1);}
    const d=k=>(last[k]||0)-(base[k]||0);
    const i=d('mp2In');
    console.log(CH, JSON.stringify({svc:last.sid,tunerBw:hw&&hw.tunerBw,
      mp2In:i, badPct:i?+(100*d('mp2Bad')/i).toFixed(2):null,
      concealed:d('mp2Concealed'), scfClamped:d('scfClamped'),
      fibRate:last.fibRate, offsetHz:last.offsetHz}));
    process.exit(0);
  },(SETTLE+SECS)*1000);
});
