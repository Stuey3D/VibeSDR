import WebSocket from 'ws';
const H='192.168.86.111:48000', CH='11A'; const sid='C'+Date.now();
const ctl=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
const gainEv=[], dabEv=[]; let t0=0, g=null, prev=null;
ctl.on('open',()=>setTimeout(()=>ctl.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
ctl.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
  let j; try{j=JSON.parse(t);}catch(e){return;}
  if(j.type==='hwinfo'){ if(g!==null&&j.gainNow!==g&&t0) gainEv.push({t:(Date.now()-t0)/1000,from:g,to:j.gainNow}); g=j.gainNow; }
  if(j.type==='dab'){
    if(!ctl.__p&&j.services&&j.services.length){ctl.__p=1;
      const d=j.services.find(x=>x.codec==='DAB+')||j.services[0];
      console.log('service:',d.label);
      ctl.send(JSON.stringify({type:'dab_service',sid:d.sid}));
      setTimeout(run,10000); return;}
    if(t0&&prev){
      const dl=Number(j.sfTried)-Number(prev.sfTried)-(Number(j.sfOk)-Number(prev.sfOk));
      const dd=Number(j.dropped)-Number(prev.dropped);
      const dj=Number(j.syncJumps)-Number(prev.syncJumps);
      if(dl>0||dd>0||dj>0) dabEv.push({t:(Date.now()-t0)/1000,sfLost:dl,drop:dd,jump:dj});
    }
    prev=j;
  }});
function run(){
  const a=new WebSocket(`ws://${H}/ws/audio?client=${sid}&user_session_id=${sid}`);
  let last=0,n=0; const gaps=[];
  a.on('open',()=>{t0=Date.now(); last=t0;});
  a.on('message',m=>{ if(typeof m==='string'||m.length<4)return;
    const now=Date.now(); const dt=now-last; last=now; n++;
    if(n>5&&dt>85) gaps.push({t:(now-t0)/1000, ms:dt});});
  setTimeout(()=>{
    console.log(`\n${gaps.length} audio gaps >85 ms, ${gainEv.length} gain changes, ${dabEv.length} decode events`);
    console.log('\ngap(s)   ms   | nearest gain change | nearest decode event');
    for(const gp of gaps.slice(0,15)){
      const ng=gainEv.map(e=>Math.abs(e.t-gp.t)).sort((x,y)=>x-y)[0];
      const nd=dabEv.map(e=>Math.abs(e.t-gp.t)).sort((x,y)=>x-y)[0];
      const de=dabEv.find(e=>Math.abs(e.t-gp.t)===nd);
      console.log(`${gp.t.toFixed(1).padStart(6)}  ${String(gp.ms).padStart(4)} | ${ng===undefined?'none':ng.toFixed(1)+'s away'} | ${nd===undefined?'none':nd.toFixed(1)+'s away '+JSON.stringify(de)}`);
    }
    process.exit(0);},70000);
}
