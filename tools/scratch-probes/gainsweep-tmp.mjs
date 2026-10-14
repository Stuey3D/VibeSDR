import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A';
const PH=Number(process.env.PH||60);
const sid='S'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{
  const w=new WebSocket(url); let last=null,hw=null,picked=false;
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(j.type==='dab'){ last=j;
      if(!picked&&j.services&&j.services.length){picked=true;
        const d=j.services.find(x=>x.codec==='DAB+')||j.services[0];
        console.log('service:',d.label,d.codec);
        w.send(JSON.stringify({type:'dab_service',sid:d.sid}));}}
    if(j.type==='hwinfo') hw=j;});
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
  // phases: AGC on, then a sweep of fixed gains (tenths of dB)
  const phases=[['AGC',null],['gain 20.7',207],['gain 28.0',280],['gain 33.8',338],['gain 38.6',386],['gain 44.5',445],['AGC',null]];
  let t=75000;
  phases.forEach(([name,g])=>{
    setTimeout(()=>{ if(g===null){ w.send(JSON.stringify({type:'agc',on:1})); }
                     else { w.send(JSON.stringify({type:'agc',on:0}));
                            w.send(JSON.stringify({type:'gain',gain:g/10})); } }, t);
    let base=null;
    setTimeout(()=>{ base=JSON.parse(JSON.stringify(last)); }, t+25000);
    setTimeout(()=>{ const b=last, d=k=>Number(b[k]||0)-Number(base[k]||0);
      const tr=d('sfTried'), ok=d('sfOk');
      console.log(`${name.padEnd(10)} gainNow ${String(hw&&hw.gainNow).padStart(4)} ovl ${String(hw&&hw.ovlSteps).padStart(2)} | sf ${ok}/${tr} = ${tr?(100*ok/tr).toFixed(1):'-'}% ok | silence ${(d('pcmFilled')/48).toFixed(0)} ms | erased ${d('erased')} | null ${b.nullDepthDb} | fib ${b.fibRate}`);
    }, t+PH*1000);
    t += PH*1000;
  });
  setTimeout(()=>process.exit(0), t+3000);
});
