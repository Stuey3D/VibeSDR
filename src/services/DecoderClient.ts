/**
 * DecoderClient.ts — native port of the skin's confirmed-working decoder wiring
 * (Scalable_Mobile_UI v6.3.1 initLsvDecoder, verified against the UberSDR Go
 * source: dxcluster_websocket.go + audio_extension_manager.go).
 *
 * HOW IT WORKS (this is the skin's mechanism, not client-side DSP):
 *   The decoders are UberSDR SERVER audio extensions. The client opens the
 *   DX-cluster WebSocket — /ws/dxcluster?user_session_id={uuid} — using the
 *   SAME session uuid as the audio stream (the extension taps that session's
 *   demodulated audio server-side), sends:
 *       { type: 'audio_extension_attach', extension_name, params }
 *   and receives binary frames in the per-extension protocols below. On stop:
 *       { type: 'audio_extension_detach' }
 *   The server allows ONE active extension per session — attaching a new one
 *   tears down the previous automatically.
 *
 * Extension names + params (skin DECODERS registry, verbatim):
 *   rtty   → 'fsk'    { center_frequency:1000, shift, baud_rate, inverted,
 *                       framing: enc==='CCIR476' ? '4/7' : '5N1.5', encoding }
 *   navtex → 'navtex' { center_frequency:500, shift:170, baud_rate:100,
 *                       inverted:false, framing:'4/7', encoding:'CCIR476' }
 *   wefax  → 'wefax'  { lpm, carrier:1900, deviation:400, image_width:1809,
 *                       bandwidth:1, use_phasing:true, auto_stop:true, auto_start:true }
 *   sstv   → 'sstv'   {}
 *   morse  → 'morse'  {}
 *   whisper→ 'whisper'{ language }
 *
 * Binary protocols (all multi-byte ints BIG-endian, per the skin parsers):
 *   RTTY/NAVTEX: 0x01 text  — u32 len @9, utf8 @13
 *                0x03 state — u8 @1: 0 no-signal, 1/2 sync, 3 decoding (rtty)
 *                0x02 sync  — (navtex)
 *   WEFAX:       0x01 line  — u32 lineNo @1, u32 width @5, pixels u8[] @9
 *                0x02 START, 0x03 transmission complete
 *   SSTV:        0x07 imageStart — u32 w @1, u32 h @5
 *                0x01 line — u32 lineNo @1, u32 width @5, rgb? u8[] @9
 *                0x02 mode — u16 len @1, name @3
 *                0x03 status — u8 code @1, u16 len @2, text @4
 *                0x04 sync · 0x05 image complete
 *   MORSE:       0x10 decoded — u8 conf @1 (0 high…3 poor), f32 pitch @6,
 *                f32 wpm @10, u32 len @14, utf8 @18
 *                0x11 tracking — f32 pitch @1, f32 wpm @5
 *                0x12 error — u32 len @1, utf8 @5
 *   WHISPER:     0x02 segments — u32 jsonLen @9, JSON @13:
 *                [{ completed, text }, …] — append completed segment text
 */

import { USER_AGENT } from '../constants/version';

// ── Types ────────────────────────────────────────────────────────────────────

export type DecoderName = 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'morse' | 'whisper' | 'time';

/** The time signal stations the server can decode. Each is a separate extension name on the
 *  wire — the shim builds a TimeDecoder for the one asked for. */
export type TimeStation = 'msf' | 'dcf77' | 'wwv' | 'wwvb' | 'rwm';

/**
 * ★★★ WHICH STATION IS ON THIS FREQUENCY. Nobody tunes 60 kHz by accident, but plenty of people
 *     do not know whether what they are hearing is MSF or WWVB — both live there, on opposite
 *     sides of the Atlantic — so asking the user to name it is asking the question the decoder
 *     exists to answer.
 * ★★ WWV and WWVH SHARE EVERY FREQUENCY (2.5/5/10/15/20 MHz) and send the same timecode; the
 *    decoder reads either, so this maps them to one station rather than pretending to choose.
 * ★ Returns null well away from any of them, so the caller can say "tune to a time station"
 *   instead of decoding silence and reporting nothing.
 */
