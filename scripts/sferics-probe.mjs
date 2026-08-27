/**
 * sferics.mjs — look for LIGHTNING on a live VibeServer, from raw spectrum frames.
 *
 *   node /tmp/sferics.mjs ws://192.168.86.88:48002 --centre 1.2M --secs 90
 *
 * A sferic is BROADBAND and BRIEF. That is the whole discriminator, and it is why this measures
 * two things per frame rather than one:
 *   - how far above ITS OWN baseline each bin is (per-bin, so a strong MW carrier is not an event)
 *   - what FRACTION of the band lifted at once (broad = lightning, narrow = a station appearing)
 * A per-bin baseline is essential on MW: the band is full of carriers 40 dB over the floor, and a
 * whole-frame median would call every one of them a candidate.
 */
import WebSocket from 'ws';

const args = process.argv.slice(2);
const base = args[0];
const opt = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 && args[i+1] ? args[i+1] : d; };
const hz = (t) => { const m = String(t).match(/^([\d.]+)\s*([kKmMgG])?$/); return m ? parseFloat(m[1]) * ({k:1e3,m:1e6,g:1e9}[(m[2]||'').toLowerCase()] || 1) : NaN; };

const CENTRE = hz(opt('centre', '1.2M'));
const SECS   = Number(opt('secs', '90'));
const BINBW  = Number(opt('binbw', '2400'));   // 1024 bins * 2400 = ~2.4 MHz span
const SID    = 'sferic-' + Math.floor(Date.now()/1000);

const SPEC_MAGIC = 0x43455053, FLAG_FULL_U8 = 0x03, U8_OFF = -256;
const url = `${base.replace(/\/+$/,'')}/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=1024`;
const ws = new WebSocket(url);
const send = (o) => ws.send(JSON.stringify(o));

let binHz = 0, centreHz = 0, nBins = 0;
let base_ = null;          // per-bin running baseline (dB)
let frames = 0, t0 = 0;
const events = [];
const stats = [];
const seen = new Set();
let lastGain = null; const gainMoves = [];

const audio = new WebSocket(`${base.replace(/\/+$/,'')}/ws/audio?user_session_id=${SID}&codec=opus`);
audio.on('error', () => {}); audio.on('message', () => {});

ws.on('open', () => {
  console.log(`connected → ${base}  centre ${(CENTRE/1e6).toFixed(3)} MHz`);
  // ★ PASSIVE BY DEFAULT. Somebody is listening on this radio; taking the dial to measure it would
  //   change the very thing being measured and interrupt them. Take whatever view the server gives.
  if (opt('passive', '') !== '1') {
    send({ type: 'zoom', frequency: CENTRE, binBandwidth: BINBW });
    send({ type: 'tune', frequency: CENTRE, mode: 'am' });
  }
  t0 = Date.now();
});

ws.on('message', (d) => {
  const b = Buffer.isBuffer(d) ? d : Buffer.from(d);
  if (!(b.length >= 22 && b[0]===0x53 && b[1]===0x50 && b[2]===0x45 && b[3]===0x43)) {
    let j; try { j = JSON.parse(String(d)); } catch { return; }
    if (!seen.has(j.type)) { seen.add(j.type); console.log('MSG', JSON.stringify(j).slice(0,300)); }
    if (j.type === 'config') { binHz = Number(j.binBandwidth)||binHz; centreHz = Number(j.centerFreq)||centreHz; }
    if (j.type === 'hwinfo' && Number.isFinite(j.gainNow)) {
      if (lastGain !== null && j.gainNow !== lastGain) gainMoves.push({ t:(Date.now()-t0)/1000, from:lastGain, to:j.gainNow });
      lastGain = j.gainNow;
    }
    return;
  }
  const dv = new DataView(b.buffer, b.byteOffset, b.length);
  if (dv.getUint32(0,true) !== SPEC_MAGIC || dv.getUint8(5) !== FLAG_FULL_U8) return;
  centreHz = Number(dv.getBigUint64(14,true));
  const n = b.length - 22, half = n >> 1;
  const bins = new Float32Array(n);
  for (let i=0;i<n;i++) bins[i] = b[22 + ((i+half)%n)] + U8_OFF;
  nBins = n; frames++;

  if (frames <= 3) {
    let mn=1e9,mx=-1e9,sum=0;
    for (let i=0;i<n;i++){ if(bins[i]<mn)mn=bins[i]; if(bins[i]>mx)mx=bins[i]; sum+=bins[i]; }
    console.log(`frame ${frames}: n=${n} min=${mn.toFixed(1)} max=${mx.toFixed(1)} mean=${(sum/n).toFixed(1)} first8=${Array.from(bins.slice(0,8)).map(v=>v.toFixed(0)).join(',')}`);
  }
  if (!base_) { base_ = Float32Array.from(bins); return; }

  // Per-bin excess over a SLOW baseline. Slow, so a sferic cannot pull its own reference up.
  let over6 = 0, over10 = 0, sumExcess = 0, peakExcess = 0, loBin = -1, hiBin = -1;
  for (let i=0;i<n;i++) {
    const ex = bins[i] - base_[i];
    if (ex > peakExcess) peakExcess = ex;
    if (ex > 6)  { over6++;  if (loBin < 0) loBin = i; hiBin = i; }
    if (ex > 10) over10++;
    if (ex > 0) sumExcess += ex;
  }
  const fracBroad = over6 / n;
  // BROADBAND + BRIEF. 25% of the window lifting ≥6 dB at once is not a station.
  stats.push({ frac: fracBroad, peak: peakExcess, over10: over10/n });
  if (fracBroad > 0.25) {
    const spanHz = binHz ? (hiBin - loBin + 1) * binHz : 0;
    events.push({ t: (Date.now()-t0)/1000, frac: fracBroad, over10: over10/n,
                  peak: peakExcess, spanMHz: spanHz/1e6,
                  loMHz: (centreHz - (n/2 - loBin)*binHz)/1e6,
                  hiMHz: (centreHz - (n/2 - hiBin)*binHz)/1e6 });
  }
  // Baseline creeps (~2 s time constant at 20 fps) — fast enough to follow the band, far too slow
  // to follow a sferic.
  const a = 0.02;
  for (let i=0;i<n;i++) base_[i] += a * (bins[i] - base_[i]);
});

