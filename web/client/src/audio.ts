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

  /** Does this browser decode Opus via WebCodecs? Awaited before we ask the server for Opus, so a
   *  browser that can't (Safari <=16.3, Firefox <130, ancient WebViews) transparently gets PCM. */
  static async supportsOpus(): Promise<boolean> {
    try {
      if (typeof AudioDecoder === 'undefined') return false;
      const s = await AudioDecoder.isConfigSupported({ codec: 'opus', sampleRate: 48000, numberOfChannels: 2 });
      return !!s.supported;
    } catch { return false; }
  }

  private _ensureOpus(ch: number) {
    if (this.opusDec && this.opusCh === ch) return;
    if (this.opusDec) { try { this.opusDec.close(); } catch {} }
    this.opusDec = new AudioDecoder({
      output: (d) => this._onOpusData(d),
      // A decode error must not kill audio silently — log it; the stream self-heals on the next
      // key packet (every Opus packet is independently decodable).
      error: (e) => console.warn('[audio] opus decode error', e),
    });
    this.opusDec.configure({ codec: 'opus', sampleRate: 48000, numberOfChannels: ch });
    this.opusCh = ch;
    this.opusTs = 0;
  }

  private _decodeOpus(buf: ArrayBuffer, channels: number) {
    this._ensureOpus(channels || 1);
    // Copy the packet out of the frame (offset 6). Each Opus packet is a self-contained 20 ms frame.
    const data = buf.slice(6);
    try {
      this.opusDec!.decode(new EncodedAudioChunk({
        type: 'key', timestamp: this.opusTs, duration: 20000, data,
      }));
    } catch (e) { console.warn('[audio] opus enqueue failed', e); }
    this.opusTs += 20000;   // 20 ms in microseconds
  }

  private _onOpusData(ad: AudioData) {
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
    this._stopAnchor();
    this.streamDest = null;
    if (this.sp) { this.sp.onaudioprocess = null; this.sp.disconnect(); this.sp = null; }
    this.ctx?.close();
    this.ctx = null;
    this.node = null;
    this.gain = null;
    this.ring = null;
  }
}