export function timeStationFor(hz: number, receiverLonDeg?: number | null): TimeStation | null {
  const near = (target: number, tolHz: number) => Math.abs(hz - target) <= tolHz;
  // ★★★ 60 kHz IS TWO STATIONS. MSF (Anthorn, UK) and WWVB (Fort Collins, US) share it, and they
  //     do NOT share a format — MSF is MSB-first, WWVB is not — so the wrong guess decodes
  //     confident nonsense rather than failing. Decided by where the RECEIVER is, which is the
  //     only thing that actually predicts which one is audible; west of 30°W means the Americas.
  //     ★ Falls back to MSF when the receiver's position is unknown, and the user can override.
  if (near(60_000, 2_000))
    return (typeof receiverLonDeg === 'number' && receiverLonDeg < -30) ? 'wwvb' : 'msf';
  if (near(77_500, 2_000))    return 'dcf77';
  if (near(66_666, 2_000))    return 'rwm';
  // ★★★ RWM IS TESTED FIRST, AND THE TOLERANCE IS TIGHT, because 4996 and 9996 kHz sit just
  //     4 kHz from WWV's 5000 and 10000. With WWV checked first at ±5 kHz, RWM could NEVER be
  //     selected — every tuning that should have read Moscow read Fort Collins instead, which
  //     decodes as silence rather than as an error. ±2 kHz keeps the two provably apart.
  for (const f of [4_996_000, 9_996_000, 14_996_000])
    if (near(f, 2_000)) return 'rwm';
  for (const f of [2_500_000, 5_000_000, 10_000_000, 15_000_000, 20_000_000, 25_000_000])
    if (near(f, 2_000)) return 'wwv';
  return null;
}

export interface RttySettings {
  shift:    number;            // 170 | 200 | 425 | 450 | 850
  baud:     number;            // 45.45 | 50 | 75 | 100
  encoding: 'ITA2' | 'ASCII' | 'CCIR476';
  inverted: boolean;
}

/** Skin RPRESETS, verbatim. */
export const RTTY_PRESETS: Record<string, RttySettings> = {
  ham:       { shift: 170, baud: 45.45, encoding: 'ITA2',    inverted: false },
  weather:   { shift: 450, baud: 50,    encoding: 'ITA2',    inverted: true  },
  'sitor-b': { shift: 170, baud: 100,   encoding: 'CCIR476', inverted: false },
};

export type MorseQuality = 'all' | 'low' | 'medium' | 'high';

// ── Spots (Digital/CW skimmer feeds — same dxcluster WS) ─────────────────────
// subscribe_digital_spots / subscribe_cw_spots → server replays its buffer
// then streams live. Messages: {type:'digital_spot'|'cw_spot', data:{…}}.

export type SpotsKind = 'digi' | 'cw';

export interface SpotRow {
  kind:    SpotsKind;
  time:    number;     // epoch ms
  mode:    string;     // FT8/FT4/WSPR/JS8 — 'CW' for skimmer spots
  band:    string;     // '40m' etc.
  call:    string;
  snr?:    number;
  wpm?:    number;
  freqHz:  number;
  distKm?: number;
  grid?:   string;    // TX Maidenhead locator (on-device FT8 spots) → distance/map
  country: string;
  /** The full decoded message — "CQ DX JA8KSF QN03". The primary payload of a spot: it is what
   *  distinguishes a CQ from a signal report from a completed QSO. UberSDR has always sent this
   *  and we discarded it. */
  msg?:     string;
  /** Degrees from the receiver. Server-supplied on UberSDR; derived from `grid` on-device. */
  bearing?: number;
}

