import WebSocket from 'ws';
const H='192.168.86.111:48000', CH='10C', SVC=Number(process.env.SVC||49903);
const sid='a'+Date.now();
const ctl=new WebSocket(`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`);
ctl.on('open',()=>{
  setTimeout(()=>ctl.send(JSON.stringify({type:'dab',on:1,channel:CH})),1500);
  setTimeout(()=>ctl.send(JSON.stringify({type:'dab_service',sid:SVC})),7000);
  setTimeout(()=>{
    const a=new WebSocket(`ws://${H}/ws/audio?client=${sid}&user_session_id=${sid}`);
    let bytes=0, msgs=0, t0=0, types={};
    a.on('open',()=>{t0=Date.now(); console.log('audio socket open');});
    a.on('message',m=>{ msgs++;
      if(typeof m==='string'||m[0]===0x7b){ try{const j=JSON.parse(m.toString()); types[j.type]=(types[j.type]||0)+1; }catch(e){} }
      else bytes+=m.length; });
    setTimeout(()=>{
      const secs=(Date.now()-t0)/1000;
      console.log(`audio: ${msgs} msgs, ${bytes} binary bytes in ${secs.toFixed(1)}s = ${(bytes/secs/1024).toFixed(1)} kB/s`);
      console.log('json types:',JSON.stringify(types));
      process.exit(0);
    },25000);
  },12000);
});
