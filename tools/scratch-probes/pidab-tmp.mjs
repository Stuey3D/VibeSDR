import WebSocket from 'ws';
const H=process.env.H||'192.168.86.88:48004', CH=process.env.CH||'12B', sid='P'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
const t0=Date.now(); const s=()=>((Date.now()-t0)/1000).toFixed(0).padStart(3)+'s';
let picked=false,n=0,last=null;
w.on('open',()=>setTimeout(()=>{console.log(s(),'-> dab on',CH); w.send(JSON.stringify({type:'dab',on:1,channel:CH}));},2500));
w.on('error',e=>console.log('ERR',e.message));
w.on('message',m=>{const t=m.toString(); if(t[0]!=='{'||!t.includes('"type":"dab"'))return;
  let j; try{j=JSON.parse(t);}catch(e){return;} last=j; n++;
  if(!picked&&j.services&&j.services.length){picked=true;
    console.log(s(),'ensemble:',j.label,'| services',j.services.length,
      '| DAB+:',j.services.filter(x=>x.codec==='DAB+').length,'MP2:',j.services.filter(x=>x.codec==='MP2').length);
    const d=j.services.find(x=>x.codec==='DAB+');
    if(d){console.log(s(),'selecting DAB+:',d.label); w.send(JSON.stringify({type:'dab_service',sid:d.sid}));}
    return;}
  if(picked&&n%25===0)
    console.log(`${s()} locked=${j.locked} fib=${j.fibRate} | aacServerSide=${j.aacServerSide} aacRate=${j.aacRateHz} aacCh=${j.aacCh} decoded=${j.aacDecoded}/${j.aus} | pcmAvail=${j.pcmAvail} silence=${(j.pcmFilled/48).toFixed(0)}ms`);
});
setTimeout(()=>{console.log(`${s()} done. msgs=${n}`);process.exit(0);},70000);