/** Trimmed string, or undefined — so an empty field never renders as a blank line. */
function spotStr(v: unknown): string | undefined {
  const s = typeof v === 'string' ? v.trim() : v == null ? '' : String(v).trim();
  return s ? s : undefined;
}
/** A finite number, or undefined. Accepts the numeric-string form some servers send. */
function spotNum(v: unknown): number | undefined {
  if (typeof v === 'number') return isFinite(v) ? v : undefined;
  if (typeof v === 'string' && v.trim()) { const n = parseFloat(v); return isFinite(n) ? n : undefined; }
  return undefined;
}

/** Skin _freqToHz: values < 1000 are MHz, otherwise already Hz. */
function spotFreqHz(f: unknown): number {
  const n = typeof f === 'number' ? f : parseFloat(String(f ?? 0));
  if (!n || isNaN(n)) return 0;
  return n < 1000 ? Math.round(n * 1e6) : Math.round(n);
}

function spotTime(ts: unknown): number {
  if (!ts) return Date.now();
  const d = new Date(ts as string | number);
  const t = d.getTime();
  return isNaN(t) ? Date.now() : t;
}

export interface DecoderCallbacks {
  onText:       (text: string) => void;
  onStatus:     (status: string) => void;
  /** 'idle' | 'sync' | 'rx' | 'active' — drives the decoder panel dot. */
  onDot:        (dot: 'idle' | 'sync' | 'rx' | 'active') => void;
  /** Image decoders (WEFAX/SSTV) — one scanline of pixel data. */
  onImageLine?: (lineNo: number, width: number, pixels: Uint8Array) => void;
  onImageStart?:(width: number, height: number) => void;
  onImageDone?: () => void;
  onError?:     (msg: string) => void;
  /** Digital/CW spots stream (after startSpots). */
  onSpot?:      (spot: SpotRow) => void;
  /** Chat (rides this WS — chat_websocket.go via the dxcluster handler).
   *  isHistory=true for the server's buffer replay after subscribe_chat —
   *  render silently, no unread pulse. Already-seen messages are deduped
   *  before this fires (reconnects replay the buffer every time). */
  onChatMessage?:    (user: string, text: string, ts: string, isHistory: boolean) => void;
  onChatUsers?:      (users: ChatUserRow[], count: number) => void;
  onChatUserUpdate?: (user: ChatUserRow) => void;
  onChatJoined?:     (username: string, isHistory: boolean) => void;
  onChatLeft?:       (username: string, isHistory: boolean) => void;
  onChatError?:      (msg: string) => void;
}

export interface ChatUserRow {
  username:      string;
  is_idle?:      boolean;
  idle_minutes?: number;
  country?:      string;
  country_code?: string;
  frequency?:    number;
  mode?:         string;
  bw_low?:       number;
  bw_high?:      number;
  zoom_bw?:      number;
  cat?:          boolean;
  tx?:           boolean;
}

// ── Client ───────────────────────────────────────────────────────────────────

export class DecoderClient {
  private baseUrl: string;
  private uuid:    string;
  private ws:      WebSocket | null = null;
  private cb:      DecoderCallbacks;
  private active:  DecoderName | null = null;
  private destroyed = false;
  private retries   = 0;

  // Per-decoder user settings
  rttySettings:  RttySettings = { ...RTTY_PRESETS.ham };
  /** ★ Set from the tuned frequency before attaching — see timeStationFor. MSF is the default
   *  only because something must be; it is overwritten on every start. */
  timeStation:   TimeStation = 'msf';
  wefaxLpm       = 120;
  whisperLang    = 'auto';
  morseQuality: MorseQuality = 'all';

  constructor(baseUrl: string, uuid: string, callbacks: DecoderCallbacks, password?: string) {
    this.baseUrl  = baseUrl.replace(/\/+$/, '');
    this.uuid     = uuid;
    this.cb       = callbacks;
    this.password = password ?? null;
  }
  private password: string | null = null;

