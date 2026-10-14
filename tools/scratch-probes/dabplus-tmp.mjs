import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'10C', SVC=Number(process.env.SVC||49903);
const sid='x'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{
  const w=new WebSocket(url); let n=0;
  w.on('open',()=>{
    setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500);
    setTimeout(()=>w.send(JSON.stringify({type:'dab_service',sid:SVC})),7000);
  });
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(j.type==='dab' && ++n%10===0)
      console.log('svc',j.sid,j.bitrate+'k','| aacServerSide',j.aacServerSide,
                  '| aacDecoded',j.aacDecoded,'| aus',j.aus,'| sfOk',j.sfOk,'| fib',j.fibRate);
  });
  setTimeout(()=>process.exit(0),75000);
});
