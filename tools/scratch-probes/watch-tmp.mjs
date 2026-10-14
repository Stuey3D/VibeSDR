import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A', sid='W'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
const t0=Date.now(); const s=()=>((Date.now()-t0)/1000).toFixed(0).padStart(3)+'s';
w.on('open',()=>{console.log(s(),'socket open'); setTimeout(()=>{console.log(s(),'-> dab on',CH); w.send(JSON.stringify({type:'dab',on:1,channel:CH}));},2000);});
let n=0;
w.on('message',m=>{const t=m.toString();
  if(t[0]!=='{'||!t.includes('"type":"dab"'))return;
  let j; try{j=JSON.parse(t);}catch(e){return;}
  if(++n%10===1) console.log(`${s()} samplesIn=${j.samplesIn} push=${j.pushOk}/${j.pushCalls} frames=${j.frames} locked=${j.locked} null=${j.nullDepthDb} prs=${j.prs} fib=${j.fibRate} rfRate=${j.rfRateHz} rfC=${j.rfCentreHz}`);
});
setTimeout(()=>process.exit(0),50000);
