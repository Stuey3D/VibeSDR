/**
 * agc-sweep.mjs — measure what gain actually does to a station, instead of arguing about it.
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
  return { channelDb, shoulderDb, contrast, snr: channelDb - floorDb };
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  await sleep(2500);
  if (!gains.length) { console.error('no gain list from hwinfo — is this an RTL radio?'); process.exit(1); }
  console.log(`sweeping ${gains.length} gain steps at ${(FREQ / 1e6).toFixed(3)} MHz`
            + (RATE ? ` @ ${(RATE / 1e6).toFixed(1)} MS/s` : '') + `, ${DWELL / 1000}s per step\n`);
  console.log('  gain    chan   shoulder  chan−shldr  contrast   SNR   MPX S/N  RDS   ST  station');
  console.log('  ──────────────────────────────────────────────────────────────────────────────');
  const rows = [];
  for (const g of gains) {
    send({ type: 'gain', value: g });
    await sleep(600);                 // the write, then a clean measurement window
    frames = []; rds = null; mpx = null;
    await sleep(DWELL);
    const m = measure();
    if (!m) { console.log(`  ${(g/10).toFixed(1)} dB — no spectrum`); continue; }
    const ber = rds && Number.isFinite(rds.ber) ? rds.ber : -1;
    const mpxDb = mpx && mpx.ok ? mpx.db : null;
    const stereo = rds ? (rds.stereo === true) : null;
    const ps  = rds && rds.ps ? String(rds.ps).trim() : '';
    rows.push({ g, ...m, ber, ps, adcPeak, mpxDb, stereo });
    console.log(`  ${(g/10).toFixed(1).padStart(5)}  `
      + `${m.channelDb.toFixed(1).padStart(6)}  ${m.shoulderDb.toFixed(1).padStart(7)}  `
      + `${(m.channelDb - m.shoulderDb).toFixed(1).padStart(9)}  ${m.contrast.toFixed(1).padStart(7)}  `
      + `${m.snr.toFixed(1).padStart(5)}  ${(mpxDb === null ? '—' : mpxDb.toFixed(1)).padStart(7)}  `
      + `${(ber < 0 ? '—' : ber + '%').padStart(5)}  ${(stereo === null ? '—' : stereo ? 'ST' : '·').padStart(2)}  ${ps}`);
  }
  // ★ The point of the exercise: where do the candidate objectives actually peak?
  const best = (key, cmp) => rows.reduce((a, b) => (cmp(b[key], a[key]) ? b : a), rows[0]);
  const withRds = rows.filter((r) => r.ber >= 0);
  console.log('\n  ── where each candidate says the gain should be ──────────────────────────');
  console.log(`  best SNR          ${(best('snr', (x, y) => x > y).g / 10).toFixed(1)} dB`);
  console.log(`  best contrast     ${(best('contrast', (x, y) => x > y).g / 10).toFixed(1)} dB`);
  const bestSep = rows.reduce((a, b) =>
    (b.channelDb - b.shoulderDb) > (a.channelDb - a.shoulderDb) ? b : a, rows[0]);
  console.log(`  station clearest of its shoulders  ${(bestSep.g / 10).toFixed(1)} dB`);
  if (withRds.length) {
    const bestBer = withRds.reduce((a, b) => (b.ber < a.ber ? b : a), withRds[0]);
    console.log(`  fewest RDS errors ${(bestBer.g / 10).toFixed(1)} dB  (${bestBer.ber}% errors)`);
  } else {
    console.log('  fewest RDS errors —  (no RDS seen at any gain)');
  }
  const withMpx = rows.filter((r) => r.mpxDb !== null);
  if (withMpx.length) {
    const bestMpx = withMpx.reduce((a, b) => (b.mpxDb > a.mpxDb ? b : a), withMpx[0]);
    console.log(`  best MPX S/N      ${(bestMpx.g / 10).toFixed(1)} dB  (${bestMpx.mpxDb.toFixed(1)} dB)`);
  } else {
    console.log('  best MPX S/N      —  (no pilot to measure against)');
  }
  const st = rows.filter((r) => r.stereo);
  if (st.length) console.log(`  stereo locked     ${(st[0].g / 10).toFixed(1)} – ${(st[st.length-1].g / 10).toFixed(1)} dB`);
  else console.log('  stereo locked     —  (never locked)');
  /* ★★★ PUT THE AGC BACK. The sweep switches it off to take control of the gain, and an earlier
   *     run left it off — so a later "where does the AGC settle?" test recorded four stations all
   *     sitting at the resting gain and looked like a loop that had stopped working. A tool that
   *     changes a receiver's configuration has to hand it back as it found it. */
  send({ type: 'gain', auto: true });     // ★ VibeAGC back on — `gain:auto`, not `agc:on`
  await sleep(500);
  ws.close(); audio.close();
  process.exit(0);
})();
