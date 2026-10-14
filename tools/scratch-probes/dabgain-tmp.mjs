import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='g'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{
  const w=new WebSocket(url); const seen=new Set();
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:'10C'})),1200));
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(!seen.has(j.type)){ seen.add(j.type);
      console.log('TYPE',j.type,'KEYS:',Object.keys(j).join(',')); }
    if(j.type==='hwinfo') console.log('HWINFO tunerBw=',j.tunerBw,'rfCentre=',j.rfCentre,'gain=',j.gain,'agc=',j.agc);
  });
  setTimeout(()=>process.exit(0),20000);
});
