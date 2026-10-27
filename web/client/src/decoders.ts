/**
 * decoders.ts — /ws/dxcluster client (RTTY, NAVTEX, WEFAX, SSTV, FT8/FT4 spots).
 *
 * The decoders all run SERVER-SIDE in the shim — no WASM, no Emscripten, no DSP
 * in the browser. We attach, and it streams back decoded text / image lines /
 * spots. This is the same channel the old UberSDR skin used, so the framing is
 * long-established; the layouts below were read out of local_sdr_shim.cpp
 * (startDecoder :1770, startWefax :1794, startSstv :1844, emitSpot :1233).
 *
 * Wire formats (all binary except spots, which are JSON text):
 *
 *   FSK (RTTY / NAVTEX)
 *     0x01 | u64 BE timestamp | u32 BE length | UTF-8 text
 *     0x03 | u8 state
 *     0x07 | u32 BE clean | u32 BE repaired | u32 BE lost   NAVTEX: one message's FEC count, after its NNNN
 *
 *   WEFAX
 *     0x01 | u32 BE line | u32 BE width | width bytes greyscale
 *     0x02 = image start,  0x03 = image stop
 *
 *   SSTV
 *     0x07 | u32 BE width | u32 BE height        image start
 *     0x01 | u32 BE y | u32 BE width | width*3 RGB
 *     0x02 | u16 BE len | name                   mode detected
 *     0x03 | 0x00 | u16 BE len | text            status
 *     0x04                                       sync
 *     0x05 | u32                                 complete
 *     0x08                                       redraw start
 *
 *   FT8 / FT4 (text frame)
 *     {"type":"digital_spot","data":{mode,callsign,snr,frequency,band,grid,how,country,timestamp}}
 *     {"type":"digital_spots_status","data":{scope:"band",modes:[{mode,state,dials,idle,shed,live}],budget}}   (V12 Band governor)
 *
 *   CW ('cw' on the listener's audio, 'cwskim' on a segment of IQ around the dial — V12, vibe_cw_run.h)
 *     0x01 text (as RTTY — the primary signal, 'cw' only) · 0x03 | u8 state (0 nothing, 1 carriers, 3 decoding)
 *     {"type":"cw_signals","data":{skimmer,centre?,width?,primary,signals:[{id,hz,audio,wpm,snr,q,active,last,first,call,country,text}]}}
 *     {"type":"cw_spot","data":{callsign,frequency,audio,wpm,snr,how,country,timestamp}}
 *
 *   PSK31/63 ('psk' on the listener's audio — the signal nearest `centre` is ONE SIGNAL's —, 'pskskim' on IQ — V12,
 *   vibe_psk_run.h): CW's frames exactly, the list and spot named psk_ and carrying the mode instead of a speed
 *     {"type":"psk_signals","data":{skimmer,centre?,width?,primary,signals:[{id,hz,audio,baud,snr,imd,q,active,last,first,call,country,text}]}}
 *     {"type":"psk_spot","data":{callsign,frequency,audio,mode,snr,how,country,timestamp}}
 *   APRS ('aprs' on the listener's audio — V12, vibe_aprs_run.h; "baud_rate": 300 for HF)
 *     {"type":"aprs_packet","data":{key,from,kind,line,raw,lat?,lon?,to?,text?}}       each new packet
 *     {"type":"aprs_list","data":{baud,frames,repaired,dupes,stations:[…],messages:[…]}} every 2 s when changed, and to a joiner
 *   DSC ('dsc' — V12, maritime Digital Selective Calling, vibe_dsc_run.h)
 *     0x01 text (one readable line per call) · 0x03 | u8 state (0 listening, 1 a call coming in)
 *     {"type":"dsc_call","data":{id,utc,rf,repeat,format,category,level,self,…,lat?,lon?,line}} — see dsc.ts
 *
 *   JTTY ('jtty' — V12, the WSJT-X 3.2 keyboard mode, vibe_jtty_run.h)
 *     0x01 text (one line per finished message) · 0x03 | u8 state (0 listening, 3 signals being decoded)
 *     {"type":"jtty_signals","data":{signals:[{id,hz,audio,snr,q,active,last,first,frames,call,country,text}]}}
 *     {"type":"jtty_spot","data":{callsign,frequency,audio,snr,how,country,timestamp}} — see jtty.ts
 *
 * NB WEFAX and SSTV BOTH use 0x01 for a line, with different payloads — so
 * frames can only be parsed against the decoder currently attached. Never sniff.
 */