ws.on('error', (e) => { console.error('ws error', e.message); process.exit(1); });

setTimeout(() => {
  const secs = (Date.now()-t0)/1000;
  console.log(`\n=== ${frames} frames in ${secs.toFixed(0)}s (${(frames/secs).toFixed(1)} fps), ${nBins} bins, binHz ${binHz}`);
  console.log(`=== span ${(nBins*binHz/1e6).toFixed(2)} MHz centred ${(centreHz/1e6).toFixed(3)} MHz`);
  const pct = (arr, q) => { const a=[...arr].sort((x,y)=>x-y); return a[Math.min(a.length-1, Math.floor(q*a.length))]; };
  const fr = stats.map(s=>s.frac), pk = stats.map(s=>s.peak);
  console.log(`=== fracBroad(>=6dB): p50 ${(pct(fr,.5)*100).toFixed(1)}%  p90 ${(pct(fr,.9)*100).toFixed(1)}%  p99 ${(pct(fr,.99)*100).toFixed(1)}%  max ${(Math.max(...fr)*100).toFixed(1)}%`);
  console.log(`=== peakExcess:       p50 ${pct(pk,.5).toFixed(1)}  p90 ${pct(pk,.9).toFixed(1)}  p99 ${pct(pk,.99).toFixed(1)}  max ${Math.max(...pk).toFixed(1)} dB`);
  const top = [...stats].sort((a,b)=>b.frac-a.frac).slice(0,8);
  console.log('=== top frames by breadth: ' + top.map(t=>`${(t.frac*100).toFixed(0)}%/+${t.peak.toFixed(0)}dB`).join('  '));
  console.log(`=== BROADBAND FRAMES: ${events.length}  (${(events.length/secs*60).toFixed(1)}/min)`);
  /* ★★★ ONE STRIKE IS NOT ONE FRAME. A sferic is microseconds long but the FFT that catches it is
   *   an integration window, and the waterfall paints ~15 fps — so a single discharge lands in two
   *   to four CONSECUTIVE frames, and a multi-stroke flash spreads wider still. Counting frames
   *   would overstate the rate by roughly 2x and could never be compared with a strike network.
   *   ★ 400 ms because that is comfortably longer than the frame smear and comfortably shorter
   *     than the gap between separate flashes at any rate a badge would report. */
  const strikes = [];
  for (const e of events) {
    const last = strikes[strikes.length-1];
    if (last && e.t - last.t < 0.4) { last.t = e.t; last.peak = Math.max(last.peak, e.peak); last.frames++; }
    else strikes.push({ t: e.t, peak: e.peak, frames: 1 });
  }
  console.log(`=== GAIN MOVES: ${gainMoves.length}` + (gainMoves.length ? '  at t=' + gainMoves.slice(0,12).map(g=>g.t.toFixed(1)).join(',') : ''));
  console.log(`=== DISTINCT STRIKES: ${strikes.length}  (${(strikes.length/secs*60).toFixed(1)}/min)  `
            + `median ${(strikes.reduce((a,b)=>a+b.frames,0)/strikes.length).toFixed(1)} frames each`);
  for (const e of events.slice(0, 40)) {
    console.log(`  t=${e.t.toFixed(1)}s  ${(e.frac*100).toFixed(0)}% of band ≥6dB, ${(e.over10*100).toFixed(0)}% ≥10dB, `
              + `peak +${e.peak.toFixed(0)}dB, extent ${e.loMHz.toFixed(2)}–${e.hiMHz.toFixed(2)} MHz`);
  }
  // ★ Did any "strike" land on a gain change? That is the false positive that would sink this.
  let coincident = 0;
  for (const st of strikes) if (gainMoves.some(g => Math.abs(g.t - st.t) < 0.5)) coincident++;
  console.log(`=== STRIKES COINCIDING WITH A GAIN MOVE (<0.5s): ${coincident} of ${strikes.length}`);
  process.exit(0);
}, SECS*1000);
