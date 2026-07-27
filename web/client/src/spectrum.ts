/**
 * spectrum.ts — VibeServer spectrum WebSocket client (browser).
 *
 * Browser port of src/services/UberSDRClient.ts, trimmed to what the shim
 * actually speaks. Shim reference: android/app/src/main/cpp/local_sdr_shim.cpp
 *
 *   /ws/user-spectrum
 *     <- text  {"type":"config"|"hwinfo"|"rds"|"pong", ...}
 *     <- bin   'SPEC' frames (22-byte header + binCount u8 bins)
 *     -> text  {"type":"zoom"|"tune"|"mode"|"bandwidth"|"gain"|... }
 *
 * Two things here are load-bearing and easy to get wrong:
 *   1. SPEC bins arrive in FFT order (DC first) — they MUST be rotated by
 *      binCount/2 to draw left-to-right.
 *   2. Zoom/pan sends are coalesced. Every view request triggers a config echo
 *      from the server; an un-throttled drag fires at 60–120 Hz and floods the
 *      link. Keep _sendView.
 */

export type SDRMode = 'usb' | 'lsb' | 'am' | 'sam' | 'fm' | 'nfm' | 'cwu' | 'cwl' | 'wfm';

/** The server applies these on every mode change and never reports bandwidth
 *  back, so the client mirrors the table to stay in sync. */
export const MODE_BANDWIDTHS: Record<SDRMode, [number, number]> = {
  usb: [50, 2700],     lsb: [-2700, -50],
  am:  [-5000, 5000],  sam: [-5000, 5000],
  cwu: [-200, 200],    cwl: [-200, 200],
  fm:  [-6000, 6000],  nfm: [-5000, 5000],
  wfm: [-100000, 100000],
};

const SPEC_MAGIC    = 0x43455053; // 'SPEC' little-endian
const FLAG_FULL_U8  = 0x03;
const U8_DBFS_OFFSET = -256;      // dBFS = u8 - 256

const VIEW_SEND_MS   = 33;
const VIEW_SETTLE_MS = 300;
// Gain is a USB CONTROL TRANSFER on the same bus as the IQ stream, so it is far
// more expensive than a view change — throttle it much harder. See setHwGain().
const GAIN_SEND_MS   = 120;
const MIN_SPAN_HZ    = 6_000;     // max-zoom floor; deeper looks frozen/artefacted

export interface Config {
  centerFreq: number;
  binCount: number;
  binBandwidth: number;
  totalBandwidth: number;
  maxBandwidth: number;
  /** The server's own demodulator, if it reported one — authoritative for a fresh client's mode. */
  serverMode?: string;
}

export interface RdsMeta {
  stereo: boolean;
  ps: string;
  radiotext: string;
  pi: number;
  ecc: number;
  /** Block error rate %, -1 = the decoder has not seen a full window yet. */
  ber: number;
  /** Recovered 57 kHz level relative to the pilot, dB (-99 = nothing). */
  sig: number;
}

/** The fields the normal RDS path discards, plus the constellation. */
export interface RdsExt {
  pty: number; tp: number; ta: number; ms: number; di: number;
  // ★ The same five UNCONFIRMED — what is arriving right now, whether or not it passed
  // confirmation. Always sent; the RAW/CONFIRMED choice is entirely the client's.
  ptyRaw: number; tpRaw: number; taRaw: number; msRaw: number; diRaw: number;
  ct: number;            // minutes since UTC midnight, -1 = none
  ctoff: number;         // local offset, half-hours
  af: number[];          // kHz, CONFIRMED only
  afseen: number;        // distinct frequencies glimpsed, confirmed or not
  grp: number[];         // 32 counters, gtype*2+version
  gtot: number;
  rtpTitle: string;      // RT+ tagged title (ODA 4BD7)
  rtpArtist: string;     // RT+ tagged artist
  longPs: string;        // 32-character Long PS (group 15A)
  ptyn: string;          // programme type NAME (10A) — the station's own words
  lang: number;          // RDS language code (1A variant 3)
  pinDay: number; pinHour: number; pinMin: number;
  eon: { pi: string; ps: string; af: number; ta: number }[];
  oda: { aid: string; grp: number }[];
  phase: number;         // RDS-to-pilot phase, degrees (-1 = no lock)
  phaseCoh: number;
  /** deg/s the RDS-to-pilot phase is turning. >0 means the station's encoder is not locked
   *  to its own pilot — a transmitter fault, not a reception one. */
  phaseDrift: number;      // 0..1 — how steady that phase is; below ~0.35 it is meaningless
  pilotDev: number;      // pilot injection, kHz deviation (spec 6.0–7.5)
  rdsDev: number;        // RDS injection, kHz deviation (typical 2–4, max 5.6)
  xy: number[];          // interleaved x,y as signed bytes (x100)
  mpx: number[];         // MPX spectrum, dB per bin, DC..100 kHz
}