import { withAuth, type AuthState } from './auth';
import { wsBase } from './origin';
import { guard, guardCallbacks, guardJson } from '../../../src/services/faultLog';

export type DecoderMode = 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'rds' | 'time' | 'cw' | 'cwskim' | 'psk' | 'pskskim' | 'aprs' | 'dsc' | 'jtty' | null;

/** ★ V12 CW: one signal the server's CW engine is decoding (or heard recently). `hz` is RF; `audio` the tone (listener). */
export interface CwSignal {
  id: number; hz: number; audio: number; wpm: number; snr: number;
  q: 'high' | 'medium' | 'low' | 'poor'; active: boolean; last: number; first: number;
  call: string; country: string; text: string;
  /** ★ PSK only: 31 or 63, and the IMD (dB, 0 = not measured). One list draws both decoders. */
  baud?: number; imd?: number;
}
export interface CwList { skimmer: boolean; centre: number; width: number; primary: number; signals: CwSignal[] }
export interface CwSpot { callsign: string; frequency: number; audio: number; wpm: number; snr: number; how: string; country: string; timestamp: number; mode?: string }

/** ★ V12 APRS: one new packet, as the server describes it. */
export interface AprsPacket { key: string; from: string; kind: string; line: string; raw: string; lat?: number; lon?: number; to?: string; text?: string }

export interface Spot {
  /** ★ V12: any mode the server decodes ("FT8", "FT4", "WSPR", "JS8", "FT2" …) — vibeserver.json `spotModes`. */
  mode: string;
  callsign: string;
  snr: number;
  frequency: number;   // RF Hz (dial + audio offset, computed server-side)
  band: string;        // e.g. "20m" — the shim works this out
  grid: string;        // Maidenhead, may be empty
  timestamp: number;   // ms
  /** ★ V12 (2026-10-27): JTTY and PSK say what the station was doing — "CQ", "QSO", "DE", "TEST" ("" otherwise). */
  how?: string;
  /** ★ …and every spot carries the call's country (the server's prefix table), "" when unknown. */
  country?: string;
}

/** ★ V12 BAND GOVERNOR: one mode's state in Band (the spots box header's chips — {"type":"digital_spots_status"}). */
export interface SpotModeState { mode: string; state: 'active' | 'light' | 'idle' | 'shed' | 'off' | 'out'; dials: number; idle: number; shed: number; live: number }
export interface SpotsStatus { scope: 'band'; modes: SpotModeState[]; budget: number | null }

