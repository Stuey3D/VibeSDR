// KiwiAdapter — native KiwiSDR backend (v3b3). Two WebSockets to
// ws(s)://host:port/ws/kiwi/<ts>/{SND,W/F}. Mirrors the shipped OWRX approach:
// everything (control, waterfall, audio decode) lives in TS, and decoded PCM is
// pushed to the native player via pushExternalPcm so background audio works the
// same as OWRX. No native Kiwi engine, no Kiwi server-side extensions (deferred —
// VibeSDR has its own decoders).
//
// Protocol distilled from the reference KiwiSDR web client (openwebrx.js /
// audio.js / kiwi_util.js) + v3 brief §4–5:
//  - Control plane: space-separated `SET key=val …` text commands.
//  - SND frame:  [0..2]="SND" [3]=flags [4..7]=seq LE [8..9]=smeter u16 BE
//                payload @10 (mono): IMA-ADPCM (kiwi) if COMPRESSED else s16 BE.
//                dBm = smeter/10 − 127.
//  - W/F frame:  4-byte tag, u32[1]=x_bin, u32[2]=(zoom&0xffff)|(flags<<16),
//                u32[3]=seq, bins (u8) @16; ADPCM (kiwi) if COMPRESSED then drop
//                first 10. Relative level → frameSink auto-ranges.
//  - Server-side zoom 0..14: span = 30 MHz / 2^z, centred on cf (kHz).
//  - Keepalive: `SET keepalive` ~1 Hz on BOTH sockets or the server kicks us.

import type { SDRMode, SDRStatus } from './UberSDRClient';
import type {
  SDRBackend, BackendCallbacks, BackendCapabilities, BackendKind,
} from './SDRBackend';
import { NativeModules } from 'react-native';
import { ImaAdpcmDecoder, decodeKiwiWaterfallFrame } from './imaAdpcm';
import { getKiwiIdent, sanitizeIdent } from './kiwiIdent';

const Vibe = NativeModules.VibePowerModule as {
  startExternalAudio?: (rate: number, pauseMode?: string) => void;
  pushExternalPcm?: (b64: string, rate: number, channels: number) => void;
  stopExternalAudio?: () => void;
} | undefined;

// Native decoder sidecar (decodes the backend audio for the client decoders).
const VibeLocal = NativeModules.VibeLocalSDR as {
  feedDecoderPcm?: (b64: string, rate: number) => void;
  setDecoderFreq?: (hz: number) => void;
} | undefined;

// Present as a real browser. KiwiSDR classifies connections that jump straight
// to the WS with a non-browser User-Agent as "ext_api" (API) connections, which
// many receivers time-limit or refuse — looking like Safari + identifying as
// the stock web client avoids that restriction.
const KIWI_UA = 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1';

const KIWI_FULL_BW = 30_000_000;   // zoom 0 span (Hz) — Kiwi's nominal 0–30 MHz
const KIWI_MAX_ZOOM = 14;
const WF_BINS = 1024;              // Kiwi waterfall is a fixed 1024-bin row

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function bytesToBase64(b: Uint8Array): string {
  let out = '';
  for (let i = 0; i < b.length; i += 3) {
    const a0 = b[i], a1 = b[i + 1], a2 = b[i + 2];
    const e0 = a0 >> 2, e1 = ((a0 & 3) << 4) | (a1 >> 4);
    const e2 = i + 1 < b.length ? (((a1 & 15) << 2) | (a2 >> 6)) : 64;
    const e3 = i + 2 < b.length ? (a2 & 63) : 64;
    out += B64[e0] + B64[e1] + (e2 === 64 ? '=' : B64[e2]) + (e3 === 64 ? '=' : B64[e3]);
  }
  return out;
}

// SND flags (audio.js)
const SND_COMPRESSED    = 0x0010;
const SND_LITTLE_ENDIAN = 0x0080;
const SND_STEREO        = 0x0008;
// W/F flags (openwebrx.js `wf`)
const WF_COMPRESSED = 1;

/** Internal SDRMode → Kiwi wire mode + default passband (Hz). cwu/cwl ride cw. */
const KIWI_MODE: Record<SDRMode, { mod: string; lo: number; hi: number }> = {
  usb: { mod: 'usb',  lo:   300, hi:  2700 },
  lsb: { mod: 'lsb',  lo: -2700, hi:  -300 },
  am:  { mod: 'am',   lo: -4900, hi:  4900 },
  sam: { mod: 'sam',  lo: -4900, hi:  4900 },
  fm:  { mod: 'nbfm', lo: -6000, hi:  6000 },
  nfm: { mod: 'nbfm', lo: -6000, hi:  6000 },
  cwu: { mod: 'cw',   lo:   300, hi:   700 },
  cwl: { mod: 'cw',   lo:  -700, hi:  -300 },
  wfm: { mod: 'nbfm', lo: -6000, hi:  6000 }, // unused (Kiwi has no WFM) — local-only mode
};

const KIWI_CAPS: Omit<BackendCapabilities, 'freqRange'> = {
  profiles: false,
  serverSideZoom: true,
  smeter: 'header',
  zoomSteps: KIWI_MAX_ZOOM + 1,   // 0..14 → 15 discrete levels
  chat: false,
  serverNR: false,
  maxBandwidth: { default: 6000, am: 9800, sam: 9800, fm: 12000, nfm: 12000 },
};

