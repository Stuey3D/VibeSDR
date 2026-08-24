/**
 * agc-band-scan.mjs — hold the GAIN still and walk the BAND, against known ground truth.
 *
 *   node scripts/agc-band-scan.mjs ws://192.168.86.88:48001 --gain 125
 *   node scripts/agc-band-scan.mjs ws://192.168.86.88:48001 --gain 27 --dwell 5
 *
 * ★★★ WHY THIS EXISTS, AND IT IS THE SISTER OF agc-sweep. That one holds the station still and
 *     moves the gain; this one holds the gain still and moves the station — which is the only way
 *     to check a detector against a LABELLED band. Stuart wrote the labels out on 2026-08-24,
 *     Northampton, and they are the truth this is scored against: which frequencies carry a
 *     station, which carry nothing, and which carry 104.2's intermodulation instead.
 * ★★★ THE POINT IS THE FALSE POSITIVE RATE. A ghost detector that fires on 103.0 is easy; one that
 *     also fires on 104.2 is worthless, and only a labelled sweep of the whole band can tell them
 *     apart. See kGhostMayCut in local_sdr_shim.cpp.
 *
 * ★★★ WHY THIS EXISTS. The AGC gained five detectors in one evening, each added to explain the
 *     last screenshot, and it ended up simultaneously too LOW on 96.1 and too HIGH on 106.0 —
 *     which is not one threshold being wrong, it is a loop steering by numbers nobody has ever
 *     plotted. Stuart found the original AGC fault this morning with one machine and one variable;
 *     this is that method, automated: hold the station still, move the gain, write down what every
 *     candidate measurement says at each step.
 *
 * ★★★ AND IT IS DELIBERATELY OUTSIDE THE SERVER. The measurements are recomputed HERE from the raw
 *     spectrum frames — the same arithmetic the AGC uses — so a detector can be judged, changed or
 *     thrown away without rebuilding and reinstalling a receiver first.
 *
 * ★★ IT TAKES THE DIAL. On a shared receiver this tunes, switches the AGC off and steps the gain,
 *    so anybody listening will hear it. Run it when the radio is yours.
 *
 * ★ Reports, per gain step: ADC peak, the tuned channel, the SHOULDERS (120–400 kHz out, clear of
 *   a 200 kHz WFM channel), band CONTRAST (p90−p25 across the window), SNR, and the RDS block
 *   error rate — which for FM is the closest thing to "is this actually listenable".
 */
import WebSocket from 'ws';

const args = process.argv.slice(2);
const base = args[0];
if (!base || base.startsWith('--')) {
  console.error('usage: node scripts/agc-band-scan.mjs ws://host:port [--gain 125] [--rate 2400000]'
              + ' [--dwell 4] [--auth "&vs_nonce=…&vs_auth=…"]');
  process.exit(1);
}
const opt = (name, dflt) => {
  const i = args.indexOf('--' + name);
  return i >= 0 && args[i + 1] ? args[i + 1] : dflt;
};
const hz = (t) => {
  const m = String(t).trim().match(/^([\d.]+)\s*([kKmMgG])?$/);
  if (!m) return NaN;
  return parseFloat(m[1]) * ({ k: 1e3, m: 1e6, g: 1e9 }[(m[2] || '').toLowerCase()] || 1);
};
const FREQ  = 104.2e6;   // ★ opening tune only; the scan below walks the whole band
const RATE  = Number(opt('rate', '0'));
const DWELL = Number(opt('dwell', '4')) * 1000;
const AUTH  = opt('auth', '');
const SID   = 'agcsweep-' + Math.floor(Date.now() / 1000);

const SPEC_MAGIC = 0x43455053, FLAG_FULL_U8 = 0x03, U8_OFF = -256;

// ── what we collect during one dwell ────────────────────────────────────────────────────────
let frames = [];            // Float32Array of dBFS, low→high
let binHz = 0, centreHz = 0;
let adcPeak = null, rds = null, sigMsg = null, gains = [], gainNow = null, mpx = null;

const url = `${base.replace(/\/+$/, '')}/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=1024${AUTH}`;
const ws = new WebSocket(url);
const send = (o) => ws.send(JSON.stringify(o));

