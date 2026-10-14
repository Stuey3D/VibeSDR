import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='X'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
const t0=Date.now(); const s=()=>((Date.now()-t0)/1000).toFixed(1).padStart(5)+'s';
let picked=false,n=0,prevFrames=-1,stale=0;
w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:'10C'})),2000));
w.on('message',m=>{const t=m.toString(); if(t[0]!=='{'||!t.includes('"type":"dab"'))return;
  let j; try{j=JSON.parse(t);}catch(e){return;} n++;
  if(!picked&&j.services&&j.services.length){picked=true;
    const d=j.services.find(x=>x.codec==='MP2');
    console.log(s(),'selecting MP2 service:',d.label,'sid',d.sid);
    w.send(JSON.stringify({type:'dab_service',sid:d.sid}));return;}
  if(picked){ if(j.frames===prevFrames) stale++; prevFrames=j.frames;
    if(n%25===0) console.log(`${s()} #${n} frames=${j.frames} sid=${j.sid} bitrate=${j.bitrate}k mp2In=${j.mp2In} hdrBad=${j.mp2HdrBad} crcBad=${j.mp2CrcBad} fib=${j.fibRate}`);}
});
setTimeout(()=>{console.log(`${s()} TOTAL ${n} messages, ${stale} with UNCHANGED frame count (stale)`);process.exit(0);},60000);