export class KiwiAdapter implements SDRBackend {
  readonly kind: BackendKind = 'kiwi';
  readonly caps: BackendCapabilities = { ...KIWI_CAPS, freqRange: [0, KIWI_FULL_BW] };
  readonly uuid: string;

  private cb: BackendCallbacks;
  private wsBase: string;
  private password: string;
  private ts = Date.now();

  private sndWs: WebSocket | null = null;
  private wfWs: WebSocket | null = null;
  private keepalive: ReturnType<typeof setInterval> | null = null;

  // RX / tuning state
  private rxBw = KIWI_FULL_BW;           // MSG bandwidth (usually 30 MHz)
  private trueAudioRate = 12000;         // MSG sample_rate (fractional)
  private freq = 9_600_000;              // tuned Hz
  private mode: SDRMode = 'am';
  private bwLow = -4900;
  private bwHigh = 4900;

  // View (server-side zoom)
  private viewCenter = KIWI_FULL_BW / 2;
  private viewBw = KIWI_FULL_BW;
  private viewInit = false;
  /** Caller had no remembered tune, so the receiver's own `init.freq`/`init.mode` may be adopted. */
  private allowServerDefault = false;
  /** `cfg` can arrive more than once; the landing spot is a first-connect decision only. */
  private adoptedDefaults = false;
  private followVfo = true;               // VFO lock (true = view follows VFO)

  private audioStarted = false;
  private lastDecoderFreq = -1;          // last dial Hz pushed to the FT8 sidecar
  private audioDec = new ImaAdpcmDecoder('kiwi', -32768, 32767);
  private started = false;
  private sndReady = false;
  private wfReady = false;

  // Connection meter (0–3 bars) from audio-frame inter-arrival — flaky links
  // stall/space the SND frames out, which is exactly the stutter the user hears.
  private gapHist: number[] = [];
  private lastFrameAt = 0;
  private connectedAt = 0;
  private lastLink = -1;

  /** Name/callsign sent as `SET ident_user=`. Loaded from the saved global identity; falls back
   *  to a non-blank default so we never connect anonymously (a common blacklist trigger). The
   *  load is local and fast, and the ident isn't sent until the SND socket's auth (a network
   *  round-trip later), so it's always ready in time. */
  private ident = 'VibeSDR';

  constructor(baseUrl: string, uuid: string, callbacks: BackendCallbacks, password?: string) {
    this.uuid = uuid;
    this.cb = callbacks;
    this.password = password ?? '';
    this.wsBase = KiwiAdapter.toWsBase(baseUrl);
    getKiwiIdent().then((v) => { const s = sanitizeIdent(v); if (s) this.ident = s; }).catch(() => {});
  }