  /** Start a decoder. Replaces any running one (server enforces one/session). */
  start(name: DecoderName) {
    this.active = name;
    if (this.ws?.readyState === WebSocket.OPEN) {
      this._attach();
    } else {
      this._open();
    }
  }

  /** Stop decoding; keeps the WS warm for quick decoder switches. */
  stop() {
    this.active = null;
    if (this.ws?.readyState === WebSocket.OPEN) {
      try { this.ws.send(JSON.stringify({ type: 'audio_extension_detach' })); } catch {}
    }
  }

  // ── Spots feed (shares this WS — brief follow-up #3) ───────────────────────
  private spotsKind: SpotsKind | null = null;

  startSpots(kind: SpotsKind) {
    this.spotsKind = kind;
    if (this.ws?.readyState === WebSocket.OPEN) this._subscribeSpots();
    else this._open();
  }

  stopSpots() {
    const kind = this.spotsKind;
    this.spotsKind = null;
    if (kind && this.ws?.readyState === WebSocket.OPEN) {
      try {
        this.ws.send(JSON.stringify({
          type: kind === 'digi' ? 'unsubscribe_digital_spots' : 'unsubscribe_cw_spots',
        }));
      } catch {}
    }
  }

  private _subscribeSpots() {
    if (!this.ws || !this.spotsKind) return;
    this.ws.send(JSON.stringify({
      type: this.spotsKind === 'digi' ? 'subscribe_digital_spots' : 'subscribe_cw_spots',
    }));
  }

  // ── Chat ────────────────────────────────────────────────────────────────────
  // subscribe_chat gates ALL chat traffic and triggers the server's message
  // buffer replay — on EVERY (re)connect. chatSeen dedupes the replays so a
  // reconnect never re-notifies; messages within the history window after a
  // subscribe are flagged isHistory (render silently, no unread pulse).

  private chatSubscribed = false;     // user-level intent (survives reconnects)
  private chatUser: string | null = null;
  private chatSubscribedAt = 0;
  private chatSeen = new Set<string>();
  private lastChatStatus = '';

  private static readonly CHAT_HISTORY_MS = 3000;

  /** Open the chat stream (history replay arrives immediately). */
  subscribeChat() {
    this.chatSubscribed = true;
    if (this.ws?.readyState === WebSocket.OPEN) this._chatSubscribe();
    else this._open();
  }

  /** Join with a username (server: 1–15 chars, alnum plus -_/ inside). */
  joinChat(username: string) {
    this.chatUser = username;
    this.chatSubscribed = true;
    if (this.ws?.readyState === WebSocket.OPEN) this._chatSubscribe();
    else this._open();
  }

  leaveChat() {
    if (this.chatUser && this.ws?.readyState === WebSocket.OPEN) {
      try { this.ws.send(JSON.stringify({ type: 'chat_leave' })); } catch {}
    }
    this.chatUser = null;
  }