export interface DecoderCallbacks {
  /** Decoded characters (RTTY/NAVTEX), appended as they arrive. */
  onText?: (text: string) => void;
  /** Decoder lock/idle state (RTTY/NAVTEX). */
  onState?: (state: number) => void;
  /** ★ RTTY AUTO's tuning guide (server 0x06 | i16 BE): audio-pitch move in Hz (+ = up), 0 = fine. */
  onTuneHint?: (audioHz: number) => void;
  /** ★ NAVTEX: the message just closed by NNNN, as the server's FEC counted it (0x07, 2026-10-05). */
  onNavtexFec?: (fec: { clean: number; repaired: number; lost: number }) => void;
  /** A new image is starting. height is 0 for WEFAX (it grows without bound). */
  onImageStart?: (width: number, height: number) => void;
  /** One image line. `rgb` is true for SSTV (3 bytes/px), false for WEFAX (grey). */
  onImageLine?: (y: number, width: number, px: Uint8Array, rgb: boolean) => void;
  onImageDone?: () => void;
  /** SSTV 0x08: the server is about to repaint the CURRENT picture with its slant/offset fixed. */
  onImageRedraw?: () => void;
  /** SSTV mode name, e.g. "Martin 1". */
  onSstvMode?: (name: string) => void;
  onStatus?: (text: string) => void;
  /** WEFAX transmission phase (server 0x04): 0 standing by, 1 start tone, 2 phasing, 3 receiving chart, 4 stop tone. */
  onWefaxPhase?: (phase: number) => void;
  /** An FT8/FT4 decode. */
  onSpot?: (spot: Spot) => void;
  /** ★ V12: Band's per-mode state (active / light / idle / shed / off), when it changes and every 15 s. */
  onSpotsStatus?: (st: SpotsStatus) => void;
  /** ★ CW: the signals being decoded (at most once a second, only when changed). */
  onCwSignals?: (list: CwList) => void;
  /** ★ CW: a callsign heard identifying itself. */
  onCwSpot?: (spot: CwSpot) => void;
  /** ★ APRS (V12): the station list (raw data — tracks.ts aprsSanitise), and each new packet. */
  onAprsList?: (data: any) => void;
  onAprsPacket?: (p: AprsPacket) => void;
  /** ★ JTTY: the stations list and a spot (as the server sent them — jtty.ts checks them). */
  onJttySignals?: (data: unknown) => void;
  onJttySpot?: (data: unknown) => void;
  /** ★ DSC: one call (its data as the server sent it — dsc.ts dscFromJson checks it). */
  onDscCall?: (data: unknown) => void;
  /** ★★ The server REFUSED a decoder — most often because every decoder slot on the box is in use.
   *  `message` is the server's own words, shown as they are (one definition, every client). */
  onRefused?: (message: string, what: 'decoder' | 'spots') => void;
  onOpen?: () => void;
  onClose?: () => void;
}

/** Attach parameters. Defaults match the shim's own (startDecoder). */
export interface DecoderParams {
  center_frequency?: number;
  shift?: number;
  baud_rate?: number;
  encoding?: string;
  framing?: string;
  inverted?: boolean;
  // WEFAX
  lpm?: number;
  image_width?: number;
  carrier?: number;
  deviation?: number;
  use_phasing?: boolean;
  auto_start?: boolean;
  auto_stop?: boolean;
  // CW skimmer: the segment around the dial, Hz
  width?: number;
  // DSC: 'hf' (with center_frequency, the tones' audio centre) or 'vhf' (channel 70 on NFM)
  band?: string;
}

/** The shim's extension names. */
const EXT: Record<Exclude<DecoderMode, null>, string> = {
  rtty: 'fsk',
  // ★ Not an audio decoder — attaching 'rds' turns on the extended RDS stream. It goes
  // through the decoder path deliberately: selecting it IS the toggle, so the cost is paid
  // only while it is open (see briefs/BRIEF-rds-receiver.md).
  rds: 'rds',
  navtex: 'navtex',
  wefax: 'wefax',
  sstv: 'sstv',
  // ★ The time signals. One extension; WHICH station is a parameter, exactly as RTTY's shift and
  //   baud are — they share a decoder and differ only in how the seconds are marked.
  time: 'time',
  // ★ V12 CW: the listener's passband, or a skimmer over a segment around the dial (one engine on the server).
  cw: 'cw',
  cwskim: 'cwskim',
  // ★ V12 PSK31/63: the same two views (decoders/psk_skimmer.h on the server).
  psk: 'psk',
  pskskim: 'pskskim',
  // ★ V12 APRS — 1200 Bd VHF / 300 Bd HF ("baud_rate")
  aprs: 'aprs',
  // ★ V12 DSC: MF/HF on SSB or VHF channel 70 on NFM — which is a parameter ("band"), as the time station is.
  dsc: 'dsc',
  // ★ V12 JTTY: every JTTY signal in the listener's passband — nothing to set.
  jtty: 'jtty',
};

