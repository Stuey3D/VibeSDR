import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='L'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
let picked=false;
w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:'11A'})),1500));
w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
  let j; try{j=JSON.parse(t);}catch(e){return;} if(j.type!=='dab')return;
  if(!picked&&j.services&&j.services.length){picked=true;
    const d=j.services.find(x=>x.codec==='MP2');
    console.log('MP2 services:',j.services.filter(x=>x.codec==='MP2').map(x=>x.label+'/'+x.subch).join(', '));
    w.send(JSON.stringify({type:'dab_service',sid:d.sid}));}
  if(picked&&j.sid) { console.log(`sid ${j.sid} bitrate ${j.bitrate}k protection ${j.protection} | mp2In ${j.mp2In} bad ${j.mp2Bad} out ${j.mp2Out} crc ${j.mp2Crc}`); w.close(); process.exit(0); }
});
setTimeout(()=>process.exit(0),40000);