  sendChat(text: string) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    this.ws.send(JSON.stringify({ type: 'chat_message', message: text }));
  }

  /** Report our tune so other users can see/sync to us. Deduped client-side
   *  (skin sendFrequencyMode parity). zoom_bw = spectrum binBandwidth. */
  sendChatStatus(s: { frequency: number; mode: string; bw_low: number; bw_high: number; zoom_bw?: number }) {
    if (!this.chatUser || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    const key = `${s.frequency}|${s.mode}|${s.bw_low}|${s.bw_high}|${s.zoom_bw ?? 0}`;
    if (key === this.lastChatStatus) return;
    this.lastChatStatus = key;
    this.ws.send(JSON.stringify({
      type: 'chat_set_frequency_mode',
      frequency: s.frequency,
      mode: s.mode.toLowerCase(),
      bw_low: s.bw_low,
      bw_high: s.bw_high,
      ...(s.zoom_bw && s.zoom_bw > 0 ? { zoom_bw: s.zoom_bw } : {}),
    }));
  }

  requestChatUsers() {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    this.ws.send(JSON.stringify({ type: 'chat_request_users' }));
  }

  private _chatSubscribe() {
    if (!this.ws) return;
    this.chatSubscribedAt = Date.now();
    this.ws.send(JSON.stringify({ type: 'subscribe_chat' }));
    if (this.chatUser) {
      this.ws.send(JSON.stringify({ type: 'chat_set_username', username: this.chatUser }));
      this.ws.send(JSON.stringify({ type: 'chat_request_users' }));
      this.lastChatStatus = '';  // force a status resend after (re)join
    }
  }

  private _chatIsHistory(): boolean {
    return Date.now() - this.chatSubscribedAt < DecoderClient.CHAT_HISTORY_MS;
  }

  private _chatSeenBefore(key: string): boolean {
    if (this.chatSeen.has(key)) return true;
    this.chatSeen.add(key);
    if (this.chatSeen.size > 500) {
      // Sets iterate in insertion order — trim the oldest entries
      for (const k of this.chatSeen) {
        this.chatSeen.delete(k);
        if (this.chatSeen.size <= 400) break;
      }
    }
    return false;
  }

  destroy() {
    this.destroyed = true;
    this.stop();
    this.stopSpots();
    this.leaveChat();
    this.chatSubscribed = false;
    this.ws?.close();
    this.ws = null;
  }

  // ── Connection ─────────────────────────────────────────────────────────────

  private _open() {
    if (this.destroyed) return;
    const url = this.baseUrl.replace(/^http/, 'ws')
      + `/ws/dxcluster?user_session_id=${this.uuid}`
      + (this.password ? `&password=${encodeURIComponent(this.password)}` : '');
    // ★ Same reason as the spectrum socket: the decoder connection is logged too. Cast for the
    //   same reason — RN takes a third options argument the DOM type does not describe.
    const ws = new (WebSocket as unknown as {
      new (u: string, p: undefined, o: { headers: Record<string, string> }): WebSocket;
    })(url, undefined, { headers: { 'User-Agent': USER_AGENT } });
    ws.binaryType = 'arraybuffer';
    this.ws = ws;

    ws.onopen = () => {
      this.retries = 0;
      if (this.active) this._attach();
      if (this.spotsKind) this._subscribeSpots();
      if (this.chatSubscribed) this._chatSubscribe();
    };
    ws.onmessage = (e) => {
      if (e.data instanceof ArrayBuffer) {
        this._handleBin(new Uint8Array(e.data));
      }
      // JSON traffic on this WS (DX spots, attach acks) — acks update status
      else if (typeof e.data === 'string') {
        try {
          const m = JSON.parse(e.data);
          if (m.type === 'audio_extension_attached') this.cb.onStatus('attached');
          else if (m.type === 'audio_extension_error') {
            // Server field is `error` (audio_extension_manager.go sendErrorSafe)
            const msg = String(m.error ?? m.message ?? 'extension error');
            this.cb.onStatus('error: ' + msg);
            this.cb.onError?.(msg);
            this.cb.onDot('idle');
          } else if (m.type === 'digital_spot' && m.data) {
            const d = m.data;
            this.cb.onSpot?.({
              kind: 'digi',
              time: spotTime(d.timestamp),
              mode: String(d.mode ?? '').toUpperCase(),
              band: String(d.band ?? ''),
              call: String(d.callsign ?? ''),
              snr:  typeof d.snr === 'number' ? d.snr : undefined,
              freqHz: spotFreqHz(d.frequency),
              distKm: typeof d.distance_km === 'number' ? d.distance_km : undefined,
              grid: d.grid ? String(d.grid) : undefined,
              country: String(d.country ?? ''),
              // ★ The wire key is NOT confirmed — the UberSDR web UI's column headings ("Message",
              // "Bearing") are not necessarily the field names. Accept the plausible spellings
              // rather than betting on one and silently showing nothing; the cost of the extra
              // `??`s is nil and the cost of guessing wrong is a feature that looks broken.
              msg: spotStr(d.message ?? d.msg ?? d.text),
              bearing: spotNum(d.bearing ?? d.bearing_deg ?? d.azimuth),
            });
          } else if (m.type === 'cw_spot' && m.data) {
            const d = m.data;
            this.cb.onSpot?.({
              kind: 'cw',
              time: spotTime(d.time),
              mode: 'CW',
              band: String(d.band ?? ''),
              call: String(d.dx_call ?? ''),
              snr:  typeof d.snr === 'number' ? d.snr : undefined,
              wpm:  typeof d.wpm === 'number' ? d.wpm : undefined,
              freqHz: spotFreqHz(d.frequency),
              distKm: typeof d.distance_km === 'number' ? d.distance_km : undefined,
              country: String(d.country ?? ''),
            });
          } else if (m.type === 'chat_message' && m.data) {
            const d = m.data;
            const user = String(d.username ?? '');
            const text = String(d.message ?? '');
            const ts   = String(d.timestamp ?? '');
            // Dedupe across buffer replays (server re-sends history on every
            // subscribe — reconnects must never re-notify)
            if (!this._chatSeenBefore(`m|${user}|${ts}|${text}`)) {
              this.cb.onChatMessage?.(user, text, ts, this._chatIsHistory());
            }
          } else if (m.type === 'chat_user_joined' && m.data) {
            const user = String(m.data.username ?? '');
            const ts   = String(m.data.timestamp ?? '');
            if (user && !this._chatSeenBefore(`j|${user}|${ts}`)) {
              this.cb.onChatJoined?.(user, this._chatIsHistory());
            }
          } else if (m.type === 'chat_user_left' && m.data) {
            const user = String(m.data.username ?? '');
            const ts   = String(m.data.timestamp ?? '');
            if (user && !this._chatSeenBefore(`l|${user}|${ts}`)) {
              this.cb.onChatLeft?.(user, this._chatIsHistory());
            }
          } else if (m.type === 'chat_active_users' && m.data) {
            this.cb.onChatUsers?.(
              (m.data.users ?? []) as ChatUserRow[],
              Number(m.data.count ?? 0),
            );
          } else if (m.type === 'chat_user_update' && m.data) {
            this.cb.onChatUserUpdate?.(m.data as ChatUserRow);
          } else if (m.type === 'chat_idle_updates' && m.data?.users) {
            for (const u of m.data.users as ChatUserRow[]) {
              this.cb.onChatUserUpdate?.(u);
            }
          } else if (m.type === 'chat_error') {
            this.cb.onChatError?.(String(m.error ?? 'chat error'));
          }
        } catch {}
      }
    };
    ws.onclose = () => {
      if (this.destroyed) return;
      this.ws = null;
      // Chat is long-lived — keep retrying indefinitely while subscribed;
      // decoders/spots alone keep the original 5-try cap
      if (this.chatSubscribed) {
        if (this.active || this.spotsKind) this.cb.onStatus('waiting for ws…');
        setTimeout(() => this._open(), 3000);
      } else if ((this.active || this.spotsKind) && this.retries < 5) {
        this.retries++;
        this.cb.onStatus('waiting for ws…');
        setTimeout(() => this._open(), 2000);
      }
    };
    ws.onerror = () => { /* onclose handles retry */ };
  }

  private _attach() {
    if (!this.ws || !this.active) return;
    const { extension_name, params } = this._paramsFor(this.active);
    this.ws.send(JSON.stringify({
      type: 'audio_extension_attach', extension_name, params,
    }));
    this.cb.onStatus('attached');
    this.cb.onDot('idle');
  }

  /** Skin DECODERS getParams, verbatim. */
  private _paramsFor(name: DecoderName): { extension_name: string; params: Record<string, unknown> } {
    switch (name) {
      case 'rtty': {
        const S = this.rttySettings;
        return { extension_name: 'fsk', params: {
          center_frequency: 1000, shift: S.shift, baud_rate: S.baud,
          inverted: S.inverted,
          framing: S.encoding === 'CCIR476' ? '4/7' : '5N1.5',
          encoding: S.encoding,
        }};
      }
      case 'navtex':
        return { extension_name: 'navtex', params: {
          center_frequency: 500, shift: 170, baud_rate: 100,
          inverted: false, framing: '4/7', encoding: 'CCIR476',
        }};
      case 'wefax':
        // ★★★ NO auto_start / auto_stop — DRAW WHATEVER IS THERE. The shim's rule is
        //     `shouldDecode = !autoStopped && (!autoStart || autoStarted)` (wefax_decoder.cpp),
        //     so asking for auto_start means NOTHING is drawn until a start tone is heard. Tune
        //     into a transmission that is already running — which is most of them, since a chart
        //     takes ten minutes — and the canvas stayed blank for the whole of it, looking like a
        //     decoder that does not work (Stuart, 2026-08-13). The web client sends none of these
        //     and free-runs: "it will draw nothing if no signal, but if I catch a transmission
        //     halfway through it will still draw the end of it."
        // ★★ use_phasing is LEFT AT ITS DEFAULT (on): phasing still aligns the image when the
        //    signal does begin properly. It is only the GATING that was wrong — the app was
        //    treating "no start tone" as "nothing to draw" rather than "start where we are".
        // ★ auto_stop dropped for the same reason: a false detection mid-chart would end a picture
        //   the operator can see is still arriving.
        return { extension_name: 'wefax', params: {
          lpm: this.wefaxLpm, carrier: 1900, deviation: 400,
          image_width: 1809, bandwidth: 1,
        }};
      case 'sstv':    return { extension_name: 'sstv',    params: {} };
      case 'morse':   return { extension_name: 'morse',   params: {} };
      case 'whisper': return { extension_name: 'whisper', params: { language: this.whisperLang } };
      // ★★ THE STATION IS THE EXTENSION NAME. The server has no single "time" decoder to
      //    parameterise — it builds a TimeDecoder for the station asked for, because the framing
      //    differs per station (MSF is MSB-first, DCF77 LSB-first, WWV is IRIG-H with the time
      //    starting at second 10). One shared decoder with a "station" parameter would have to
      //    switch all of that at runtime for no gain.
      case 'time':    return { extension_name: this.timeStation, params: {} };
    }
  }

  // ── Binary frame routing (skin handleBin parsers, verbatim) ────────────────

  private _handleBin(u8: Uint8Array) {
    const name = this.active;
    if (!name || u8.length === 0) return;
    const v = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    const t = u8[0];

    // ★ TIME rides the same 0x01 text frame as RTTY/NAVTEX — the shim writes every station's
    //   output into the one decoded-text buffer, so no new parser is needed here.
    if (name === 'rtty' || name === 'navtex' || name === 'time') {
      if (t === 0x01) {
        if (u8.length < 13) return;
        const tl = v.getUint32(9, false);
        if (tl <= 0 || u8.length < 13 + tl) return;
        this.cb.onText(utf8(u8.subarray(13, 13 + tl)));
        this.cb.onDot('active');
      } else if (name === 'rtty' && t === 0x03) {
        if (u8.length < 2) return;
        const s = u8[1];
        this.cb.onStatus(['no signal', 'sync 1', 'sync 2', 'decoding'][s] ?? 'state ' + s);
        this.cb.onDot(s === 3 ? 'active' : s >= 1 ? 'sync' : 'idle');
      } else if (name === 'navtex' && t === 0x02) {
        this.cb.onDot('sync');
      }

    } else if (name === 'wefax') {
      if (t === 0x01) {
        if (u8.length < 9) return;
        const ln = v.getUint32(1, false);
        const w  = v.getUint32(5, false);
        this.cb.onImageLine?.(ln, w, u8.subarray(9));
        this.cb.onDot('rx');
      } else if (t === 0x02) { this.cb.onStatus('START received'); this.cb.onDot('sync'); }
      else if (t === 0x03) {
        this.cb.onStatus('transmission complete');
        this.cb.onDot('active');
        this.cb.onImageDone?.();
      }

    } else if (name === 'sstv') {
      if (t === 0x07) {
        const w = v.getUint32(1, false), h = v.getUint32(5, false);
        this.cb.onImageStart?.(w, h); this.cb.onDot('sync');
      } else if (t === 0x01) {
        const ln = v.getUint32(1, false), w = v.getUint32(5, false);
        this.cb.onImageLine?.(ln, w, u8.subarray(9));
        this.cb.onDot('rx');
      } else if (t === 0x02) {
        const ml = v.getUint16(1, false);
        this.cb.onStatus('mode: ' + utf8(u8.subarray(3, 3 + ml)));
      } else if (t === 0x03) {
        const sl = v.getUint16(2, false);
        this.cb.onStatus(utf8(u8.subarray(4, 4 + sl)));
      } else if (t === 0x05) {
        this.cb.onStatus('image complete'); this.cb.onDot('active'); this.cb.onImageDone?.();
      } else if (t === 0x04) { this.cb.onDot('sync'); this.cb.onStatus('sync detected'); }

    } else if (name === 'morse') {
      if (t === 0x10) {
        if (u8.length < 18) return;
        const conf  = u8[1];
        const pitch = v.getFloat32(6, false);
        const wpm   = v.getFloat32(10, false);
        const tlen  = v.getUint32(14, false);
        if (u8.length < 18 + tlen) return;
        const confName = (['high', 'medium', 'low', 'poor'][conf] ?? 'poor') as
          'high' | 'medium' | 'low' | 'poor';
        const rank    = { high: 3, medium: 2, low: 1, poor: 0 }[confName];
        const minRank = { all: 0, low: 1, medium: 2, high: 3 }[this.morseQuality];
        if (rank >= minRank) this.cb.onText(utf8(u8.subarray(18, 18 + tlen)));
        this.cb.onDot('active');
        this.cb.onStatus(`${Math.round(pitch)}Hz · ${wpm.toFixed(1)} WPM · ${confName}`);
      } else if (t === 0x11) {
        if (u8.length < 9) return;
        this.cb.onStatus(`${Math.round(v.getFloat32(1, false))}Hz · ${v.getFloat32(5, false).toFixed(1)} WPM`);
        this.cb.onDot('sync');
      } else if (t === 0x12) {
        if (u8.length < 5) return;
        const ml = v.getUint32(1, false);
        this.cb.onStatus('error: ' + utf8(u8.subarray(5, 5 + ml)));
        this.cb.onDot('idle');
      }

    } else if (name === 'whisper') {
      if (t === 0x02) {
        if (u8.length < 13) return;
        const jlen = v.getUint32(9, false);
        if (u8.length < 13 + jlen) return;
        try {
          const segs = JSON.parse(utf8(u8.subarray(13, 13 + jlen)));
          if (Array.isArray(segs)) {
            for (const seg of segs) {
              if (seg?.completed && seg.text) this.cb.onText(seg.text + '\n');
            }
            this.cb.onDot('active');
          }
        } catch {}
      }
    }
  }
}

// ── Helpers ──────────────────────────────────────────────────────────────────

function utf8(u8: Uint8Array): string {
  // TextDecoder exists in Hermes ≥ RN 0.74; fall back to manual decode
  try { return new TextDecoder('utf-8').decode(u8); }
  catch {
    let s = '';
    for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
    return s;
  }
}