/** What the RUNNING receiver can actually do. A dongle and an RSP are different radios with
 *  different controls, and a client that assumes one will misrepresent the other. */
export interface RadioCaps {
  driver: 'rtl' | 'sdrplay' | 'airspyhf' | string;
  model?: string;
  lnaStates?: number;
  ifGrMin?: number;
  ifGrMax?: number;
  rfNotch?: boolean;
  dabNotch?: boolean;
  biasT?: boolean;
  // ── Airspy HF+ ────────────────────────────────────────────────────────────
  attSteps?: number;      // count of attenuator positions (9 = 0..8)
  attStepDb?: number;     // dB per step (6)
  hfLna?: boolean;        // has the +6 dB preamp
  hfAgc?: boolean;        // has its own AGC
  agcThreshold?: boolean; // ...with a low/high threshold
  calPpb?: boolean;       // calibration is in parts per BILLION, not ppm
  rates?: number[];       // the rates THIS radio offers, ascending
  /** ★ Tunable windows, Hz. An HF+ Discovery has a REAL GAP at 31-60 MHz — not a weak spot,
   *  absent — so a client that does not know will let a user park on a dead frequency. */
  ranges?: [number, number][];
}

export interface SpectrumCallbacks {
  onBins?:   (bins: Float32Array, centerHz: number, bwHz: number) => void;
  onConfig?: (cfg: Config) => void;
  /** The server's radio was unplugged (false) or came back (true). */
  onDevice?: (present: boolean) => void;
  /** The person at the server is looking for this window. */
  onSummon?: () => void;
  onHwInfo?: (gains: number[], rates: number[], lockedRate: number, maxFftRate: number,
              forceIdleSaver?: boolean, radio?: RadioCaps | null) => void;
  onRds?:    (meta: RdsMeta) => void;
  /** Advanced RDS payload — only sent while the RDS decoder is attached. */
  onRdsX?:   (x: RdsExt) => void;
  /** Live RSP gain state — the AGC moves the IF reduction, so slider positions are not it. */
  onRspStat?: (systemGainDb: number, lna: number, ifgr: number, overload: boolean,
               settling: boolean) => void;
  onStatus?: (s: 'connecting' | 'open' | 'closed' | 'error', detail?: string) => void;
  /** The server is already serving someone else — do not retry. */
  onBusy?: () => void;
  onRtt?:    (ms: number) => void;
  /** Bytes received on the spectrum socket — the BIGGER half of the link. */
  onBytes?:  (n: number) => void;
}

export class SpectrumClient {
  private ws: WebSocket | null = null;
  private url: string;
  private cb: SpectrumCallbacks;
  private closedByUs = false;

  // Server-reported geometry.
  cfg: Config = {
    centerFreq: 0, binCount: 4096, binBandwidth: 0,
    totalBandwidth: 0, maxBandwidth: 0,
  };

  // VFO state (client-owned; the server never reports it back).
  frequency = 0;
  mode: SDRMode = 'nfm';
  bandwidthLow = -5000;
  bandwidthHigh = 5000;

  /** Locked = the view follows the VFO. Unlocked = pan freely. */
  followVfo = true;

  // Predicted view (what we've asked for), vs cfg (what the server acked).
  private view = { centerHz: 0, binBandwidth: 0 };
  private pendingView: { frequency: number; binBandwidth: number } | null = null;
  private sendTimer: number | null = null;
  private lastSendAt = 0;
  private pingTimer: number | null = null;
  private lastPingAt = 0;
  private pendingGain: { tenthDb: number; auto: boolean } | null = null;
  private gainTimer: number | null = null;
  private lastGainAt = 0;
  private reconnectTimer: number | null = null;
  /** Set when the server told us it is busy — suppresses the auto-reconnect. */
  private refused = false;