/**
 * ★★★ THE AUDIO SOCKET IS NOT OPTIONAL, EVEN THOUGH WE THROW THE AUDIO AWAY. A spectrum-only
 *  client has no DEMODULATOR running on the server, so there is no RDS, no stereo pilot and no
 *  MPX S/N to read — the first sweep saw RDS only because somebody else was listening at the time,
 *  and the second saw none at all. The demodulated measurements are the ones Stuart judges by, so
 *  the tool has to make the server actually demodulate.
 * ★ And `rdsx` on, because the MPX S/N is computed only while somebody is looking at it — the
 *  server does not spend that CPU on nobody.
 */
const audio = new WebSocket(`${base.replace(/\/+$/, '')}/ws/audio?user_session_id=${SID}&codec=opus${AUTH}`);
audio.on('error', () => {});      // ★ the audio itself is of no interest; only its side effect is
audio.on('message', () => {});

ws.on('open', () => {
  console.log(`connected → ${base}`);
  send({ type: 'zoom', frequency: FREQ, binBandwidth: 1200 });
  send({ type: 'rdsx', on: true });
  send({ type: 'tune', frequency: FREQ, mode: 'wfm' });
  // ★ The whole point is to drive the gain by hand. A `gain` with a VALUE is itself the switch to
  //   manual on an RTL — `{type:'agc'}` is the RSP's IF AGC and does nothing here, which is how an
  //   earlier run "turned the AGC off" and left it on.
  if (RATE > 0) send({ type: 'sampleRate', value: RATE });
});

ws.on('message', (d, isBin) => {
  // ★★ RECOGNISE A SPECTRUM FRAME BY ITS MAGIC, NOT BY THE OPCODE. The shim sends these with the
  //    TEXT opcode, so `isBinary` is false and a tidy `if (isBin)` throws every frame away — which
  //    is exactly what "no spectrum" was. The payload is a Buffer either way, so the bytes are
  //    intact; only the label was misleading.
  const b = Buffer.isBuffer(d) ? d : Buffer.from(d);
  const looksLikeSpec = b.length >= 22 && b[0] === 0x53 && b[1] === 0x50 && b[2] === 0x45 && b[3] === 0x43;
  if (looksLikeSpec) {
    const dv = new DataView(b.buffer, b.byteOffset, b.length);
    if (dv.getUint32(0, true) !== SPEC_MAGIC || dv.getUint8(5) !== FLAG_FULL_U8) return;
    centreHz = Number(dv.getBigUint64(14, true));
    const n = b.length - 22, half = n >> 1;
    const bins = new Float32Array(n);
    for (let i = 0; i < n; i++) bins[i] = b[22 + ((i + half) % n)] + U8_OFF;
    frames.push(bins);
    return;
  }
  let j; try { j = JSON.parse(String(d)); } catch { return; }
  if (j.type === 'config') { binHz = Number(j.binBandwidth) || binHz; }
  if (j.type === 'hwinfo') {
    if (Array.isArray(j.gains) && j.gains.length) gains = j.gains.slice();
    if (Number.isFinite(j.gainNow)) gainNow = j.gainNow;
    if (Number.isFinite(j.adcPeak)) adcPeak = j.adcPeak;
  }
  if (j.type === 'adc' && Number.isFinite(j.peak)) adcPeak = j.peak;
  if (j.type === 'rds') rds = j;
  // ★★★ THE MPX S/N — pilot against the transmitted-silence gap at 15–19 kHz, the same figure the
  //     noise reduction steers by. Stuart: "that is a good measure of the stations actual
  //     strength", and unlike everything else here it is measured on the DEMODULATED signal, so
  //     neither an adjacent station nor the converter's level can flatter it.
  if (j.type === 'rdsx') { if (Number.isFinite(j.mpxSnr)) mpx = { db: j.mpxSnr, ok: j.snrOk === 1 || j.snrOk === true }; }
  if (j.type === 'sig') sigMsg = j;
});
ws.on('error', (e) => { console.error('socket error:', e.message); process.exit(1); });
ws.on('close', (c) => { console.log('socket closed', c); });

