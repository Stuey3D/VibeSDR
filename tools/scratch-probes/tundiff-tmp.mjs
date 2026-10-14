import WebSocket from 'ws';
const LAN='ws://192.168.86.111:48000';
const TUN=process.env.TUN||'wss://stuey3d-xcover4s.vibeserver.vibesdr.net';
const CH='11A';
function measure(base,label,sid,pick){
  return new Promise(res=>{
    const ctl=new WebSocket(`${base}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
    let done=false;
    ctl.on('error',e=>{if(!done){done=true;res({label,err:e.message});}});
    ctl.on('open',()=>setTimeout(()=>ctl.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500));
    ctl.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
      let j; try{j=JSON.parse(t);}catch(e){return;}
      if(j.type==='dab'&&!ctl.__p&&j.services&&j.services.length){ctl.__p=1;
        const d=j.services.find(x=>x.codec==='DAB+')||j.services[0];
        ctl.send(JSON.stringify({type:'dab_service',sid:d.sid}));
        setTimeout(()=>run(d.label),6000);}});
    function run(svc){
      const a=new WebSocket(`${base}/ws/audio?client=${sid}&user_session_id=${sid}`);
      let last=0,n=0,t0=0,bytes=0; const gaps=[];
      a.on('error',e=>{if(!done){done=true;res({label,err:e.message});}});
      a.on('close',()=>{});
      a.on('open',()=>{t0=Date.now();last=t0;});
      a.on('message',m=>{ if(typeof m==='string'||m.length<4)return;
        const now=Date.now(); const dt=now-last; last=now; n++; bytes+=m.length;
        if(n>5&&dt>90) gaps.push(dt);});
      setTimeout(()=>{ if(done)return; done=true;
        const secs=(Date.now()-t0)/1000; gaps.sort((x,y)=>y-x);
        res({label,svc,pkts:n,rate:(n/secs).toFixed(1),kB:(bytes/secs/1024).toFixed(1),
             gaps:gaps.length,worst:gaps.slice(0,6)});},60000);
    }
  });
}
const s=Date.now();
Promise.all([measure(LAN,'LAN   ','L'+s),measure(TUN,'TUNNEL','T'+s)]).then(r=>{
  for(const x of r){
    if(x.err){console.log(`${x.label}  ERROR: ${x.err}`);continue;}
    console.log(`${x.label}  ${x.svc} | ${x.pkts} pkts ${x.rate}/s ${x.kB} kB/s | ★ gaps>90ms: ${x.gaps} ${x.worst.length?'worst '+x.worst.join(', ')+' ms':''}`);
  }
  process.exit(0);
});
