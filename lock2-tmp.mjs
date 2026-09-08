import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'10C', sid='K'+Date.now()+CH;
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),2000));
let n=0, last=null;
w.on('message',m=>{const t=m.toString();
  if(t[0]!=='{'||!t.includes('"type":"dab"'))return;
  try{last=JSON.parse(t);}catch(e){return;} n++;});
setTimeout(()=>{
  if(!last){console.log(`${CH}: NO DAB STATS AT ALL (${n} msgs)`);process.exit(0);}
  const j=last;
  console.log(`${CH}: locked=${j.locked} null=${j.nullDepthDb}dB prs=${j.prs} off=${j.offsetHz} shift=${j.carrierShift} fib=${j.fibRate} (${j.fibOk}/${j.fibTotal}) frames=${j.frames} svcs=${(j.services||[]).length} eid=0x${(j.eid||0).toString(16)} label="${j.label}"`);
  process.exit(0);},40000);