export class DecoderClient {
  private ws: WebSocket | null = null;
  private url: string;
  private cb: DecoderCallbacks;
  private closedByUs = false;
  /** The parameters the current decoder was attached with, so a reconnect restores it exactly. */
  private params: DecoderParams = {};
  /** What's attached — REQUIRED to parse frames (0x01 is ambiguous). */
  private mode: DecoderMode = null;
  private spotsOn = false;
  /** ★ V12: 'single' (the listener's audio, FT8 + FT4 as ever) or 'band' (the band's IQ slice, every mode on its dial). */
  private spotScope: 'single' | 'band' = 'single';
  /** ★★ V12 — WHICH VFO each runs on (decode on either VFO, 2026-10-27): fixed when it is started, re-asserted with it on
   *  a reconnect. 'B' goes on the wire as "vfo":"B"; A sends nothing, exactly what every older server understands. */
  private decVfo: 'A' | 'B' = 'A';
  private spotsVfo: 'A' | 'B' = 'A';
  /** connect() has been called (the page is live) — the socket itself opens only when wanted, see _sync. */
  private enabled = false;
  private idleTimer: ReturnType<typeof setTimeout> | null = null;
  private retryTimer: ReturnType<typeof setTimeout> | null = null;

  constructor(host: string, auth: AuthState, cb: DecoderCallbacks, sessionId = '') {
    // ★ Same rule as every other URL: an https page cannot open a ws:// socket. See origin.ts.
    // ★★★ WITH THE SESSION ID, like the spectrum and audio sockets. This socket carried none, so on
    //     a per-VFO server the decoders could not tell WHOSE audio to decode and fell back to "the
    //     first listener": another listener's WEFAX replaced your RTTY, and FT8 decoded somebody
    //     else's frequency (Pi 500, 2026-09-30). The app has always sent it.
    const q = sessionId ? `/ws/dxcluster?user_session_id=${encodeURIComponent(sessionId)}` : '/ws/dxcluster';
    this.url = `${wsBase(host)}${withAuth(q, auth)}`;
    this.cb = guardCallbacks('web-ui', cb);   // ★ a decoder panel that throws stays its own problem
  }

  /* ★★★ OPENED ONLY WHILE SOMETHING IS RUNNING (Stuart, 2026-10-10: "make the connection as efficient as possible,
   *  minimal data rate"; "light everywhere — light on CPU on client and server and light on data"). This socket was
   *  opened at page load and held for the life of the page, whether or not a decoder was ever started:
   *   • behind the Cloudflare tunnel it carried nothing, was cut at ~100 s idle and reconnected for ever — the
   *     console "as long as my arm";
   *   • on a shared dial the server MIRRORS a running decoder to every decoder socket, and this client throws
   *     frames away unless a decoder of its OWN is attached (_handleBinary) — data and server work for nothing;
   *   • one more socket, thread and peer per listener on the server, for most listeners who never decode.
   *  Now: connect() only enables. The socket opens when a decoder is attached or spots switched on, and closes
   *  IDLE_CLOSE_MS after the last one stops (a grace, so switching RTTY → NAVTEX does not reconnect). Callers are
   *  unchanged — attach/detach/setSpots drive it. */
  connect() {
    this.enabled = true;
    this._sync();
  }

  private static readonly IDLE_CLOSE_MS = 8000;
  private _wanted() { return this.enabled && (this.mode !== null || this.spotsOn); }
  private _sync() {
    if (this._wanted()) {
      if (this.idleTimer !== null) { clearTimeout(this.idleTimer); this.idleTimer = null; }
      if (!this.ws && this.retryTimer === null) this._open();
    } else if (this.ws && this.idleTimer === null) {
      this.idleTimer = setTimeout(() => {
        this.idleTimer = null;
        if (!this._wanted()) this._drop();
      }, DecoderClient.IDLE_CLOSE_MS);
    }
  }
  private _drop() {
    const ws = this.ws;
    this.ws = null;
    if (ws) { ws.onclose = null; ws.onmessage = null; ws.onerror = null; try { ws.close(); } catch { /* gone */ } }
  }

