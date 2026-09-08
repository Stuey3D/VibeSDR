import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='I'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
let picked=false, base=null, bt=0, last=null, lt=0; const iv=[];
w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:'11A'})),1500));
w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
  let j; try{j=JSON.parse(t);}catch(e){return;} if(j.type!=='dab')return;
  if(!picked&&j.services&&j.services.length){picked=true;
    const d=j.services.find(x=>x.codec==='DAB+')||j.services[0];
    w.send(JSON.stringify({type:'dab_service',sid:d.sid}));
    setTimeout(()=>{base=j.samplesIn;bt=Date.now();},20000);return;}
  if(!base)return;
  const now=Date.now();
  if(last!==null){
    const dn=Number(j.samplesIn)-Number(last), dt=(now-lt)/1000;
    if(dt>0.05) iv.push({rate:dn/dt, dt});
  }
  last=Number(j.samplesIn); lt=now;
});
setTimeout(()=>{
  if(!base){console.log('no data');process.exit(1);}
  const secs=(Date.now()-bt)/1000, got=Number(last)-Number(base);
  const nominal=2400000*secs;
  console.log(`samplesIn ${got} in ${secs.toFixed(2)}s`);
  console.log(`  actual ${(got/secs/1e6).toFixed(4)} MS/s vs nominal 2.4000 MS/s`);
  console.log(`  ★ shortfall ${((1-got/nominal)*100).toFixed(3)}%  = ${((nominal-got)/2400000*1000).toFixed(0)} ms of MISSING capture`);
  const r=iv.map(x=>x.rate).sort((a,b)=>a-b);
  const q=f=>r[Math.floor(f*(r.length-1))];
  console.log(`  per-interval MS/s: min ${(q(0)/1e6).toFixed(3)}  p10 ${(q(.1)/1e6).toFixed(3)}  median ${(q(.5)/1e6).toFixed(3)}  max ${(q(1)/1e6).toFixed(3)}`);
  const bad=r.filter(x=>x<2300000).length;
  console.log(`  ★ intervals running under 2.3 MS/s: ${bad} of ${r.length}`);
  process.exit(0);},80000);
