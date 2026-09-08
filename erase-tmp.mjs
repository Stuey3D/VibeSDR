import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A';
const sid='E'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{
  const w=new WebSocket(url); let g=null, picked=false, lastEr=0, t0=Date.now();
  const s=()=>((Date.now()-t0)/1000).toFixed(0).padStart(4)+'s';
  w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(j.type==='hwinfo' && j.gainNow!==g){
      console.log(s(),`GAIN ${g}->${j.gainNow} (${((j.gainNow-(g??j.gainNow))/10).toFixed(1)} dB)  ovl=${j.ovlSteps}`); g=j.gainNow; }
    if(j.type==='dab'){
      if(!picked&&j.services&&j.services.length){picked=true;
        const d=j.services.find(x=>x.codec==='DAB+')||j.services[0];
        w.send(JSON.stringify({type:'dab_service',sid:d.sid}));}
      if(j.erased>lastEr){ console.log(s(),`ERASED +${j.erased-lastEr} (total ${j.erased})  prs=${j.prs} ref=${j.prsRef} ratio=${j.prsRatio} gain=${g}`); lastEr=j.erased; }
    }});
  setTimeout(()=>{console.log('--- done, total erased',lastEr);process.exit(0);},90000);
});
