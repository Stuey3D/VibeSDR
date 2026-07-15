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
    this.target = 48000 * 0.25;        // 250ms jitter buffer before playout
    this.port.onmessage = (e) => {
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
      if (!this.started && this.filled >= this.target) this.started = true;
    };
  }
  process(_inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    if (!this.started || this.filled < n) {
      // Underrun — output silence and re-arm the jitter buffer.
      if (this.started && this.filled < n) this.started = false;
      for (let c = 0; c < out.length; c++) out[c].fill(0);
      return true;
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

export class AudioPlayer {
  private ws: WebSocket | null = null;
  private ctx: AudioContext | null = null;
  private node: AudioWorkletNode | null = null;
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
  private ring: [Float32Array, Float32Array] | null = null;
  private cap = 48000 * 2;
  private wPos = 0;
  private rPos = 0;
  private filled = 0;
  private playing = false;
  private url: string;
  private cb: AudioCallbacks;
  private closedByUs = false;
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

      // Play OUT through a media element, so the OS sees real playback and gives
      // us Now Playing / lock-screen / media-key control. Falls back to the plain
      // destination if the browser won't take a MediaStream.
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
        if (this.playing) this.playing = false;
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
    if (!this.playing && this.filled >= 48000 * 0.25) this.playing = true;
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
      if (!(e.data instanceof ArrayBuffer)) return;
      this.cb.onBytes?.(e.data.byteLength);
      this._handleFrame(e.data);
    };
  }

  private _handleFrame(buf: ArrayBuffer) {
    if (buf.byteLength < 6) return;
    const dv = new DataView(buf);
    const channels = dv.getUint8(0);
    const format = dv.getUint8(1);

    let pcm: Int16Array;
    let ch: number;
    if (format === 0) {
      ch = channels;
      pcm = new Int16Array(buf, 6, (buf.byteLength - 6) >> 1);
    } else {
      const d = decodeVibeAdpcmFrame(buf);
      ch = d.channels;
      pcm = d.pcm;
    }

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
  get health(): 'ok' | 'suspended' | 'no-stream' | 'muted' | 'squelched' | 'silent' {
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
    this.streamDest = null;
    if (this.sp) { this.sp.onaudioprocess = null; this.sp.disconnect(); this.sp = null; }
    this.ctx?.close();
    this.ctx = null;
    this.node = null;
    this.gain = null;
    this.ring = null;
  }
}