  private _open() {
    this.closedByUs = false;
    const ws = new WebSocket(this.url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onopen = () => {
      this.cb.onOpen?.();
      // Re-assert across reconnects — the shim keeps no per-client state.
      // ★★★ WITH THE PARAMETERS, and that was the bug: this re-attached the MODE alone, so every
      //     reconnect silently reverted the decoder to the server's DEFAULTS. Selecting RWM gave
      //     "[RWM] reading the minute" and then, moments later, "[MSF] …" — the station had been
      //     thrown away and msf is the default (Stuart, 2026-08-11).
      //     ★★ It was never specific to the time decoder: RTTY's shift, baud, encoding and
      //        inversion, and WEFAX's LPM, were dropped the same way and silently reverted. The
      //        time decoder only made it VISIBLE, because it prints the station's name.
      if (this.mode) this._sendAttach(this.mode, this.params);
      if (this.spotsOn) this._send(this._spotsMsg());
    };
    // ★ Guarded per message — dropped, counted and logged (faultLog), never silent.
    ws.onmessage = (e) => {
      if (typeof e.data === 'string') this._handleText(e.data);
      else {
        const buf = e.data as ArrayBuffer;
        guard('web-decoder', 'binary', () => this._handleBinary(buf), `len=${buf.byteLength}`);
      }
    };
    ws.onclose = () => {
      if (this.ws !== ws) return;          // superseded or dropped on purpose
      this.ws = null;
      this.cb.onClose?.();
      // ★ Reconnect only while something still needs it — an idle page leaves it closed.
      if (!this.closedByUs && this._wanted() && this.retryTimer === null) {
        this.retryTimer = setTimeout(() => { this.retryTimer = null; if (this._wanted() && !this.ws) this._open(); }, 3000);
      }
    };
    ws.onerror = () => {};
  }

  close() {
    this.closedByUs = true;
    this.enabled = false;
    if (this.idleTimer !== null) { clearTimeout(this.idleTimer); this.idleTimer = null; }
    if (this.retryTimer !== null) { clearTimeout(this.retryTimer); this.retryTimer = null; }
    this._drop();
  }

  private _send(o: Record<string, unknown>) {
    const ws = this.ws;
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(o));
  }

  private _sendAttach(mode: Exclude<DecoderMode, null>, params: DecoderParams = {}) {
    this._send({ type: 'audio_extension_attach', extension_name: EXT[mode], ...params, ...(this.decVfo === 'B' ? { vfo: 'B' } : {}) });
  }

  /** Attach a decoder (detaching whatever was running). `vfo` — the VFO it runs on (V12 VFO A/B); omitted = the one it
   *  already runs on (a re-attach for new settings stays where it is). */
  attach(mode: Exclude<DecoderMode, null>, params: DecoderParams = {}, vfo?: 'A' | 'B') {
    if (vfo) this.decVfo = vfo;
    this.mode = mode;
    // ★ Remembered so a reconnect can re-assert the SAME decoder, not the default one.
    this.params = params;
    if (this.ws?.readyState === WebSocket.OPEN) this._sendAttach(mode, params);
    else this._sync();                     // opening: onopen re-asserts mode + params
  }

  detach() {
    if (!this.mode) return;
    this.mode = null;
    this.params = {};
    this._send({ type: 'audio_extension_detach' });
    this._sync();
  }

  /** FT8/FT4 spots run independently of the text/image decoders. */
  /** ★ The subscribe message. Single sends no scope at all — exactly what every older server understands. */
  private _spotsMsg() {
    const v = this.spotsVfo === 'B' ? { vfo: 'B' } : {};
    return this.spotScope === 'band' ? { type: 'subscribe_digital_spots', scope: 'band', ...v } : { type: 'subscribe_digital_spots', ...v };
  }
  /** `vfo` — the VFO the spots run on (V12 VFO A/B); omitted = where they already run. */
  setSpots(on: boolean, scope: 'single' | 'band' = 'single', vfo?: 'A' | 'B') {
    if (this.spotsOn === on && (!on || (this.spotScope === scope && (!vfo || vfo === this.spotsVfo)))) return;
    const was = this.spotsOn;
    this.spotsOn = on;
    if (on) { this.spotScope = scope; if (vfo) this.spotsVfo = vfo; }
    // ★ Single ↔ Band: one subscribe with the new scope — the server replaces the run and keeps the slot.
    if (this.ws?.readyState === WebSocket.OPEN) this._send(on ? this._spotsMsg() : { type: 'unsubscribe_digital_spots' });
    void was;
    this._sync();                          // opening: onopen subscribes
  }