  // Scratch buffers, resized on bin-count change.
  private bins: Float32Array | null = null;

  constructor(url: string, cb: SpectrumCallbacks) {
    this.url = url;
    this.cb = cb;
  }

  connect() {
    this.closedByUs = false;
    this.cb.onStatus?.('connecting');
    const ws = new WebSocket(this.url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;

    ws.onopen = () => {
      this.cb.onStatus?.('open');
      // Ask for the view we want (server echoes a config back).
      if (this.view.centerHz && this.view.binBandwidth) {
        this._flushView();
      }
      this.pingTimer = window.setInterval(() => {
        this.lastPingAt = performance.now();
        this._send({ type: 'ping' });
      }, 5000);
    };

    ws.onmessage = (e) => {
      if (typeof e.data === 'string') {
        this.cb.onBytes?.(e.data.length);
        this._handleText(e.data);
      } else {
        const buf = e.data as ArrayBuffer;
        this.cb.onBytes?.(buf.byteLength);
        this._handleBinary(buf);
      }
    };

    ws.onerror = () => this.cb.onStatus?.('error', 'websocket error');

    ws.onclose = (e) => {
      this._stopTimers();
      this.cb.onStatus?.('closed', e.code === 1006 ? 'connection lost' : `closed (${e.code})`);
      if (!this.closedByUs && !this.refused) {
        this.reconnectTimer = window.setTimeout(() => this.connect(), 3000);
      }
    };
  }

  close() {
    this.closedByUs = true;
    this._stopTimers();
    if (this.reconnectTimer) { clearTimeout(this.reconnectTimer); this.reconnectTimer = null; }
    this.ws?.close();
    this.ws = null;
  }

  private _stopTimers() {
    if (this.pingTimer) { clearInterval(this.pingTimer); this.pingTimer = null; }
    if (this.sendTimer) { clearTimeout(this.sendTimer); this.sendTimer = null; }
  }

  /** Send an arbitrary control message. Used for radio-specific controls (RSP gain, notches)
   *  that have no place in the shared tune/gain vocabulary. */
  send(obj: Record<string, unknown>) { this._send(obj); }

  private _send(obj: Record<string, unknown>) {
    const ws = this.ws;
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
  }

  // ── Inbound ────────────────────────────────────────────────────────────────

  private _handleText(raw: string) {
    let msg: any;
    try { msg = JSON.parse(raw); } catch { return; }
    switch (msg.type) {
      case 'config': {
        const cfg: Config = {
          centerFreq:     msg.centerFreq ?? this.cfg.centerFreq,
          binCount:       msg.binCount ?? this.cfg.binCount,
          binBandwidth:   msg.binBandwidth ?? this.cfg.binBandwidth,
          totalBandwidth: msg.totalBandwidth ?? this.cfg.totalBandwidth,
          maxBandwidth:   msg.maxBandwidth ?? this.cfg.maxBandwidth,
          serverMode:     typeof msg.mode === 'string' ? msg.mode : this.cfg.serverMode,
        };
        this.cfg = cfg;
        // ★ Never let the view be WIDER than the capture. On a sample-rate DROP the device span
        // shrinks, but our old (wider) view span would persist and over-spread the axis — signals
        // bunch toward the VFO and the ticker reads a span the dongle can't actually see (Stuart
        // 2026-07-24). Clamp the held span to the new maxBandwidth and re-request so the server's
        // crop matches. Runs even mid-gesture, because an over-wide view is never valid.
        if (cfg.maxBandwidth > 0 && cfg.binCount > 0) {
          const widestBinBw = cfg.maxBandwidth / cfg.binCount;   // binBw for the full-capture span
          if (this.view.binBandwidth > widestBinBw * 1.001) {
            this.view.binBandwidth = widestBinBw;
            if (this.view.centerHz) this.zoom(this.view.centerHz, this.view.binBandwidth);
          }
        }
        // Adopt the server's view once our own sends have settled. While a
        // gesture is in flight our predicted view wins, otherwise the config
        // echoes fight the drag.
        if (!this._viewInFlight()) {
          this.view.centerHz     = cfg.centerFreq;
          this.view.binBandwidth = cfg.binBandwidth;
        }
        this.cb.onConfig?.(cfg);
        break;
      }
      case 'busy':
        // Someone else has the radio. The server refuses one occupant at a time and closes us
        // right after this frame — so DON'T reconnect (the default 3s retry would hammer a busy
        // server forever, which is the OWRX-style bad-neighbour behaviour we avoid elsewhere).
        this.refused = true;
        this.cb.onBusy?.();
        break;
      case 'device':
        // The server has lost (or regained) its radio. Without this the page just stops updating
        // and looks broken — a black waterfall with working controls, which tells the user nothing.
        this.cb.onDevice?.(msg.present !== false);
        break;
      case 'summon':
        // The host machine is looking for this tab. Handled by main.ts (flash + focus attempt).
        this.cb.onSummon?.();
        break;
      case 'hwinfo':
        this.cb.onHwInfo?.(msg.gains ?? [], msg.rates ?? [], Number(msg.lockedRate) || 0,
                           Number(msg.maxFftRate) || 0, Number(msg.forceIdleSaver) === 1,
                           (msg.radio ?? null) as RadioCaps | null);
        break;
      case 'rds':
        this.cb.onRds?.({
          stereo: !!msg.stereo, ps: msg.ps ?? '', radiotext: msg.radiotext ?? '',
          pi: msg.pi ?? -1, ecc: msg.ecc ?? 0,
          ber: typeof msg.ber === 'number' ? msg.ber : -1,
          sig: typeof msg.sig === 'number' ? msg.sig : -99,
        });
        break;
      case 'rdsx':
        this.cb.onRdsX?.({
          pty: Number(msg.pty ?? -1), tp: Number(msg.tp ?? -1),
          ta: Number(msg.ta ?? -1), ms: Number(msg.ms ?? -1),
          di: Number(msg.di ?? -1),
          ptyRaw: Number(msg.ptyRaw ?? -1), tpRaw: Number(msg.tpRaw ?? -1),
          taRaw: Number(msg.taRaw ?? -1), msRaw: Number(msg.msRaw ?? -1),
          diRaw: Number(msg.diRaw ?? -1),
          ct: Number(msg.ct ?? -1), ctoff: Number(msg.ctoff ?? 0),
          af: Array.isArray(msg.af) ? msg.af : [],
          afseen: Number(msg.afseen ?? 0),
          grp: Array.isArray(msg.grp) ? msg.grp : [],
          gtot: Number(msg.gtot ?? 0),
          rtpTitle: String(msg.rtpTitle ?? ''),
          rtpArtist: String(msg.rtpArtist ?? ''),
          longPs: String(msg.longPs ?? ''),
          ptyn: String(msg.ptyn ?? ''),
          lang: Number(msg.lang ?? 0),
          pinDay: Number(msg.pinDay ?? 0),
          pinHour: Number(msg.pinHour ?? -1),
          pinMin: Number(msg.pinMin ?? 0),
          eon: Array.isArray(msg.eon) ? msg.eon : [],
          oda: Array.isArray(msg.oda) ? msg.oda : [],
          phase: Number(msg.phase ?? -1),
          phaseCoh: Number(msg.phaseCoh ?? 0),
          phaseDrift: Number(msg.phaseDrift ?? 0),
          pilotDev: Number(msg.pilotDev ?? 0),
          rdsDev: Number(msg.rdsDev ?? 0),
          xy: Array.isArray(msg.xy) ? msg.xy : [],
          mpx: Array.isArray(msg.mpx) ? msg.mpx : [],
        });
        break;
      case 'rspstat':
        this.cb.onRspStat?.(Number(msg.sysGain) || 0, Number(msg.lna) || 0, Number(msg.ifgr) || 0,
                            Number(msg.overload) === 1, Number(msg.settling) === 1);
        break;
      case 'pong':
        if (this.lastPingAt) this.cb.onRtt?.(performance.now() - this.lastPingAt);
        break;
    }
  }

  private _handleBinary(buf: ArrayBuffer) {
    if (buf.byteLength < 22) return;
    const dv = new DataView(buf);
    if (dv.getUint32(0, true) !== SPEC_MAGIC) return;
    const flags = dv.getUint8(5);
    if (flags !== FLAG_FULL_U8) return; // shim only ever emits FULL_UINT8

    const centerHz = Number(dv.getBigUint64(14, true));
    const n = buf.byteLength - 22;
    if (n <= 0) return;

    if (!this.bins || this.bins.length !== n) this.bins = new Float32Array(n);
    const bins = this.bins;
    const u8 = new Uint8Array(buf, 22, n);

    // Bins arrive in FFT order (DC first, then +f, then -f). Rotate by n/2 so
    // the array runs low→high frequency for the renderer.
    const half = n >> 1;
    for (let i = 0; i < n; i++) {
      const src = (i + half) % n;
      bins[i] = u8[src] + U8_DBFS_OFFSET;
    }

    const bwHz = this.cfg.binBandwidth * n;
    this.cb.onBins?.(bins, centerHz, bwHz);
  }

  // ── Outbound ───────────────────────────────────────────────────────────────

  /**
   * A sensible display span for a mode: roughly ten times the demod bandwidth.
   * Wide enough to see the signal in context, narrow enough that it isn't a
   * speck. (Same rule we settled on for the watch.)
   */
  private defaultSpanHz(mode: SDRMode): number {
    const [lo, hi] = MODE_BANDWIDTHS[mode];
    const bw = Math.abs(hi - lo);
    const span = bw * 10;
    const cap = this.cfg.maxBandwidth || span;
    return Math.max(MIN_SPAN_HZ, Math.min(span, cap));
  }

  /**
   * Tune the VFO. Recentres the view when locked (or when forced).
   *
   * `retarget` — for a DISCRETE JUMP (frequency entry, bookmark, search, spot):
   * also reset the span to something sensible for the mode. Carrying the old span
   * across a jump is how you end up tuning from 648 kHz AM (zoomed right in) to
   * 96.6 MHz FM and finding a single station filling a 250 kHz window. A jump to
   * a different band should not inherit the zoom of the one you left.
   */
  tune(frequency: number, mode?: SDRMode, opts?: { recenter?: boolean; retarget?: boolean }) {
    if (frequency) this.frequency = Math.round(frequency);
    if (mode) this.setMode(mode);
    else this._send({
      type: 'tune', frequency: this.frequency, mode: this.mode,
      bandwidthLow: this.bandwidthLow, bandwidthHigh: this.bandwidthHigh,
    });
    if (this.followVfo || opts?.recenter) {
      const n = this.cfg.binCount || 4096;
      let bb: number;
      if (opts?.retarget) {
        // A discrete jump. DON'T slam to the tight per-mode default — that lands a broadcast in a
        // ~20 kHz window that looks blocky/low-res (Stuart 2026-07-24). Instead: keep your current
        // zoom if it's already sensible, and only zoom PART way in from a wide view — to a
        // comfortable "landing" span, never all the way to the tight default. Clamp the preserved
        // zoom to [default, landing] so a jump across bands can't carry an absurdly tight window.
        const cur = (this.view.binBandwidth || this.cfg.binBandwidth) * n;
        const dflt = this.defaultSpanHz(this.mode);
        const landing = Math.min(this.cfg.maxBandwidth || Infinity, Math.max(dflt * 8, 200_000));
        const span = Math.max(dflt, Math.min(cur || landing, landing));
        bb = span / n;
      } else {
        bb = this.view.binBandwidth || this.cfg.binBandwidth;
      }
      if (bb) this.zoom(this.frequency, bb);
    }
  }

  setMode(mode: SDRMode) {
    this.mode = mode;
    const bw = MODE_BANDWIDTHS[mode];
    if (bw) { this.bandwidthLow = bw[0]; this.bandwidthHigh = bw[1]; }
    // The shim IGNORES bandwidth fields on a tune that also changes mode
    // (local_sdr_shim.cpp:1529) — so send mode, then bandwidth separately.
    this._send({ type: 'tune', frequency: this.frequency, mode });
    this._send({ type: 'bandwidth', bandwidthLow: this.bandwidthLow, bandwidthHigh: this.bandwidthHigh });
  }

  setBandwidth(low: number, high: number) {
    this.bandwidthLow = low;
    this.bandwidthHigh = high;
    this._send({ type: 'bandwidth', bandwidthLow: low, bandwidthHigh: high });
  }

  /** Current display span (Hz). */
  spanHz(): number {
    const n = this.cfg.binCount || 4096;
    return (this.view.binBandwidth || this.cfg.binBandwidth) * n;
  }

  viewCenterHz(): number { return this.view.centerHz || this.cfg.centerFreq; }

  // ── Capture geometry (mirrors of the shim's own maths) ──────────────────────
  // No protocol field carries these — the client reproduces the server's
  // arithmetic. Ported from UberSDRClient (captureBandwidth/localMargin/
  // rfCenterHz/panSpan), which mirrors the shim's viewDongleMargin/dongleForView.

  /** Real captured bandwidth (Hz) — the shim reports it as config.maxBandwidth. */
  captureBandwidth(): number { return this.cfg.maxBandwidth || 0; }

  /** Margin keeping the VFO inside the usable capture: above the 50 kHz
   *  auto-retune threshold AND clear of the RTL anti-alias rolloff (~10%). */
  private localMargin(fs: number): number { return Math.max(fs * 0.10, 60_000); }

  /** The RF (dongle) centre the shim is parked at: it follows the view but is
   *  clamped so the VFO stays captured, then locks. This is the "second VFO"
   *  marker — where the hardware actually is, vs where you're looking. */
  rfCenterHz(): number {
    const fs = this.captureBandwidth();
    if (!fs) return this.cfg.centerFreq;
    const lim = fs / 2 - this.localMargin(fs);
    const vfo = this.frequency;
    return Math.max(vfo - lim, Math.min(vfo + lim, this.viewCenterHz()));
  }

  /** How far the VIEW centre can roam before it hits the capture edge. */
  panSpan(): { loHz: number; hiHz: number } | null {
    const fs = this.captureBandwidth();
    if (!fs) return null;
    const reach = Math.max(0, fs - this.localMargin(fs) - this.spanHz() / 2);
    const vfo = this.frequency;
    return { loHz: vfo - reach, hiHz: vfo + reach };
  }

  /** Set view centre + span. binBandwidth is clamped to sane zoom limits. */
  zoom(frequency: number, binBandwidth: number) {
    const n = this.cfg.binCount || 4096;
    const spanCap = this.cfg.maxBandwidth > 0 ? this.cfg.maxBandwidth : 30e6;
    const bb = Math.max(MIN_SPAN_HZ / n, Math.min(binBandwidth, spanCap / n));
    this.view.centerHz = Math.round(frequency);
    this.view.binBandwidth = bb;
    this._sendView(this.view.centerHz, bb);
  }

  /**
   * Zoom by a factor about an ANCHOR frequency (>1 = zoom in). The anchor stays
   * pinned under the same screen position, so wheel-zooming homes in on whatever
   * you pointed at.
   *
   * Zooming about the VIEW CENTRE (the old behaviour) meant that when locked —
   * where the view centre IS the VFO — every zoom felt welded to the RF centre and
   * you couldn't zoom in on anything else.
   *
   * Omit the anchor to zoom about the VFO, which is what the +/- buttons want:
   * the thing you're listening to stays put and the span closes in around it.
   */
  zoomBy(factor: number, anchorHz?: number) {
    const bb = this.view.binBandwidth || this.cfg.binBandwidth;
    if (!bb) return;
    const n = this.cfg.binCount || 4096;

    // Clamp FIRST, so the anchor maths uses the span we'll actually get —
    // otherwise, at the zoom limit, the centre would keep sliding towards the
    // anchor while the span refused to change.
    const spanCap = this.cfg.maxBandwidth > 0 ? this.cfg.maxBandwidth : 30e6;
    const newBb = Math.max(MIN_SPAN_HZ / n, Math.min(bb / factor, spanCap / n));
    const actual = bb / newBb;               // the zoom we're really applying

    const anchor = anchorHz ?? this.frequency ?? this.viewCenterHz();
    const centre = this.viewCenterHz();
    const newCentre = anchor - (anchor - centre) / actual;

    this.zoom(newCentre, newBb);
  }

  pan(frequency: number) {
    this.zoom(frequency, this.view.binBandwidth || this.cfg.binBandwidth);
  }

  resetView() { this._send({ type: 'reset' }); }

  /** Frame decimation: server emits only every Nth frame. NB this saves BANDWIDTH
   *  ONLY — the server still computes every FFT. Use setFftRate() to save power. */
  setRateDivisor(n: number) { this._send({ type: 'set_rate', divisor: Math.max(1, Math.round(n)) }); }

  /** Live spectrum frame rate on the SERVER. Lowering it makes the serving phone
   *  skip the FFT work outright, so it saves real CPU and radio power — the point
   *  of the idle throttle. Audio is unaffected. */
  setFftRate(fps: number) { this._send({ type: 'fftRate', value: fps }); }

  // Hardware controls — the client drives the remote radio.

  /**
   * Set tuner gain — COALESCED, like the view sender, and for a harder reason.
   *
   * Every gain message becomes a synchronous USB control transfer to the dongle,
   * on the same bus that is carrying the bulk IQ stream. Dragging the slider fired
   * one per step (~10 in 200ms), and each one elbows the sample flow aside — which
   * is audible as breakup while you drag. Rate-limit to one per GAIN_SEND_MS, with
   * the trailing edge always delivered so the gain you release on is the gain the
   * radio actually ends up at.
   */
  setHwGain(tenthDb: number, auto: boolean) {
    this.pendingGain = { tenthDb, auto };
    const wait = this.lastGainAt + GAIN_SEND_MS - Date.now();
    if (wait <= 0) { this._flushGain(); return; }
    if (!this.gainTimer) {
      this.gainTimer = window.setTimeout(() => {
        this.gainTimer = null;
        this._flushGain();
      }, wait);
    }
  }

  private _flushGain() {
    const p = this.pendingGain;
    if (!p) return;
    this.pendingGain = null;
    this.lastGainAt = Date.now();
    this._send(p.auto ? { type: 'gain', auto: true } : { type: 'gain', value: Math.round(p.tenthDb) });
  }
  setHwBiasT(on: boolean)  { this._send({ type: 'biasT', on }); }
  setHwAgc(on: boolean)    { this._send({ type: 'agc', on }); }
  setHwPpm(ppm: number)    { this._send({ type: 'ppm', value: Math.round(ppm) }); }
  setHwSampleRate(r: number) { this._send({ type: 'sampleRate', value: Math.round(r) }); }
  setHwDirectSampling(v: 0 | 1 | 2) { this._send({ type: 'directSampling', value: v }); }

  // Audio DSP — runs server-side in the shim (the client stays a thin renderer).
  /** db <= -100 turns squelch off, matching the app's convention. */
  setSquelch(db: number) { this._send({ type: 'squelch', db }); }
  setNr(on: boolean, strength: number) { this._send({ type: 'nr', on, strength }); }
  setNotch(on: boolean) { this._send({ type: 'notch', on }); }
  /** tau in seconds: 0 = off, 50e-6 or 75e-6. */
  setDeemph(tau: number) { this._send({ type: 'deemph', tau }); }
  setStereo(on: boolean) { this._send({ type: 'stereo', on }); }

  // ── Coalesced view sender ──────────────────────────────────────────────────

  private _viewInFlight(): boolean {
    return Date.now() - this.lastSendAt < VIEW_SETTLE_MS;
  }

  /** Keep only the latest target; send ≤1 per VIEW_SEND_MS, trailing edge always
   *  delivered so the final position of a gesture lands. */
  private _sendView(frequency: number, binBandwidth: number) {
    this.pendingView = { frequency, binBandwidth };
    const wait = this.lastSendAt + VIEW_SEND_MS - Date.now();
    if (wait <= 0) { this._flushView(); return; }
    if (!this.sendTimer) {
      this.sendTimer = window.setTimeout(() => {
        this.sendTimer = null;
        this._flushView();
      }, wait);
    }
  }

  private _flushView() {
    const p = this.pendingView ?? {
      frequency: this.view.centerHz, binBandwidth: this.view.binBandwidth,
    };
    if (!p.frequency || !p.binBandwidth) return;
    this.pendingView = null;
    this.lastSendAt = Date.now();
    this._send({ type: 'zoom', frequency: p.frequency, binBandwidth: p.binBandwidth });
  }
}
