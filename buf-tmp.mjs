import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A';
const sid='B'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{ const w=new WebSocket(url); let picked=false; const v=[];
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;} if(j.type!=='dab')return;
    if(!picked&&j.services&&j.services.length){picked=true;
      const d=j.services.find(x=>x.codec==='DAB+')||j.services[0];
      console.log('service:',d.label,d.codec);
      w.send(JSON.stringify({type:'dab_service',sid:d.sid}));return;}
    if(picked&&j.pcmAvail!==undefined) v.push(j.pcmAvail);});
  setTimeout(()=>{
    if(v.length<10){console.log('too few samples');process.exit(1);}
    const s=v.slice(5); s.sort((a,b)=>a-b);
    const q=f=>s[Math.floor(f*(s.length-1))];
    console.log(`pcmAvail over ${v.length} samples (48 frames = 1 ms):`);
    console.log(`  min ${q(0)}  p10 ${q(.1)}  median ${q(.5)}  p90 ${q(.9)}  max ${q(1)}`);
    console.log(`  in ms:  min ${(q(0)/48).toFixed(0)}  median ${(q(.5)/48).toFixed(0)}  max ${(q(1)/48).toFixed(0)}`);
    const under=s.filter(x=>x<480).length;
    console.log(`  ★ samples under 10 ms of audio: ${under} of ${s.length} (${(100*under/s.length).toFixed(1)}%)`);
    console.log(`  raw tail: ${v.slice(-30).join(' ')}`);
    process.exit(0);},70000);
});
