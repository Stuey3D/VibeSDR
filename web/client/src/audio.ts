/**
 * audio.ts — VibeServer /ws/audio consumer (browser).
 *
 * Mirrors src/components/LocalAudioPlayer.tsx, but plays out through WebAudio
 * instead of the native module. Decoding reuses the app's own ADPCM decoder
 * (src/services/imaAdpcm.ts) verbatim — one codec across phone and web.
 *
 * Wire format (local_sdr_shim.cpp sendAudioPcm:1057):
 *   [0]    channels (1|2)
 *   [1]    format: 0 = raw int16, 1 = ADPCM mono, 2 = ADPCM mid/side
 *   [2..5] uint32 LE sample rate (48000)
 *   raw:   [6..]  interleaved int16 LE
 *   adpcm: [6..7] uint16 LE sample count per channel, [8..] self-seeded blocks
 *
 * A WFM stream silently drops from format 2 to format 1 when the stereo pilot
 * unlocks, so channel count must be read per frame, never cached.
 */

import { decodeVibeAdpcmFrame } from '../../../src/services/imaAdpcm';
// ★★★ THE OPUS DECODER THAT ALWAYS EXISTS. WebCodecs' AudioDecoder is [SecureContext] — on
// http://vibeserver.local:48000 it is simply UNDEFINED, so `supportsOpus()` said no, we asked
// for uncompressed, and the server (uncompressed off) REFUSED the socket: audio 0 KB/s, silent.
// It only ever worked on the dev Mac because loopback is exempt from the policy and gets raw PCM.
// UberSDR ships this same library (`opus-decoder.min.js`, wasm-audio-decoders, MIT) and plays
// fine over plain http on a LAN IP — WASM has no secure-context gate and no platform media stack
// to disagree with. The wasm is inlined in the module, so the single-file page stays self-contained.
import { OpusDecoder } from 'opus-decoder';
import { AudioSelfHeal, type HealDecision } from '../../../src/services/audioSelfHeal';
import { initSegment, mediaSegment } from './fmp4';
import { guard, guardCallbacks, guardJson, noteFault } from '../../../src/services/faultLog';

/** How much audio to hold before playout starts, in seconds. This is also very nearly
 *  how far the audio LAGS THE WATERFALL, so it is the A/V sync knob.
 *
 *  ★★★ 150 ms IS NOW SAFE, AND IT WAS NOT BEFORE. The first attempt at 150 broke tuning:
 *  a retune tore down and rebuilt the whole DSP audio chain, clearing its buffers, and
 *  the thinner cushion drained during that gap — silence and a re-arm on every dial step.
 *  That break has since been removed at the source: a retune inside the same mode and
 *  bandwidth now just re-points the NCO and rebuilds nothing (RxPipeline::setTune), so
 *  there is no deliberate gap left for the buffer to have to cover.
 *  ★ Which means the ORDER MATTERS if this is ever revisited: this number is only safe
 *    while that holds. If retunes start breaking the audio again, look for a rebuild that
 *    has crept back into the tune path before you reach for this constant.
 *  ★ ~~IF THE PUBLIC LINK STUTTERS, PUT IT BACK TO 0.25.~~ It did stutter, and this is now the
 *    STARTING depth rather than the whole policy — the buffer grows itself when it underruns.
 *    See JITTER_MAX_SEC below for why a constant was the wrong shape for this.
 */
const JITTER_SEC = 0.15;

/** ★★★ THE BUFFER GROWS WHEN IT IS PROVED TOO SHALLOW, and shrinks again when it is proved too
 *  deep. One constant could not serve both listeners: this is the A/V SYNC KNOB, so every
 *  millisecond added to cover a remote listener's jitter tail is a millisecond of lag charged to
 *  a LAN listener who never had a problem.
 *
 *  MEASURED, on the public demo through the Cloudflare tunnel (2026-08-10, after M9PSY reported
 *  audio dropping out): holes of up to 245 ms in the audio stream, and 14 of 14 of them were
 *  followed by a CATCH-UP BURST. That is the whole justification for a buffer here —
 *
 *  ★★★ NOTHING IS EVER LOST ON THIS PATH. It is a WebSocket, so it is TCP: a gap can only mean
 *      the stream was held up (head-of-line blocking behind a retransmit) and the packets behind
 *      it arrive the instant it clears. Late data is exactly what a buffer is for. Contrast the
 *      spectrum's "sticking", which looked identical and was frames the server never sent at all —
 *      no buffer could have helped there. Same symptom, opposite cause, opposite fix; the test
 *      that separates them is whether a burst FOLLOWS the gap.
 *
 *  ★★ 150 ms could not survive a 245 ms hole — it drains and re-arms, which IS the dropout. But
 *     a fixed 250 would have cleared that particular window by 5 ms, and picking a constant off
 *     one bad window is how you end up back here. So: start where the LAN wants it, and let the
 *     link itself say how much more it needs.
 *  ★ Growth is fast and decay is slow, deliberately: an underrun is audible and a little extra
 *    lag is not, so it should cost several clean minutes to give the depth back.
 */
const JITTER_MAX_SEC  = 0.40;   // ceiling — beyond this the lag is worse than the stutter
const JITTER_STEP_SEC = 0.06;   // added per underrun
const JITTER_DECAY_SEC = 45;    // clean run required before giving a step back

/** Playout worklet: a ring buffer drained at the device rate. Kept tiny — it
 *  runs on the audio thread. Late frames are dropped, not queued, so a stalled
 *  link never accumulates lag. */
const WORKLET_SRC = `
class VibeSink extends AudioWorkletProcessor {
  constructor() {
    super();
    this.cap = 48000 * 2;              // ~2s per channel
    this.buf = [new Float32Array(this.cap), new Float32Array(this.cap)];
    this.w = 0; this.r = 0; this.filled = 0;
    this.started = false;
    // See JITTER_SEC — the buffer is what makes the audio lag the waterfall, and it became
    // obvious once the waterfall was tied to the display refresh and stopped hitching.
    this.target = 48000 * ${JITTER_SEC};
    this.base   = 48000 * ${JITTER_SEC};
    this.max    = 48000 * ${JITTER_MAX_SEC};
    this.step   = 48000 * ${JITTER_STEP_SEC};
    this.decayAfter = 48000 * ${JITTER_DECAY_SEC};
    this.cleanFor = 0;          // samples drained since the last underrun
    // ★ A retune FLUSH is not an underrun. It deliberately empties the buffer and re-arms, and
    //   counting it would make the buffer grow every time the dial moved — punishing the user for
    //   tuning, which is the one thing they do constantly.
    this.armedByFlush = false;
    this.drained = 0;            // samples this node has actually put out — see process()
    // ★★★ COUNT THE STARVATIONS, and say so unconditionally. The jitter report below only fires
    //     when the target CHANGES, so a node already at max underruns in complete silence — which
    //     is precisely the case a listener notices and we could not see (Stuart, 2026-08-21: "the
    //     audio is still dropping"). A stutter caused by starvation and one caused by the decoder
    //     look identical from the page; this is the single number that separates them.
    this.underruns = 0;
    // ★★★ THE OTHER WAY SAMPLES DISAPPEAR, AND NOTHING COUNTED IT. Both discard paths below throw
    //     audio away WITHOUT an underrun — the overflow guard and the latency trim — so a listener
    //     hears a hitch while every counter reads clean. Stuart, with the status row showing no
    //     dry count at all and the audio still breaking up: "cant see a dry measurement". There
    //     was nothing wrong with the measurement; it was measuring the wrong discard.
    //  ★ A trim is EXPECTED occasionally (the server's clock and the sound card's differ). A trim
    //    every few seconds means audio is arriving in bursts, which is what a DSP at 85% of real
    //    time does.
    this.skips = 0;
    this.lastReport = 0;
    /* ★★★ A SECOND WAY IN, SO THE MAIN THREAD NEED NOT BE ON THE AUDIO PATH AT ALL. An
     *     AudioWorkletNode's own port belongs to the page, so decoding in a Worker and posting
     *     through it would still hop through the very thread we are trying to get out of. Instead
     *     the page transfers us one end of a MessageChannel ({sinkPort}) and hands the other to
     *     the Worker, which then feeds this node DIRECTLY. Rendering can jank as much as it likes.
     * ★★ TELEMETRY STILL GOES OUT ON this.port, never on the feed: skips, underruns, jitter and
     *    the drained heartbeat are all read by the PAGE, and the Worker is not where they are
     *    wanted. One way in, two ways out, and they are deliberately not the same channel.
     * ★ Same handler for both, so a flush from the page and PCM from the Worker cannot drift
     *   apart — there is one implementation of what a message means. */
    this.feed = null;
    // ★ Samples handed IN (fed) as distinct from samples played OUT (drained) — see the self-heal
    //   watchdog on the page: the two together say whether a silence is the decoder upstream or
    //   this node, which is what decides the repair. Reported every 250 ms of input.
    this.fed = 0; this.lastFedReport = 0;
    // ★ Fault injection for the self-heal test (AudioPlayer.debugFault('player')): stop draining
    //   while frames keep arriving — the exact signature of a wedged output. A NEW node clears it,
    //   which is the repair being tested.
    this.faultStall = false;
    this.port.onmessage = (e) => {
      if (e.data && e.data.fault === 'stall') { this.faultStall = true; return; }
      if (e.data && e.data.sinkPort) {
        this.feed = e.data.sinkPort;
        this.feed.onmessage = (ev) => this.onAudioMsg(ev);
        return;
      }
      this.onAudioMsg(e);
    };
  }

  onAudioMsg(e) {
    {
      // ★★★ FLUSH ON RETUNE. Everything already queued was demodulated at the OLD frequency, so
      //     playing it out after the dial has moved is just the previous station arriving late —
      //     which is exactly what "the audio is a second behind the waterfall when I tune" is
      //     (Stuart, 2026-08-07). Dropping it costs a few milliseconds of silence and removes the
      //     entire lag; keeping it buys nothing anybody wants to hear.
      //     * Re-arm rather than play immediately: started=false makes the buffer refill to
      //       its target before playout resumes, the same protection a cold start gets.
      //     * NOTE: this block lives inside a template literal — no backticks in here.
      if (e.data && e.data.flush) {
        this.r = this.w; this.filled = 0; this.started = false;
        this.armedByFlush = true;      // the re-arm that follows is ours, not the link's fault
        return;
      }
      const { l, r } = e.data;
      const n = l.length;
      this.fed += n;
      if (this.fed - this.lastFedReport >= 12000) { this.lastFedReport = this.fed; this.port.postMessage({ fed: this.fed }); }
      if (this.filled + n > this.cap) {   // overflow: drop oldest
        this.skips++; this.port.postMessage({ skips: this.skips });
        const drop = this.filled + n - this.cap;
        this.r = (this.r + drop) % this.cap;
        this.filled -= drop;
      }
      for (let i = 0; i < n; i++) {
        const w = (this.w + i) % this.cap;
        this.buf[0][w] = l[i];
        this.buf[1][w] = r[i];
      }
      this.w = (this.w + n) % this.cap;
      this.filled += n;
      // ★★★ BOUND THE LATENCY, do not just bound the memory. The only trim here was the overflow
      //     guard above, which fires at 2 SECONDS — so if frames arrive even slightly faster than
      //     the device drains them (they do: the server's clock and the sound card's are not the
      //     same crystal), the buffer creeps up and STAYS there. The audio then lags the waterfall
      //     by however far it crept, permanently, and nothing ever brings it back. That is the
      //     other half of "the audio can be up to a full second behind" (Stuart, 2026-08-07).
      //     ★★ Trim back to the target, not to zero: dropping to empty would re-arm and stutter.
      //        The discarded samples are the OLDEST, so what is thrown away is the stalest audio.
      //     ★ The margin is deliberately generous (2.5x). Trimming near the target would fight
      //       normal jitter and click constantly; at this depth it fires rarely, and a rare small
      //       discontinuity is far cheaper than a permanent half-second of lag.
      const ceiling = this.target * 2.5;
      if (this.filled > ceiling) {
        this.skips++; this.port.postMessage({ skips: this.skips });
        const drop = this.filled - this.target;
        this.r = (this.r + drop) % this.cap;
        this.filled -= drop;
      }
      if (!this.started && this.filled >= this.target) this.started = true;
    };
  }
  process(_inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    if (this.faultStall) { for (let c = 0; c < out.length; c++) out[c].fill(0); return true; }
    if (!this.started || this.filled < n) {
      // Underrun — output silence and re-arm the jitter buffer.
      if (this.started && this.filled < n) {
        this.started = false;
        // ★★ THE LINK HAS JUST PROVED THIS DEPTH TOO SHALLOW. Grow, unless we emptied the buffer
        //    ourselves on a retune — that re-arm is expected and says nothing about the network.
        // ★ Read and clear ONCE. Both tests below need it, and clearing it inside the first
        //   would make the second read false and grow the buffer on every retune.
        const wasFlush = this.armedByFlush;
        this.armedByFlush = false;
        if (!wasFlush) {
          // ★ Counted BEFORE the growth test, so an underrun at max depth is still an underrun.
          this.underruns++;
          this.port.postMessage({ underruns: this.underruns });
        }
        if (!wasFlush && this.target < this.max) {
          this.target = Math.min(this.max, this.target + this.step);
          this.cleanFor = 0;
          // Tell the page, so a listener's depth is observable rather than inferred — the whole
          // reason this was hard to diagnose is that a stutter looks the same from every cause.
          this.port.postMessage({ jitterMs: Math.round(this.target / 48) });
        }
      }
      for (let c = 0; c < out.length; c++) out[c].fill(0);
      return true;
    }
    // ★ Decay: a long clean run means we are carrying lag we no longer need. Give a step back,
    //   slowly — see JITTER_DECAY_SEC. Counted in samples actually DRAINED, so a paused or
    //   silent stream cannot earn its way down without really having played.
    this.cleanFor += n;
    if (this.cleanFor >= this.decayAfter) {
      this.cleanFor = 0;
      if (this.target > this.base) {
        this.target = Math.max(this.base, this.target - this.step);
        this.port.postMessage({ jitterMs: Math.round(this.target / 48) });
      }
    }
    let pk = 0;
    for (let i = 0; i < n; i++) {
      const r = (this.r + i) % this.cap;
      for (let c = 0; c < out.length; c++) out[c][i] = this.buf[Math.min(c, 1)][r];
      const a = Math.abs(this.buf[0][r]); if (a > pk) pk = a;
    }
    this.r = (this.r + n) % this.cap;
    this.filled -= n;
    // ★ AUDIBLE, as distinct from DRAINED: drained counts silence too (the buffer plays out
    //   zeros between stations), so a "has the new station started?" test built on it fired
    //   2-3 s early (Stuart, 2026-09-10). This is the peak of what actually left, on every
    //   decode path, reported at most every ~100 ms.
    if (pk > 0.002) {
      this.audibleSince = (this.audibleSince || 0) + n;
      if (this.audibleSince >= 4800) { this.audibleSince = 0; this.port.postMessage({ audible: 1 }); }
    }
    // ★★★ SAY THAT SOUND IS ACTUALLY LEAVING. Everything else the page can see — frames arriving,
    //     packets decoding, a peak level, even a clean RECORDING — is measured BEFORE this node.
    //     So a stalled output looks identical to a healthy stream from every vantage point the
    //     page had, which is exactly how a listener sat in silence while his own recording of the
    //     same stream came out perfect (Stuart, 2026-08-20, shared VFO, another user tuning).
    /* ★ FOUR TIMES A SECOND, NOT ONCE — AND THE RATIO IS THE WHOLE POINT. This is a heartbeat,
     *   not telemetry, and the watchdog on the far side calls the node dead after 2 s. A 1 s
     *   heartbeat against a 2 s deadline is only TWO MISSES of margin, and 'lastDrainAt' is
     *   stamped when the MAIN THREAD receives the message — so a main thread busy for a second
     *   (GC, a heavy render) made a perfectly healthy node look stalled and got it REBUILT.
     * ★★★ THAT IS AUDIBLE, AND IT IS THE BUG IT WAS MEANT TO FIX WEARING THE OTHER MASK: rebuilding
     *     the playout node interrupts playback, so a false positive does not merely waste work, it
     *     IS a stutter. Reported by a second owner on Firefox (2026-08-26) — 23 rebuilds, every one
     *     logged "attempt 1", while Edge and Safari on the same server were clean. He could HEAR
     *     the audio: a node that had truly stopped draining would have been SILENT.
     * ★★ 12000 samples = 250 ms of output, so the deadline is now eight misses rather than two.
     *    A genuine stall still shows nothing for 2 s and is still caught; only the false positives
     *    go. Four postMessages a second is nothing next to the audio it carries. */
    this.drained += n;
    if (this.drained - this.lastReport >= 12000) {
      this.lastReport = this.drained;
      this.port.postMessage({ drained: this.drained });
    }
    return true;
  }
}
registerProcessor('vibe-sink', VibeSink);
`;

/** Wrap int16 PCM in a 44-byte canonical WAV header. */
/** ★★★ THE AUDIO PATH, OFF THE PAGE'S THREAD ENTIRELY.
 *
 *  Everything the sound needs — receiving the socket, decoding Opus, converting to float — used to
 *  run on the main thread, and every one of those steps competes with drawing. That is survivable
 *  until the browser is busy: on Firefox, with the tab VISIBLE, the waterfall starved this path and
 *  the jitter buffer ran dry; the same session with the tab hidden (no rendering) was clean, on the
 *  same server, over the same tunnel. Safari and Edge coped, which is why it read as a Firefox bug
 *  rather than as an architectural one (a second owner, 2026-08-26).
 *  ★★★ MOVING THE DECODE ALONE WOULD ACHIEVE NOTHING. An AudioWorkletNode's port belongs to the
 *      page, so PCM would still hop through the main thread on its way to the speaker. The Worker
 *      is handed one end of a MessageChannel whose other end has been TRANSFERRED INTO THE WORKLET,
 *      so it feeds the node directly and the page is not involved at all.
 *  ★★ WebCodecs ONLY, deliberately. It exists wherever an AudioWorklet does (both want a secure
 *     context) and Firefox has it — its own console says "requesting Opus (WebCodecs decoder)".
 *     The WASM decoder stays on the main thread for browsers without it, which are the same
 *     plain-http LAN origins that have no AudioWorklet either and were never on this path.
 *  ★ No imports: this is a blob-URL Worker, exactly like the worklet above, so it cannot pull in
 *    a module. That constraint is what keeps it out of the build system.
 *  ★ NOTE: this block lives inside a template literal — no backticks in here. */
