// Fetch and render the server's spectrogram as ASCII — proves the wire format parses, the
// orientation is right (oldest at top, newest at bottom) and the carriers land where they should.
const PORT=process.argv[2], HOST=process.env.HOST||'127.0.0.1';
const buf=Buffer.from(await (await fetch(`http://${HOST}:${PORT}/vibeserver/spectrogram`)).arrayBuffer());
if(buf.subarray(0,4).toString()!=='VSPG'){console.log('bad magic');process.exit(1);}
const bins=buf.readUInt16LE(5), rows=buf.readUInt16LE(7);
const centre=buf.readDoubleLE(9), span=buf.readDoubleLE(17);
console.log(`  VSPG v${buf[4]}  ${rows} rows x ${bins} bins  centre ${(centre/1e6).toFixed(3)} MHz  span ${(span/1e3).toFixed(0)} kHz`);
const RAMP=' .:-=+*#%@';
const COLS=72;
const t0=Number(buf.readBigInt64LE(25));
for(let r=0;r<rows;r++){
  const off=25+r*(8+bins);
  const t=Number(buf.readBigInt64LE(off));
  let line='';
  for(let c=0;c<COLS;c++){
    const lo=Math.floor(c*bins/COLS), hi=Math.floor((c+1)*bins/COLS);
    let best=0; for(let i=lo;i<hi;i++) best=Math.max(best,buf[off+8+i]);
    line+=RAMP[Math.max(0,Math.min(9,Math.round((best-170)/9)))];
  }
  const age=((t-t0)/1000).toFixed(0).padStart(4);
  if(r<3||r>=rows-3) console.log(`  +${age}s |${line}|`);
  else if(r===3) console.log('        |' + ' '.repeat(COLS) + '|   … ' + (rows-6) + ' more rows');
}
const loMHz=(centre-span/2)/1e6, hiMHz=(centre+span/2)/1e6;
console.log(`         ${loMHz.toFixed(2)} MHz${' '.repeat(COLS-18)}${hiMHz.toFixed(2)} MHz`);
console.log(`\n  stations should appear as vertical columns (fake source: centre and ±120 kHz)`);
