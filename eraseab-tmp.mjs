import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A';
const PHASE=Number(process.env.PHASE||60);
const sid='A'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{
  const w=new WebSocket(url); let last=null, picked=false;
  const snap=()=>JSON.parse(JSON.stringify(last));
  const rows=[];
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(j.type!=='dab')return; last=j;
    if(!picked&&j.services&&j.services.length){picked=true;
      const d=j.services.find(x=>x.codec==='DAB+')||j.services[0];
      console.log('service:',d.label||d.sid,d.codec);
      w.send(JSON.stringify({type:'dab_service',sid:d.sid}));}});
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
  const setFrac=f=>w.send(JSON.stringify({type:'dab_erase',frac:f}));
  const phases=[['ON  0.25',0.25],['OFF 0.00',0.0],['ON  0.25',0.25],['OFF 0.00',0.0]];
  let t=70000;   // settle: AGC climb + lock
  phases.forEach(([name,f],i)=>{
    setTimeout(()=>{ setFrac(f); }, t);
    setTimeout(()=>{ rows.push([name,snap()]); }, t+3000);
    setTimeout(()=>{ const a=rows[rows.length-1][1], b=snap();
      const d=k=>Number(b[k]||0)-Number(a[k]||0);
      const secs=(PHASE-3);
      console.log(`${name}: aus/s ${(d('aus')/secs).toFixed(2)}  sfOk/s ${(d('sfOk')/secs).toFixed(2)}  ` +
                  `erased ${d('erased')}  silenceFilled ${d('pcmFilled')} (${(d('pcmFilled')/48).toFixed(0)} ms)  fib ${b.fibRate}`);
    }, t+PHASE*1000);
    t += PHASE*1000;
  });
  setTimeout(()=>{console.log('--- ideal aus/s for a 16 kHz-core DAB+ service is 16.67');process.exit(0);}, t+4000);
});