const WORKER_SRC = `
let sink = null;         // MessagePort straight to the worklet
let ws = null;
let dec = null;
let decCh = 0;
let ts = 0;
let recording = false;
let closedByUs = false;
let url = '';
// ★ Media playout (Safari): hand the Opus packets to the page UNDECODED — a MediaSource on an
//   <audio> element decodes them. The decoder here is then only for the recorder.
let rawOpus = false;
// ★ Fault injection for the self-heal test (AudioPlayer.debugFault). 'decoder' swallows decoded
//   output until the decoder is rebuilt ('reset'); 'drop' discards frames after counting them and
//   'freeze' ignores the socket entirely (a half-open link) — both until the socket is reopened.
let fault = '';

function toFloat(pcm, ch, frames) {
  const l = new Float32Array(frames);
  const r = new Float32Array(frames);
  if (ch === 2) {
    for (let i = 0; i < frames; i++) { l[i] = pcm[i*2] / 32768; r[i] = pcm[i*2+1] / 32768; }
  } else {
    for (let i = 0; i < frames; i++) { const v = pcm[i] / 32768; l[i] = v; r[i] = v; }
  }
  return [l, r];
}

function emit(pcm, ch) {
  const frames = Math.floor(pcm.length / Math.max(1, ch));
  if (frames <= 0) return;
  // ★ The recorder lives on the page and taps PCM BEFORE the node, so a recording stays perfect
  //   even when playout is not. Forward it ONLY while recording, so the ordinary case never pays.
  if (recording) self.postMessage({ type: 'pcm', pcm, ch }, [pcm.buffer.slice(0)]);
  if (!sink) return;
  const [l, r] = toFloat(pcm, ch, frames);
  sink.postMessage({ l, r }, [l.buffer, r.buffer]);
}

function ensureDec(ch) {
  if (dec && decCh === ch) return;
  if (dec) { try { dec.close(); } catch (e) {} }
  decCh = ch;
  dec = new AudioDecoder({
    output: (ad) => {
      if (fault === 'decoder') { ad.close(); return; }
      const n = ad.numberOfFrames, nc = ad.numberOfChannels;
      const pcm = new Int16Array(n * nc);
      const plane = new Float32Array(n);
      for (let c = 0; c < nc; c++) {
        /* ★★★ format:'f32-planar' IS NOT OPTIONAL, AND LEAVING IT OFF COST THE WHOLE FEATURE.
         *     Firefox's decoder hands back INTERLEAVED f32, so copyTo with a planeIndex and no
         *     format wants room for every channel — "destination buffer of length 3840 too small
         *     for copying 7680 elements", which threw, which tripped the Worker's own error path,
         *     which fell back to the main thread on EVERY session. The fallback worked perfectly;
         *     that is why it looked like the Worker had been tried and had not helped.
         * ★★★ AND THE WORKING LINE WAS TWENTY LINES AWAY. _onOpusData has always passed this
         *     option. I rewrote the copy loop from memory instead of copying the one that was
         *     already right — in a Worker, where the only symptom is a fallback that hides it. */
        ad.copyTo(plane, { planeIndex: c, format: 'f32-planar' });
        for (let i = 0; i < n; i++) {
          let v = Math.round(plane[i] * 32767);
          pcm[i*nc + c] = v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
        }
      }
      ad.close();
      emit(pcm, nc);
    },
    error: (e) => self.postMessage({ type: 'decoderFailed', why: String(e && e.message || e) }),
  });
  dec.configure({ codec: 'opus', sampleRate: 48000, numberOfChannels: ch });
}

function onFrame(buf) {
  if (buf.byteLength < 6) return;
  const dv = new DataView(buf);
  const channels = dv.getUint8(0);
  const format = dv.getUint8(1);
  if (fault === 'freeze') return;
  self.postMessage({ type: 'bytes', n: buf.byteLength, f: format });
  if (fault === 'drop') return;
  if (format === 3) {
    const ch = channels || 1;
    // ★ Raw mode: the page's media element decodes. Decode here too ONLY while recording, and
    //   slice first — the transfer below detaches the buffer.
    if (rawOpus) {
      if (recording) {
        try { ensureDec(ch); dec.decode(new EncodedAudioChunk({ type: 'key', timestamp: ts, duration: 20000, data: buf.slice(6) })); ts += 20000; }
        catch (e) { /* the recording is best-effort on this path */ }
      }
      self.postMessage({ type: 'opus', buf }, [buf]);
      return;
    }
    try {
      ensureDec(ch);
      dec.decode(new EncodedAudioChunk({ type: 'key', timestamp: ts, duration: 20000, data: buf.slice(6) }));
      ts += 20000;
    } catch (e) { self.postMessage({ type: 'decoderFailed', why: String(e && e.message || e) }); }
    return;
  }
  if (format === 0) { emit(new Int16Array(buf, 6, (buf.byteLength - 6) >> 1), channels); return; }
  /* ★★★ DAB+ (format 4) GOES TO THE PAGE, BUT THE SOCKET STAYS HERE. It cannot be decoded in a
   *  worker: the AAC path negotiates a decoder configuration, falls back between 960 and 1024
   *  frame lengths, and falls back again to MediaSource — which needs an <audio> element and a
   *  document, neither of which exists in here.
   *  ★★★ BUT "unhandled" TORE DOWN THE WHOLE WORKER, taking the WebSocket and the Opus decoder to
   *      the main thread with it — so selecting one DAB+ service moved EVERYTHING onto the thread
   *      that also draws the waterfall. Both consoles showed it: "worker path failed (format 4) —
   *      falling back to the main thread". Forwarding the frame instead keeps the network and
   *      every other format off the main thread, and hands over only what genuinely cannot run
   *      here. The buffer is transferred, not copied. */
  if (format === 4) { self.postMessage({ type: 'passthru', buf }, [buf]); return; }
  // ★ Legacy IMA-ADPCM is retired server-side. Hand anything else back rather than guess.
  self.postMessage({ type: 'unhandled', format });
}

/* ★★★ ONE SOCKET, AND A REPLACED ONE IS RETIRED — NOT MERELY CLOSED. The self-heal's socket
 *  reopen ('url' below) used to do closedByUs = true; ws.close(); closedByUs = false; open(). But
 *  close() is ASYNCHRONOUS: the old socket's close event arrives after the flag is already false
 *  again, so its onclose scheduled a reconnect of its own, and 3 s later a SECOND socket opened.
 *  On a shared dial the server closes the older socket of the same session when a new one arrives
 *  (local_sdr_shim.cpp, "A RECONNECT IS STILL A RECONNECT"), whose onclose again reconnected: an
 *  audio reconnect every 3 s, for the life of the Worker, from ONE repair. On a server that does
 *  not take over, both sockets stayed up and fed the same decoder and playout node — twice the
 *  audio into a buffer drained at 1x. Measured with this exact source against both server rules
 *  (2026-09-29). The page's own _reopenSocket always nulled onclose first; this copy did not.
 *  ★ So: a replaced socket has its handlers detached before it is closed, every handler ignores a
 *    socket that is no longer the current one, and there is one reconnect timer, cleared by
 *    anything that opens or closes on purpose. */
let reconnectTimer = null;
function retire(s) {
  if (!s) return;
  s.onopen = null; s.onerror = null; s.onclose = null; s.onmessage = null;
  try { s.close(); } catch (err) { self.postMessage({ type: 'fault', kind: 'retire', why: String(err && err.message || err) }); }
}
function open() {
  if (reconnectTimer !== null) { clearTimeout(reconnectTimer); reconnectTimer = null; }
  closedByUs = false;
  retire(ws);
  const me = new WebSocket(url);
  ws = me;
  me.binaryType = 'arraybuffer';
  me.onopen  = () => { if (ws === me) self.postMessage({ type: 'status', s: 'open' }); };
  me.onerror = () => { if (ws === me) self.postMessage({ type: 'status', s: 'error', msg: 'audio websocket error' }); };
  me.onclose = () => {
    if (ws !== me) return;           // superseded — its replacement owns the reconnect
    self.postMessage({ type: 'status', s: 'closed' });
    // ★ Same three seconds as the page used to use — one reconnect policy, moved, not rewritten.
    if (!closedByUs && reconnectTimer === null) reconnectTimer = setTimeout(() => { reconnectTimer = null; open(); }, 3000);
  };
  /* ★★★ ONE BAD FRAME COSTS ONE FRAME. A throw out of onFrame used to reach the page as the
   *  Worker's onerror — which tears the whole Worker down and falls audio back to the main thread.
   *  Now the frame is dropped and the page is told, so it can count and log it (faultLog). */
  me.onmessage = (e) => {
    if (ws !== me) return;             // a retired socket's late frames are not this stream
    if (typeof e.data === 'string') {
      let m = null;
      try { m = JSON.parse(e.data); }
      catch (err) { self.postMessage({ type: 'fault', kind: 'bad-json', why: String(err && err.message || err) }); return; }
      if (m && m.type === 'needs_codec') self.postMessage({ type: 'needsCodec' });
      return;
    }
    if (e.data instanceof ArrayBuffer) {
      const n = e.data.byteLength;
      try { onFrame(e.data); }
      catch (err) { self.postMessage({ type: 'fault', kind: 'frame', why: String(err && err.message || err) + ' len=' + n }); }
    }
  };
}

self.onmessage = (e) => {
  const d = e.data || {};
  if (d.type === 'init')      { sink = d.sinkPort || null; rawOpus = !!d.rawOpus; url = d.url; open(); }
  // ★ The self-heal's socket reopen. open() retires the current socket itself — see retire().
  else if (d.type === 'url')  { if (fault === 'drop' || fault === 'freeze') fault = ''; url = d.url; open(); }
  /* ★★ SELF-HEAL, rung one: a NEW decoder (the old one may be wedged or erroring quietly) and, when
   *    the page rebuilt the playout node, the new node's feed port. The socket is untouched. */
  else if (d.type === 'reset') {
    if (fault === 'decoder') fault = '';
    if (dec) { try { dec.close(); } catch (err) {} }
    dec = null; decCh = 0; ts = 0;
    if (d.sinkPort) sink = d.sinkPort;
  }
  else if (d.type === 'fault') { fault = d.kind || ''; }
  else if (d.type === 'rec')  { recording = !!d.on; }
  else if (d.type === 'close'){
    closedByUs = true;
    if (reconnectTimer !== null) { clearTimeout(reconnectTimer); reconnectTimer = null; }
    try { ws && ws.close(); } catch (err) { self.postMessage({ type: 'fault', kind: 'close', why: String(err && err.message || err) }); }
  }
};
`;

/* ── A minimal WebM (Matroska) muxer for live Opus ───────────────────────────────────────────
 *  Just enough EBML for a MediaSource: an initialisation segment (EBML header + Segment of
 *  unknown size with Info and one A_OPUS TrackEntry) and then one Cluster per packet. Sizes are
 *  written exactly, except the Segment's, which is the "unknown" marker so it never has to be
 *  patched. This is what UberSDR's player does, and what Chrome's MediaRecorder writes. */
