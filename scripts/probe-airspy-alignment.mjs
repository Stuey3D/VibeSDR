// ★★★ WHERE IS THIS RADIO ACTUALLY LISTENING? Stuart's Airspy shows Radio Caroline near 665 kHz
//     when it is on 648, at every sample rate, on a fresh install. Three theories have died on
//     that fact already, so this measures instead of reasoning.
//
// ★★★ THE 9 kHz RASTER IS THE TRICK. Every medium-wave carrier in ITU Region 1 sits on an exact
//     multiple of 9 kHz. So the measurement needs NO station identification and no prior knowledge
//     of what is on air: capture the band, find the strong peaks, and ask how far each sits from
//     the nearest multiple of 9 kHz. If the radio is tuned correctly the residuals scatter around
//     zero. If they all share the same offset, that offset IS the tuning error — and it is
//     self-validating, because a wrong measurement would not produce agreement across peaks.
//
// ★★ AND IT SEPARATES THE TWO CANDIDATES. Repeat at a second centre frequency far away:
//       same error in Hz            → the tuner is off by a fixed amount
//       same error in parts-per-mil → a calibration (ppb) is being applied
//     Those live in completely different code, and nothing readable in the source says which.
//
//   node scripts/probe-airspy-alignment.mjs 192.168.86.88:48003
import assert from 'node:assert';

const HOST = process.argv[2] || '192.168.86.88:48003';
const CENTRES = (process.argv[3] || '648000').split(',').map(Number);
// ★ MW in Region 1 is a 9 kHz raster; shortwave broadcast is 5 kHz. Both are exact.
const RASTER = Number(process.argv[4] || 9000);

const connect = (centreHz) => new Promise((resolve, reject) => {
  const ws = new WebSocket(`ws://${HOST}/ws/user-spectrum?user_session_id=probe&mode=binary8`);
  ws.binaryType = 'arraybuffer';
  const out = { config: null, frames: [], centre: 0 };
  const timer = setTimeout(() => { try { ws.close(); } catch {} ;
    out.frames.length ? resolve(out) : reject(new Error('timed out with no frames')); }, 15000);

  ws.onerror = (e) => { clearTimeout(timer); reject(new Error('websocket error (auth?)')); };
  ws.onopen = () => {
    // ★ The web client tunes over the SPECTRUM socket, not the audio one — see
    //   clients_differ_which_socket. Getting this wrong is why an earlier probe read a frequency
    //   nobody was tuned to.
    ws.send(JSON.stringify({ type: 'tune', frequency: centreHz, mode: 'am',
                             bandwidthLow: -4500, bandwidthHigh: 4500 }));
  };
  ws.onmessage = (e) => {
    if (typeof e.data === 'string') {
      const m = JSON.parse(e.data);
      if (m.type === 'config') out.config = m;
      return;
    }
    const dv = new DataView(e.data);
    if (dv.getUint32(0, true) !== 0x43455053) return;      // 'SPEC'
    out.centre = Number(dv.getBigUint64(14, true));
    // ★ Skip the first few: the radio is still settling after the retune, and an AGC still moving
    //   makes peaks come and go.
    out.frames.push(e.data);
    if (out.frames.length >= 12) { clearTimeout(timer); try { ws.close(); } catch {} ; resolve(out); }
  };
});

/** Average several frames — one frame of a noisy MW band is not a measurement. */
function averageBins(frames, n) {
  const acc = new Float64Array(n);
  const use = frames.slice(4);                              // drop the settling frames
  for (const f of use) {
    const u8 = new Uint8Array(f, 22, n);
    const half = n >> 1;
    for (let i = 0; i < n; i++) acc[i] += (u8[(i + half) % n] - 256);   // fftshift + dBFS
  }
  for (let i = 0; i < n; i++) acc[i] /= use.length;
  return acc;
}

for (const want of CENTRES) {
  console.log(`\n── asked for ${(want / 1000).toFixed(0)} kHz ──`);
  let r;
  try { r = await connect(want); }
  catch (e) { console.log(`   could not measure: ${e.message}`); continue; }

  const n = r.config.binCount;
  const span = r.config.totalBandwidth;
  const binHz = span / n;
  console.log(`   server says: centre ${(r.centre / 1000).toFixed(1)} kHz, span ` +
              `${(span / 1000).toFixed(1)} kHz, ${n} bins (${binHz.toFixed(1)} Hz/bin)`);
  if (Math.abs(r.centre - want) > 1)
    console.log(`   ★ the server did not go where it was asked: off by ${r.centre - want} Hz`);

  const bins = averageBins(r.frames, n);
  const lo = r.centre - span / 2;

  // ★ Peaks: a bin that beats both neighbours and stands well clear of the median floor. Crude on
  //   purpose — the raster test below is what gives the answer, not the peak finder's elegance.
  const sorted = Float64Array.from(bins).sort();
  const floor = sorted[Math.floor(n / 2)];
  const peaks = [];
  for (let i = 2; i < n - 2; i++) {
    if (bins[i] > floor + 12 && bins[i] >= bins[i - 1] && bins[i] > bins[i + 1] &&
        bins[i] >= bins[i - 2] && bins[i] > bins[i + 2]) {
      // ★ Parabolic interpolation: a carrier rarely sits in the middle of a bin, and without this
      //   the residual is quantised to +/- half a bin, which at 223 Hz/bin would hide the answer.
      const a = bins[i - 1], b = bins[i], c = bins[i + 1];
      const d = (a - c) / (2 * (a - 2 * b + c) || 1);
      peaks.push({ hz: lo + (i + d) * binHz, db: b });
    }
  }
  peaks.sort((x, y) => y.db - x.db);
  const top = peaks.slice(0, 12);
  if (top.length < 3) { console.log(`   only ${top.length} peaks — nothing to measure`); continue; }

  console.log(`   ${peaks.length} peaks, strongest ${top.length}:`);
  const residuals = [];
  for (const p of top) {
    const nearest = Math.round(p.hz / RASTER) * RASTER;
    const err = p.hz - nearest;
    residuals.push(err);
    console.log(`     ${(p.hz / 1000).toFixed(2).padStart(9)} kHz  ${p.db.toFixed(0).padStart(4)} dB` +
                `   nearest 9k raster ${(nearest / 1000).toFixed(0).padStart(5)}  →  ` +
                `${err >= 0 ? '+' : ''}${(err / 1000).toFixed(2)} kHz`);
  }
  const med = residuals.slice().sort((a, b) => a - b)[residuals.length >> 1];
  const agree = residuals.filter((x) => Math.abs(x - med) < 500).length;
  console.log(`   ★★★ median offset from the 9 kHz raster: ${med >= 0 ? '+' : ''}` +
              `${(med / 1000).toFixed(2)} kHz  (${agree}/${residuals.length} peaks agree)`);
  console.log(`       as parts-per-million of ${(r.centre / 1000).toFixed(0)} kHz: ` +
              `${(med / r.centre * 1e6).toFixed(0)} ppm`);
}
console.log('\nCompare the two: same kHz => fixed tuner offset. Same ppm => calibration.\n');
