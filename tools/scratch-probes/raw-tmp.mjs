import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='R'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:'11A'})),2000));
let n=0;
w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
  if(t.includes('"dab"')&&++n<3){ console.log('RAW:',t.slice(0,420));
    try{JSON.parse(t);console.log('  -> parses OK');}catch(e){console.log('  -> PARSE FAILS:',e.message);} }});
setTimeout(()=>process.exit(0),25000);
