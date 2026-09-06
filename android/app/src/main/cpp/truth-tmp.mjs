import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'11A', sid='T'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
const t0=Date.now(); const s=()=>((Date.now()-t0)/1000).toFixed(1).padStart(5)+'s';
let n=0, firstFrames=null;
w.on('open',()=>setTimeout(()=>{console.log(s(),'-> dab on',CH); w.send(JSON.stringify({type:'dab',on:1,channel:CH}));},2000));
w.on('close',c=>console.log(s(),'CLOSED',c));
w.on('message',m=>{const t=m.toString();
  if(t[0]!=='{'||!t.includes('"type":"dab"'))return;
  let j; try{j=JSON.parse(t);}catch(e){return;}
  n++; if(firstFrames===null) firstFrames=j.frames;
  if(n<=3||n%20===0) console.log(`${s()} #${n} frames=${j.frames} locked=${j.locked} fib=${j.fibRate} rfC=${j.rfCentreHz} null=${j.nullDepthDb}`);
});
setTimeout(()=>{console.log(`${s()} TOTAL dab messages: ${n}`);process.exit(0);},60000);