  /** http(s)/ws(s)://host:port[/…] → ws(s)://host:port (no trailing path). */
  static toWsBase(baseUrl: string): string {
    let u = baseUrl.trim().replace(/\/+$/, '');
    if (u.startsWith('https://'))      u = 'wss://' + u.slice(8);
    else if (u.startsWith('http://'))  u = 'ws://'  + u.slice(7);
    else if (!/^wss?:\/\//.test(u))    u = 'ws://'  + u;
    return u.replace(/\/ws(\/.*)?$/, '');
  }

  private url(stream: 'SND' | 'W/F'): string {
    return `${this.wsBase}/ws/kiwi/${this.ts}/${stream}`;
  }

  /** Receiver location from the Kiwi /status text endpoint (`gps=(lat, lon)`)
   *  → ITU region, for custom Kiwi hosts not carrying a directory longitude. */
  private async fetchReceiverLon(): Promise<void> {
    try {
      const http = this.wsBase.replace(/^wss:\/\//, 'https://').replace(/^ws:\/\//, 'http://');
      const r = await fetch(http + '/status', { signal: AbortSignal.timeout(8000) });
      if (!r.ok) return;
      const m = /gps=\(([-\d.]+),\s*([-\d.]+)\)/.exec(await r.text());
      if (m) {
        const lat = Number(m[1]); const lon = Number(m[2]);
        if (Number.isFinite(lon)) this.cb.onReceiverLon?.(lon);
        // Full receiver position → spot distances + FT8 map (on-device decoder).
        if (Number.isFinite(lat) && Number.isFinite(lon)) this.cb.onReceiverLoc?.(lat, lon);
      }
    } catch {}
  }

  // ── connect ────────────────────────────────────────────────────────────────
  connect(frequency?: number, mode?: SDRMode, opts?: { allowServerDefault?: boolean }): Promise<void> {
    this.fetchReceiverLon();
    // Only ever true on a first visit — a remembered tune wins, so the screen leaves it unset then.
    this.allowServerDefault = opts?.allowServerDefault === true;
    this.adoptedDefaults = false;
    if (frequency) this.freq = frequency;
    if (mode) { this.mode = mode; const p = KIWI_MODE[mode]; this.bwLow = p.lo; this.bwHigh = p.hi; }
    this.started = true;
    this.viewInit = false;
    this.wfOpened = false;
    this.connectedAt = Date.now();
    this.gapHist = []; this.lastFrameAt = 0; this.lastLink = -1;
    this.verMaj = null; this.verMin = null; this.serverInfoSent = false;
    this.everConnected = false; this.errorShown = false;

    return new Promise<void>((resolve, reject) => {
      let settled = false;
      const done = () => { if (!settled) { settled = true; resolve(); } };
      const fail = (e: any) => { if (!settled) { settled = true; reject(e); } };

      // Open the SND socket FIRST. The reference client only opens W/F *after*
      // the SND auth succeeds (kiwi.js: "repeat the auth for the second
      // websocket … we only get here if the first auth has worked"). Opening
      // both at once makes the Kiwi drop the SND connection after a few seconds.
      try {
        this.sndWs = new (WebSocket as any)(this.url('SND'), null, { headers: { 'User-Agent': KIWI_UA } }) as WebSocket;
      } catch (e) { fail(e); return; }
      this.sndWs.binaryType = 'arraybuffer';

      this.sndWs.onopen = () => {
        this.dbg('SND open');
        this.sndSend(`SET auth t=kiwi p=${this.password}`);
        // Ident goes EARLY, right after auth — a "require name/callsign" server checks it at
        // connect time, so sending it late (buried in the RX params) means the refusal has
        // already happened. See kiwiIdent / IdentModal.
        this.sndSend(`SET ident_user=${this.ident}`);
        this.sndSend('SERVER DE CLIENT openwebrx.js SND');
        // RX params (which START the audio stream) — a short tick lets the
        // server process auth first; also re-asserted on the audio_rate MSG.
        setTimeout(() => { if (this.started) this.sendRxParams(); }, 150);
      };
      this.sndWs.onmessage = (e) => {
        try {
          if (typeof e.data === 'string') this.onText(e.data, 'SND');
          else this.onBinaryFrame(new Uint8Array(e.data as ArrayBuffer), 'SND');
          this.openWf();
        } catch (err: any) { this.dbg('SND msg err: ' + (err?.message ?? err)); }
      };
      this.sndWs.onerror = () => { this.dbg('SND error'); fail(new Error('KiwiSDR SND socket error')); };
      this.sndWs.onclose = (ev: any) => { this.dbg('SND close code=' + ev?.code + ' reason=' + ev?.reason); this.onSocketDrop(ev?.reason ?? ''); fail(new Error('KiwiSDR SND closed')); };

      // Open W/F right away too (both share this.ts). The first-SND-MSG gate
      // (openWf in onmessage) is kept as a no-op fallback via the wfOpened guard.
      this.openWf();

      // Keepalive on BOTH sockets (Kiwi kicks idle clients).
      this.keepalive = setInterval(() => {
        this.sndSend('SET keepalive');
        this.wfSend('SET keepalive');
      }, 1000);

      // Resolve once audio params are away (we're effectively connected); guard
      // with a timeout so a silent server still rejects.
      const t = setTimeout(() => fail(new Error('KiwiSDR handshake timed out')), 12000);
      this._onConnected = () => { clearTimeout(t); this.cb.onConnect(); done(); };
    });
  }

  private _onConnected: (() => void) | null = null;
  private wfOpened = false;

  /** Open the W/F socket AFTER the SND auth (first SND MSG ⇒ auth processed),
   *  matching the reference client's two-step handshake. */
  private openWf(): void {
    if (this.wfOpened || !this.started) return;
    this.wfOpened = true;
    try { this.wfWs = new (WebSocket as any)(this.url('W/F'), null, { headers: { 'User-Agent': KIWI_UA } }) as WebSocket; }
    catch (e) { this.dbg('WF open failed: ' + e); return; }
    this.wfWs.binaryType = 'arraybuffer';
    this.wfWs.onopen = () => {
      this.dbg('WF open');
      this.wfSend(`SET auth t=kiwi p=${this.password}`);
      this.wfSend('SERVER DE CLIENT openwebrx.js W/F');
      this.wfSend('SET send_dB=1');
      this.wfSend('SET wf_comp=1');
      this.wfSend('SET wf_speed=4');
      this.wfSend('SET maxdb=-10 mindb=-110');
      this.sendZoom();              // initial full-span view
    };
    this.wfWs.onmessage = (e) => {
      try {
        if (typeof e.data === 'string') this.onText(e.data, 'W/F');
        else this.onBinaryFrame(new Uint8Array(e.data as ArrayBuffer), 'W/F');
      } catch (err: any) { this.dbg('WF msg err: ' + (err?.message ?? err)); }
    };
    this.wfWs.onerror = () => { this.dbg('WF error'); };
    this.wfWs.onclose = (ev: any) => { this.dbg('WF close code=' + ev?.code + ' reason=' + ev?.reason); this.onSocketDrop(ev?.reason ?? ''); };
  }

  // ── binary frame dispatch ───────────────────────────────────────────────
  // KiwiSDR sends EVERYTHING as binary WebSocket frames, each prefixed with a
  // 3-char ASCII tag ('MSG'/'SND'/'W/F'/'EXT'). MSG carries the text control
  // plane (audio_rate, sample_rate, badp, too_busy …) — it is NOT a text frame.
  private onBinaryFrame(buf: Uint8Array, stream: 'SND' | 'W/F'): void {
    if (buf.length < 3) return;
    const tag = String.fromCharCode(buf[0], buf[1], buf[2]);
    if (tag === 'MSG') {
      let s = '';
      for (let i = 0; i < buf.length; i++) s += String.fromCharCode(buf[i]);  // latin1
      this.onText(s, stream);
    } else if (tag === 'SND') {
      this.onSndBinary(buf);
    } else if (tag === 'W/F') {
      this.onWfBinary(buf);
    }
    // other tags (EXT/CLI) ignored
  }

  // ── text (MSG) ───────────────────────────────────────────────────────────
  private onText(data: string, stream: 'SND' | 'W/F'): void {
    this.dbg(stream + ' rx: ' + data.slice(0, 120));
    if (!data.startsWith('MSG')) return;            // CLI / other — ignore
    const body = data.slice(4);                     // skip "MSG "
    for (const tok of body.split(' ')) {
      const eq = tok.indexOf('=');
      if (eq < 0) continue;
      const key = tok.slice(0, eq), val = tok.slice(eq + 1);
      this.onMsg(key, val, stream);
    }
  }

  private onMsg(key: string, val: string, stream: 'SND' | 'W/F'): void {
    switch (key) {
      case 'audio_rate': {
        const r = parseInt(val, 10) || 12000;
        this.sndSend(`SET AR OK in=${r} out=44100`);
        // audio_rate is a server MSG → auth has been processed; (re)assert the
        // RX params here too so audio starts even if the 150 ms tick raced auth.
        if (stream === 'SND') this.sendRxParams();
        break;
      }
      case 'sample_rate': {
        const f = parseFloat(val);
        if (Number.isFinite(f) && f > 1000) this.trueAudioRate = f;
        break;
      }
      // Where this KiwiSDR's owner wants you to start. Kiwi ships its whole public config to the
      // browser as `cfg=<url-encoded JSON>`, and its OWN client reads the landing spot from it
      // (kiwi_get_init_settings(), web/openwebrx/openwebrx.js):
      //
      //     init_f    = ext_get_cfg_param('init.freq', init_f)     // kHz, Kiwi's fallback 7020
      //     init_mode = ext_get_cfg_param('init.mode', 'lsb')
      //
      // ★ init.freq IS IN kHz, not Hz — the Kiwi client works in kHz throughout. Reading it as Hz
      //   lands every connection near DC and reads as a tuning bug rather than a units bug.
      case 'cfg': {
        if (!this.allowServerDefault || this.adoptedDefaults) break;
        this.adoptedDefaults = true;
        let ini: { freq?: unknown; mode?: unknown } | undefined;
        try {
          ini = (JSON.parse(decodeURIComponent(val)) as { init?: typeof ini }).init;
        } catch { break; }
        if (!ini) break;
        let changed = false;
        if (typeof ini.freq === 'number' && ini.freq > 0) {
          const hz = Math.round(ini.freq * 1000);
          if (!this.rxBw || hz <= this.rxBw) { this.freq = hz; this.viewCenter = hz; changed = true; }
        }
        // Kiwi's bare "cw" carries no sideband and its UI treats it as CW-upper. Anything we don't
        // recognise is left alone rather than guessed at.
        if (typeof ini.mode === 'string') {
          const m = (ini.mode.toLowerCase() === 'cw' ? 'cwu' : ini.mode.toLowerCase()) as SDRMode;
          if (m in KIWI_MODE) {
            this.mode = m; this.bwLow = KIWI_MODE[m].lo; this.bwHigh = KIWI_MODE[m].hi; changed = true;
          }
        }
        // We already asserted the caller's tune at handshake, so the server must be told again —
        // and the screen, or the readout shows one frequency while the audio plays another.
        if (changed) { this.sendDemod(); this.cb.onStatus(this.getStatus()); }
        break;
      }
      case 'bandwidth': {
        const bw = parseFloat(val);
        if (Number.isFinite(bw) && bw > 1000) {
          this.rxBw = bw;
          (this.caps as any).freqRange = [0, bw];
          if (!this.viewInit) { this.viewCenter = bw / 2; this.viewBw = bw; }
        }
        break;
      }
      case 'wf_setup':
        if (!this.wfReady) { this.wfReady = true; this.sendZoom(); }
        break;
      case 'audio_adpcm_state': {
        const [idx, prev] = val.split(',').map(Number);
        if (Number.isFinite(idx) && Number.isFinite(prev)) this.audioDec.setState(idx, prev);
        break;
      }
      case 'too_busy':
        // too_busy=0 is a NORMAL status ('you are NOT too busy') that healthy
        // Kiwis broadcast — only a NON-ZERO value means the receiver is full.
        // (We were self-booting on too_busy=0 → false 'server full' on clear
        // servers.) On a real busy, mark not-started so the close doesn't also
        // fire the generic serverLost.
        if (val !== '0' && val !== '') {
          this.dbg('too_busy=' + val + ' → busy');
          this.started = false;
          this.cb.onServerBusy?.();
        }
        break;
      case 'badp':
        // 0 = sign-in OK. Non-zero = the server rejected the sign-in itself. It can't tell us
        // WHY on the wire, but it's always one of: the owner set a private listen PASSWORD we
        // don't have; the owner only allows their own WEB PAGE and blocks apps; or a slot/IP
        // limit. All are owner settings, not an app fault — say so in plain English (and NOT via
        // the UberSDR bypass-password box, which can't touch any of these; see SDRScreen.onError).
        // Guard on errorShown: badp arrives on BOTH the SND and W/F sockets, so without this it
        // fired onError (a native Alert) twice — two stacked alerts, the second lost when the
        // first navigates back. One refusal, one message.
        // badp=1 is a SPECIFIC signal: the sign-in was rejected because this receiver has a
        // listen PASSWORD set and we connected without one. (Guard on errorShown: badp arrives on
        // BOTH sockets, and a socket close may also fire — one refusal, one message.)
        if (val !== '0' && !this.errorShown) { this.errorShown = true; this.cb.onError('This KiwiSDR is password-protected — the owner requires a listen password, which VibeSDR doesn’t have. It’s a private receiver; try another KiwiSDR, or use UberSDR or OpenWebRX.'); }
        break;
      case 'version_maj': this.verMaj = val; this.emitServerInfo(); break;
      case 'version_min': this.verMin = val; this.emitServerInfo(); break;
      case 'redirect':
        this.dbg('redirect ' + val);   // proxy.kiwisdr.com hop — TODO follow if needed
        break;
    }
  }

  /** Fire onConnect/resolve once, on the first real frame from either socket. */
  // Server version from MSG version_maj/version_min (e.g. 1 + 900 → "1.900").
  private verMaj: string | null = null;
  private verMin: string | null = null;
  private serverInfoSent = false;
  private emitServerInfo(): void {
    if (this.serverInfoSent || this.verMaj == null || this.verMin == null) return;
    this.serverInfoSent = true;
    this.cb.onServerInfo?.({ name: 'KiwiSDR', version: `${this.verMaj}.${this.verMin}` });
  }

  /** True once we've received a real frame — i.e. the connection actually opened. Lets a socket
   *  close be read as a REFUSAL (never connected) vs a mid-session DROP (was streaming). */
  private everConnected = false;
  /** Set once we've already surfaced a refusal reason (e.g. badp), so a following socket close
   *  doesn't show a second, duplicate message. */
  private errorShown = false;

  private maybeConnected(): void {
    this.everConnected = true;
    if (this._onConnected) { const f = this._onConnected; this._onConnected = null; f(); }
  }

  /** The demod line — sent on EVERY tune/mode/bandwidth change. The Kiwi expects
   *  the FULL `SET mod=… low_cut=… high_cut=… freq=…` (reference doset()); a bare
   *  `SET freq=` is ignored, so tuning silently did nothing.
   *
   *  THROTTLED: the VFO drum fires a demod change per frequency step (dozens/sec);
   *  KiwiSDR has flood protection and KICKS clients that spam SET commands. The
   *  reference throttles via `demodulator_response_time`. We coalesce to ~1 every
   *  DEMOD_MIN_MS with a guaranteed trailing send of the final value. */
  private demodTimer: ReturnType<typeof setTimeout> | null = null;
  private demodPending = false;
  private lastDemodAt = 0;
  private static DEMOD_MIN_MS = 110;

  private sendDemod(): void {
    const since = Date.now() - this.lastDemodAt;
    if (since >= KiwiAdapter.DEMOD_MIN_MS) {
      this.lastDemodAt = Date.now();
      this.sendDemodNow();
    } else {
      this.demodPending = true;
      if (!this.demodTimer) {
        this.demodTimer = setTimeout(() => {
          this.demodTimer = null;
          if (this.demodPending) { this.demodPending = false; this.lastDemodAt = Date.now(); this.sendDemodNow(); }
        }, KiwiAdapter.DEMOD_MIN_MS - since);
      }
    }
  }

  private sendDemodNow(): void {
    const wire = KIWI_MODE[this.mode].mod;
    this.sndSend(`SET mod=${wire} low_cut=${Math.round(this.bwLow)} high_cut=${Math.round(this.bwHigh)} freq=${(this.freq / 1000).toFixed(3)}`);
  }

  /** Send the SND receive params once we know the true rate (per reference order). */
  private sendRxParams(): void {
    this.sendDemod();
    this.sndSend('SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50');
    this.sndSend('SET compression=1');
    // (ident_user is now sent EARLY, right after auth in the SND onopen — see there.)
    this.cb.onStatus(this.getStatus());
  }

  // ── audio (SND binary) ─────────────────────────────────────────────────────
  private onSndBinary(buf: Uint8Array): void {
    if (buf.length < 10 || buf[0] !== 0x53 /*S*/ || buf[1] !== 0x4e /*N*/ || buf[2] !== 0x44 /*D*/) return;
    const flags = buf[3];
    const smeter = (buf[8] << 8) | buf[9];                 // BE u16
    const dbm = smeter / 10 - 127;
    this.cb.onSMeter?.(dbm);
    // Track audio-frame inter-arrival → connection meter (stutters space frames out).
    const now = Date.now();
    if (this.lastFrameAt > 0) { this.gapHist.push(now - this.lastFrameAt); if (this.gapHist.length > 40) this.gapHist.shift(); }
    this.lastFrameAt = now;
    this.evalLink();
    this.maybeConnected();

    const offset = (flags & SND_STEREO) ? 20 : 10;
    const payload = buf.subarray(offset);
    if (!payload.length) return;

    let pcm: Int16Array;
    if (flags & SND_COMPRESSED) {
      pcm = this.audioDec.decode(payload);                 // persistent kiwi-flavour state
    } else {
      const little = !!(flags & SND_LITTLE_ENDIAN);
      const n = payload.length >> 1;
      pcm = new Int16Array(n);
      const dv = new DataView(payload.buffer, payload.byteOffset, n * 2);
      for (let i = 0; i < n; i++) pcm[i] = dv.getInt16(i * 2, little);   // network = BE by default
    }
    if (!pcm.length) return;

    const rate = Math.round(this.trueAudioRate);
    if (!this.audioStarted) { Vibe?.startExternalAudio?.(rate, 'reconnect'); this.audioStarted = true; }
    const b64 = bytesToBase64(new Uint8Array(pcm.buffer, pcm.byteOffset, pcm.byteLength));
    Vibe?.pushExternalPcm?.(b64, rate, 1);
    // Tell the sidecar the dial frequency (when it changes) so FT8 spots get the
    // right RF freq + band — otherwise they sit at the 100 MHz default (no band
    // colour, wrong tune-on-tap). Pushed here so the sidecar is already running.
    if (this.lastDecoderFreq !== this.freq) {
      this.lastDecoderFreq = this.freq;
      VibeLocal?.setDecoderFreq?.(this.freq);
    }
    // Also feed the native decoder sidecar (RTTY/WEFAX/SSTV/FT8 on Kiwi audio).
    // No-op natively unless the decoder service is running.
    VibeLocal?.feedDecoderPcm?.(b64, rate);
  }

  // ── waterfall (W/F binary) ─────────────────────────────────────────────────
  private onWfBinary(buf: Uint8Array): void {
    if (buf.length < 16) return;
    // bytes 0..3 = tag; u32[1..3] @ offset 4 = x_bin, zoom|flags, seq
    const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    const zoomFlags = dv.getUint32(8, true);
    const wfFlags = (zoomFlags >> 16) & 0xffff;
    let bins = buf.subarray(16);
    if (wfFlags & WF_COMPRESSED) bins = decodeKiwiWaterfallFrame(bins);
    const n = bins.length;
    if (n < 8) return;

    // u8 → dBm (bin − 255); relative level, the UI auto-ranges absolute scale.
    const row = new Float32Array(n);
    for (let i = 0; i < n; i++) row[i] = bins[i] - 255;

    if (!this.viewInit) { this.viewInit = true; }
    this.maybeConnected();
    this.cb.onSpectrum(row, this.statusForRow(n));
  }

  // ── view / zoom ────────────────────────────────────────────────────────────
  private zoomLevel(): number {
    const z = Math.round(Math.log2(KIWI_FULL_BW / Math.max(1, this.viewBw)));
    return Math.min(Math.max(z, 0), KIWI_MAX_ZOOM);
  }

  /** Snap viewBw to the quantised zoom span and push `SET zoom=z cf=<kHz>`.
   *  Throttled like the demod line — the zoom drum floods the W/F socket. */
  private zoomTimer: ReturnType<typeof setTimeout> | null = null;
  private zoomPending = false;
  private lastZoomAt = 0;

  private sendZoom(): void {
    const z = this.zoomLevel();
    this.viewBw = KIWI_FULL_BW / Math.pow(2, z);          // authoritative snap
    this.cb.onStatus(this.getStatus());                   // UI updates immediately
    const since = Date.now() - this.lastZoomAt;
    if (since >= KiwiAdapter.DEMOD_MIN_MS) {
      this.lastZoomAt = Date.now();
      this.sendZoomNow();
    } else {
      this.zoomPending = true;
      if (!this.zoomTimer) {
        this.zoomTimer = setTimeout(() => {
          this.zoomTimer = null;
          if (this.zoomPending) { this.zoomPending = false; this.lastZoomAt = Date.now(); this.sendZoomNow(); }
        }, KiwiAdapter.DEMOD_MIN_MS - since);
      }
    }
  }

  private sendZoomNow(): void {
    const z = this.zoomLevel();
    this.wfSend(`SET zoom=${z} cf=${(this.viewCenter / 1000).toFixed(3)}`);
  }

  // ── SDRBackend surface ───────────────────────────────────────────────────
  tune(frequency: number, mode?: SDRMode, opts?: { recenter?: boolean }): void {
    this.freq = Math.min(Math.max(frequency, 0), this.rxBw);
    if (mode && mode !== this.mode) { this.setMode(mode); return; }
    this.sendDemod();                     // FULL demod line — bare SET freq is ignored
    // Re-centre the waterfall on the VFO so it stays centred (like UberSDR's
    // server-side zoom). sendZoom() is throttled, so a drum spin won't flood.
    // Only when locked (followVfo) or a discrete jump forces it (opts.recenter).
    if (this.viewInit && (this.followVfo || opts?.recenter)) { this.viewCenter = this.freq; this.sendZoom(); }
    else this.cb.onStatus(this.getStatus());
  }

  setFollowMode(follow: boolean): void { this.followVfo = follow; }

  panSpan(): { loHz: number; hiHz: number; movable: boolean } {
    return { loHz: 0, hiHz: this.rxBw, movable: false };
  }

  syncFrequency(frequency: number, mode?: SDRMode): void {
    this.freq = Math.min(Math.max(frequency, 0), this.rxBw);
    if (mode) this.mode = mode;
    this.cb.onStatus(this.getStatus());
  }

  setMode(mode: SDRMode): void {
    this.mode = mode;
    const p = KIWI_MODE[mode];
    this.bwLow = p.lo; this.bwHigh = p.hi;
    this.sendDemod();
    this.cb.onStatus(this.getStatus());
  }

  setBandwidth(low: number, high: number): void {
    this.bwLow = low; this.bwHigh = high;
    this.sendDemod();
    this.cb.onStatus(this.getStatus());
  }

  // ── Noise filters / blanker (server-side DSP) ──────────────────────────────
  // Exposed as DSP filter descriptors so they reuse the same menu UI as the
  // UberSDR server DSP. Kiwi has 3 noise-filter algos + the noise blanker, each
  // with its own params; we map a selected filter+params to its SET nr/nb seq.
  // Param order in each descriptor == Kiwi's `param=` index.
  static readonly DSP_FILTERS = [
    { name: 'Spectral NR', params: [
      { name: 'gain',       type: 'float', min: '-30',    max: '30',  default: '0'    },
      { name: 'alpha',      type: 'float', min: '0.90',   max: '0.99', default: '0.95' },
      { name: 'active_snr', type: 'int',   min: '2',      max: '30',  default: '30'   },
    ] },
    { name: 'WDSP Denoise', params: [
      { name: 'taps',    type: 'int', min: '16', max: '128', default: '64' },
      { name: 'delay',   type: 'int', min: '2',  max: '128', default: '16' },
      { name: 'gain',    type: 'int', min: '1',  max: '20',  default: '10' },
      { name: 'leakage', type: 'int', min: '1',  max: '23',  default: '7'  },
    ] },
    { name: 'LMS Denoise', params: [
      { name: 'delay', type: 'int',   min: '1',      max: '200', default: '1'    },
      { name: 'beta',  type: 'float', min: '0.0001', max: '0.15', default: '0.05' },
      { name: 'decay', type: 'float', min: '0.90',   max: '1.0', default: '0.98' },
    ] },
    { name: 'Noise Blanker', params: [
      { name: 'gate',      type: 'int', min: '100', max: '5000', default: '100' },
      { name: 'threshold', type: 'int', min: '0',   max: '100',  default: '50'  },
    ] },
  ];
  private static readonly NR_ALGO: Record<string, number> = {
    'WDSP Denoise': 1, 'LMS Denoise': 2, 'Spectral NR': 3,
  };

  private dspEnabled = false;
  private dspFilter  = 'Spectral NR';
  private dspParams: Record<string, string> = {};

  /** Apply the selected noise filter / blanker (enabled + filter + params). */
  setDsp(enabled: boolean, filter: string, params: Record<string, string>): void {
    this.dspEnabled = enabled;
    this.dspFilter  = filter || this.dspFilter;
    this.dspParams  = { ...params };
    this.applyDsp();
  }
  setDspFilter(filter: string, params: Record<string, string>): void {
    this.dspFilter = filter;
    this.dspParams = { ...params };
    this.applyDsp();
  }
  setDspParams(params: Record<string, string>): void {
    this.dspParams = { ...params };
    this.applyDsp();
  }

  private applyDsp(): void {
    const desc = KiwiAdapter.DSP_FILTERS.find(f => f.name === this.dspFilter);
    const isNB = this.dspFilter === 'Noise Blanker';
    if (!this.dspEnabled || !desc) {
      // Off: disable both the noise filter and the blanker.
      this.sndSend('SET nr algo=0'); this.sndSend('SET nr type=0 en=0');
      this.sndSend('SET nb algo=0'); this.sndSend('SET nb type=0 en=0');
      return;
    }
    const pval = (name: string, def: string) => {
      const v = this.dspParams[name];
      return (v != null && v !== '') ? v : def;
    };
    if (isNB) {
      this.sndSend('SET nr algo=0'); this.sndSend('SET nr type=0 en=0');   // drop NR
      this.sndSend('SET nb algo=1');                                        // standard blanker
      desc.params.forEach((p, i) => this.sndSend(`SET nb type=0 param=${i} pval=${pval(p.name, p.default)}`));
      this.sndSend('SET nb type=0 en=1');
    } else {
      this.sndSend('SET nb algo=0'); this.sndSend('SET nb type=0 en=0');   // drop blanker
      this.sndSend(`SET nr algo=${KiwiAdapter.NR_ALGO[this.dspFilter] ?? 3}`);
      desc.params.forEach((p, i) => this.sndSend(`SET nr type=0 param=${i} pval=${pval(p.name, p.default)}`));
      this.sndSend('SET nr type=0 en=1');                                  // type 0 = denoiser
    }
  }

  /** Squelch 0–99 (0 = off). Kiwi gates the audio server-side. */
  setSquelch(level: number): void {
    const v = Math.max(0, Math.min(99, Math.round(level)));
    this.sndSend(`SET squelch=${v} param=0`);
  }

  zoom(frequency: number, binBandwidth: number): void {
    this.viewCenter = frequency;
    this.viewBw = Math.max(1, binBandwidth * WF_BINS);
    this.sendZoom();
  }

  pan(frequency: number): void {
    this.viewCenter = frequency;
    this.sendZoom();
  }

  resetView(): void {
    this.viewCenter = this.rxBw / 2;
    this.viewBw = this.rxBw;
    this.sendZoom();
  }

  setRate(_divisor: number): void { /* Kiwi waterfall speed is server-fixed (wf_speed) */ }
  pauseSpectrum(): void { this.wfSend('SET wf_speed=0'); }
  resumeSpectrum(): void { this.wfSend('SET wf_speed=4'); }

  getStatus(): SDRStatus {
    return this.statusForRow(this.viewInit ? WF_BINS : 0);
  }
  getView(): SDRStatus { return this.getStatus(); }

  private statusForRow(bins: number): SDRStatus {
    const bw = this.viewInit ? this.viewBw : this.rxBw;
    return {
      frequency: this.freq, mode: this.mode,
      bandwidthLow: this.bwLow, bandwidthHigh: this.bwHigh,
      binCount: bins, binBandwidth: bw / Math.max(1, bins || WF_BINS),
      centerHz: this.viewInit ? this.viewCenter : this.rxBw / 2,
      bwHz: bw,
    };
  }

  // ── teardown ────────────────────────────────────────────────────────────
  destroy(): void {
    this.started = false;
    this.stopKeepalive();
    if (this.audioStarted) { Vibe?.stopExternalAudio?.(); this.audioStarted = false; this.lastDecoderFreq = -1; }
    this.closeSocket('sndWs');
    this.closeSocket('wfWs');
  }

  /** Pause-disconnect: drop the sockets but keep the native audio session. */
  disconnectSocket(): void {
    this.started = false;
    this.stopKeepalive();
    this.closeSocket('sndWs');
    this.closeSocket('wfWs');
  }

  private closeSocket(which: 'sndWs' | 'wfWs'): void {
    const ws = this[which]; this[which] = null;
    if (ws) { try { ws.onclose = null; ws.onerror = null; ws.close(); } catch {} }
  }

  private stopKeepalive(): void {
    if (this.keepalive) { clearInterval(this.keepalive); this.keepalive = null; }
    if (this.demodTimer) { clearTimeout(this.demodTimer); this.demodTimer = null; }
    if (this.zoomTimer) { clearTimeout(this.zoomTimer); this.zoomTimer = null; }
  }

  /** Score the link 0–3 bars from audio-frame timing (median-relative, like OWRX). */
  private evalLink(): void {
    let q: 0 | 1 | 2 | 3;
    if (this.sndWs?.readyState !== WebSocket.OPEN) { q = 0; }
    else {
      const now = Date.now(), h = this.gapHist;
      let med = 150;
      if (h.length >= 5) { const s = [...h].sort((a, b) => a - b); med = s[s.length >> 1]; }
      let stalls = 0;
      for (let i = 0; i < h.length; i++) if (h[i] > med * 2.5 + 60) stalls++;
      const starving = this.lastFrameAt > 0 && now - this.lastFrameAt > Math.max(2000, med * 4);
      if (now - this.connectedAt < 4000 && h.length < 5) q = 2;
      else if (stalls >= 3 || starving) q = 1;
      else if (stalls >= 1) q = 2;
      else q = 3;
    }
    if (q !== this.lastLink) { this.lastLink = q; this.cb.onLink?.(q); }
  }

  private onSocketDrop(closeReason: string = ''): void {
    if (!this.started) return;        // our own close() / already torn down
    this.started = false;
    // Fully tear down: a half-open connection (one socket wobbled closed on flaky
    // cellular while the other kept streaming) was leaving audio playing after the
    // 'lost' card — and even after navigating back. Close BOTH + stop native audio.
    this.stopKeepalive();
    this.closeSocket('sndWs');
    this.closeSocket('wfWs');
    if (this.audioStarted) { Vibe?.stopExternalAudio?.(); this.audioStarted = false; this.lastDecoderFreq = -1; }
    this.cb.onLink?.(0);
    this.cb.onDisconnect();
    if (!this.everConnected) {
      // Closed BEFORE we ever received a frame = a REFUSAL, not a mid-session drop. Blocked Kiwis
      // usually just hang up with no `badp` MSG — so the ONLY thing we can report is the WebSocket
      // close reason, IF the server bothered to send one (most don't). Be honest either way, and
      // skip if badp already explained it.
      if (!this.errorShown) {
        this.errorShown = true;
        const reason = String(closeReason ?? '').trim();
        // A WebSocket-HANDSHAKE failure (invalid accept, redirect, 30x/401/403) means the server
        // didn't open a data socket at all — it bounced us to its own web page. That's the
        // "only allows its own web interface" block, and compatibility mode is exactly the fix,
        // so say so in plain English rather than dumping the raw protocol string on the user.
        const handshakeBlock = /sec-websocket|websocket|handshake|redirect|30[12]|\b40[13]\b|forbidden|moved/i.test(reason);
        this.cb.onError(
          handshakeBlock
            ? 'This KiwiSDR wouldn’t open a data connection for the app — it sent us to its own web page instead. Many owners only allow their own web interface. Use “Open in compatibility mode” below to listen via the receiver’s web page, or try another KiwiSDR.'
          : reason
            ? `This KiwiSDR closed the connection: “${reason}”. Try another KiwiSDR, or use UberSDR or OpenWebRX.`
            : 'This KiwiSDR closed the connection without giving a reason — most likely it only allows its own web page and blocks apps like VibeSDR. Try another KiwiSDR, or use UberSDR or OpenWebRX.');
      }
    } else {
      // Was streaming, then dropped — a genuine mid-session loss.
      this.cb.onServerLost?.();
    }
  }

  // ── helpers ────────────────────────────────────────────────────────────────
  private sndSend(s: string): void { try { if (this.sndWs?.readyState === WebSocket.OPEN) { if (s !== 'SET keepalive') this.dbg('SND tx: ' + s); this.sndWs.send(s); } } catch {} }
  private wfSend(s: string): void { try { if (this.wfWs?.readyState === WebSocket.OPEN) { if (s !== 'SET keepalive') this.dbg('WF tx: ' + s); this.wfWs.send(s); } } catch {} }
  private dbg(m: string): void { console.log('[kiwi] ' + m); this.cb.onDbg?.('[kiwi] ' + m); }
}