const pct = (arr, p) => {
  const a = Float64Array.from(arr).sort();
  return a[Math.min(a.length - 1, Math.max(0, Math.floor(a.length * p)))];
};

/** The same measurements the AGC makes, computed here so they can be judged. */
function measure() {
  if (!frames.length || !binHz) return null;
  // ★ Average the frames in the dwell: one FFT is noisy, and the AGC reads a settled figure too.
  const n = frames[0].length;
  const avg = new Float64Array(n);
  for (const f of frames) for (let i = 0; i < n; i++) avg[i] += f[i];
  for (let i = 0; i < n; i++) avg[i] /= frames.length;

  const mid = n >> 1;
  const at = (offHz) => mid + Math.round(offHz / binHz);
  const band = (loHz, hiHz) => {
    const out = [];
    for (let k = Math.max(0, at(loHz)); k <= Math.min(n - 1, at(hiHz)); k++) out.push(avg[k]);
    return out;
  };
  // The channel: ±100 kHz, as the shim measures it.
  const chan = band(-100e3, 100e3);
  // The shoulders: 120–400 kHz either side — OUTSIDE a 200 kHz WFM channel.
  const shoulders = [...band(-400e3, -120e3), ...band(120e3, 400e3)];
  const all = Array.from(avg);
  const channelDb  = chan.reduce((a, b) => a + b, 0) / chan.length;
  const shoulderDb = pct(shoulders, 0.5);
  const contrast   = pct(all, 0.9) - pct(all, 0.25);
  const floorDb    = pct(all, 0.25);
  /* ★★★ THE WOBBLE. Stuart's eye, 2026-08-24: an image "wobbles about", is "enlarged to one side",
   *     and near 104.2 "all the ghost images move about and wobble a lot" — where a real station
   *     sits still. The averaged figures above HIDE that by construction, so measure the channel
   *     in each frame separately and report how much it moves. Unlike the channel-vs-gain ratio
   *     this needs no gain step, so it can judge a loop that is sitting still. */
  const perFrame = frames.map((f) => {
    let sum = 0, cnt = 0;
    for (let k = Math.max(0, at(-100e3)); k <= Math.min(n - 1, at(100e3)); k++) { sum += f[k]; cnt++; }
    return cnt ? sum / cnt : 0;
  });
  const pfMean = perFrame.reduce((a, b) => a + b, 0) / perFrame.length;
  const wobble = Math.sqrt(perFrame.reduce((a, b) => a + (b - pfMean) ** 2, 0) / perFrame.length);
  /* ★ AND THE ASYMMETRY — "enlarged to one side". A broadcast FM carrier is symmetrical about its
   *   centre; a mixing product need not be, and his screenshots show it lopsided. */
  const lo = band(-100e3, -10e3), hi = band(10e3, 100e3);
  const skew = Math.abs(lo.reduce((a, b) => a + b, 0) / lo.length
                      - hi.reduce((a, b) => a + b, 0) / hi.length);
  return { channelDb, shoulderDb, contrast, snr: channelDb - floorDb, wobble, skew, frames: frames.length };
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  await sleep(2500);
  if (!gains.length) { console.error('no gain list from hwinfo — is this an RTL radio?'); process.exit(1); }
  if (RATE) console.log(`  ${(RATE / 1e6).toFixed(1)} MS/s`);
  /* ★★★ STUART'S LABELS, 2026-08-24 — the ground truth this is scored against. His words kept
   *     verbatim, because "very very barely there" is a measurement and paraphrasing it loses
   *     the thing that makes a row judgeable. */
  const BAND = [
    { hz: 102.3e6, real: true,  note: 'HFM' },
    { hz: 102.6e6, real: true,  note: 'Heart' },
    { hz: 102.8e6, real: true,  note: 'Diversity — very faint, just above noise; 104.2 intermod STARTS here' },
    { hz: 103.0e6, real: false, note: 'next to nothing — the 104.2 image' },
    { hz: 103.3e6, real: true,  note: 'Heart' },
    { hz: 103.6e6, real: true,  note: 'BBC 3CR — very very barely there' },
    { hz: 103.8e6, real: true,  note: 'BBC 3CR' },
    { hz: 104.2e6, real: true,  note: 'BBC R Northampton — ULTRA STRONG' },
    { hz: 104.5e6, real: true,  note: 'BBC 3CR' },
    { hz: 104.7e6, real: true,  note: 'Horizon' },
    { hz: 104.9e6, real: true,  note: 'BBC R Leicester — barely clinging on' },
    { hz: 105.4e6, real: true,  note: 'Capital — faint, stereo lock just about obtainable' },
    { hz: 106.0e6, real: true,  note: 'Greatest Hits — very strong with enough gain' },
  ];
  const GAIN = Number(opt('gain', '125'));
  send({ type: 'gain', value: GAIN });
  await sleep(800);
  console.log(`  scanning the band at a FIXED gain of ${(GAIN/10).toFixed(1)} dB\n`);
  console.log('   freq     chan   shoulder  sep    SNR   pk  wobble  skew  RDS  ST  truth');
  console.log('  ' + '─'.repeat(94));
  const scored = [];
  for (const b of BAND) {
    send({ type: 'zoom', frequency: b.hz, binBandwidth: 1200 });
    send({ type: 'tune', frequency: b.hz, mode: 'wfm' });
    await sleep(1200);
    frames = []; rds = null; mpx = null;
    await sleep(DWELL);
    const m = measure();
    if (!m) { console.log(`  ${(b.hz/1e6).toFixed(1)} — no spectrum`); continue; }
    const ber = rds && Number.isFinite(rds.ber) ? rds.ber : -1;
    const stereo = rds ? (rds.stereo === true) : null;
    scored.push({ ...b, ...m });
    console.log(`  ${(b.hz/1e6).toFixed(1).padStart(6)}  ${m.channelDb.toFixed(1).padStart(6)}  `
      + `${m.shoulderDb.toFixed(1).padStart(7)}  ${(m.channelDb-m.shoulderDb).toFixed(1).padStart(5)}  `
      + `${m.snr.toFixed(1).padStart(5)}  ${(typeof adcPeak === 'number' ? adcPeak.toFixed(1) : '—').padStart(5)}  ${m.wobble.toFixed(2).padStart(6)}  ${m.skew.toFixed(1).padStart(4)}  `
      + `${(ber<0?'—':ber+'%').padStart(4)}  ${(stereo===null?'—':stereo?'ST':'·').padStart(2)}  `
      + `${b.real ? '' : 'GHOST · '}${b.note}`);
  }
  /* ★★★ SCORE THE DETECTOR, do not eyeball it. The only number that matters is whether one
   *     threshold puts every labelled ghost on one side and every real station on the other. */
  const ghosts = scored.filter((r) => !r.real), reals = scored.filter((r) => r.real);
  if (ghosts.length && reals.length) {
    const worstReal = Math.max(...reals.map((r) => r.skew));
    const bestGhost = Math.min(...ghosts.map((r) => r.skew));
    console.log(`\n  ── skew as a ghost detector ──────────────────────────────────`);
    console.log(`  highest skew on a REAL station   ${worstReal.toFixed(1)} dB  `
      + `(${reals.find((r) => r.skew === worstReal).note.split(' —')[0]})`);
    console.log(`  lowest skew on a GHOST           ${bestGhost.toFixed(1)} dB`);
    console.log(bestGhost > worstReal
      ? `  ✅ SEPARABLE — put the threshold at ${((worstReal + bestGhost) / 2).toFixed(1)} dB`
      : `  ❌ NOT SEPARABLE at this gain — the populations overlap`);
  }
  /* ★★★ PUT THE AGC BACK — a tool that changes a receiver's configuration hands it back as it
   *     found it. An earlier sweep left it off and the next test recorded four stations sitting at
   *     the resting gain, which read as a loop that had stopped working. */
  send({ type: 'gain', auto: true });     // ★ VibeAGC back on — `gain:auto`, not `agc:on`
  await sleep(500);
  ws.close(); audio.close();
  process.exit(0);
})();
