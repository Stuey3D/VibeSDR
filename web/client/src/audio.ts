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
    this.port.onmessage = (e) => {
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
      if (this.filled + n > this.cap) {   // overflow: drop oldest
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
    if (!this.started || this.filled < n) {
      // Underrun — output silence and re-arm the jitter buffer.
      if (this.started && this.filled < n) {
        this.started = false;
        // ★★ THE LINK HAS JUST PROVED THIS DEPTH TOO SHALLOW. Grow, unless we emptied the buffer
        //    ourselves on a retune — that re-arm is expected and says nothing about the network.
        if (this.armedByFlush) {
          this.armedByFlush = false;
        } else if (this.target < this.max) {
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
    for (let i = 0; i < n; i++) {
      const r = (this.r + i) % this.cap;
      for (let c = 0; c < out.length; c++) out[c][i] = this.buf[Math.min(c, 1)][r];
    }
    this.r = (this.r + n) % this.cap;
    this.filled -= n;
    return true;
  }
}
registerProcessor('vibe-sink', VibeSink);
`;

/** Wrap int16 PCM in a 44-byte canonical WAV header. */
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
    this.cb = cb;
  }

  /** Must be called from a user gesture — browsers block audio otherwise. */
  async start() {
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
        this.node.port.onmessage = (e: MessageEvent) => {
          const ms = (e.data as { jitterMs?: number })?.jitterMs;
          if (typeof ms === 'number') this.jitterMs = ms;
        };
        this.node.connect(this.gain);
        this._connectOutput();
      } else {
        this._startScriptProcessor();
      }
    }
    if (this.ctx.state === 'suspended') await this.ctx.resume();
    this._openWs();
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
  get element(): HTMLAudioElement | null { return this.mediaEl; }

  /** ★ Drop everything queued for playout. See the worklet's flush handler for why. Called on
   *  every retune, through SpectrumClient.tune(). */
  flush() {
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
        try {
          if (JSON.parse(e.data)?.type === 'needs_codec') {
            console.error('[audio] server requires Opus and refused this socket');
            this.needsCodec = true;
          }
        } catch { /* not ours */ }
        return;
      }
      if (!(e.data instanceof ArrayBuffer)) return;
      this.needsCodec = false;
      this.cb.onBytes?.(e.data.byteLength);
      this._handleFrame(e.data);
    };
  }

  private _handleFrame(buf: ArrayBuffer) {
    if (buf.byteLength < 6) return;
    const dv = new DataView(buf);
    const channels = dv.getUint8(0);
    const format = dv.getUint8(1);

    // format 3 = Opus (VibeServer compressed audio). Decoded ASYNCHRONOUSLY via WebCodecs — the
    // decoded PCM lands in _onOpusData → _playPcm, same tail as the sync paths below.
    if (format === 3) { this._decodeOpus(buf, channels); return; }

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
  private _playPcm(pcm: Int16Array, ch: number) {
    const frames = Math.floor(pcm.length / Math.max(1, ch));
    if (frames <= 0) return;

    if (this.rec) {
      // Always store stereo. A WFM stream silently drops from 2ch to 1ch when
      // the pilot unlocks, and a WAV header can't change channel count midway —
      // so duplicate mono rather than write a file that desyncs halfway through.
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
    if (peak > 0.002) this.lastAudibleAt = performance.now();
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
  }

  get recording(): boolean { return this.rec !== null; }

  /** Seconds recorded so far. */
  get recordedSeconds(): number {
    return this.rec ? (Date.now() - this.rec.startedAt) / 1000 : 0;
  }

  /** Stop and return a WAV blob (null if nothing was captured). */
  stopRecording(): Blob | null {
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
  get suspended(): boolean { return !!this.ctx && this.ctx.state === 'suspended'; }

  /** True while audio frames are actually arriving. */
  get streaming(): boolean { return this.ws?.readyState === WebSocket.OPEN; }

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
    if (!this.ctx) return 'no-stream';
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

  async resume() {
    if (this.ctx && this.ctx.state === 'suspended') await this.ctx.resume();
    if (this.mediaEl && this.mediaEl.paused) await this.mediaEl.play().catch(() => {});
  }

  set volume(v: number) {
    this._volume = Math.max(0, Math.min(1, v));
    if (this.gain && !this._muted) this.gain.gain.value = this._volume;
  }
  get volume() { return this._volume; }

  set muted(m: boolean) {
    this._muted = m;
    if (this.gain) this.gain.gain.value = m ? 0 : this._volume;
  }
  get muted() { return this._muted; }

  close() {
    this.closedByUs = true;
    this.ws?.close();
    this.ws = null;
    if (this.mediaEl) { this.mediaEl.pause(); this.mediaEl.srcObject = null; this.mediaEl = null; }
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