function ebmlId(id: number): number[] {
  const out: number[] = [];
  if (id >= 0x1000000) out.push((id >>> 24) & 0xff);
  if (id >= 0x10000)   out.push((id >>> 16) & 0xff);
  if (id >= 0x100)     out.push((id >>> 8) & 0xff);
  out.push(id & 0xff);
  return out;
}
function ebmlSize(n: number): number[] {
  // Variable-length integer: 1..8 bytes, leading 1-bit marks the length.
  if (n < 0x7f)         return [0x80 | n];
  if (n < 0x3fff)       return [0x40 | (n >>> 8), n & 0xff];
  if (n < 0x1fffff)     return [0x20 | (n >>> 16), (n >>> 8) & 0xff, n & 0xff];
  if (n < 0x0fffffff)   return [0x10 | (n >>> 24), (n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff];
  return [0x01, 0, 0, 0, (n >>> 24) & 0xff, (n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff];
}
function ebml(id: number, body: number[] | Uint8Array): number[] {
  const b = body instanceof Uint8Array ? Array.from(body) : body;
  return [...ebmlId(id), ...ebmlSize(b.length), ...b];
}
function ebmlUint(id: number, v: number): number[] {
  const b: number[] = [];
  let x = v;
  do { b.unshift(x & 0xff); x = Math.floor(x / 256); } while (x > 0);
  return ebml(id, b);
}
function ebmlStr(id: number, s: string): number[] {
  return ebml(id, Array.from(s, (c) => c.charCodeAt(0) & 0x7f));
}
function ebmlFloat(id: number, v: number): number[] {
  const dv = new DataView(new ArrayBuffer(4)); dv.setFloat32(0, v);
  return ebml(id, [dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3)]);
}
function opusHead(channels: number): number[] {
  // RFC 7845 §5.1. Pre-skip 312 and a 6.5 ms CodecDelay are libopus's own figures.
  const b = new Uint8Array(19); const dv = new DataView(b.buffer);
  'OpusHead'.split('').forEach((c, i) => { b[i] = c.charCodeAt(0); });
  b[8] = 1; b[9] = channels;
  dv.setUint16(10, 312, true); dv.setUint32(12, 48000, true); dv.setInt16(16, 0, true); b[18] = 0;
  return Array.from(b);
}
function webmInit(channels: number): Uint8Array {
  const header = ebml(0x1A45DFA3, [
    ...ebmlUint(0x4286, 1), ...ebmlUint(0x42F7, 1), ...ebmlUint(0x42F2, 4), ...ebmlUint(0x42F3, 8),
    ...ebmlStr(0x4282, 'webm'), ...ebmlUint(0x4287, 4), ...ebmlUint(0x4285, 2),
  ]);
  const info = ebml(0x1549A966, [
    ...ebmlUint(0x2AD7B1, 1000000), ...ebmlStr(0x4D80, 'vibesdr'), ...ebmlStr(0x5741, 'vibesdr'),
  ]);
  /* ★★★ DefaultDuration IS LOAD-BEARING. A SimpleBlock carries no duration; without this the
   *  demuxer GUESSES one, and Safari's guess made the element's timeline run ~6 % faster than
   *  the audio inside the packets (the cushion grew 60 ms every second on a steady stream,
   *  2026-09-14, and every attempt to hold it produced stutter — "the audio is really
   *  unstable"). 20 ms per block, exactly what the encoder cuts (960 samples at 48 kHz). */
  const track = ebml(0xAE, [
    ...ebmlUint(0xD7, 1), ...ebmlUint(0x73C5, 1), ...ebmlUint(0x83, 2), ...ebmlStr(0x86, 'A_OPUS'),
    ...ebmlUint(0x23E383, 20_000_000),
    ...ebmlUint(0x56AA, 6500000), ...ebmlUint(0x56BB, 80000000), ...ebml(0x63A2, opusHead(channels)),
    ...ebml(0xE1, [...ebmlFloat(0xB5, 48000), ...ebmlUint(0x9F, channels)]),
  ]);
  const tracks = ebml(0x1654AE6B, track);
  // Segment with the unknown-size marker, then its first children.
  const segment = [...ebmlId(0x18538067), 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, ...info, ...tracks];
  return new Uint8Array([...header, ...segment]);
}
function webmCluster(timecodeMs: number, packets: Uint8Array[]): Uint8Array {
  // One SimpleBlock per packet: track 1 (vint 0x81), relative timecode (int16 ms, 20 per block),
  // flags 0x80 (keyframe — every Opus packet is), then the packet.
  const blocks: Uint8Array[] = packets.map((p, i) => {
    const rel = i * 20;
    const head = [...ebmlId(0xA3), ...ebmlSize(4 + p.byteLength), 0x81, (rel >> 8) & 0xff, rel & 0xff, 0x80];
    const b = new Uint8Array(head.length + p.byteLength);
    b.set(head, 0); b.set(p, head.length);
    return b;
  });
  const tc = ebmlUint(0xE7, timecodeMs);
  let bodyLen = tc.length; for (const b of blocks) bodyLen += b.byteLength;
  const head = [...ebmlId(0x1F43B675), ...ebmlSize(bodyLen), ...tc];
  const out = new Uint8Array(head.length + bodyLen - tc.length);
  out.set(head, 0);
  let off = head.length; for (const b of blocks) { out.set(b, off); off += b.byteLength; }
  return out;
}

function wavBlob(pcm: Int16Array, channels: number, rate: number): Blob {
  const dataBytes = pcm.length * 2;
  const header = new ArrayBuffer(44);
  const dv = new DataView(header);
  const ascii = (off: number, s: string) => {
    for (let i = 0; i < s.length; i++) dv.setUint8(off + i, s.charCodeAt(i));
  };
  ascii(0, 'RIFF');
  dv.setUint32(4, 36 + dataBytes, true);
  ascii(8, 'WAVE');
  ascii(12, 'fmt ');
  dv.setUint32(16, 16, true);              // PCM chunk size
  dv.setUint16(20, 1, true);               // format = PCM
  dv.setUint16(22, channels, true);
  dv.setUint32(24, rate, true);
  dv.setUint32(28, rate * channels * 2, true);  // byte rate
  dv.setUint16(32, channels * 2, true);         // block align
  dv.setUint16(34, 16, true);                   // bits per sample
  ascii(36, 'data');
  dv.setUint32(40, dataBytes, true);
  const body = new Uint8Array(pcm.buffer as ArrayBuffer, pcm.byteOffset, dataBytes);
  return new Blob([header, body], { type: 'audio/wav' });
}

export interface AudioCallbacks {
  onStatus?: (s: 'open' | 'closed' | 'error', detail?: string) => void;
  /** Bytes received, for the link meter. */
  onBytes?: (n: number) => void;
  /** Peak level of the last frame, 0..1 — drives the audio meter. */
  onLevel?: (peak: number) => void;
}

/**
 * One second of silence, 8 kHz mono WAV. Only ever used as the Chromium media-session anchor.
 *
 * ★★ SERVED AS A BLOB, NOT A data: URI. As a data: URI, looping this leaked ~700 MB/SECOND in
 * Chromium — see `_needsAnchor`. A blob URL is decoded from a real resource and is the shape
 * Chromium's media pipeline expects; whether that alone cures it is UNVERIFIED, which is why the
 * anchor stays opt-in.
 */
const SILENT_WAV_B64 = 'UklGRiQAAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQAAAAA=';
function silentLoopUrl(): string {
  const bin = atob(SILENT_WAV_B64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return URL.createObjectURL(new Blob([bytes], { type: 'audio/wav' }));
}

export class AudioPlayer {
  /**
   * ★★ OFF BY DEFAULT — THIS LEAKED ~700 MB/SECOND IN CHROMIUM.
   *
   * The anchor is a silent, looping <audio> element added so Chromium would attach its Global
   * Media Controls (it refuses to attach them to a MediaStream `srcObject`). It worked — Now
   * Playing appeared in Edge — and it also made Edge consume 13 GB of RAM, peg a performance core
   * at ~38%, run at 89°C, hang on opening a tab, and stall the whole browser. Chromium appears to
   * re-decode the clip on every loop and never release it: 3,600 loops an hour.
   *
   * MEASURED, same machine and page: with the anchor Edge sat at 38% CPU and climbing memory;
   * without it, 6.8% and flat — lower than Safari. Nothing else moved that number, and several
   * plausible-looking render fixes were tried first and did not.
   *
   * ★★ THE BLOB URL WAS TESTED TOO, AND LEAKS IDENTICALLY — straight back to 38%. So it is not the
   * URI scheme: Chromium leaks on a LOOPING MEDIA ELEMENT itself. The controls do come back, so the
   * mechanism works perfectly; it is simply unaffordable. DO NOT retry this with another URL form,
   * another container, or a longer clip — a longer clip only makes the leak slower, and a slow leak
   * is worse than a fast one because it hides.
   *
   * Now Playing on Chromium is therefore ABANDONED until Chromium changes or a fundamentally
   * different mechanism appears (something that is not a looping element — a real streamed
   * response, or whatever Chromium eventually accepts for Web Audio). The cost of the feature is a
   * browser that eats 13 GB and stops responding; the cost of not having it is a missing widget.
   * That is not a close call.
   *
   * `#anchor` still exists purely so the experiment can be repeated cheaply if the landscape
   * changes. Watch MEMORY for minutes, not CPU for seconds.
   */
  /**
   * `#nomediastream` — skip the MediaStream element and connect straight to ctx.destination
   * (what the client did before Now Playing was added, afe54bd9).
   *
   * ★ IT DOES NOT GET YOU AIRPODS SPATIAL AUDIO. That was the reason it was written and the
   * reason failed: tested 2026-07-25 on macOS + AirPods Max, "Spatialise Stereo" stayed
   * Not Available with the MediaStream gone, while YouTube in the same Safari offered it. So the
   * blocker is NOT the MediaStream/call-like classification (which is real, and is why Chromium
   * won't attach its widget to `srcObject` — but it is not what gates spatialisation).
   *
   * ★ What it DOES do, found by accident: on CHROMIUM the media keys start working. With no
   * element, Chromium registers the page's Media Session action handlers, so play/pause and
   * next/prev reach us and the skip buttons genuinely tune. The card carries no artwork and no
   * metadata, but the CONTROLS work — which is more than the default path gives Chromium, and it
   * costs nothing (no anchor element, so none of the 13 GB leak above).
   *
   * On Safari it is a pure loss: the default path already gives a full card with metadata and
   * artwork, and this throws that away for nothing. Hence a flag, not a default — until the
   * Chromium half is confirmed against a no-flag baseline.
   */
  private static _isChromium(): boolean {
    const ua = navigator.userAgent;
    return /Chrome|Chromium|Edg\//.test(ua) && !/^((?!Chrome|Chromium).)*Safari/.test(ua);
  }

  /**
   * Whether to route through the MediaStream element at all. THE TWO ENGINES WANT OPPOSITE
   * THINGS, so this is decided per browser rather than picked once:
   *
   *   Safari   — YES. The element is what gives the full Now Playing card: title, frequency,
   *              station name and artwork. Works today, keep it.
   *   Chromium — NO. It has never attached its Global Media Controls to a `srcObject` element
   *              (it treats MediaStream as call-like), so the element buys Chromium nothing —
   *              and while it is present the media keys don't reach us either. Drop it and
   *              Chromium registers the page's Media Session action handlers instead: the
   *              transport buttons work and the skip keys genuinely tune. Metadata and artwork
   *              still don't attach, but working controls beat a widget we never got.
   *
   * Overrides for A/B: `#mediastream` forces it on, `#nomediastream` forces it off.
   */
  private static _useMediaStream(): boolean {
    if (location.hash.includes('nomediastream')) return false;
    if (location.hash.includes('mediastream')) return true;
    return !AudioPlayer._isChromium();
  }

  /** WebKit proper: Safari on macOS and iOS, and every iOS browser (they are all WebKit). Not
   *  Chromium (which also says AppleWebKit) and not Firefox (which does not). */
  static isWebKit(): boolean {
    return /AppleWebKit/.test(navigator.userAgent) && !AudioPlayer._isChromium();
  }

  /** ★ Set once the media path has proved unable to play in this page — the Web Audio path takes
   *  over for the rest of the session, and the toggle is left as the listener set it. */
  private static mediaPlayoutBroken = false;

  /**
   * ★★★ MEDIA PLAYOUT — Safari plays our Opus through a MediaSource on an <audio> element, with NO
   * Web Audio anywhere in the chain. Two reasons, both Safari-only, both measured:
   *   1. macOS spatialises MEDIA-ELEMENT playback (Apple's media renderer) and never Web Audio
   *      output (a raw output unit) — proved 2026-08-02 with a plain WAV in an <audio src>, and
   *      the reason "Spatialise Stereo" was grey for every Web Audio path we tried.
   *   2. The Safari silence (2026-09-14): the Web Audio output unit stops delivering render
   *      callbacks while the context still says "running" (WebKit bug 263627, open since 2023).
   *      The media renderer is a different playout path with its own session bookkeeping.
   * Opt-in from the audio menu (WebKit only; the row is not offered elsewhere), `#msaudio` forces
   * it on for a test and `#nomsaudio` off. Chromium and Firefox never take this branch: their
   * Web Audio path works and is untouched.
   */
  private static _useMediaPlayout(): boolean {
    if (AudioPlayer.mediaPlayoutBroken) return false;
    if (location.hash.includes('nomsaudio')) return false;
    if (location.hash.includes('msaudio')) return AudioPlayer.isWebKit();
    // ★★★ NO USER-FACING SWITCH. It was the default for two hours on 2026-09-14, then a switch,
    //     and Stuart tested it on the LAN to rule out the tunnel: "hyper unreliable now even on
    //     local network". Only the #msaudio hash reaches it, for measurement. Web Audio with the
    //     clock watchdog is the path.
    return false;
  }

  private static _needsAnchor(): boolean {
    if (!location.hash.includes('anchor') || location.hash.includes('noanchor')) return false;
    // `#anchor` alone is the Chromium Now Playing experiment (UA-gated, see above).
    // `#anchor2` forces it on ANY browser — a DIAGNOSTIC for the Spatial Audio question:
    // it puts a real <audio src=blob:> media element in the page while our radio audio goes
    // out through ctx.destination. If macOS then offers Spatialise Stereo, the rule is
    // "Safari spatialises media elements, not Web Audio", and the only real route is a muxed
    // Opus/WebM stream into <audio src>. If it still doesn't, the browser cannot do it at all.
    // NOTE the anchor is what leaked 13 GB on Chromium — keep it to Safari and to testing.
    if (location.hash.includes('anchor2')) return true;
    return AudioPlayer._isChromium();
  }

  private ws: WebSocket | null = null;
  private ctx: AudioContext | null = null;
  private node: AudioWorkletNode | null = null;
  /** Current playout cushion in ms — starts at JITTER_SEC and the worklet adapts it to the link.
   *  Read it to explain how far the audio sits behind the waterfall. */
  jitterMs = Math.round(JITTER_SEC * 1000);
  /** How many times playout has run dry. See the worklet — the one number that tells a starvation
   *  stutter from a decode one, and it is shown in the status row for exactly that reason. */
  underruns = 0;
  /** Samples thrown away because they arrived faster than they could be played — a hitch with no
   *  underrun. See the worklet: this is the discard nothing was counting. */
  skips = 0;
  private gain: GainNode | null = null;

  // ScriptProcessor fallback — see start(). Only one of `node` / `sp` is live.
  private sp: ScriptProcessorNode | null = null;
  /**
   * Real <audio> element fed by a MediaStream from the Web Audio graph.
   *
   * The OS media widget (macOS Now Playing, Windows, Android lock screen, media
   * keys) only attaches to a MEDIA ELEMENT that is genuinely playing. A Web Audio
   * graph alone does NOT register — UberSDR hit this too and says so plainly:
   * "a pure AudioContext + navigator.mediaSession metadata is not sufficient".
   *
   * They solved it by streaming WebM/Opus over HTTP into an <audio src=...>.
   * We can't (no Opus/WebM muxer in the shim) — but we don't need to: routing our
   * OWN decoded audio into a MediaStreamAudioDestinationNode gives a real element
   * playing real audio, with no server change and no second copy of the stream.
   */
  private mediaEl: HTMLAudioElement | null = null;
  private streamDest: MediaStreamAudioDestinationNode | null = null;
  /**
   * ★ CHROMIUM ANCHOR. The MediaStream element above is enough for Safari, but Chromium does NOT
   * attach its Global Media Controls to an element fed by `srcObject` — MediaStream playback is
   * treated as call-like audio and deliberately kept out of the media widget. So on Chrome/Edge
   * (and therefore on Windows too) Now Playing stayed empty while the audio played perfectly.
   *
   * The fix the web has settled on: a second element playing a real `src` — a silent, looping
   * clip — purely to give Chromium something it is willing to attach the session to. It makes no
   * sound and carries no audio of ours; the actual radio still comes out of the Web Audio graph.
   */
  private anchorEl: HTMLAudioElement | null = null;
  private ring: [Float32Array, Float32Array] | null = null;
  // WebCodecs Opus decoder (VibeServer compressed audio). Lazily configured; reconfigured if the
  // channel count changes (WFM stereo <-> mono). Decoded PCM lands in _onOpusData -> _playPcm.
  private opusDec: AudioDecoder | null = null;
  private opusCh = 0;
  private opusTs = 0;
  private cap = 48000 * 2;
  private wPos = 0;
  private rPos = 0;
  private filled = 0;
  private playing = false;
  /** Fallback path only: suppresses buffer growth for the re-arm that a retune flush causes. */
  private armedByFlush = false;
  private url: string;
  /** The Worker that owns the socket and the decoder when this browser can support it — see
   *  WORKER_SRC. Null means the legacy main-thread path is running instead. */
  private worker: Worker | null = null;
  /** ★★ The Worker's socket state, mirrored. `streaming` used to read this.ws directly, which is
   *  null once the Worker owns the socket — health() would then have reported 'no-stream' on a
   *  perfectly good stream, which is the sort of thing that gets chased as a server fault. */
  private workerOpen = false;
  private cb: AudioCallbacks;
  private closedByUs = false;

  /** Tear down the Chromium anchor alongside the real output. */
  private _stopAnchor() {
    if (!this.anchorEl) return;
    try {
      const src = this.anchorEl.src;
      this.anchorEl.pause();
      this.anchorEl.src = '';
      if (src.startsWith('blob:')) URL.revokeObjectURL(src);
    } catch { /* already gone */ }
    this.anchorEl = null;
  }
  private _volume = 1;
  private _muted = false;
  /** Server-side squelch threshold, dB. <= -100 means OFF (matches the app's convention). */
  private _squelchDb = -100;
  set squelchDb(db: number) { this._squelchDb = db; }
  get squelchActive() { return this._squelchDb > -100; }

  constructor(url: string, cb: AudioCallbacks = {}) {
    this.url = url;
    this.cb = guardCallbacks('web-ui', cb);   // ★ a meter or recorder UI that throws stays its own problem
  }

  /** Must be called from a user gesture — browsers block audio otherwise. */
  async start() {
    if (AudioPlayer._useMediaPlayout()) {
      if (!this.omEl) await this._startMediaPlayout();
      if (this.omEl) {
        // The Worker still owns the socket; it forwards Opus undecoded (rawOpus). No worklet,
        // no context — the element is the whole output chain.
        if (typeof Worker !== 'undefined' && !location.hash.includes('legacyaudio')) this._startWorker();
        else this._openWs();
        return;
      }
      console.warn('[audio] media playout unavailable — using Web Audio');
    }
    if (!this.ctx) {
      this.ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'playback' });
      this.gain = this.ctx.createGain();
      this.gain.gain.value = this._muted ? 0 : this._volume;

      // ── How the OS media widget gets attached: ONE mechanism per engine ──────────────────
      //
      // ★ CHROMIUM: the silent anchor, and audio goes STRAIGHT to the destination.
      //   Routing playback through a MediaStream element on Chromium drags it into the WebRTC
      //   playout path, which applies ADAPTIVE RESAMPLING to manage latency — and that is audible
      //   as the pitch drifting up and down for the first few seconds while it converges (reported
      //   on Edge, and previously against the Android server on a work laptop; Safari never did
      //   it). Chromium will not attach Global Media Controls to a srcObject element anyway, so
      //   the MediaStream bought us nothing there and cost us the wobble.
      //
      // ★ SAFARI: the MediaStream element, because it is the only thing Safari attaches Now
      //   Playing to, and Safari's playout of it is clean.
      // #noanchor disables the Chromium media-session anchor — it is the ONLY Chromium-only code
      // we have, which makes it the first suspect for a Chromium-only leak.
      if (!AudioPlayer._useMediaStream() && !AudioPlayer._needsAnchor()) {
        // No element at all — audio goes straight to ctx.destination (_connectOutput).
        // On Chromium this is what lets the Media Session action handlers register.
        this.streamDest = null;
        this.mediaEl = null;
      } else if (AudioPlayer._needsAnchor()) {
        try {
          this.anchorEl = new Audio(silentLoopUrl());
          this.anchorEl.loop = true;
          // Not zero: Chromium can treat a muted element as inaudible and skip the widget
          // entirely. The clip is silent by CONTENT, so this is still completely inaudible.
          this.anchorEl.volume = 1;
          void this.anchorEl.play().catch(() => { /* resumed on the next gesture */ });
        } catch { this.anchorEl = null; }
      } else {
        try {
          this.streamDest = this.ctx.createMediaStreamDestination();
          this.mediaEl = new Audio();
          this.mediaEl.srcObject = this.streamDest.stream;
          this.mediaEl.autoplay = true;
          // The GainNode already carries volume/mute; keep the element wide open or
          // the two would fight.
          this.mediaEl.volume = 1;
          void this.mediaEl.play().catch(() => { /* resumed on the next gesture */ });
        } catch {
          this.streamDest = null;
          this.mediaEl = null;
        }
      }

      // AudioWorklet is [SecureContext]-only. A VibeServer is plain http:// on a
      // LAN IP, so `ctx.audioWorklet` is UNDEFINED there and there is no worklet
      // path at all — fall back to ScriptProcessor, which has no such gate.
      // (Everything works on localhost, so this only ever bites on the device.)
      if (this.ctx.audioWorklet) {
        const blob = new Blob([WORKLET_SRC], { type: 'application/javascript' });
        const url = URL.createObjectURL(blob);
        await this.ctx.audioWorklet.addModule(url);
        URL.revokeObjectURL(url);
        this.node = new AudioWorkletNode(this.ctx, 'vibe-sink', { outputChannelCount: [2] });
        // ★ The worklet reports its depth whenever it changes. Record it so "why is the audio
        //   behind the waterfall?" has an answer on this side of the port — an adaptive value
        //   nobody can read is indistinguishable from a bug.
        this._wireNode(this.node);
        this.node.connect(this.gain);
        this._connectOutput();
        this.lastDrainAt = performance.now();
        this._watchOutput();
      } else {
        this._startScriptProcessor();
      }
    }
    if (this.ctx.state === 'suspended') await this.ctx.resume();
    /* ★★★ WHO OWNS THE SOCKET. If we have a worklet AND WebCodecs, the Worker takes the whole
     *     audio path — receive, decode, convert — and feeds the node through a transferred port,
     *     so drawing cannot starve it. Otherwise nothing changes and the page keeps doing it.
     * ★★ THE TWO CONDITIONS ARE NOT ARBITRARY. Without a worklet there is no port to transfer
     *    (plain-http LAN origins, which fall back to ScriptProcessor); without WebCodecs the
     *    Worker has no decoder, and the WASM one lives on this side. Either way the old path is
     *    still there, whole, and is what runs.
     * ★ #legacyaudio forces the old path at run time, so a bad night needs a reload rather than a
     *   release. The same escape hatch as #webcodecs below it. */
    const canWorker = !!this.node
                   && typeof Worker !== 'undefined'
                   && typeof AudioDecoder !== 'undefined'
                   && !location.hash.includes('legacyaudio');
    if (canWorker) this._startWorker();
    else           this._openWs();
  }

  /** Same ring buffer as the worklet, drained on the main thread instead. */
  private _startScriptProcessor() {
    const ctx = this.ctx!;
    this.ring = [new Float32Array(this.cap), new Float32Array(this.cap)];
    const sp = ctx.createScriptProcessor(4096, 0, 2);
    sp.onaudioprocess = (e) => {
      const outL = e.outputBuffer.getChannelData(0);
      const outR = e.outputBuffer.getChannelData(1);
      const n = outL.length;

      // Re-arm ONLY on a true underrun (not enough samples for this block).
      // An earlier version also paused whenever the buffer dipped below a
      // fraction of the target — which fires constantly while the buffer is
      // still filling on a fresh connect, so playback stalled, rebuilt, stalled
      // again, and the audio chopped until it happened to outrun the threshold.
      // Never gate playback on how FULL the buffer is; only on whether the next
      // block can actually be served.
      if (!this.playing || this.filled < n) {
        if (this.playing) {
          this.playing = false;
          // ★ Grow on a real underrun, exactly as the worklet does — and skip the one that our
          //   own retune flush caused, for the same reason. Without this the fallback would adapt
          //   its arm threshold and never actually raise it, which is no adaptation at all.
          if (this.armedByFlush) this.armedByFlush = false;
          else this.jitterMs = Math.min(JITTER_MAX_SEC * 1000, this.jitterMs + JITTER_STEP_SEC * 1000);
        }
        outL.fill(0);
        outR.fill(0);
        return;
      }

      const [bl, br] = this.ring!;
      for (let i = 0; i < n; i++) {
        const r = (this.rPos + i) % this.cap;
        outL[i] = bl[r];
        outR[i] = br[r];
      }
      this.rPos = (this.rPos + n) % this.cap;
      this.filled -= n;
      this.playedTotal += n;          // the self-heal watchdog's "played" on this path
    };
    sp.connect(this.gain!);
    this._connectOutput();
    this.sp = sp;
  }

  /** Send the mixed output to the media element when we have one (so the OS sees
   *  playback), otherwise straight to the speakers. Never both — that would play
   *  the audio twice. */
  private _connectOutput() {
    if (!this.gain || !this.ctx) return;
    if (this.streamDest) this.gain.connect(this.streamDest);
    else this.gain.connect(this.ctx.destination);
  }

  /** The element the OS media controls attach to (null if unavailable). */
  get element(): HTMLAudioElement | null { return this.omEl ?? this.mediaEl; }

  /** ★ Drop everything queued for playout. See the worklet's flush handler for why. Called on
   *  every retune, through SpectrumClient.tune(). */
  flush() {
    // ★ A retune empties the playout buffer on purpose and re-arms it: not a stall to repair.
    this.heal.hold(performance.now(), 2000);
    // Media playout: what is buffered was demodulated at the old frequency — skip past it.
    if (this.omEl) { this._mediaSkipToLive(Math.min(0.1, this.omTarget)); }
    // The worklet path.
    if (this.node) { try { this.node.port.postMessage({ flush: true }); } catch { /* closing */ } }
    // The main-thread fallback path uses its own ring; clear that too, or the fallback keeps the
    // very bug this fixes.
    this.rPos = this.wPos; this.filled = 0; this.playing = false;
    this.armedByFlush = true;   // the fallback's re-arm after a retune is ours, not the link's
  }

  /** Push decoded frames into the fallback ring buffer. */
  private _pushRing(l: Float32Array, r: Float32Array) {
    const n = l.length;
    const [bl, br] = this.ring!;
    if (this.filled + n > this.cap) {          // overflow: drop oldest, never lag
      const drop = this.filled + n - this.cap;
      this.rPos = (this.rPos + drop) % this.cap;
      this.filled -= drop;
    }
    for (let i = 0; i < n; i++) {
      const w = (this.wPos + i) % this.cap;
      bl[w] = l[i];
      br[w] = r[i];
    }
    this.wPos = (this.wPos + n) % this.cap;
    this.filled += n;
    // Same cushion as the worklet — this is the fallback path, not a different policy. ★ Which
    // means it follows the ADAPTED depth, not the starting constant: this path is what runs on
    // exactly the setups that cannot use a worklet (a page served over plain HTTP to a LAN IP),
    // and leaving it pinned at 150 ms would give the fallback the old bug back.
    if (!this.playing && this.filled >= 48 * this.jitterMs) this.playing = true;
  }

  /** Hand the socket and the decoder to a Worker, wired straight into the worklet. */
  private _startWorker() {
    try {
      const blob = new Blob([WORKER_SRC], { type: 'application/javascript' });
      const wurl = URL.createObjectURL(blob);
      const w = new Worker(wurl);
      URL.revokeObjectURL(wurl);
      this.worker = w;

      /* ★★★ THE TRANSFER IS THE WHOLE POINT. port2 goes INTO the worklet, port1 to the Worker, and
       *     from then on PCM never touches this thread. Transferring detaches the port here, which
       *     is exactly what we want: there is no second owner to get confused about. */
      if (this.node) {
        const ch = new MessageChannel();
        this.node.port.postMessage({ sinkPort: ch.port2 }, [ch.port2]);
        w.postMessage({ type: 'init', url: this.url, sinkPort: ch.port1 }, [ch.port1]);
      } else {
        // Media playout: no worklet to feed — the packets come back here for the element.
        w.postMessage({ type: 'init', url: this.url, rawOpus: true });
      }
      if (this.rec) w.postMessage({ type: 'rec', on: true });
      /* ★★★ SAY SO, BECAUSE OTHERWISE NOBODY CAN TELL. Every failure path here logs loudly, but
       *     SUCCESS said nothing — so a console with no warnings was indistinguishable from a
       *     console where the Worker had never been attempted, and the one line people DID see
       *     ("requesting Opus…") comes from main.ts before any of this and appears either way.
       *     An owner testing on a browser we cannot run had no way to answer "is it even on?" —
       *     which is the first question any test of this needs (2026-08-26). */
      console.info('[audio] decoding in a Worker — off the main thread');

      w.onmessage = (e: MessageEvent) => {
        const d = e.data as { type?: string; s?: string; msg?: string; n?: number;
                              pcm?: Int16Array; ch?: number; why?: string; format?: number;
                              buf?: ArrayBuffer };
        switch (d?.type) {
          case 'status':
            this.workerOpen = d.s === 'open';
            this.cb.onStatus?.(d.s as any, d.msg);
            break;
          case 'bytes':  this.cb.onBytes?.(d.n || 0); this._noteRx((d as { f?: number }).f ?? 3); break;
          case 'needsCodec':
            console.error('[audio] server requires Opus and refused this socket');
            this.needsCodec = true;
            break;
          // ★ Recording taps PCM on THIS side, so the Worker forwards it — but only while a
          //   recording is running. See emit() in WORKER_SRC.
          case 'pcm':    if (d.pcm && this.rec) this._recordPcm(d.pcm, d.ch || 1); break;
          /* ★ A frame the worker cannot decode but the page can — DAB+ AAC. See the worker's own
           *   note: forwarding it keeps the socket and every other format off the main thread. */
          case 'passthru': if (d.buf) { const b = d.buf as ArrayBuffer; guard('web-audio', 'frame', () => this._handleFrame(b)); } break;
          // ★ A frame the Worker dropped — counted and logged here, where faultLog lives.
          case 'fault':    noteFault('web-audio-worker', String((d as { kind?: string }).kind ?? 'frame'), new Error(d.why || 'bad frame')); break;
          case 'opus':     if (d.buf) this._mediaFeed(d.buf as ArrayBuffer); break;
          /* ★★★ A DECODER THAT WILL NOT WORK IN THE WORKER MUST NOT MEAN SILENCE. Fall the whole
           *     path back to the page, which still has the WASM decoder and every fallback this
           *     class has always had. Better a busy main thread than no audio. */
          case 'decoderFailed':
          case 'unhandled':
            console.warn('[audio] worker path failed (' + (d.why || ('format ' + d.format)) +
                         ') — falling back to the main thread');
            this._stopWorker();
            this._openWs();
            break;
        }
      };
      w.onerror = () => {
        console.warn('[audio] audio worker error — falling back to the main thread');
        this._stopWorker();
        this._openWs();
      };
    } catch (e) {
      console.warn('[audio] could not start the audio worker — using the main thread', e);
      this._stopWorker();
      this._openWs();
    }
  }

  private _stopWorker() {
    if (!this.worker) return;
    try { this.worker.postMessage({ type: 'close' }); } catch { /* going anyway */ }
    try { this.worker.terminate(); } catch { /* already gone */ }
    this.worker = null;
    this.workerOpen = false;
  }

  private _openWs() {
    this.closedByUs = false;
    const ws = new WebSocket(this.url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;

    ws.onopen = () => this.cb.onStatus?.('open');
    ws.onerror = () => this.cb.onStatus?.('error', 'audio websocket error');
    ws.onclose = () => {
      this.cb.onStatus?.('closed');
      if (!this.closedByUs) setTimeout(() => this._openWs(), 3000);
    };
    ws.onmessage = (e) => {
      // ★★★ THE SERVER EXPLAINS ITSELF AND WE USED TO THROW IT AWAY. `local_sdr_shim.cpp:3125`
      //     refuses a socket opened without `codec=opus` when the owner forbids uncompressed —
      //     it sends {"type":"needs_codec"} and closes. Nothing anywhere handled that message,
      //     so the only evidence was `audio 0 KB/s` and a reconnect loop every 3 s, for ever.
      //     With the WASM decoder we should never be refused again; if we are, SAY SO.
      if (typeof e.data === 'string') {
        guardJson('web-audio', e.data, (m) => {
          if (m.type === 'needs_codec') {
            console.error('[audio] server requires Opus and refused this socket');
            this.needsCodec = true;
          }
        });
        return;
      }
      if (!(e.data instanceof ArrayBuffer)) return;
      if (this.fault === 'freeze') return;                  // self-heal test: a half-open socket
      this.needsCodec = false;
      this.cb.onBytes?.(e.data.byteLength);
      this._noteRx(e.data.byteLength >= 2 ? new Uint8Array(e.data, 1, 1)[0] : 3);
      if (this.fault === 'drop') return;                    // self-heal test: frames discarded here
      // ★ One bad frame costs one frame — dropped, counted, logged (faultLog).
      const buf = e.data;
      guard('web-audio', 'frame', () => this._handleFrame(buf), `len=${buf.byteLength}`);
    };
  }

  private _handleFrame(buf: ArrayBuffer) {
    if (buf.byteLength < 6) return;
    const dv = new DataView(buf);
    const channels = dv.getUint8(0);
    const format = dv.getUint8(1);

    // format 3 = Opus (VibeServer compressed audio). Decoded ASYNCHRONOUSLY via WebCodecs — the
    // decoded PCM lands in _onOpusData → _playPcm, same tail as the sync paths below.
    if (format === 3) {
      if (this.omEl) { this._mediaFeed(buf); return; }
      this._decodeOpus(buf, channels); return;
    }

    /* ★★★ format 4 = DAB+ AAC, in ADTS. VibeServer links no AAC decoder — it de-interleaves the
     *  super frame, corrects it with Reed-Solomon and reframes the access units, and the BROWSER's
     *  own decoder does the rest, which it is already licensed for. Most of the UK's stations are
     *  DAB+, and every one of them was silent before this. */
    if (format === 4) {
      /* ★ Low nibble = the CORE channel count, bit 7 = parametric stereo. See the server's note
       *  on aacParametricStereo(): a mono core with PS must be declared as HE-AAC v2 or the
       *  decoder plays the core alone — mono, and half the bandwidth. */
      const ps = (channels & 0x80) !== 0;
      if (ps !== this.aacPs) { this.aacTriedMse = false; this.aacMonoFrames = 0; }
      this.aacPs = ps;
      this._decodeAac(buf, channels & 0x0f, new DataView(buf).getUint32(2, true));
      return;
    }

    let pcm: Int16Array;
    let ch: number;
    if (format === 0) {
      ch = channels;
      pcm = new Int16Array(buf, 6, (buf.byteLength - 6) >> 1);
    } else {
      // formats 1/2 = legacy IMA-ADPCM (retired server-side; kept for any old stream).
      const d = decodeVibeAdpcmFrame(buf);
      ch = d.channels;
      pcm = d.pcm;
    }
    this._playPcm(pcm, ch);
  }

  /** ★★★ CAN THIS BROWSER PLAY OPUS AT ALL? Now always YES — we carry a WASM decoder, so the
   *  answer no longer depends on the browser, the origin's secure-context status, or the OS media
   *  stack. This matters because a "no" here is NOT a graceful degrade: on a server with
   *  uncompressed off, asking for PCM gets the socket REFUSED, which is silence. The old answer
   *  was a WebCodecs probe, and WebCodecs is unavailable on exactly the http:// LAN origin every
   *  real listener uses. Kept as a method (and still awaited) so the call sites read the same. */
  static async supportsOpus(): Promise<boolean> { return true; }

  /** Does this browser have the CHEAP path — hardware/platform Opus via WebCodecs? Used to pick a
   *  decoder, never to decide whether to ask for Opus. A `false` here now costs CPU, not audio. */
  static async supportsWebCodecsOpus(): Promise<boolean> {
    try {
      if (typeof AudioDecoder === 'undefined') return false;
      // ★★ PROBE BOTH CHANNEL COUNTS — we used to test STEREO and then configure with the stream's
      // ACTUAL count, which is MONO by default. So the capability we tested was not the capability
      // we used, and a platform decoder that supports one and not the other passed the probe and
      // then failed for real. Edge on Windows 11 routes through a different media stack from
      // Chrome on macOS and could not play Opus at all (Stuart, 2026-07-31); this is one of the
      // candidate causes.
      for (const numberOfChannels of [1, 2]) {
        const s = await AudioDecoder.isConfigSupported({ codec: 'opus', sampleRate: 48000, numberOfChannels });
        if (!s.supported) return false;
      }
      return true;
    } catch { return false; }
  }

  /** ★★★ Set once a decode or configure has actually FAILED, so the caller can fall back to PCM.
   *  `isConfigSupported` is a PREDICTION, not a guarantee — see _ensureOpus. */
  opusBroken = false;
  /** Called on the first real failure so the page can re-open the audio socket without `codec=opus`. */
  onOpusFailure: (() => void) | null = null;
  private opusFailed = false;
  private opusFails = 0;
  /** ★ Set by the page from the server's advertised policy. When the owner does NOT allow
   *  uncompressed audio there is nothing to fall back TO — the server refuses a socket opened
   *  without `codec=opus` — so giving up on Opus produces silence rather than a fallback. */
  allowUncompressed = false;
  /** Opus is failing repeatedly and this server forbids the uncompressed fallback. */
  private opusStuck = false;
  /** The server told us outright that it refuses this socket without Opus (`needs_codec`). */
  private needsCodec = false;
  /** ★★ HOW MANY CONSECUTIVE FAILURES BEFORE WE GIVE UP ON OPUS. One is not enough: every
   *  Opus packet is independently decodable, so a single bad frame genuinely does self-heal,
   *  and tearing the decoder down for it turns a glitch into a permanent outage. */
  private static readonly OPUS_FAIL_LIMIT = 4;

  private _failOpus(what: string, e: unknown) {
    this.opusFails++;
    // ★★★ REBUILD FIRST, GIVE UP LAST. An Opus decoder that has errored is usually unhappy for
    //     the next packet too, so the recovery that actually works is a NEW decoder — the same
    //     fix the iOS and Android clients needed today, where a decode failure was silent and
    //     permanent. Dropping the decoder here makes _ensureOpus build a fresh one.
    try { this.opusDec?.close(); } catch {}
    this.opusDec = null;
    if (this.opusFails < AudioPlayer.OPUS_FAIL_LIMIT) {
      console.warn(`[audio] opus ${what} failed (${this.opusFails}) — rebuilding the decoder`, e);
      return;
    }
    // ★★★ THE REAL FALLBACK IS ANOTHER DECODER, NOT ANOTHER STREAM. Whatever WebCodecs cannot
    //     do here (Edge's media stack was the suspect for a week), libopus-in-WASM can — it is
    //     the same code the native apps run, with no platform in the way. Switch and stay
    //     switched; only if WASM ALSO fails is there anything to escalate.
    if (!this.useWasm) {
      console.warn(`[audio] opus ${what} failed ${this.opusFails}x on WebCodecs — switching to the WASM decoder`, e);
      this.useWasm = true;
      this.opusFails = 0;
      return;
    }
    // ★★★ AND IF THERE IS NOWHERE TO FALL BACK TO, DO NOT FALL BACK. With the owner's
    //     uncompressed policy OFF, re-opening without `codec=opus` is REFUSED by the server —
    //     so the "fallback" replaced a struggling stream with no stream at all, silently. Keep
    //     rebuilding instead, and say so out loud: a listener who can hear nothing deserves to
    //     know why, and the owner is the only one who can change the policy.
    if (!this.allowUncompressed) {
      if (this.opusFails === AudioPlayer.OPUS_FAIL_LIMIT || this.opusFails % 50 === 0) {
        console.error(`[audio] opus ${what} keeps failing and this server does not allow `
          + `uncompressed audio — still retrying`, e);
      }
      this.opusStuck = true;   // surfaced through health, on the meter where faults live
      return;
    }
    console.warn(`[audio] opus ${what} failed ${this.opusFails}x — falling back to uncompressed`, e);
    if (this.opusFailed) return;      // one shot: the socket is being replaced
    this.opusFailed = true;
    this.opusBroken = true;
    this.onOpusFailure?.();
  }

  private _ensureOpus(ch: number) {
    if (this.opusDec && this.opusCh === ch) return;
    if (this.opusDec) { try { this.opusDec.close(); } catch {} }
    this.opusDec = new AudioDecoder({
      output: (d) => this._onOpusData(d),
      // ★★★ A DECODE ERROR USED TO ONLY BE LOGGED, on the reasoning that "the stream self-heals on
      // the next key packet (every Opus packet is independently decodable)". That is true for ONE
      // bad packet and FALSE when every packet fails: the audio then stays silent for ever, the only
      // evidence is a console warning nobody sees, and the user has to discover the uncompressed
      // setting for themselves — which is exactly what happened on Edge/Windows 11.
      // ★★ So a persistent failure now falls back to PCM, which we KNOW works on that machine.
      error: (e) => this._failOpus('decode', e),
    });
    try {
      this.opusDec.configure({ codec: 'opus', sampleRate: 48000, numberOfChannels: ch });
    } catch (e) { this._failOpus('configure', e); return; }
    this.opusCh = ch;
    this.opusTs = 0;
  }

  // ── WASM Opus (the always-available path) ──────────────────────────────────
  private wasmDec: OpusDecoder | null = null;
  private wasmReady = false;
  private wasmCh = 0;
  /** Set once we have decided WebCodecs is not usable here — either absent (insecure origin) or
   *  it failed for real. From then on every packet goes to WASM and we stop probing. */
  // ★ `#wasmopus` forces it even where WebCodecs works. Without an override the WASM path is
  //   invisible from the dev loop — localhost IS a secure context, so the Mac would always take
  //   WebCodecs and the decoder every real listener uses would never be exercised where it is
  //   developed. That is precisely how this bug survived a week. `#webcodecs` forces the other way.
  private useWasm = location.hash.includes('wasmopus')
    || (!location.hash.includes('webcodecs') && typeof AudioDecoder === 'undefined');

  /** Build (or rebuild) the WASM decoder for `ch` channels. Decoding is synchronous once ready;
   *  the ~20 ms of packets that arrive during startup are dropped, which is inaudible. */
  /** ★★ A decoder that will not BUILD must be built once, not once per packet. The first attempt
   *  threw for every 20 ms frame — 50 identical stack traces a second, which buries the one line
   *  that says why, and makes a startup failure look like a decode failure. Construction is
   *  attempted once; per-packet decode errors are a separate thing and still counted in _failOpus. */
  private wasmDead = false;

  private _ensureWasm(ch: number) {
    if (this.wasmDead) return;
    if (this.wasmDec && this.wasmCh === ch) return;
    try { this.wasmDec?.free(); } catch {}
    this.wasmReady = false;
    this.wasmCh = ch;
    let dec: OpusDecoder;
    try {
      // ★ Construction itself can throw — the module's WASM payload is CRC-checked, and a page
      //   that mangled it fails HERE, not at decode time. That distinction was invisible while
      //   this ran unguarded once per packet.
      dec = new OpusDecoder({ channels: ch });
    } catch (e) {
      console.error('[audio] the WASM Opus decoder would not build — audio cannot play', e);
      this.wasmDead = true;
      this.wasmDec = null;
      this.opusStuck = true;
      return;
    }
    this.wasmDec = dec;
    dec.ready.then(() => {
      if (this.wasmDec === dec) this.wasmReady = true;
    }).catch((e) => {
      // Nothing left to try: WebCodecs is gone or broken and WASM will not start. Say so on the
      // meter rather than going quiet — see the `opus-stuck` note in _failOpus.
      console.error('[audio] the WASM Opus decoder failed to start', e);
      if (this.wasmDec === dec) { this.wasmDec = null; this.wasmDead = true; this.opusStuck = true; }
    });
  }

  private _decodeWasm(packet: Uint8Array, ch: number) {
    this._ensureWasm(ch);
    if (!this.wasmDec || !this.wasmReady) return;   // still starting up — drop, do not queue
    let out;
    try {
      out = this.wasmDec.decodeFrame(packet);
    } catch (e) {
      // Per-packet failure. Every Opus packet is independently decodable, so one bad frame
      // self-heals; only a run of them means anything, and _failOpus counts them.
      this._failOpus('wasm decode', e);
      return;
    }
    const chans = out.channelData;
    const n = out.samplesDecoded;
    if (!chans.length || n <= 0) return;
    this.opusFails = 0;
    this.opusStuck = false;
    const nc = chans.length;
    const pcm = new Int16Array(n * nc);
    for (let c = 0; c < nc; c++) {
      const src = chans[c];
      for (let i = 0; i < n; i++) {
        let s = Math.round(src[i] * 32767);
        s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
        pcm[i * nc + c] = s;
      }
    }
    this._playPcm(pcm, nc);
  }

  private _decodeOpus(buf: ArrayBuffer, channels: number) {
    if (this.opusBroken) return;      // fallback in progress; drop rather than log-spam
    const ch = channels || 1;
    if (this.useWasm) { this._decodeWasm(new Uint8Array(buf, 6), ch); return; }
    this._ensureOpus(ch);
    if (!this.opusDec) return;        // configure failed → _failOpus already fired
    // Copy the packet out of the frame (offset 6). Each Opus packet is a self-contained 20 ms frame.
    const data = buf.slice(6);
    try {
      this.opusDec!.decode(new EncodedAudioChunk({
        type: 'key', timestamp: this.opusTs, duration: 20000, data,
      }));
    } catch (e) { this._failOpus('enqueue', e); return; }
    this.opusTs += 20000;   // 20 ms in microseconds
  }

  /** ★★★ DAB+ ACCESS UNITS, HANDED TO THE PLATFORM. WebCodecs takes ADTS directly when no
   *  `description` is supplied, so the AAC config travels in the bitstream where DAB put it.
   *  ★ The rate we CONFIGURE with is the AAC CORE rate the ADTS header carries; under SBR the
   *    decoder doubles it and reports the real thing on the AudioData, which is what _playPcm is
   *    given. Configuring with the output rate is the 2x chipmunk — DAB has already caught us
   *    with one of those, in the MP2 path, so it is spelled out here.
   *  ★ 1024 samples per AU at the core rate is the AAC frame length, hence the timestamp step. */
  private aacDec: AudioDecoder | null = null;
  private aacRate = 0;
  private aacCh = 0;
  private aacTs = 0;
  private aacBroken = false;
  /** 7 when the decoder wanted raw AAC with a description, so the ADTS header is removed. */
  private aacStrip = 0;

  /** AudioSpecificConfig for AAC-LC: 5 bits object type, 4 bits rate index, 4 bits channels. */
  private _ascFor(rateHz: number, ch: number, len960 = true): Uint8Array | null {
    const table = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350];
    const idx = table.indexOf(rateHz);
    if (idx < 0 || ch < 1 || ch > 2) return null;
    // ★ frameLengthFlag = 1 (bit 2): DAB+ frames are 960 samples — see _ascExplicitSbr.
    return new Uint8Array([ (2 << 3) | (idx >> 1),
                            ((idx & 1) << 7) | (ch << 3) | (len960 ? (1 << 2) : 0) ]);
  }

  /** ★★★ DAB+ HAS EXACTLY FOUR FORMATS, so SBR is derivable from the core rate alone (TS 102 563
   *  table 2): 16k core -> 32k out and 24k core -> 48k out both carry SBR; 32k and 48k do not.
   *  The server sends only the CORE rate, and it is right to — but the fMP4 timeline has to know
   *  what actually comes OUT. Returns 0 when there is no SBR. */
  /** Samples the DECODER will hand back per access unit: its frame length, doubled under SBR. */
  private _msePerAu(coreRateHz: number): number {
    return (this.aacUse960 ? 960 : 1024) * (this._sbrOutRate(coreRateHz) ? 2 : 1);
  }

  /** ★★★ THE TIMESCALE ABSORBS A DECODER THAT CANNOT DO 960 — the same arithmetic that already
   *  rescues the WebCodecs path, applied to the mp4 timeline.
   *  A DAB+ access unit is 60 ms. Apple cannot decode 960-sample frames, so it hands back 1024
   *  core samples — 2048 under SBR — where 60 ms at 32 kHz is only 1920. Its output is genuinely
   *  6.67% too many samples, so NO choice of duration is right at the nominal rate: declare 1920
   *  and the samples do not fit; declare 2048 and the unit lasts 64 ms. Both play slow, which is
   *  exactly what Stuart heard twice — "barry white ... but it is stereo and it is sounding
   *  better", then "still slow".
   *  ★ So state the rate at which those samples ARE 60 ms: 2048 / 0.060 = 34133 Hz. The duration
   *    then matches the decoder's output AND the timeline matches real time, and the samples play
   *    slightly faster, which is what restores the pitch. An unusual track rate is legal in mp4
   *    and is the honest description of what this decoder produces.
   *  ★ With a decoder that DOES do 960 the numbers collapse back to the nominal rate on their own
   *    — 1920 / 0.060 = 32000 — so there is no special case to maintain. */
  private _mseTimescale(coreRateHz: number): number {
    return Math.round(this._msePerAu(coreRateHz) / 0.060);
  }

  private _sbrOutRate(coreRateHz: number): number {
    if (coreRateHz === 16000) return 32000;
    if (coreRateHz === 24000) return 48000;
    return 0;
  }

  /** ★★★ AN EXPLICIT (HIERARCHICAL) AudioSpecificConfig FOR HE-AAC — AOT 5, carrying the
   *  extension sampling frequency, then AOT 2 for the core.
   *  ★★★ WHY: the implicit config (_ascFor: AAC-LC at the core rate) leaves the decoder to detect
   *      SBR for itself, and then the mp4 timeline and the decoder disagree about how long an
   *      access unit is. Declaring 1024 samples at a 24 kHz timescale while the decoder returns
   *      2048 at 48 kHz makes the browser STRETCH it — Stuart, 2026-09-05: "DAB+ sample rate
   *      seems to be too slow now... justin timberlake is sounding closer to barry white." An
   *      octave down is the unmistakable signature of exactly 2x.
   *   ★★ Explicit signalling plus an OUTPUT-rate timeline (2048 per AU) is what mp4 muxers write
   *      for HE-AAC, and it takes the guess away from both sides. This is also the most likely
   *      reason Safari's MediaSource path never produced audio: it was handed a config it had to
   *      infer from, on a timeline that then contradicted the inference.
   *   ★ 25 bits: AOT(5)=5, freqIdx(4), channels(4), extFreqIdx(4), AOT(5)=2, GASpecificConfig(3). */
  private _ascExplicitSbr(coreRateHz: number, outRateHz: number, ch: number,
                          len960 = true): Uint8Array | null {
    const table = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350];
    const ci = table.indexOf(coreRateHz), oi = table.indexOf(outRateHz);
    if (ci < 0 || oi < 0 || ch < 1 || ch > 2) return null;
    let acc = 0, nbits = 0;
    const out: number[] = [];
    const put = (v: number, n: number) => {
      acc = (acc << n) | (v & ((1 << n) - 1)); nbits += n;
      while (nbits >= 8) { nbits -= 8; out.push((acc >> nbits) & 0xff); }
    };
    /* ★★★ frameLengthFlag = 1 — DAB+ USES 960-SAMPLE AAC FRAMES, NOT 1024. The superframe is
     *  120 ms and carries 2/3/4/6 access units at 16/24/32/48 kHz core; only 960 divides that
     *  (960/16000 x 2 = 120 ms, and so on for every row). At 1024 each row would be 128 ms.
     *  We signalled 1024 to every decoder, so each service ran at 1024/960 = 1.0667x the right
     *  speed, and the error rides on top of whatever the SBR handling did — which is exactly
     *  Stuart's "some stations are slow and others are fast" rather than one uniform wrongness.
     *  ★ GASpecificConfig: frameLengthFlag(1)=1, dependsOnCoreCoder(1)=0, extensionFlag(1)=0. */
    /* ★★★ AOT 29 = HE-AAC v2 (SBR + PARAMETRIC STEREO); AOT 5 = HE-AAC v1 (SBR only). The rest
     *  of the explicit hierarchical config is identical: core rate, CORE channel count, the
     *  extension rate, then AOT 2 for the core itself. Declaring 5 for a PS stream is what left
     *  Safari playing a mono 16 kHz core — audible as telephone-grade "hold music" — while the
     *  same service in Edge came out full-bandwidth and in stereo. */
    put(this.aacPs ? 29 : 5, 5);
    put(ci, 4); put(ch, 4); put(oi, 4); put(2, 5); put(len960 ? 4 : 0, 3);
    if (nbits) out.push((acc << (8 - nbits)) & 0xff);
    return new Uint8Array(out);
  }

  /** ★★★ THROW THE WHOLE MediaSource AWAY. Its configuration is fixed at creation — the ASC, the
   *  timescale and the samples-per-AU all describe ONE service — so it cannot be reused for a
   *  service with a different rate or channel count. Called when the format changes. */
  private _mseTeardown() {
    try { this.mseEl?.pause(); } catch { /* already gone */ }
    try { this.mseEl?.remove(); } catch { /* not in the document */ }
    this.mseEl = null; this.mseSrc = null; this.mseBuf = null;
    this.mseQueue = []; this.mseSeq = 1; this.mseTime = 0;
    this.mseStreaming = false; this.mseSawStreaming = false;
    this.mseCore = 0; this.mseCh = 0;
    this.aacProbing = false;      // ★ so the next service may build a fresh one
  }

  private _decodeAac(buf: ArrayBuffer, channels: number, coreRateHz: number) {
    if (this.mseBuf) {                       // Safari path — see _startMse
      /* ★★★ ONE SOURCE PER FORMAT. A MediaSource is configured ONCE, at creation: the
       *  AudioSpecificConfig, the timescale and the samples per access unit all describe one
       *  service. This fed every later service into whatever source the FIRST one built, so
       *  changing station — or going to an MP2 service and back — appended 24 kHz-core frames to
       *  a buffer told to expect 16 kHz, and Safari played nothing at all.
       *  ★★★ STUART DIAGNOSED THIS FROM THE SYMPTOM: "so aac in safari did something then didnt
       *      ... i chose an MP2 station on the same ensemble, the MP2 played, so i went back to
       *      the DAB+ then it refused to play", then "the sample rate mismatch making it not play
       *      at all in safari?" — yes, exactly that.
       *  ★ Rebuilt rather than patched: there is no way to re-point a source buffer's codec
       *    configuration, and pretending otherwise is what produced the flashes of sound. */
      if (this.mseCore !== coreRateHz || this.mseCh !== channels) {
        console.info(`[audio] DAB+ format changed (${this.mseCore}/${this.mseCh} -> `
                   + `${coreRateHz}/${channels}) — rebuilding the MediaSource`);
        this._mseTeardown();
      } else {
        // ★ 2048 under SBR: the AU is 1024 CORE samples, which is 2048 at the doubled output rate
        //   the timeline is written in. See _ascExplicitSbr.
        this.mseDur = this._msePerAu(coreRateHz);
        this._mseFeed(new Uint8Array(buf, 6 + 7));  // strip the ADTS header; the ASC has the config
        return;
      }
    }
    if (this.aacBroken || !coreRateHz) return;
    /* ★★★ NO WebCodecs AT ALL — GO STRAIGHT TO MediaSource. This used to call _failAac and
     *  return, and _failAac sets aacBroken, so the fMP4 path built in 4.2.2 was never once
     *  ATTEMPTED on a browser without AudioDecoder: the fallback was only reachable from inside
     *  the WebCodecs probe, which cannot run when there is nothing to probe. Safari reported "no
     *  AAC support" three builds running while the code meant to rescue it sat unreached.
     *  ★ The same mistake as the DAB gate that only worked on a locked radio: a fallback placed
     *    inside the thing it is a fallback FOR. */
    if (typeof AudioDecoder === 'undefined') {
      if (!this.aacProbing) {
        this.aacProbing = true;
        void this._startMse(coreRateHz, channels).then((ok) => {
          this.aacProbing = false;
          if (!ok) this._failAac('support', 'no WebCodecs AAC and no MediaSource path');
        });
      }
      return;
    }
    if (!this.aacDec || this.aacRate !== coreRateHz || this.aacCh !== channels) {
      /* ★★★ PROBE, DO NOT try/catch A configure(). This is the bug Stuart's snippet exposed:
       *  configure() does NOT throw for a codec the browser cannot decode — it fails
       *  ASYNCHRONOUSLY through the error callback. So the old loop "succeeded" on its first
       *  candidate every time and NEVER tried the others; the fallbacks I added for Safari could
       *  not have run. isConfigSupported() is the question actually being asked.
       *  ★ Candidates include the ZERO-PADDED forms. 'mp4a.40.2' and 'mp4a.40.02' are the same
       *    codec and browsers differ over which spelling they accept — Stuart found the padded
       *    one in Apple's own example. */
      /* ★★★ CLOSE THE OLD DECODER, AND FLUSH WHAT IT ALREADY PRODUCED. This branch used to drop
       *  the reference and nothing else, and BOTH halves of that are wrong:
       *
       *  ★★★ THE FAULT IT FIXES. Stuart, 2026-09-08, changing service inside one ensemble: "if I
       *      move from a 48KHz station to a 32KHz one there is a bit of stutter and broken audio
       *      but mostly silence until I select the station again." A dropped AudioDecoder is not
       *      a closed one — it keeps its queue and its output callback still points here. So its
       *      LATE frames, decoded at the OLD rate, arrive after `aacRate` has already been moved
       *      to the NEW one, and _onAacData resamples them against a rate they were never
       *      sampled at. Every one of those frames is wrong, and they arrive interleaved with the
       *      new decoder's correct ones: stutter, then broken audio, then a playout buffer full
       *      of the wrong service. Selecting the station again "fixes" it only because by then
       *      the orphan has drained and the rate no longer changes, so this branch is a no-op.
       *  ★★★ IT IS ONLY THE WebCodecs HALF THAT WAS WRONG. The MediaSource path four lines up
       *      does the whole job — _mseTeardown() rebuilds the element, the source, the queue and
       *      the timeline — because Safari made the fault audible immediately (a format change
       *      played NOTHING). WebCodecs degraded quietly instead, so the same rule got enforced
       *      properly in one reader and half-heartedly in the other.
       *  ★ aacOkFrames / aacMonoFrames / aacTriedMse are judgements about a DECODER DECODING THIS
       *    SERVICE — "the browser managed a frame", "it promised stereo and gave mono". Carried
       *    across a reconfiguration they answer a question nobody asked: _recoverAac's 960-frame
       *    fallback tests `aacOkFrames === 0` and would never fire again after the first service
       *    of the session had decoded one frame. A new configuration is a new claim to test. */
      try { this.aacDec?.close(); } catch { /* already closing */ }
      this.aacRate = coreRateHz; this.aacCh = channels; this.aacTs = 0; this.aacStrip = 0;
      this.aacDec = null;
      this.aacOkFrames = 0; this.aacMonoFrames = 0; this.aacTriedMse = false;
      this.flush();                 // the old service's PCM is still queued ahead of the new one
      if (!this.aacProbing) { this.aacProbing = true; void this._openAac(coreRateHz, channels); }
      return;                       // AUs during the probe are dropped; it resolves in a moment
    }
    try {
      this.aacDec.decode(new EncodedAudioChunk({
        type: 'key', timestamp: this.aacTs, data: buf.slice(6 + this.aacStrip),
      }));
    } catch (e) { this._failAac('enqueue', e); return; }
    this.aacTs += Math.round(960 * 1e6 / coreRateHz);   // ★ 960-sample DAB+ frames
  }

  private aacProbing = false;

  /** Find a config this browser will actually take, then open the decoder with it. */
  private async _openAac(coreRateHz: number, channels: number) {
    const asc = this._ascFor(coreRateHz, channels, this.aacUse960);
    const tries: { codec: string; description?: Uint8Array; strip: number }[] = [];
    /* ★★★ THE EXPLICIT CONFIG FIRST — ADTS CANNOT SAY 960. DAB+ uses 960-sample AAC frames, and
     *  an ADTS header has no field for the frame length, so a decoder handed ADTS assumes 1024 and
     *  runs 1024/960 = 1.0667x fast. Stuart, on Edge: "Lou Reed is not quite a chipmunk but he is
     *  fast" — that ratio exactly. Only the AudioSpecificConfig carries frameLengthFlag, so the
     *  raw-AAC-plus-description candidates must be tried BEFORE the ADTS ones.
     *  ★ ADTS is kept as the last resort: slightly fast beats silent on a decoder that will not
     *    take a description. */
    /* ★ mp4a.40.29 is HE-AAC v2 and must be offered FIRST when the service uses parametric
     *  stereo, or a decoder takes the v1 string at its word and never reconstructs the second
     *  channel. */
    const codecs = this.aacPs ? ['mp4a.40.29', 'mp4a.40.5', 'mp4a.40.05', 'mp4a.40.2', 'mp4a.40.02']
                              : ['mp4a.40.5', 'mp4a.40.05', 'mp4a.40.2', 'mp4a.40.02'];
    for (const codec of codecs) {
      if (asc) tries.push({ codec, description: asc, strip: 7 });  // raw AAC + explicit config
    }
    for (const codec of codecs) {
      tries.push({ codec, strip: 0 });                             // ADTS, config inferred
    }
    for (const t of tries) {
      const cfg: AudioDecoderConfig = {
        codec: t.codec, sampleRate: coreRateHz, numberOfChannels: channels,
        ...(t.description ? { description: t.description } : {}),
      };
      let ok = false;
      try { ok = (await AudioDecoder.isConfigSupported(cfg)).supported === true; }
      catch { ok = false; }
      if (!ok) continue;
      try {
        const dec = new AudioDecoder({
          output: (ad) => this._onAacData(ad),
          error:  (e)  => this._recoverAac(e),
        });
        dec.configure(cfg);
        this.aacDec = dec;
        this.aacStrip = t.strip;
        this.aacProbing = false;
        console.info(`[audio] DAB+ decoding as ${t.codec}`
                     + `${t.description ? ' (raw + ASC)' : ' (ADTS)'} @ ${coreRateHz} Hz, ${channels}ch`);
        return;
      } catch (e) { console.warn(`[audio] AAC configure ${t.codec} failed:`, e); }
    }
    this.aacProbing = false;
    /* ★★★ NO WebCodecs AAC — TRY MediaSource. Safari has AAC throughout its media stack and
     *  takes audio/mp4 in MSE; it simply has not implemented AAC in WebCodecs. The access units
     *  are the same bytes either way, so they are wrapped as fragmented MP4 and handed to the
     *  platform decoder through a <audio> element. We still decode nothing ourselves. */
    if (await this._startMse(coreRateHz, channels)) return;
    this._failAac('support', 'no AAC configuration this browser accepts');
  }

  // ── Media playout (Safari): Opus in WebM through a MediaSource ─────────────────────────────
  //   See _useMediaPlayout for why. One element and one source for the life of the player; a
  //   channel-count change (FM stereo ↔ a mono mode) appends a fresh initialisation segment to
  //   the same buffer, which MSE permits for the same codec.
  private omEl: HTMLAudioElement | null = null;
  private omSrc: MediaSource | null = null;
  private omBuf: SourceBuffer | null = null;
  private omQueue: Uint8Array[] = [];
  private omCh = 0;                 // channels the current init segment declared
  private omSeq = 0;                // packets appended (20 ms each) — the cluster clock
  private omManaged = false;
  private omStreaming = false;
  private omSawStreaming = false;
  private omTimer: number | null = null;
  private omFedAt = 0;              // last packet queued
  private omProgressAt = 0;         // last time the element's clock moved while playing
  private omLastTime = -1;
  private omStartedAt = 0;
  /** buffered-end advance ÷ packet seconds over the last window — 1.000 means the timeline is
   *  honest. Watched because a wrong guess here is what made the first build unplayable. */
  private omRatio = 0;
  private omRatioEnd = 0; private omRatioPk = 0;
  /** Packets waiting to be written into one cluster — see OM_PER_CLUSTER. */
  private omPending: Uint8Array[] = [];
  /** ★★★ TEN BLOCKS PER CLUSTER, NOT ONE. With every 20 ms packet in its own cluster the audio
   *  still stuttered at 1.4 s buffered (Stuart, 2026-09-14) — a starved element cannot explain
   *  that, a decoder restarted at every cluster boundary can. 200 ms per cluster. */
  private static readonly OM_PER_CLUSTER = 10;
  /** Playout cushion behind the live edge, seconds — ADAPTIVE, like the worklet's jitter buffer:
   *  starts here and grows on every stall (a tunnel delivers in bursts; the LAN does not), up to
   *  OM_CUSHION_MAX. Never shrinks within a session: a link that stalled once will stall again. */
  private static readonly OM_CUSHION = 0.4;
  private static readonly OM_CUSHION_MAX = 1.5;
  private omTarget = 0.4;
  private omStalls = 0;

  private async _startMediaPlayout(): Promise<boolean> {
    /* ★★★ PLAIN MediaSource FIRST. A ManagedMediaSource does not open until the element has
     *  actually started playing, and start() runs at connect time, not from a tap — so on the
     *  Mac (measured 2026-09-14) the managed one sat "closed" for the 4 s timeout while a plain
     *  one was "open" at once. The iPhone has ONLY the managed kind; there the source opens on
     *  the first tap (resume() plays the element), and we queue packets until it does. */
    const Managed = (self as unknown as { ManagedMediaSource?: typeof MediaSource }).ManagedMediaSource;
    const Plain: typeof MediaSource | undefined = typeof MediaSource !== 'undefined' ? MediaSource : undefined;
    const MS = Plain ?? Managed;
    const mime = 'audio/webm; codecs="opus"';
    let ok = false;
    try { ok = !!MS && MS.isTypeSupported(mime); } catch { ok = false; }
    if (!MS || !ok) { console.warn('[audio] media playout: no MediaSource for ' + mime); return false; }
    this.omManaged = !Plain && !!Managed;
    this.omStreaming = false; this.omSawStreaming = false;
    return await new Promise<boolean>((resolve) => {
      const el = document.createElement('audio');
      el.autoplay = true;
      // ★ In the document and attached with srcObject — the two Safari rules the DAB+ path
      //   learned the hard way (see _startMse). play() BEFORE sourceopen, for the same reason.
      el.style.display = 'none';
      document.body.appendChild(el);
      el.volume = this._volume; el.muted = this._muted;
      const ms = new MS();
      const anyEl = el as unknown as { srcObject: unknown; src: string };
      if ('srcObject' in el) { try { anyEl.srcObject = ms; } catch { anyEl.src = URL.createObjectURL(ms as unknown as MediaSource); } }
      else anyEl.src = URL.createObjectURL(ms as unknown as MediaSource);
      void el.play().catch(() => { /* a gesture may still be needed; the source still opens */ });
      let settled = false;
      ms.addEventListener('sourceopen', () => {
        try {
          const sb = ms.addSourceBuffer(mime);
          sb.mode = 'sequence';
          sb.addEventListener('updateend', () => this._mediaDrain());
          sb.addEventListener('error', () => console.warn('[audio] media playout: SourceBuffer error'));
          const mms = ms as unknown as EventTarget;
          mms.addEventListener('startstreaming', () => { this.omStreaming = true; this.omSawStreaming = true; this._mediaDrain(); });
          mms.addEventListener('endstreaming',   () => { this.omStreaming = false; });
          setTimeout(() => {
            if (!this.omSawStreaming) { this.omManaged = false; this._mediaDrain(); }
          }, 1500);
          this.omEl = el; this.omSrc = ms; this.omBuf = sb;
          // Packets queued before the source opened start again from a fresh init segment.
          this.omCh = 0; this.omSeq = 0; this.omQueue = []; this.omPending = [];
          this.omStartedAt = performance.now(); this.omProgressAt = 0; this.omLastTime = -1;
          el.addEventListener('timeupdate', () => {
            if (el.paused || el.muted) return;
            if (el.currentTime > this.omLastTime + 0.01) {
              this.omLastTime = el.currentTime;
              this.omProgressAt = performance.now();
              this._noteAudible();
            }
          });
          el.addEventListener('error', () => {
            const err = (el.error && el.error.message) || String(el.error && el.error.code);
            console.warn('[audio] media playout: element error — ' + err);
          });
          /* ★★ A STALL GROWS THE CUSHION. 'waiting' fires when the element runs out of buffered
           *  audio while playing — through the tunnel (Stuart, 2026-09-14: "the audio is
           *  dropping out") a 0.25 s cushion was eaten by every burst gap. Each stall adds
           *  0.3 s, the same shape as the worklet's JITTER_STEP; the rate trim then holds it. */
          el.addEventListener('waiting', () => {
            if (this.omFedAt && performance.now() - this.omFedAt < 3000) {
              this.omStalls++;
              const t = Math.min(AudioPlayer.OM_CUSHION_MAX, this.omTarget + 0.3);
              if (t !== this.omTarget) { this.omTarget = t; console.info('[audio] media playout: stall ' + this.omStalls + ' — cushion now ' + t.toFixed(1) + ' s'); }
            }
          });
          this._watchMediaPlayout();
          console.info('[audio] media playout: Opus/WebM via ' + (this.omManaged ? 'ManagedMediaSource' : 'MediaSource'));
          if (!settled) { settled = true; resolve(true); }
        } catch (e) {
          console.warn('[audio] media playout: setup failed — ' + ((e as Error)?.name || e));
          el.remove();
          if (!settled) { settled = true; resolve(false); }
        }
      }, { once: true });
      /* ★ Not open yet is not a failure: a managed source waits for the tap. Keep the element,
       *   say so, and let the packets queue — the watchdog hands back to Web Audio only if the
       *   element is FED and never plays, which cannot trigger before the source opens. */
      setTimeout(() => {
        if (!settled) {
          settled = true;
          console.info('[audio] media playout: source not open yet (readyState ' + ms.readyState + ') — waiting for a tap');
          this.omEl = el; this.omSrc = ms;
          resolve(true);
        }
      }, 1500);
    });
  }

  /** One Opus wire frame ([0]=ch [1]=3 [2..5]=rate, then the packet) → one WebM cluster. */
  private _mediaFeed(buf: ArrayBuffer) {
    if (!this.omEl || buf.byteLength < 7) return;
    const ch = new DataView(buf).getUint8(0) || 1;
    if (ch !== this.omCh) {
      // A new initialisation segment on the same buffer: same codec, new channel count.
      this.omQueue.push(webmInit(ch));
      this.omCh = ch;
      if (this.omSeq) console.info('[audio] media playout: channel count now ' + ch);
    }
    this.omPending.push(new Uint8Array(buf, 6));
    this.omSeq++;
    this.omFedAt = performance.now();
    if (this.omPending.length >= AudioPlayer.OM_PER_CLUSTER) {
      const first = this.omSeq - this.omPending.length;
      this.omQueue.push(webmCluster(first * 20, this.omPending));
      this.omPending = [];
    }
    // ★ Bounded, like every other audio queue here: audio seconds late is worse than a gap.
    while (this.omQueue.length > 100) this.omQueue.shift();
    this._mediaDrain();
  }

  private _mediaDrain() {
    const sb = this.omBuf;
    if (!sb || sb.updating || !this.omQueue.length) return;
    if (this.omManaged && !this.omStreaming) return;
    // ★ Coalesce what is waiting into ONE append — appends are the expensive part, not bytes.
    let n = 0; for (const s of this.omQueue) n += s.byteLength;
    const all = new Uint8Array(n); let off = 0;
    for (const s of this.omQueue) { all.set(s, off); off += s.byteLength; }
    this.omQueue = [];
    try { sb.appendBuffer(all as unknown as BufferSource); }
    catch (e) { console.warn('[audio] media playout: append failed — ' + ((e as Error)?.name || e)); }
  }

  /** Jump to (live edge − cushion). Used by flush() on a retune and by the lag guard. */
  private _mediaSkipToLive(cushion: number) {
    const el = this.omEl; if (!el) return;
    try {
      const b = el.buffered;
      if (!b.length) return;
      const end = b.end(b.length - 1);
      const to = Math.max(b.start(b.length - 1), end - cushion);
      if (to > el.currentTime) el.currentTime = to;
    } catch { /* not seekable yet */ }
  }

  /** Keep the element near the live edge, trim old ranges, restart a paused element, and hand
   *  the session back to Web Audio if this path never produces sound. */
  private _watchMediaPlayout() {
    if (this.omTimer !== null) return;
    this.omTimer = window.setInterval(() => {
      const el = this.omEl, sb = this.omBuf;
      if (!el || !sb) return;
      const now = performance.now();
      if (typeof document !== 'undefined' && document.visibilityState !== 'visible') return;
      try {
        const b = el.buffered;
        if (b.length) {
          const end = b.end(b.length - 1);
          const lag = end - el.currentTime;
          // The status row's "buf" figure is the cushion on this path, not the worklet's.
          this.jitterMs = Math.round(lag * 1000);
          if (this.omRatioPk === 0) { this.omRatioEnd = end; this.omRatioPk = this.omSeq; }
          else if (this.omSeq - this.omRatioPk >= 250) {
            this.omRatio = (end - this.omRatioEnd) / ((this.omSeq - this.omRatioPk) * 0.02);
            this.omRatioEnd = end; this.omRatioPk = this.omSeq;
          }
          /* ★★★ RATE, NOT SEEKS. The server's clock and the Mac's audio clock differ by a few
           *  hundred ppm, so the cushion creeps; the first version SKIPPED half a second whenever
           *  it passed 0.75 s — an audible jump every twenty seconds ("the audio is flapping on
           *  the pi", Stuart 2026-09-14) and each seek blinked the media session, which is why
           *  Spatialise Stereo "keeps disappearing and reappearing". Now the playback rate is
           *  trimmed by up to 2 % around the target cushion; Safari preserves pitch by default,
           *  so a trim that small is inaudible. A seek is kept only for a real pile-up. */
          /* ★★★ RATE 1.0, ALWAYS. The trim (±1–2 %) kept Safari time-stretching the whole time,
           *  and the stutter survived a 1.4 s cushion — so the stretch, not starvation, is the
           *  suspect. With an honest timeline (DefaultDuration) the only drift left is the server's
           *  clock against this machine's, hundreds of ppm at most: half a second takes many
           *  minutes to accumulate, and ONE small seek then is a far smaller wound than a
           *  continuous stretch. */
          if (el.playbackRate !== 1) el.playbackRate = 1;
          if (lag > this.omTarget + 0.5) this._mediaSkipToLive(this.omTarget);
          // Trim what has played, so the buffer never grows for the life of the page.
          if (!sb.updating && el.currentTime - b.start(0) > 20) sb.remove(0, el.currentTime - 10);
        }
      } catch { /* between states */ }
      if (el.paused && !this._muted) void el.play().catch(() => {});
      // ★★ FED BUT NEVER PLAYING: 8 s of packets with the clock stuck is this path failing in a
      //    browser that claimed to support it. Give the session back to Web Audio rather than sit
      //    silent — the same rule the Opus decoder follows (rebuild, never limit permanently).
      const fed = this.omFedAt > 0 && now - this.omFedAt < 2000;
      const since = Math.max(this.omProgressAt, this.omStartedAt);
      if (fed && since > 0 && now - since > 8000 && !this._muted) {
        console.warn('[audio] media playout: packets arriving but the element is not playing — back to Web Audio for this session');
        AudioPlayer.mediaPlayoutBroken = true;
        const wasRec = !!this.rec;
        this.close(); this.closedByUs = false;
        void this.start().then(() => { if (wasRec && this.worker) this.worker.postMessage({ type: 'rec', on: true }); });
      }
    }, 500);
  }

  private _mediaTeardown() {
    if (this.omTimer !== null) { clearInterval(this.omTimer); this.omTimer = null; }
    const el = this.omEl;
    if (el) {
      try { el.pause(); } catch { /* gone */ }
      try { (el as unknown as { srcObject: unknown }).srcObject = null; } catch { /* gone */ }
      try { el.remove(); } catch { /* not in the document */ }
    }
    this.omEl = null; this.omSrc = null; this.omBuf = null; this.omQueue = [];
    this.omCh = 0; this.omSeq = 0; this.omFedAt = 0; this.omProgressAt = 0; this.omStartedAt = 0;
    this.omTarget = AudioPlayer.OM_CUSHION; this.omStalls = 0; this.omPending = [];
  }

  // ── MediaSource fallback (Safari) ──────────────────────────────────────────────────────────
  private mseEl: HTMLAudioElement | null = null;
  private mseSrc: MediaSource | null = null;
  private mseBuf: SourceBuffer | null = null;
  private mseQueue: Uint8Array[] = [];
  private mseSeq = 1;
  private mseTime = 0;
  private mseDur = 1024;
  /** True when the source is a ManagedMediaSource, which gates appends — see _startMse. */
  private mseManaged = false;
  private mseStreaming = false;
  private mseSawStreaming = false;
  /** The format the live MediaSource was BUILT for; a change means rebuild. See _mseTeardown. */
  private mseCore = 0;
  private mseCh = 0;

  /** Open an <audio> + MediaSource pipeline for AAC. Returns false when the browser cannot. */
  private async _startMse(coreRateHz: number, channels: number): Promise<boolean> {
    const Managed = (self as unknown as { ManagedMediaSource?: typeof MediaSource }).ManagedMediaSource;
    const MS: typeof MediaSource | undefined =
      Managed ?? (typeof MediaSource !== 'undefined' ? MediaSource : undefined);
    this.mseManaged = !!Managed && MS === Managed;
    this.mseStreaming = false; this.mseSawStreaming = false;
    /* ★★★ 1024 FRAMING ON THIS PATH, ALWAYS — IT HAS EXACTLY ONE CONSUMER AND IT CANNOT DO 960.
     *  MediaSource is only reached when WebCodecs is absent or has failed us, which in practice
     *  means Safari, and Apple's decoder has been measured twice as unable to handle 960-sample
     *  AAC frames: afconvert renders 500 access units as 32.000 s where the air says 30.000
     *  (500 x 1024/16000 exactly), and Safari's AudioDecoder refuses a 960 configuration outright.
     *  ★★★ DECLARING 960 HERE PRODUCES THE SLOW PLAYBACK STUART HEARS: the config says one frame
     *      length and the decoder uses another, so the timeline and the audio disagree by
     *      1024/960 — "the barry white impressions are back but it is stereo and it is sounding
     *      better", which is the MediaSource switch working and the framing still wrong.
     *  ★ Both halves move together: the AudioSpecificConfig below and the samples-per-AU on the
     *    timeline are built from this same flag, so they cannot disagree about the frame length.
     *  ★ It does not touch the WebCodecs path, where 960 is declared truthfully and only dropped
     *    when a decoder actually rejects it. */
    this.aacUse960 = false;
    /* ★★★ THE MSE TIMELINE RUNS AT THE OUTPUT RATE, NOT THE CORE — see _ascExplicitSbr. */
    const outRate = this._sbrOutRate(coreRateHz);
    const asc = outRate ? this._ascExplicitSbr(coreRateHz, outRate, channels, this.aacUse960)
                        : this._ascFor(coreRateHz, channels, this.aacUse960);
    const tsRate = this._mseTimescale(coreRateHz);
    if (!MS)  { this._aacNote('no MediaSource in this browser'); return false; }
    if (!asc) { this._aacNote(`no config for ${coreRateHz} Hz / ${channels}ch`); return false; }
    const mime = [...(this.aacPs ? ['audio/mp4; codecs="mp4a.40.29"'] : []),
                  'audio/mp4; codecs="mp4a.40.5"', 'audio/mp4; codecs="mp4a.40.2"',
                  'audio/mp4; codecs="mp4a.40.05"', 'audio/mp4; codecs="mp4a.40.02"']
      .find((m) => { try { return MS.isTypeSupported(m); } catch { return false; } });
    if (!mime) { this._aacNote('MediaSource supports no AAC in mp4'); return false; }
    console.info(`[audio] DAB+ opening ${Managed ? 'ManagedMediaSource' : 'MediaSource'} with ${mime}`);

    return await new Promise<boolean>((resolve) => {
      const el = document.createElement('audio');
      el.autoplay = true;
      (el as unknown as { disableRemotePlayback: boolean }).disableRemotePlayback = true;
      /* ★★★ IN THE DOCUMENT, AND ATTACHED WITH srcObject. Two Safari requirements that a
       *  detached element with a blob URL fails silently:
       *   - ManagedMediaSource CANNOT be attached with URL.createObjectURL — it is srcObject
       *     only, and createObjectURL(MediaSource) throws or yields a source that never opens;
       *   - the element has to be in the document for Safari to start the media pipeline.
       *  Either mistake shows up as sourceopen simply never firing, which is what the 3-second
       *  timeout was catching and reporting as "no AAC support". */
      el.style.display = 'none';
      document.body.appendChild(el);
      const ms = new MS();
      const anyEl = el as unknown as { srcObject: unknown; src: string };
      // ★ `el.src` inside this guard narrows el to never — assign through anyEl in BOTH arms.
      if ('srcObject' in el) { try { anyEl.srcObject = ms; } catch { anyEl.src = URL.createObjectURL(ms as unknown as MediaSource); } }
      else anyEl.src = URL.createObjectURL(ms as unknown as MediaSource);
      /* ★★★ PLAY FIRST, THEN WAIT FOR sourceopen — NOT THE OTHER WAY ROUND. A ManagedMediaSource
       *  does not open until the element actually ATTEMPTS PLAYBACK; that is the whole basis of it
       *  being "managed". We called play() inside the sourceopen handler, so we were waiting for
       *  an event that could only be caused by a call we would make once it arrived. It timed out
       *  every single time and the client reported "DAB+ needs AAC support this browser does not
       *  offer" — Safari's own screenshot, 2026-09-06, on a page where the ensemble and the
       *  station list were decoding perfectly beside the message.
       *  ★★ THIS IS WHY THE startstreaming FIX IN 4.6.1 CHANGED NOTHING: it lives inside
       *     sourceopen, which never ran. A fix behind a door that never opens.
       *  ★ Rejection is expected and harmless when a gesture is still needed — the element keeps
       *    the source attached, sourceopen still fires, and the buffer fills meanwhile. */
      void el.play().catch(() => { /* gesture may be required; the source still opens */ });
      let settled = false;
      ms.addEventListener('sourceopen', () => {
        try {
          const sb = ms.addSourceBuffer(mime);
          sb.mode = 'sequence';
          sb.addEventListener('updateend', () => this._mseDrain());
          /* ★★★ ManagedMediaSource ONLY WANTS DATA BETWEEN startstreaming AND endstreaming.
           *  This is the half of the MMS contract we were not keeping. Safari's managed source
           *  decides for itself when it is prepared to buffer — that is the entire point of it,
           *  and the reason it exists on iOS where an unmanaged MediaSource is not available —
           *  and appends made outside that window can be dropped on the floor. The source opens,
           *  we queue happily, and nothing ever plays: precisely what Stuart has seen every time
           *  ("no aac in safari", three builds running).
           *  ★★ Harmless on a plain MediaSource, which never fires these, so the queue drains on
           *     updateend as before. No branch on browser identity — the events decide.
           *  ★ While not streaming we hold the frames instead of appending; DAB+ access units are
           *    about 170 bytes, so a brief hold costs nothing and keeps the timeline continuous. */
          const mms = ms as unknown as EventTarget;
          mms.addEventListener('startstreaming', () => {
            this.mseStreaming = true; this.mseSawStreaming = true; this._mseDrain();
          });
          mms.addEventListener('endstreaming',   () => { this.mseStreaming = false; });
          /* ★★★ AND A WAY OUT, because I cannot test Safari from here. If the gate above is wrong
           *  for this browser — startstreaming never fires, or fires only once data has arrived —
           *  then holding everything back would be WORSE than the bug it is meant to fix: silence
           *  by a new route. After 1.5 s with no such event, assume an unmanaged source and append
           *  regardless. A guess that can only ADD appends, never remove them. */
          setTimeout(() => {
            if (!this.mseSawStreaming) {
              console.info('[audio] no startstreaming in 1.5 s — treating the source as unmanaged');
              this.mseManaged = false;
              this._mseDrain();
            }
          }, 1500);
          this.mseEl = el; this.mseSrc = ms; this.mseBuf = sb;
          this.mseCore = coreRateHz; this.mseCh = channels;
          this.mseSeq = 1; this.mseTime = 0; this.mseDur = this._msePerAu(coreRateHz);
          this.mseQueue = [initSegment(tsRate, channels, asc)];
          this._mseDrain();
          void el.play().catch(() => { /* a gesture may be needed; the buffer keeps filling */ });
          console.info(`[audio] DAB+ via MediaSource ${mime} @ ${coreRateHz} Hz, ${channels}ch`);
          if (!settled) { settled = true; resolve(true); }
        } catch (e) {
          this._aacNote(`MediaSource setup failed: ${(e as Error)?.name || e}`);
          if (!settled) { settled = true; resolve(false); }
        }
      }, { once: true });
      setTimeout(() => {
        if (!settled) {
          settled = true;
          this._aacNote(`MediaSource never opened (readyState ${ms.readyState}${this.mseManaged ? ', managed' : ''})`);
          el.remove();
          resolve(false);
        }
      }, 4000);
    });
  }

  /** Append one queued segment; the rest follow on updateend. */
  private _mseDrain() {
    const sb = this.mseBuf;
    if (!sb || sb.updating || !this.mseQueue.length) return;
    // ★ See the startstreaming note: a managed source takes data only while it asks for it.
    if (this.mseManaged && !this.mseStreaming) return;
    const seg = this.mseQueue.shift()!;
    try { sb.appendBuffer(seg as unknown as BufferSource); }
    catch (e) {
      this._aacNote(`MSE append failed: ${(e as Error)?.name || e}`);
      this.mseQueue.length = 0;
    }
  }

  /** Wrap one access unit and queue it. Returns true when MSE is carrying the audio. */
  private _mseFeed(au: Uint8Array): boolean {
    if (!this.mseBuf) return false;
    this.mseQueue.push(mediaSegment(this.mseSeq++, this.mseTime, this.mseDur, au));
    this.mseTime += this.mseDur;
    // ★ Bounded, like every other audio queue here: audio minutes late is worse than a gap.
    while (this.mseQueue.length > 60) this.mseQueue.shift();
    this._mseDrain();
    return true;
  }

  /** ★ Say it where the listener is looking. A DAB+ service that cannot be decoded must not
   *  present as a working radio with no sound — the decoder box says so instead. */
  /** ★★★ A DECODE ERROR IS NOT A CAPABILITY VERDICT. AudioDecoder reports a bad access unit
   *  through the same error callback as an unsupported configuration, and both went to
   *  _failAac — which sets aacBroken FOR THE SESSION and tells the listener their browser cannot
   *  play AAC. Stuart, 2026-09-05, on Classic FM (64k HE-AAC, the best DAB+ we have): "it worked
   *  for about a minute in edge before giving up and saying this browser doesnt support aac."
   *  The browser plainly did support it — it had just played a minute of it.
   *  ★★★ And DAB HANDS US CORRUPT FRAMES BY DESIGN on a marginal signal; that is the whole point
   *      of the error protection. A decoder that dies on the first one cannot work on radio.
   *  ★★ NEVER LIMIT PERMANENTLY: rebuild the decoder and carry on. Only if it fails repeatedly
   *     WITHOUT ever producing audio is "this browser cannot" an honest thing to say.
   *  ★ The decoder is dropped rather than reset: _decodeAac reconfigures when aacDec is null,
   *    which is the existing path and needs no second one. */
  private aacOkFrames = 0;
  private aacRecoveries = 0;
  /** ★★★ WHETHER TO TELL THE DECODER THE TRUTH ABOUT 960-SAMPLE FRAMES.
   *  DAB+ genuinely uses them, and saying so is correct — but APPLE'S DECODER CANNOT DECODE THEM.
   *  Safari's console, 2026-09-06: "DAB+ decoding as mp4a.40.5 (raw + ASC) @ 16000 Hz, 2ch" then
   *  "decode error after 0 good frames — EncodingError: InternalAudioDecoderCocoa decoding
   *  failed", over and over. ffmpeg refuses the same thing ("SBR with 960 frame length is not
   *  implemented"), so Apple is not alone.
   *  ★★★ AND FALLING BACK IS NOW SAFE, which it would not have been a build ago: since the
   *      playback rate is COMPUTED as frames x core / 960, a decoder that assumes 1024 hands back
   *      2048 samples for a 60 ms unit and we play them at 34133 Hz — restoring the duration AND
   *      the pitch. The lie costs nothing but a slightly different transform.
   *  ★ Truth first, then the lie: a decoder that handles 960 gets it. Only one that fails without
   *    ever producing a frame is told 1024 instead. */
  private aacUse960 = true;
  /** Parametric stereo (HE-AAC v2): mono core, stereo out. Set from the frame header. */
  private aacPs = false;
  /** Consecutive mono frames from a service declared as parametric stereo — see _onAacData. */
  private aacMonoFrames = 0;
  private aacTriedMse = false;
  private _recoverAac(e: unknown) {
    if (this.aacBroken) return;
    /* ★★★ NEVER PRODUCED A FRAME? THE CONFIG IS THE SUSPECT, NOT THE DATA. Try the other frame
     *  length before concluding anything about the browser — see aacUse960. */
    if (this.aacOkFrames === 0 && this.aacUse960) {
      this.aacUse960 = false;
      this._aacNote('decoder rejected 960-sample frames — retrying as 1024 '
                  + '(playback rate is computed, so the speed stays right)');
      try { this.aacDec?.close(); } catch { /* already gone */ }
      this.aacDec = null; this.aacTs = 0; this.aacRecoveries = 0;
      return;
    }
    if (this.aacOkFrames === 0 && ++this.aacRecoveries > 3) {
      this._failAac('decode', e);          // never once worked — that IS a capability answer
      return;
    }
    this.aacRecoveries++;
    console.warn(`[audio] DAB+ decode error after ${this.aacOkFrames} good frames — `
               + `rebuilding the decoder (recovery ${this.aacRecoveries}):`, e);
    try { this.aacDec?.close(); } catch { /* already gone */ }
    this.aacDec = null;
    this.aacTs = 0;
    this.aacOkFrames = 0;
  }

  /** ★★★ THE REASON, ON SCREEN. "this browser does not offer AAC" was shown for every possible
   *  failure, including ones where the browser plainly did offer it — Safari has now spent four
   *  builds reporting a missing feature when the real faults were, in order: a fallback placed
   *  inside the thing it was a fallback for, a config it had to infer from, a play() call made
   *  inside the event that only play() could cause, and a source reused across formats. Each time
   *  the message sent us looking at the browser instead of at us, and each time it needed Stuart
   *  at a keyboard to find out otherwise.
   *  ★ The trail is kept in `aacWhy` as the path is walked, so whatever finally fails can say
   *    where it got to. Costs nothing and removes a round trip through a person. */
  private aacWhy = '';
  private _aacNote(s: string) { this.aacWhy = s; console.info(`[audio] DAB+: ${s}`); }
  private _failAac(stage: string, e: unknown) {
    if (this.aacBroken) return;
    this.aacBroken = true;
    console.warn(`[audio] DAB+ AAC ${stage} failed:`, e);
    const st = document.getElementById('decStatus');
    if (st) st.textContent = `DAB+ audio unavailable — ${this.aacWhy || stage}`;
  }

  private _onAacData(ad: AudioData) {
    const ch = ad.numberOfChannels, frames = ad.numberOfFrames;
    /* ★★★ RESAMPLE TO 48 kHz FROM WHAT THE DECODER ACTUALLY PRODUCED. _playPcm has one rate and
     *  it is 48000; a DAB+ service at 32 kbit/s decodes to 32 kHz, and handing that straight over
     *  plays it 1.5x fast. Stuart, on Edge: "chipmunks in DAB+" — the SECOND time DAB has produced
     *  chipmunks, and for the same underlying reason both times: a rate that was assumed instead
     *  of read. The first was mono MP2 at 24 kHz. Read ad.sampleRate; never assume it.
     *  ★ It is read from the AudioData rather than computed from the ADTS, because whether the
     *    decoder applied SBR (doubling it) is the decoder's business and differs between them. */
    this.aacOkFrames++;                  // ★ proof the browser CAN decode this — see _recoverAac
    /* ★★★ THE DECODER SAID YES AND THEN DID NOT DO IT. A parametric-stereo service has a MONO
     *  core and a reconstructed second channel; if the service is PS and the decoder keeps
     *  handing back ONE channel, it is not reconstructing anything — we are hearing the bare
     *  16 kHz core, which is Stuart's "hold music on a phone call" and, after the AOT 29 fix,
     *  still "sounds like arse in safari".
     *  ★★★ Safari's WebCodecs AudioDecoder accepts the HE-AAC v2 configuration and decodes only
     *      the core; its MEDIASOURCE path is the full AVFoundation pipeline and is a different
     *      decoder entirely. We never reached it because WebCodecs exists, so its presence was
     *      being taken as proof it was the better route. It is not, for this codec.
     *  ★★ Judged on OUTPUT, not on what the config promised: eight frames of mono when stereo was
     *     declared is a fact, where isConfigSupported was only ever a prediction.
     *  ★ One switch per service. If MediaSource cannot take it either, _startMse returns false
     *    and nothing is worse than it was. */
    if (this.aacPs && ch === 1 && !this.aacTriedMse) {
      if (++this.aacMonoFrames >= 8) {
        this.aacTriedMse = true;
        this._aacNote('parametric stereo declared but the decoder returns mono — '
                    + 'switching to MediaSource');
        try { this.aacDec?.close(); } catch { /* already gone */ }
        this.aacDec = null; this.aacTs = 0; this.aacOkFrames = 0;
        ad.close();
        void this._startMse(this.aacRate, this.aacCh);
        return;
      }
    } else if (ch > 1) {
      this.aacMonoFrames = 0;
    }
    /* ★★★ TRUST THE FRAME COUNT OVER THE REPORTED RATE. Under SBR an access unit is 1024 CORE
     *  samples and the decoder returns 2048 at DOUBLE the rate — but some decoders report the
     *  CORE rate on the AudioData while handing back the doubled samples. Resampling from the
     *  core rate then stretches everything by exactly 2: Stuart, on Edge, "DAB+ sample rate seems
     *  to be too slow ... justin timberlake is sounding closer to barry white". An octave down is
     *  the unmistakable signature of 2x, and the frame count is the half of the pair that cannot
     *  lie about it. */
    /* ★★★ COMPUTE THE RATE, DO NOT ASK FOR IT. Every DAB+ access unit is 960 CORE samples —
     *  measured on air, not assumed: 576 AUs in 34.6 s on a 16 kHz-core service is 60.0 ms each,
     *  and 60 ms x 16000 = 960 exactly. So whatever the decoder hands back covers a known 960/core
     *  seconds, and its true rate is frames x core / 960 — arithmetic, with nothing to disagree
     *  about.
     *  ★★★ ad.sampleRate IS NOT RELIABLE HERE and that is the whole bug. Decoders differ over
     *      whether they report the CORE rate or the SBR-doubled one, so believing it made some
     *      services play fast and others slow on the same browser — Stuart: "some stations are
     *      slow and others are fast", then "Lou Reed ... is fast" and later "Axel Rose is sounding
     *      more like a soul singer". Two directions of error from one assumption.
     *  ★ ad.sampleRate is kept only as the fallback when the frame count is missing, and the
     *    result is sanity-clamped: an absurd rate should degrade to "believe the decoder", not to
     *    silence or a squeal. */
    let srIn = ad.sampleRate || 48000;
    if (frames > 0 && this.aacRate > 0) {
      const computed = Math.round(frames * this.aacRate / 960);
      if (computed >= 8000 && computed <= 96000) srIn = computed;
    }
    const plane = new Float32Array(frames);
    const planes: Float32Array[] = [];
    for (let c = 0; c < ch; c++) {
      const p = new Float32Array(frames);
      ad.copyTo(p, { planeIndex: c, format: 'f32-planar' });
      planes.push(p);
    }
    ad.close();
    void plane;

    if (srIn === 48000) {
      const pcm = new Int16Array(frames * ch);
      for (let c = 0; c < ch; c++)
        for (let i = 0; i < frames; i++) {
          let sm = Math.round(planes[c][i] * 32767);
          pcm[i * ch + c] = sm < -32768 ? -32768 : sm > 32767 ? 32767 : sm;
        }
      this._playPcm(pcm, ch);
      return;
    }

    const step = srIn / 48000;
    const out = Math.max(0, Math.floor((frames - 1) / step));
    if (out <= 0) return;
    const pcm = new Int16Array(out * ch);
    for (let c = 0; c < ch; c++) {
      const src = planes[c];
      for (let n = 0; n < out; n++) {
        const t = n * step;
        const i = t | 0;
        const fr = t - i;
        const v = src[i] + (src[i + 1 < frames ? i + 1 : i] - src[i]) * fr;
        let sm = Math.round(v * 32767);
        pcm[n * ch + c] = sm < -32768 ? -32768 : sm > 32767 ? 32767 : sm;
      }
    }
    this._playPcm(pcm, ch);
  }

  private _onOpusData(ad: AudioData) {
    // ★ A packet decoded = the decoder is healthy. Without this reset, four failures spread
    //   across an hour of perfect audio would eventually trip the limit.
    this.opusFails = 0;
    this.opusStuck = false;
    const ch = ad.numberOfChannels, frames = ad.numberOfFrames;
    const pcm = new Int16Array(frames * ch);
    const plane = new Float32Array(frames);
    for (let c = 0; c < ch; c++) {
      ad.copyTo(plane, { planeIndex: c, format: 'f32-planar' });
      for (let i = 0; i < frames; i++) {
        let sm = Math.round(plane[i] * 32767);
        sm = sm < -32768 ? -32768 : sm > 32767 ? 32767 : sm;
        pcm[i * ch + c] = sm;   // interleaved for stereo, sequential for mono (ch===1)
      }
    }
    ad.close();
    this._playPcm(pcm, ch);
  }

  /** Common tail: record tap + int16 → float L/R + push to the worklet. Fed by PCM/ADPCM (sync)
   *  and by the Opus decoder (async). `pcm` is interleaved for stereo, sequential for mono. */
  /** ★★ THE RECORD TAP, ONCE. Both paths reach it: the page's own decode calls it from _playPcm,
   *  and the Worker forwards PCM here while a recording is running. Two copies of this would drift,
   *  and the one that drifted would be discovered in somebody's saved file rather than on air.
   *  ★ Always stereo. A WFM stream silently drops from 2ch to 1ch when the pilot unlocks, and a WAV
   *    header cannot change channel count midway — so duplicate mono rather than write a file that
   *    desyncs halfway through. */
  private _recordPcm(pcm: Int16Array, ch: number) {
    if (!this.rec) return;
    const frames = Math.floor(pcm.length / Math.max(1, ch));
    if (frames <= 0) return;
    let out: Int16Array;
    if (ch === 2) {
      out = pcm.slice(0, frames * 2);
    } else {
      out = new Int16Array(frames * 2);
      for (let i = 0; i < frames; i++) { out[i * 2] = pcm[i]; out[i * 2 + 1] = pcm[i]; }
    }
    this.rec.chunks.push(out);
    this.rec.frames += frames;
    this.rec.ch = 2;
  }

  private _playPcm(pcm: Int16Array, ch: number) {
    const frames = Math.floor(pcm.length / Math.max(1, ch));
    if (frames <= 0) return;
    if (this.fault === 'decoder') return;       // self-heal test: a decoder that stopped emitting

    if (this.rec) this._recordPcm(pcm, ch);

    const l = new Float32Array(frames);
    const r = new Float32Array(frames);
    let peak = 0;
    if (ch === 2) {
      for (let i = 0; i < frames; i++) {
        const a = pcm[i * 2] / 32768;
        const b = pcm[i * 2 + 1] / 32768;
        l[i] = a; r[i] = b;
        const m = Math.max(Math.abs(a), Math.abs(b));
        if (m > peak) peak = m;
      }
    } else {
      for (let i = 0; i < frames; i++) {
        const a = pcm[i] / 32768;
        l[i] = a; r[i] = a;
        const m = Math.abs(a);
        if (m > peak) peak = m;
      }
    }
    if (peak > 0.002) this._noteAudible();
    this.cb.onLevel?.(peak);
    if (this.node) this.node.port.postMessage({ l, r }, [l.buffer, r.buffer]);
    else if (this.ring) this._pushRing(l, r);
  }

  // ── Recording ──────────────────────────────────────────────────────────────
  // Tapped off the DECODED int16 stream, not the speaker output: what lands in
  // the file is bit-exact what the server sent, with no second lossy encode.

  private rec: { chunks: Int16Array[]; frames: number; ch: number; startedAt: number } | null = null;

  startRecording() {
    this.rec = { chunks: [], frames: 0, ch: 1, startedAt: Date.now() };
    // ★ The Worker does not forward PCM unless someone is recording — see emit() in WORKER_SRC.
    this.worker?.postMessage({ type: 'rec', on: true });
  }

  get recording(): boolean { return this.rec !== null; }

  /** Seconds recorded so far. */
  get recordedSeconds(): number {
    return this.rec ? (Date.now() - this.rec.startedAt) / 1000 : 0;
  }

  /** Stop and return a WAV blob (null if nothing was captured). */
  stopRecording(): Blob | null {
    this.worker?.postMessage({ type: 'rec', on: false });
    const r = this.rec;
    this.rec = null;
    if (!r || !r.frames) return null;

    const total = r.chunks.reduce((n, c) => n + c.length, 0);
    const pcm = new Int16Array(total);
    let off = 0;
    for (const c of r.chunks) { pcm.set(c, off); off += c.length; }

    return wavBlob(pcm, r.ch, 48000);
  }

  /** True when the browser is holding playback until a user gesture. */
  /** ★ On the media path "suspended" means the element is blocked waiting for a tap: Safari
   *  will not play() outside a gesture, and without this the page's TAP TO START badge never
   *  showed — "the audio will sit silent until I touch a control" (Stuart, 2026-09-14). Only once
   *  packets have arrived, so a connecting page is not told to tap for nothing. */
  get suspended(): boolean {
    if (this.omEl) return this.omEl.paused && this.omSeq > 0 && !this._muted;
    if (!this.ctx) return false;
    if (this.ctx.state === 'suspended' || (this.ctx.state as string) === 'interrupted') return true;
    // ★ Safari: the context can be running while the element that carries its output is still
    //   paused — silent, and only a tap can play it. That is a gate to show, not a mystery.
    return !!this.mediaEl && !!this.streamDest && this.mediaEl.paused && !this._muted;
  }

  /** True while audio frames are actually arriving. */
  get streaming(): boolean { return this.workerOpen || this.ws?.readyState === WebSocket.OPEN; }

  /**
   * What's actually wrong with the audio, for the status line. Silence has
   * several causes that look identical from the outside — a suspended context, a
   * dead socket, our own mute, or (invisibly to us) SAFARI'S PER-TAB MUTE, which
   * no in-page control can override. Say which, rather than just going quiet.
   */
  get health(): 'ok' | 'suspended' | 'no-stream' | 'muted' | 'squelched' | 'silent' | 'opus-stuck' {
    // ★ Ranked ABOVE the others: when Opus is failing on a server that forbids the fallback,
    //   every other symptom ('silent', 'no-stream') is a consequence, and reporting the
    //   consequence sends the listener looking at their tab mute instead of the real cause.
    if (this.needsCodec || this.opusStuck) return 'opus-stuck';
    if (!this.ctx && !this.omEl) return 'no-stream';
    if (this.suspended) return 'suspended';
    if (!this.streaming) return 'no-stream';
    if (this._muted) return 'muted';
    // Frames arriving, context running, not muted — but nothing heard for a while.
    // If squelch is ARMED, that is the squelch doing its job, not a fault. Only with
    // squelch OFF is silence genuinely suspicious, and then a tab-level mute is the
    // likeliest cause (and one no in-page control can override).
    if (this.lastAudibleAt && performance.now() - this.lastAudibleAt > 5000) {
      return this.squelchActive ? 'squelched' : 'silent';
    }
    return 'ok';
  }

  private lastAudibleAt = 0;
  /** performance.now() of the last audible output, 0 if none yet — for "has anything played since
   *  I pressed that station?" (the DAB tuning-in line). */
  get lastAudibleAtMs(): number { return this.lastAudibleAt; }
  /** ★ performance.now() of the last moment ANY path put samples out: the PCM level scan OR the
   *  playout node's drain report. `lastAudibleAt` alone is only written on the PCM path, so on a
   *  worker-decoded stream it never moved and the DAB "tuning in" line stuck for a minute after
   *  the audio had started (Stuart, 2026-09-10). */
  get lastOutputAtMs(): number { return this.lastAudibleAt; }
  /** When the current unbroken run of audible output began (performance.now()), and how long it
   *  has lasted. ★ A DAB station can fire a FLASH of audio as it starts and fall silent again
   *  (Stuart, 2026-09-10), so "started" needs a sustained run, not a first peak. A gap of more
   *  than 400 ms between audible reports starts a new run. */
  get audibleRunStartAtMs(): number { return this.audibleRunStart; }
  get audibleRunMs(): number { return this.lastAudibleAt > 0 ? this.lastAudibleAt - this.audibleRunStart : 0; }
  private audibleRunStart = 0;
  private _noteAudible() {
    const now = performance.now();
    if (now - this.lastAudibleAt > 400) this.audibleRunStart = now;
    this.lastAudibleAt = now;
  }
  /** When the playout node last reported that it had actually put samples out. */
  private lastDrainAt = 0;
  private lastCtxTime = 0;         // the context clock as last seen by the watchdog
  private ctxAdvancedAt = 0;       // when it last moved
  private contextRebuilds = 0;     // full teardowns — capped so a broken browser cannot loop
  private rebuilding = false;
  private hiddenAt = 0;            // when the tab went to the background (0 = visible)

  /** Tear everything down and start again: the only cure for a context whose render thread has
   *  died. Volume, mute and recording survive; the socket is reopened; the caller's callbacks
   *  are the same object, so the UI keeps working. */
  private async _rebuildContext() {
    if (this.rebuilding) return;
    this.rebuilding = true;
    const wasRec = !!this.rec;
    try {
      this.close();
      this.closedByUs = false;
      this.lastCtxTime = 0; this.ctxAdvancedAt = 0;
      this.lastAudibleAt = 0; this.lastDrainAt = 0;
      await this.start();
      if (wasRec && this.worker) this.worker.postMessage({ type: 'rec', on: true });
      console.info('[audio] audio context rebuilt');
    } catch (e) {
      console.error('[audio] rebuilding the audio context failed', e);
    } finally {
      this.rebuilding = false;
    }
  }
  private stallWatch: number | null = null;

  /* ★★★ SELF-HEALING — IS WHAT ARRIVES ACTUALLY BEING PLAYED? (Stuart, 2026-09-28)
   *
   *  Audio stopped in FM stereo and DAB while the server kept sending and everything else looked
   *  fine; a page refresh cured it. The watchdog that used to sit here judged "feeding" by AUDIBLE
   *  OUTPUT FROM THE WORKLET — the very thing that had stopped — so on the Worker path (every
   *  modern browser: PCM goes Worker → worklet and never touches this thread) `feeding` went false
   *  the moment the output stalled and the watchdog stood down exactly when it was needed. Its
   *  clock-frozen and suspended-context branches had the same blind spot.
   *
   *  ★★ NOW: two counters, compared by the shared rules in src/services/audioSelfHeal.ts —
   *     rxFrames   every frame off the socket (the server sends continuous frames in every mode,
   *                silent and squelched included), counted where it arrives;
   *     playedTotal samples the OUTPUT consumed (the worklet's drain count, or the ScriptProcessor's).
   *     Receiving and nothing played for 3 s → rebuild the local pipeline (resume/kick the context,
   *     a fresh playout node, a fresh decoder); still silent 3 s later → reopen the socket; the next
   *     round rebuilds the WHOLE context; back-off and a 4-a-minute cap, never permanent. Nothing
   *     received for 6 s (a half-open socket: the Worker's own reconnect only fires on a CLOSE)
   *     → reopen the socket.
   *  ★★ NEVER FIGHT THE LISTENER. Not judged while muted, while the context has never played (the
   *     autoplay gate — only a tap cures that, and the page already shows TAP TO START), on media
   *     playout (its own watchdog), while DAB+ AAC is arriving (it may play through an element the
   *     counters cannot see), in a hidden WebKit tab (Safari stops rendering there by design —
   *     the false rebuilds of 2026-09-14), or within 2 s of a retune flush / 6 s of a DAB switch.
   *  ★ Every repair is logged with the counters, and kept in debugState().heal, so a recurrence
   *    leaves evidence rather than a mystery. */
  private heal = new AudioSelfHeal({ notRecvMs: 6000 });
  private rxFrames = 0;
  private lastAacAt = 0;
  private playedTotal = 0;
  private nodeDrained = 0;          // the CURRENT node's own cumulative figure (it restarts at 0)
  private fedTotal = 0;
  private nodeFed = 0;
  private healLog: Array<{ at: string; state: string; action: string; attempt: number; reason: string; counters: string }> = [];
  /** Self-heal fault injection — see debugFault(). '' = none. */
  private fault = '';

  private _noteRx(format: number) {
    this.rxFrames++;
    if (format === 4) this.lastAacAt = performance.now();
  }

  /** Suppress self-heal judgement for a legitimate transition the page knows about (a DAB service
   *  switch, a mode change): the audio may pause while the server re-primes, and that is not a
   *  fault to repair. */
  holdHealing(ms: number) { this.heal.hold(performance.now(), ms); }

  /** One handler for every playout node — the one start() builds and every rebuilt one. */
  private _wireNode(node: AudioWorkletNode) {
    this.nodeDrained = 0; this.nodeFed = 0;
    node.port.onmessage = (e: MessageEvent) => {
      const d = e.data as { jitterMs?: number; drained?: number; underruns?: number;
                            skips?: number; audible?: number; fed?: number };
      if (typeof d?.jitterMs === 'number') this.jitterMs = d.jitterMs;
      if (typeof d?.underruns === 'number') this.underruns = d.underruns;
      if (typeof d?.skips === 'number') this.skips = d.skips;
      // ★★★ THE ONLY PROOF THAT SOUND IS LEAVING. See the note in the worklet: every other
      //     signal this class has is measured before the node.
      if (typeof d?.drained === 'number') {
        this.lastDrainAt = performance.now();
        if (d.drained > this.nodeDrained) this.playedTotal += d.drained - this.nodeDrained;
        this.nodeDrained = d.drained;
      }
      if (typeof d?.fed === 'number') {
        if (d.fed > this.nodeFed) this.fedTotal += d.fed - this.nodeFed;
        this.nodeFed = d.fed;
      }
      if (d?.audible) this._noteAudible();
    };
  }

  private _watchOutput() {
    if (this.stallWatch !== null) return;
    this.stallWatch = window.setInterval(() => {
      if (!this.ctx) return;
      const now = performance.now();
      /* ★★★ A HIDDEN WEBKIT TAB IS NOT A STALL. Safari throttles or pauses audio rendering in a
       *   background tab: the worklet stops draining and the context clock stops while the state
       *   still says "running". The old node rebuild fired on Stuart's idle Pi tab ("the audio
       *   rebuild is false triggering", 2026-09-14). Chromium and Firefox keep rendering a
       *   playing tab in the background, so there a stall while hidden IS a stall. */
      const hidden = typeof document !== 'undefined' && document.visibilityState !== 'visible';
      if (hidden && AudioPlayer.isWebKit()) this.hiddenAt = this.hiddenAt || now;
      else this.hiddenAt = 0;
      const ct = this.ctx.currentTime;
      if (ct > this.lastCtxTime + 0.05 || this.ctxAdvancedAt === 0) { this.lastCtxTime = ct; this.ctxAdvancedAt = now; }
      const aac = this.lastAacAt > 0 && now - this.lastAacAt < 3000;
      const gated = this.ctx.state !== 'running' && this.playedTotal === 0;
      const expected = (!!this.node || !!this.sp) && !this.omEl && !this._muted && !this.hiddenAt
                    && !aac && !gated && !this.rebuilding && !this.needsCodec && !this.opusStuck;
      const d = this.heal.tick({ now, rx: this.rxFrames, played: this.playedTotal, expected });
      if (d.action !== 'none') this._repair(d);
    }, 1000);
  }

  private _repair(d: HealDecision) {
    const counters = `rx=${this.rxFrames} fed=${this.fedTotal} played=${this.playedTotal} `
                   + `ctx=${this.ctx?.state}@${this.ctx?.currentTime.toFixed(2)} `
                   + `path=${this.worker ? 'worker' : (this.ws ? 'main' : 'none')} `
                   + `underruns=${this.underruns} skips=${this.skips}`;
    const rung = d.action === 'reopen-socket' ? 'reopening the audio socket'
               : d.attempt >= 3 ? 'rebuilding the whole audio context'
               : 'rebuilding the local pipeline (context, playout node, decoder)';
    console.warn(`[audio] SELF-HEAL #${this.heal.repairs} ${d.state}: ${d.reason} — ${rung} (attempt ${d.attempt}) · ${counters}`);
    this.healLog.push({ at: new Date().toISOString(), state: d.state, action: d.action,
                        attempt: d.attempt, reason: d.reason, counters });
    if (this.healLog.length > 20) this.healLog.shift();
    if (d.action === 'reopen-socket') { this._reopenSocket(); return; }
    if (d.attempt >= 3) {
      // ★ The third rung: a new context, worklet, element and socket — the cure for Safari's dead
      //   render thread (state "running", clock frozen), where a new node on a dead context is
      //   still dead. Everything the listener set survives (see _rebuildContext).
      this.contextRebuilds++;
      void this._rebuildContext();
      return;
    }
    this._rebuildPipeline();
  }

  /** ★★ RUNG ONE: EVERYTHING LOCAL, NOTHING ON THE WIRE. Cheap, and no gap anybody can hear that
   *  was not already silence. The socket and the session are untouched. */
  private _rebuildPipeline() {
    const ctx = this.ctx;
    if (!ctx) return;
    const now = performance.now();
    // 1. The context. Not running → resume (Safari has a THIRD state, `interrupted`, that the
    //    old check skipped). Running with a frozen clock → the suspend/resume kick.
    if (ctx.state !== 'running') void ctx.resume().catch(() => {});
    else if (now - this.ctxAdvancedAt > 2500) {
      const c = ctx;
      void c.suspend().then(() => c.resume()).catch(() => {});
    }
    if (this.mediaEl && this.mediaEl.paused && !this._muted) void this.mediaEl.play().catch(() => {});
    // 2. A fresh playout node (and, below, its feed port to the Worker).
    if (this.node && this.gain) {
      try { this.node.disconnect(); } catch { /* already gone */ }
      try {
        const node = new AudioWorkletNode(ctx, 'vibe-sink', { outputChannelCount: [2] });
        this._wireNode(node);
        node.connect(this.gain);
        this.node = node;
        this.lastDrainAt = now;
        // 3. A fresh decoder. In the Worker it is rebuilt there, and handed the new node's port.
        if (this.worker) {
          const ch = new MessageChannel();
          node.port.postMessage({ sinkPort: ch.port2 }, [ch.port2]);
          this.worker.postMessage({ type: 'reset', sinkPort: ch.port1 }, [ch.port1]);
        }
      } catch (e) {
        console.error('[audio] self-heal: rebuilding the playout node failed', e);
      }
    } else if (this.sp) {
      this.rPos = this.wPos; this.filled = 0; this.playing = false;
    }
    if (!this.worker) {
      try { this.opusDec?.close(); } catch { /* already closed */ }
      this.opusDec = null; this.opusCh = 0; this.opusTs = 0;
      try { this.wasmDec?.free(); } catch { /* already freed */ }
      this.wasmDec = null; this.wasmReady = false; this.wasmCh = 0;
    }
    if (this.fault === 'decoder' || this.fault === 'player') this.fault = '';
  }

  /** ★★ RUNG TWO: THE SOCKET. The Worker's own reconnect only fires on a CLOSE, and a half-open
   *  socket never closes — so this is the only thing that reopens one that went quiet. Same URL,
   *  same session id: the server sees the same listener come back. */
  private _reopenSocket() {
    if (this.fault === 'drop' || this.fault === 'freeze') this.fault = '';
    if (this.worker) { this.worker.postMessage({ type: 'url', url: this.url }); return; }
    const old = this.ws;
    this.closedByUs = true;
    if (old) { old.onclose = null; try { old.close(); } catch { /* already closed */ } }
    this.ws = null;
    this._openWs();
  }

  /** ★ FAULT INJECTION, for proving the self-heal (and for nothing else). From the console or a
   *  driver: window.__vibeAudio.debugFault(kind), where kind is
   *    'suspend'  — suspend the AudioContext behind the page's back;
   *    'decoder'  — the decoder stops emitting (cleared by a decoder rebuild);
   *    'player'   — the playout node stops draining (cleared by a new node);
   *    'drop'     — frames are counted and then discarded (cleared by a socket reopen);
   *    'freeze'   — the socket goes half-open: nothing is delivered (cleared by a reopen). */
  debugFault(kind: 'suspend' | 'decoder' | 'player' | 'drop' | 'freeze') {
    console.warn(`[audio] debugFault: injecting '${kind}'`);
    if (kind === 'suspend') { void this.ctx?.suspend(); return; }
    if (kind === 'player') { this.node?.port.postMessage({ fault: 'stall' }); this.fault = 'player'; return; }
    this.fault = kind;
    this.worker?.postMessage({ type: 'fault', kind });
  }

  /** ★ Everything a driver or a bug report needs to say WHERE the audio stopped. The Safari
   *  silence of 2026-09-14 ("a flash of audio then silence, sometimes a refresh does not fix
   *  it") lives somewhere between the worklet, the MediaStream destination and the <audio>
   *  element; the stall watchdog only sees the worklet. Read as window.__vibeAudio.debugState(). */
  debugState() {
    const now = performance.now();
    const st = this.streamDest?.stream;
    const tr = st ? st.getAudioTracks()[0] : null;
    const el = this.mediaEl;
    const om = this.omEl;
    let omBuffered = -1;
    try { if (om && om.buffered.length) omBuffered = om.buffered.end(om.buffered.length - 1) - om.currentTime; } catch { /* between states */ }
    return {
      path: this.worker ? 'worker' : (this.ws ? 'main' : 'none'),
      // Media playout (Safari): the element IS the output chain — no context below.
      mediaPlayout: !!om,
      mediaTime: om?.currentTime, mediaPaused: om?.paused, mediaReady: om?.readyState,
      mediaBufferedAheadS: omBuffered, mediaTargetS: this.omTarget, mediaStalls: this.omStalls,
      mediaTimelineRatio: this.omRatio,
      mediaRate: om?.playbackRate, mediaPackets: this.omSeq, mediaChannels: this.omCh,
      mediaManaged: this.omManaged,
      ctxState: this.ctx?.state, ctxTime: this.ctx?.currentTime, ctxRate: this.ctx?.sampleRate,
      baseLatency: this.ctx?.baseLatency,
      mediaStream: !!this.streamDest, streamActive: st?.active,
      trackState: tr?.readyState, trackMuted: tr?.muted, trackEnabled: tr?.enabled,
      elPaused: el?.paused, elReady: el?.readyState, elTime: el?.currentTime, elMuted: el?.muted,
      elVolume: el?.volume, elError: el?.error?.code,
      anchor: !!this.anchorEl, anchorPaused: this.anchorEl?.paused,
      audibleAgoMs: this.lastAudibleAt > 0 ? Math.round(now - this.lastAudibleAt) : -1,
      drainAgoMs: this.lastDrainAt > 0 ? Math.round(now - this.lastDrainAt) : -1,
      underruns: this.underruns, skips: this.skips, jitterMs: this.jitterMs,
      workerOpen: this.workerOpen, contextRebuilds: this.contextRebuilds,
      // ★ The self-heal: its counters, its verdict, and the last 20 repairs with their reasons.
      heal: { state: this.heal.state, repairs: this.heal.repairs, rx: this.rxFrames,
              fed: this.fedTotal, played: this.playedTotal, fault: this.fault, log: this.healLog },
      ctxAdvancedAgoMs: this.ctxAdvancedAt > 0 ? Math.round(now - this.ctxAdvancedAt) : -1,
      opusBroken: this.opusBroken, opusStuck: this.opusStuck, needsCodec: this.needsCodec,
      muted: this._muted, volume: this._volume, squelch: this.squelchActive,
      suspended: this.suspended,
    };
  }

  /** ★★★ THE ELEMENT FIRST, INSIDE THE GESTURE. On Safari the audio leaves through a media
   *  element fed by the context (Now Playing needs it). This used to AWAIT ctx.resume() and only
   *  then call play() — and Safari treats that await as the end of the user activation, so the
   *  play() was refused and the page sat silent until the NEXT tap did it synchronously. Stuart,
   *  2026-09-14: "often safari needs a second click to get the audio flowing… I have to click
   *  start audio and then tune or interact with the page". Now every play() is issued before any
   *  await, and the context resumes alongside. `interrupted` (Safari's third state) counts too. */
  async resume() {
    const plays: Promise<unknown>[] = [];
    if (this.mediaEl && this.mediaEl.paused) plays.push(this.mediaEl.play().catch(() => {}));
    if (this.omEl && this.omEl.paused) plays.push(this.omEl.play().catch(() => {}));
    const ctxP = (this.ctx && this.ctx.state !== 'running' && this.ctx.state !== 'closed')
      ? this.ctx.resume().catch(() => {}) : null;
    await Promise.all(plays);
    if (ctxP) await ctxP;
  }

  set volume(v: number) {
    this._volume = Math.max(0, Math.min(1, v));
    if (this.gain && !this._muted) this.gain.gain.value = this._volume;
    // ★ Media playout: the element owns the level (read-only on iOS, where the buttons do).
    if (this.omEl) { try { this.omEl.volume = this._volume; } catch { /* iOS */ } }
  }
  get volume() { return this._volume; }

  set muted(m: boolean) {
    this._muted = m;
    if (this.gain) this.gain.gain.value = m ? 0 : this._volume;
    if (this.omEl) this.omEl.muted = m;
  }
  get muted() { return this._muted; }

  close() {
    // ★ Stop the output watchdog with the output it watches — an interval that outlives its
    //   AudioContext is a timer firing against a dead node for the life of the page.
    if (this.stallWatch !== null) { clearInterval(this.stallWatch); this.stallWatch = null; }
    this.closedByUs = true;
    // ★★ THE WORKER HOLDS A SOCKET AND A RECONNECT TIMER OF ITS OWN. Closing only `this.ws` would
    //    leave it reconnecting to a receiver nobody is listening to, for the life of the page.
    this._stopWorker();
    this.ws?.close();
    this.ws = null;
    if (this.mediaEl) { this.mediaEl.pause(); this.mediaEl.srcObject = null; this.mediaEl = null; }
    this._mediaTeardown();
    this._stopAnchor();
    this.streamDest = null;
    if (this.sp) { this.sp.onaudioprocess = null; this.sp.disconnect(); this.sp = null; }
    this.ctx?.close();
    this.ctx = null;
    this.node = null;
    this.gain = null;
    this.ring = null;
    try { this.wasmDec?.free(); } catch {}
    this.wasmDec = null;
    this.wasmReady = false;
  }
}
