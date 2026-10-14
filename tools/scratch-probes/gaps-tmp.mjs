import WebSocket from 'ws';
const H='192.168.86.111:48000', CH=process.env.CH||'10C', sid='G'+Date.now();
const w=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
let picked=false,seen='',last=null;
w.on('open',()=>setTimeout(()=>w.send(JSON.stringify({type:'dab',on:1,channel:CH})),2000));
w.on('message',m=>{const t=m.toString(); if(t[0]!=='{'||!t.includes('"type":"dab"'))return;
  let j; try{j=JSON.parse(t);}catch(e){return;} last=j;
  if(!picked&&j.services&&j.services.length){picked=true;
    const d=j.services.find(x=>x.codec==='MP2');
    console.log('service:',d.label,d.sid); w.send(JSON.stringify({type:'dab_service',sid:d.sid}));return;}
  if(picked&&j.noSyncGaps&&j.noSyncGaps!==seen){seen=j.noSyncGaps;
    console.log(`  gaps between sync failures: [${j.noSyncGaps}]`);}
});
setTimeout(()=>{console.log(`final: noSync ${last.mp2NoSync} of ${last.mp2In} frames`);process.exit(0);},70000);