  get attached(): DecoderMode { return this.mode; }
  /** ★ V12: the VFO the decoder / the spots run on (meaningful only while they run). */
  get decoderVfo(): 'A' | 'B' { return this.decVfo; }
  get spotsVfoNow(): 'A' | 'B' { return this.spotsVfo; }
  get spotsEnabled(): boolean { return this.spotsOn; }
  /** 'single' / 'band' while spots run, null otherwise. */
  get spotsScope(): 'single' | 'band' | null { return this.spotsOn ? this.spotScope : null; }

  // ── Inbound ────────────────────────────────────────────────────────────────

  private _handleText(raw: string) {
    guardJson('web-decoder', raw, (m) => this._handleMessage(m));
  }

  private _handleMessage(msg: any) {
    if (msg.type === 'decoder_refused') {
      // ★★ FORGET WHAT WAS REFUSED, so a reconnect does not ask again and again: the onopen handler
      //    re-asserts `mode` and `spotsOn`, which is exactly right for a decoder that was RUNNING and
      //    exactly wrong for one the server said no to. The listener asks again when they choose to.
      const what = msg.what === 'spots' ? 'spots' : 'decoder';
      if (what === 'spots') this.spotsOn = false; else { this.mode = null; this.params = {}; }
      this._sync();
      this.cb.onRefused?.(String(msg.message || 'The server could not start that decoder.'), what);
      return;
    }
    if ((msg.type === 'cw_signals' || msg.type === 'psk_signals') && msg.data && Array.isArray(msg.data.signals)) {
      const psk = msg.type === 'psk_signals';
      if (psk ? this.mode !== 'psk' && this.mode !== 'pskskim' : this.mode !== 'cw' && this.mode !== 'cwskim') return;
      const d = msg.data;
      const str = (v: unknown, n: number) => (typeof v === 'string' ? v.slice(0, n) : '');
      const num = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : 0);
      const q = (v: unknown): CwSignal['q'] => (v === 'high' || v === 'medium' || v === 'low' ? v : 'poor');
      this.cb.onCwSignals?.({
        skimmer: d.skimmer === true, centre: num(d.centre), width: num(d.width), primary: num(d.primary),
        signals: d.signals.slice(0, 80).map((x: any) => ({
          id: num(x?.id), hz: num(x?.hz), audio: num(x?.audio), wpm: num(x?.wpm), snr: num(x?.snr), q: q(x?.q),
          active: x?.active === true, last: num(x?.last), first: num(x?.first),
          call: str(x?.call, 16), country: str(x?.country, 40), text: str(x?.text, 160),
          ...(psk ? { baud: num(x?.baud) > 40 ? 63 : 31, imd: num(x?.imd) } : {}),
        })),
      });
      return;
    }
    if (msg.type === 'aprs_list' && msg.data) {
      if (this.mode !== 'aprs') return;
      this.cb.onAprsList?.(msg.data);
      return;
    }
    if (msg.type === 'aprs_packet' && msg.data) {
      if (this.mode !== 'aprs') return;
      const d = msg.data;
      const s = (v: unknown, n: number) => (typeof v === 'string' ? v.slice(0, n) : '');
      const n = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) ? v : undefined);
      if (!s(d.key, 40)) return;
      this.cb.onAprsPacket?.({ key: s(d.key, 40), from: s(d.from, 16), kind: s(d.kind, 14), line: s(d.line, 300), raw: s(d.raw, 400),
                               lat: n(d.lat), lon: n(d.lon), to: s(d.to, 12) || undefined, text: s(d.text, 200) || undefined });
      return;
    }
    if ((msg.type === 'jtty_signals' || msg.type === 'jtty_spot') && msg.data) {
      if (this.mode !== 'jtty') return;
      if (msg.type === 'jtty_signals') this.cb.onJttySignals?.(msg.data); else this.cb.onJttySpot?.(msg.data);
      return;
    }
    if (msg.type === 'dsc_call' && msg.data) {
      if (this.mode !== 'dsc') return;
      this.cb.onDscCall?.(msg.data);
      return;
    }
    if ((msg.type === 'cw_spot' || msg.type === 'psk_spot') && msg.data) {
      const d = msg.data;
      if (typeof d.callsign !== 'string' || !d.callsign) return;
      const psk = msg.type === 'psk_spot';
      if (psk ? this.mode !== 'psk' && this.mode !== 'pskskim' : this.mode !== 'cw' && this.mode !== 'cwskim') return;
      this.cb.onCwSpot?.({
        callsign: d.callsign.slice(0, 16), frequency: Number(d.frequency) || 0, audio: Number(d.audio) || 0,
        wpm: Number(d.wpm) || 0, snr: Number(d.snr) || 0, how: String(d.how || '').slice(0, 8),
        country: String(d.country || '').slice(0, 40), timestamp: Number(d.timestamp) || Date.now(),
        ...(psk ? { mode: d.mode === 'BPSK63' ? 'BPSK63' : 'BPSK31' } : {}),
      });
      return;
    }
    if (msg.type === 'digital_spot' && msg.data) {
      const d = msg.data;
      this.cb.onSpot?.({
        // ★ Any mode the server names, kept to a mode's alphabet (it is printed into the list and the map).
        mode: (typeof d.mode === 'string' ? d.mode.replace(/[^0-9A-Za-z+-]/g, '').slice(0, 12) : '') || 'FT8',
        callsign: d.callsign ?? '',
        snr: d.snr ?? 0,
        frequency: d.frequency ?? 0,
        band: d.band ?? '',
        grid: d.grid ?? '',
        timestamp: d.timestamp ?? Date.now(),
        how: typeof d.how === 'string' ? d.how.replace(/[^A-Z]/g, '').slice(0, 8) : '',
        country: typeof d.country === 'string' ? d.country.replace(/[^ -~]/g, '').slice(0, 40) : '',
      });
    }
    if (msg.type === 'digital_spots_status' && msg.data && Array.isArray(msg.data.modes)) {
      // ★ Off the server, so bounded and checked: a state not in the five is dropped, names kept to a mode's alphabet.
      const ok = ['active', 'light', 'idle', 'shed', 'off', 'out'];   // ★ 'out': its slice is beyond this radio's capture
      const n = (v: unknown) => (typeof v === 'number' && Number.isFinite(v) && v >= 0 ? Math.floor(v) : 0);
      const modes = msg.data.modes.slice(0, 16).filter((m: any) => m && typeof m.mode === 'string' && ok.includes(m.state))
        .map((m: any) => ({ mode: m.mode.replace(/[^0-9A-Za-z+-]/g, '').slice(0, 12), state: m.state, dials: n(m.dials), idle: n(m.idle), shed: n(m.shed), live: n(m.live) }));
      const b = msg.data.budget;
      this.cb.onSpotsStatus?.({ scope: 'band', modes, budget: typeof b === 'number' && Number.isFinite(b) ? b : null });
    }
  }

  private _handleBinary(buf: ArrayBuffer) {
    if (buf.byteLength < 1) return;
    const dv = new DataView(buf);
    const op = dv.getUint8(0);

    // Frames are only meaningful against the attached decoder — 0x01 means
    // "text" for FSK, "greyscale line" for WEFAX and "RGB line" for SSTV.
    switch (this.mode) {
      case 'rtty':
      case 'navtex':
      // ★ Same text framing: a decoded minute, and the state/carrier lines that say what the
      //   decoder is doing WHILE it waits — a panel silent for a minute looks broken.
      case 'time':
      // ★ CW: the primary signal's text, as RTTY's; the skimmer sends no text frame, only its state (and JSON lists).
      case 'cw':
      case 'cwskim':
      // ★ PSK: the same — the primary signal's text, the state, and JSON lists.
      case 'psk':
      case 'pskskim':
      // ★ DSC: a call's line (the box draws from the JSON instead) and the state.
      case 'dsc':
      // ★ JTTY: a finished message's line (the box draws its rows from the JSON) and the state.
      case 'jtty':
        if (op === 0x01 && buf.byteLength >= 13) {
          const len = dv.getUint32(9, false);           // big-endian
          const text = new TextDecoder().decode(new Uint8Array(buf, 13, Math.min(len, buf.byteLength - 13)));
          if (text) this.cb.onText?.(text);
        } else if (op === 0x03 && buf.byteLength >= 2) {
          this.cb.onState?.(dv.getUint8(1));
        } else if (op === 0x06 && buf.byteLength >= 3 && this.mode === 'rtty') {
          this.cb.onTuneHint?.(dv.getInt16(1, false));
        } else if (op === 0x07 && buf.byteLength >= 13 && this.mode === 'navtex') {
          this.cb.onNavtexFec?.({ clean: dv.getUint32(1, false), repaired: dv.getUint32(5, false), lost: dv.getUint32(9, false) });
        }
        return;

      case 'wefax':
        if (op === 0x01 && buf.byteLength >= 9) {
          const line = dv.getUint32(1, false);
          const width = dv.getUint32(5, false);
          const px = new Uint8Array(buf, 9, Math.min(width, buf.byteLength - 9));
          this.cb.onImageLine?.(line, width, px, false);
        } else if (op === 0x02) {
          this.cb.onImageStart?.(0, 0);                 // width arrives with the first line
        } else if (op === 0x03) {
          this.cb.onImageDone?.();
        } else if (op === 0x04 && buf.byteLength >= 2) {
          this.cb.onWefaxPhase?.(dv.getUint8(1));
        } else if (op === 0x08) {
          // ★ The decoder's own account of a phasing decision (2026-10-07) — for whoever opens the console.
          console.info('[wefax]', new TextDecoder().decode(new Uint8Array(buf, 1)));
        }
        return;

      case 'sstv':
        switch (op) {
          case 0x07:
            if (buf.byteLength >= 9) {
              this.cb.onImageStart?.(dv.getUint32(1, false), dv.getUint32(5, false));
            }
            return;
          case 0x01:
            if (buf.byteLength >= 9) {
              const y = dv.getUint32(1, false);
              const w = dv.getUint32(5, false);
              const px = new Uint8Array(buf, 9, Math.min(w * 3, buf.byteLength - 9));
              this.cb.onImageLine?.(y, w, px, true);
            }
            return;
          case 0x02:
            if (buf.byteLength >= 3) {
              const len = dv.getUint16(1, false);
              this.cb.onSstvMode?.(
                new TextDecoder().decode(new Uint8Array(buf, 3, Math.min(len, buf.byteLength - 3))));
            }
            return;
          case 0x03:
            if (buf.byteLength >= 4) {
              const len = dv.getUint16(2, false);        // note the 0x00 pad byte at [1]
              this.cb.onStatus?.(
                new TextDecoder().decode(new Uint8Array(buf, 4, Math.min(len, buf.byteLength - 4))));
            }
            return;
          case 0x05:
            this.cb.onImageDone?.();
            return;
          case 0x08:
            // ★★ A REDRAW IS NOT A NEW PICTURE (audit 2026-10-04, decoders.ts:303 / main.ts:11514). This
            //    called onImageStart, so the web client banked the UNCORRECTED copy as PREV (the
            //    picture had just completed) and drew the corrected one on a fresh 600-line canvas —
            //    every slant-corrected SSTV picture left its own crooked twin behind as PREV. The app
            //    (DecoderClient.ts) ignores 0x08 and lets the repainted lines land in place; so now
            //    does this, through onImageRedraw.
            this.cb.onImageRedraw?.();
            return;
          default:
            return;                                      // 0x04 sync — nothing to show
        }

      default:
        return;   // nothing attached; spots arrive as text, not here
    }
  }
}
