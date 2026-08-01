const sid='fr2'+Math.floor(Math.random()*1e6);
const ws=new WebSocket(`ws://192.168.86.88:48000/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=1024`);
ws.binaryType='arraybuffer';
let count=0;
ws.onmessage=e=>{ if(e.data instanceof ArrayBuffer) count++; };
ws.onerror=e=>{console.log('err',e.message||e);process.exit(1);};
const test = async (rate) => {
  count=0;
  ws.send(JSON.stringify({type:'sampleRate', value:rate}));
  await new Promise(x=>setTimeout(x,5000));
  count=0;
  await new Promise(x=>setTimeout(x,3000));
  console.log(`  rate ${String(rate/1000).padStart(4)}k -> ${count} binary frames in 3 s`);
};
ws.onopen=async()=>{ for (const r of [912000,456000,228000,114000]) await test(r); ws.close(); process.exit(0); };
