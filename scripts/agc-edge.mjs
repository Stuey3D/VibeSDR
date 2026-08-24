/**
 * agc-edge.mjs — where does this radio's own anti-alias roll-off actually begin?
 *
 *   node scripts/agc-edge.mjs ws://host:port 96.0M
 *   node scripts/agc-edge.mjs ws://host:port 106.3M --vfo 105.4M
 *
 * ★★★ WHY: edgeCutoffHz() crops the DISPLAY to what a radio can really receive, but returns 0 for
 *     anything that is not an Airspy HF+ ("only the HF+ has lobes this wide"). When moving the RF
 *     centre away from the VFO began to look like the HF+'s auto-contrast fault, the question was
 *     whether an RTL has a skirt worth cropping too.
 * ★★★ THE ANSWER, MEASURED ON AN RTL AT 2.4 MS/s: NO. The delivered span is flat to both edges —
 *     0 kHz of roll-off at -3, -6, -10 and -20 dB, with the view centred AND with it offset from
 *     the VFO. So the "~10% anti-alias rolloff" figure that viewDongleMargin() carries is a safety
 *     margin, not something visible in the spectrum, and cropping the display would remove real
 *     band. What looks like the HF+ fault is auto-contrast honestly refitting to a window that now
 *     holds more empty spectrum.
 * ★ Kept because a negative result is a result: the next person to see that shape will otherwise
 *   re-derive the same wrong theory.
 *
 *   node scripts/agc-sweep.mjs ws://192.168.86.88:48001 --freq 96.1M
 *   node scripts/agc-sweep.mjs ws://192.168.86.88:48001 --freq 105.8M --rate 2400000 --dwell 5
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
  console.error('usage: node scripts/agc-sweep.mjs ws://host:port <96.1M|--freq 96.1M> [--rate 2400000]'
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
/* ★★★ NO SILENT DEFAULT FREQUENCY, AND TAKE IT POSITIONALLY LIKE agc-settle DOES. This defaulted
 *     to '96.1M' — a REAL station — so `agc-sweep.mjs ws://... 105.4M` swept 96.1 and printed a
 *     full, plausible, entirely wrong table for it. Twice. I read the numbers and not the header
 *     line that says which frequency they came from, and reported "105.4 has no signal at any
 *     gain" to Stuart on the strength of it.
 *  ★★ The two tools disagreeing about how to name a station is what made it possible: settle takes
 *     positionals, sweep took --freq. Both now take either.
 *  ★ A default that happens to be valid is worse than no default: it cannot be distinguished from
 *    the answer you asked for. */
const positional = args.slice(1).filter((a, i) => !a.startsWith('--')
                                                 && !(args[i] || '').startsWith('--'));
const FREQ  = hz(opt('freq', positional[0] || ''));
if (!Number.isFinite(FREQ) || FREQ <= 0) {
  console.error('agc-sweep: which frequency? e.g. `105.4M` or `--freq 105.4M` — there is no default.');
  process.exit(1);
}
const RATE  = Number(opt('rate', '0'));
const DWELL = Number(opt('dwell', '4')) * 1000;
const AUTH  = opt('auth', '');
const SID   = 'agcsweep-' + Math.floor(Date.now() / 1000);

const SPEC_MAGIC = 0x43455053, FLAG_FULL_U8 = 0x03, U8_OFF = -256;

// ── what we collect during one dwell ────────────────────────────────────────────────────────
let frames = [];            // Float32Array of dBFS, low→high
let binHz = 0, centreHz = 0;
let adcPeak = null, adcClip = null, rds = null, sigMsg = null, gains = [], gainNow = null, mpx = null;

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
  /* ★ The server sends this ~once a second now (it used to send nothing of the sort, so this
   *   handler sat here waiting for a message that did not exist and the column read a stale
   *   connect-time value). `clip` is the figure that decides harm — a peak near full scale is
   *   fine, samples ON the rail are not. */
  if (j.type === 'adc') {
    if (Number.isFinite(j.peak)) adcPeak = j.peak;
    if (Number.isFinite(j.clip)) adcClip = j.clip;
  }
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

  const rows = [];
  /* ★★★ WHERE DOES THIS RADIO'S OWN ANTI-ALIAS ROLL-OFF BEGIN? edgeCutoffHz() in the shim returns
   *     0 for anything that is not an HF+ — "only the HF+ has lobes this wide" — so an RTL's
   *     display has never been cropped and shows the full sample rate, skirt included. That was
   *     harmless while the view sat mid-capture; move the RF centre away from the VFO (which is
   *     what escaping a blowtorch requires) and the view slides towards the capture edge, the
   *     skirt comes into view, and it drags the auto-contrast down. Stuart: "it introduces the
   *     airspy bug with the auto contrast, but the signal is cleaner".
   *  ★ The crop figure has to be a number taken off this radio, not a percentage somebody liked.
   *    Measured against the floor in the middle third, which is flat by construction. */
  send({ type: 'gain', value: 250 });
  if (opt('tunerbw', '')) { send({ type: 'tunerbw', value: Number(opt('tunerbw', '0')) }); }
  // ★ --vfo lets the VFO and the VIEW differ, which is the whole point: the artefact only appears
  //   once the RF centre is pushed away from the station being listened to.
  const VFO = opt('vfo', '') ? hz(opt('vfo', '')) : FREQ;
  send({ type: 'zoom', frequency: FREQ, binBandwidth: 2400 });
  send({ type: 'tune', frequency: VFO, mode: 'wfm' });
  console.log(`  VFO ${(VFO/1e6).toFixed(2)} MHz, view centre ${(FREQ/1e6).toFixed(2)} MHz`);
  frames = [];
  await sleep(7000);
  if (!frames.length) { console.error('no spectrum frames'); process.exit(1); }
  const n = frames[0].length, avg = new Float64Array(n);
  for (const f of frames) for (let i = 0; i < n; i++) avg[i] += f[i];
  for (let i = 0; i < n; i++) avg[i] /= frames.length;
  const mid = n >> 1;
  const midBins = Array.from(avg.slice(Math.floor(n / 3), Math.floor((2 * n) / 3))).sort((a, b) => a - b);
  const ref = midBins[Math.floor(midBins.length * 0.25)];
  const span = n * binHz;
  console.log(`  ${n} bins x ${(binHz / 1e3).toFixed(1)} kHz = ${(span / 1e6).toFixed(2)} MHz captured`);
  console.log(`  mid-band floor (p25): ${ref.toFixed(1)} dB   [${frames.length} frames]\n`);
  console.log('  fall from the mid-band floor    lower edge    upper edge   (kHz in from each end)');
  for (const drop of [3, 6, 10, 20]) {
    let lo = 0;  while (lo < mid && avg[lo] < ref - drop) lo++;
    let hi = n - 1; while (hi > mid && avg[hi] < ref - drop) hi--;
    console.log(`   -${String(drop).padStart(2)} dB                          `
      + `${((lo * binHz) / 1e3).toFixed(0).padStart(6)}        ${(((n - 1 - hi) * binHz) / 1e3).toFixed(0).padStart(6)}`
      + `      (${(100 * lo / n).toFixed(1)}% / ${(100 * (n-1-hi) / n).toFixed(1)}% of span)`);
  }
  send({ type: 'gain', auto: true });
  await sleep(500);
  ws.close(); audio.close();
  process.exit(0);
})();
