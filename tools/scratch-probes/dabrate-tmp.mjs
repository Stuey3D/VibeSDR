import WebSocket from 'ws';
const H='192.168.86.111:48000', sid='r'+Date.now();
const url=`ws://${H}/ws/user-spectrum?client=${sid}&user_session_id=${sid}`;
const p=new WebSocket(url); p.on('open',()=>p.close());
p.on('close',()=>{
  const w=new WebSocket(url); let c=null,h=null,d=null;
  w.on('message',m=>{const t=m.toString(); if(t[0]!=='{')return;
    let j; try{j=JSON.parse(t);}catch(e){return;}
    if(j.type==='config')c=j; if(j.type==='hwinfo')h=j; if(j.type==='dab')d=j;});
  setTimeout(()=>{
    console.log('config : totalBandwidth=',c&&c.totalBandwidth,'maxBandwidth=',c&&c.maxBandwidth,
                'binBandwidth=',c&&c.binBandwidth,'binCount=',c&&c.binCount,'locked=',c&&c.locked);
    console.log('hwinfo : lockedRate=',h&&h.lockedRate,'lockedCentre=',h&&h.lockedCentre,
                'tunerBw=',h&&h.tunerBw);
    console.log('dab    : rfRateHz=',d&&d.rfRateHz,'rfCentreHz=',d&&d.rfCentreHz);
  },9000);
  setTimeout(()=>process.exit(0),10000);
});
