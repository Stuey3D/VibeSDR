import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='F'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:'12B'})),2500));
let done=0;
w.on('message',m=>{const t=m.toString(); if(t[0]!=='{'||!t.includes('"type":"dab"'))return;
  try{JSON.parse(t);}catch(e){
    if(done++)return;
    const pos=Number(String(e.message).match(/position (\d+)/)?.[1]||0);
    console.log('PARSE FAILS at',pos);
    console.log('...context:', JSON.stringify(t.slice(Math.max(0,pos-90), pos+90)));
    process.exit(0);
  }});
setTimeout(()=>{console.log('no parse failure seen');process.exit(0);},30000);
