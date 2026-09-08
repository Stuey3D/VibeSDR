import WebSocket from 'ws';
const H=process.env.H||'192.168.86.111:48000', CH='11A';
const sid='G'+Date.now();
const ctl=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
let picked=false;
ctl.on('open',()=>setTimeout(()=>ctl.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
ctl.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
  let j; try{j=JSON.parse(t);}catch(e){return;}
  if(j.type==='dab'&&!picked&&j.services&&j.services.length){picked=true;
    const d=j.services.find(x=>x.codec==="MP2")||j.services[0];
    console.log('service:',d.label,d.codec);
    ctl.send(JSON.stringify({type:'dab_service',sid:d.sid}));
    setTimeout(run,8000);}});
function run(){
  const a=new WebSocket(`ws://${H}/ws/audio?client=${sid}&user_session_id=${sid}`);
  const gaps=[]; let last=0,n=0,bytes=0,t0=0;
  a.on('open',()=>{t0=Date.now(); last=t0; console.log('audio open, measuring 45 s...');});
  a.on('message',m=>{ if(typeof m==='string'||m.length<4) return;
    const now=Date.now(); const dt=now-last; last=now; n++; bytes+=m.length;
    if(n>3&&dt>90) gaps.push(dt); });
  setTimeout(()=>{
    const secs=(Date.now()-t0)/1000;
    gaps.sort((x,y)=>y-x);
    console.log(`packets ${n} in ${secs.toFixed(1)}s = ${(n/secs).toFixed(1)}/s, ${(bytes/secs/1024).toFixed(1)} kB/s`);
    console.log(`★ inter-packet gaps > 90 ms: ${gaps.length}  ${gaps.length?'worst: '+gaps.slice(0,8).join(', ')+' ms':''}`);
    process.exit(0);},45000);
}
