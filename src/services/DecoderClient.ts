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
 *                0x03 state — u8 @1: 0 no-signal, 1/2 sync, 3 decoding (rtty AND navtex)
 *   NAVTEX:      0x07 message FEC count — u32 BE clean @1, repaired @5, lost @9; sent after the text with its NNNN
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

import { noteDecision } from './protocolLog';
import { USER_AGENT } from '../constants/version';
import { rttyFraming, RTTY_WEATHER } from '../utils/rttySpec';
import { guard, guardJson } from './faultLog';
import { cleanText, cleanMode } from '../utils/safeText';
import { parseCwSignals, parseCwSpot, CW_DEFAULT_WIDTH, PSK_DEFAULT_WIDTH, type CwList, type CwSpot } from './cwList';
import { parseJttySignals, parseJttySpot, jttyStateText, type JttySignal, type JttySpot } from './jtty';
import { parseDscCall, dscStateText, type DscCall } from './dsc';
import { parseSitorStatus, sitorStatusText, type SitorStatus } from './sitora';
import { parsePagingStatus, pagingStatusText, type PagingStatus } from './paging';
import { sameStateText, selcalStateText } from './alertDecoders';   // ★ V12 EAS / SAME and SELCAL
import { isTextFamily, parseTextModeStatus, textModeStatusText, type TextFamily, type TextModeStatus } from './textModes';   // ★ V12 MFSK / OLIVIA / THOR
import { hellStatusText, parseHellStatus } from './feldHell';   // ★ V12 FELD HELL
import { parseSpotsStatus, type SpotModeState } from './spotsBand';

// ── Types ────────────────────────────────────────────────────────────────────

/** ★ V12 'cw' — a VibeServer's own CW decoder: ONE SIGNAL ('cw', on this listener's audio) or the SKIMMER ('cwskim',
 *  a segment of IQ centred on the dial). One name here; which extension is attached is `cwSkim`. Not UberSDR's `morse`. */
/** ★ V12 PSK31/63 ('psk' / 'pskskim' — `pskSkim`), JTTY ('jtty') and DSC ('dsc') — a VibeServer's own decoders. */
export type DecoderName = 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'morse' | 'whisper' | 'time' | 'cw' | 'psk' | 'jtty' | 'dsc'
  | 'mfsk' | 'olivia' | 'thor'   // ★ V12 the MFSK keyboard modes (textModes.ts): text as it decodes, 0x03, textmode_status
  | 'hell'    // ★ V12 FELD HELL — drawn by a VibeServer (feldHell.ts): WEFAX's grey lines (re-sent as a strip fills), hell_status
  | 'selcal'  // ★ V12 SELCAL — a VibeServer's aeronautical selective-call decoder: one text line per call
  | 'same'    // ★ V12 EAS / SAME — a VibeServer's US alert-header decoder: one text line per alert, 0x03 state
  | 'paging'  // ★ V12 PAGING — POCSAG with AUTO; only on the phone's own radio (paging.ts), text + status JSON
  | 'sitora'  // ★ V12 SITOR-A — a VibeServer's maritime ARQ listener (vibe_sitora_run.h): text, link state, status JSON
  | 'aprs';   // ★ V12 APRS — a VibeServer's own packet decoder (vibe_aprs_run.h): JSON lists and packets, no text frame

/** The time signal stations the server can decode. Each is a separate extension name on the
 *  wire — the shim builds a TimeDecoder for the one asked for. */
export type TimeStation = 'msf' | 'dcf77' | 'wwv' | 'wwvb' | 'rwm' | 'jjy';

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
  // ★ V12: and JJY's Hagane-yama is on 60 kHz too — east of 100°E (Japan and its neighbours) it is the one heard.
  if (near(60_000, 2_000)) {
    if (typeof receiverLonDeg === 'number' && receiverLonDeg > 100) return 'jjy';
    return (typeof receiverLonDeg === 'number' && receiverLonDeg < -30) ? 'wwvb' : 'msf';
  }
  if (near(40_000, 2_000))    return 'jjy';     // ★ V12: JJY Ohtakadoya-yama — 40 kHz is JJY's alone
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
  encoding: 'ITA2' | 'ASCII' | 'CCIR476';   // ★ ASCII is decoded by the server since 2026-10-04 (7/8 bits + parity)
  /** ★ Stop bits for ITA2 (2026-10-04): 1.5 is the norm (amateur, DWD); 1 for e.g. PBB Den Helder. 2-stop signals decode
   *  with 1 (the decoder waits for each start bit). Absent = 1.5. */
  stop?:    1 | 1.5 | 2;
  /** ★ ASCII only (the full RTTY spec, 2026-10-04): data bits and parity. */
  dataBits?: 7 | 8;
  parity?:  'N' | 'E' | 'O' | 'M' | 'S';
  /** ★ Unshift on space: back to letters after each space (many amateur stations). Off by default — it turns DWD's
   *  number groups into letters. Manual decoding only; AUTO leaves it off. */
  usos?:    boolean;
  inverted: boolean;
  /** ★★ AUTO (2026-10-04): the server finds shift, centre, baud and polarity from the signal (decoders/rtty_auto.h).
   *  The other fields are still sent — an older server simply decodes with them. */
  auto?:    boolean;
}

/** Skin RPRESETS, verbatim — plus AUTO, first and the default ("a one click use for users", Stuart 2026-10-04). */
export const RTTY_PRESETS: Record<string, RttySettings> = {
  auto:      { shift: 170, baud: 45.45, encoding: 'ITA2',    inverted: false, auto: true },
  ham:       { shift: 170, baud: 45.45, encoding: 'ITA2',    inverted: false },
  weather:   { ...RTTY_WEATHER },   // ★ rttySpec.ts RTTY_WEATHER — DWD's 50 Bd / 450 Hz, shared with the known-signal labels
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
  /** ★ RTTY AUTO's tuning guide (server 0x06): how far the tones should move in AUDIO pitch (+ = up), 0 = fine. */
  onTuneHint?:  (audioHz: number) => void;
  /** ★ NAVTEX: the message just closed by NNNN, as the server's FEC counted it (0x07, VibeServer 2026-10-05). */
  onNavtexFec?: (fec: { clean: number; repaired: number; lost: number }) => void;
  /** ★ NAVTEX: the server decoder's state (0x03: 0 no signal, 1 hunting, 3 reading) — a lock lost ends the open
   *  message (NavtexAssembler.lockState; NickB 2026-10-12, a co-channel station taking over 490). */
  onNavtexState?: (state: number) => void;
  /** Digital/CW spots stream (after startSpots). */
  onSpot?:      (spot: SpotRow) => void;
  /** ★ V12 CW (VibeServer): the signals being decoded (at most once a second, only when changed — vibe_cw_run.h). */
  onCwSignals?: (list: CwList) => void;
  /** ★ V12 CW (VibeServer): a callsign heard identifying itself (CQ, DE, TEST — twice). */
  onCwSpot?:    (spot: CwSpot) => void;
  /** ★ V12 JTTY (VibeServer): the stations list (once a second, when it changed) and each call heard. */
  onJttySignals?: (list: JttySignal[]) => void;
  onJttySpot?:    (spot: JttySpot) => void;
  /** ★ V12 DSC (VibeServer): one call (a repeat comes again with the same id and a higher count). */
  onDscCall?:     (call: DscCall) => void;
  /** ★ V12 BAND GOVERNOR: Band's per-mode state (active / light / idle / shed / off), when it changes and every 15 s. */
  onSpotsStatus?: (modes: SpotModeState[]) => void;
  /** ★ V12 APRS (VibeServer): the station list (every 2 s when it changed; at once, with tracks, to a joiner) and each new
   *  packet — as the server sent them; src/services/tracks.ts aprsSanitise / aprsPacketSanitise check them. */
  onAprsList?:   (data: unknown) => void;
  onAprsPacket?: (data: unknown) => void;
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

/** ★★ A chat user row from the server, checked field by field (security pass, 2026-10-03). The list arrived
 *  as `m.data.users ?? []` cast straight to rows: an object instead of an array, or a row whose `mode` was a
 *  number, threw later in render or in applyChatSync. A row without a usable name is dropped. */
export function cleanChatUserRow(v: unknown): ChatUserRow | null {
  if (!v || typeof v !== 'object') return null;
  const r = v as Record<string, unknown>;
  const username = cleanText(r.username, 32);
  if (!username) return null;
  const n = (x: unknown) => (typeof x === 'number' && Number.isFinite(x) ? x : undefined);
  const out: ChatUserRow = { username };
  if (typeof r.is_idle === 'boolean') out.is_idle = r.is_idle;
  if (n(r.idle_minutes) !== undefined) out.idle_minutes = n(r.idle_minutes);
  const country = cleanText(r.country, 48); if (country) out.country = country;
  if (typeof r.country_code === 'string' && /^[A-Za-z]{2}$/.test(r.country_code)) out.country_code = r.country_code;
  if (n(r.frequency) !== undefined) out.frequency = n(r.frequency);
  const mode = cleanMode(r.mode); if (mode) out.mode = mode;
  if (n(r.bw_low) !== undefined) out.bw_low = n(r.bw_low);
  if (n(r.bw_high) !== undefined) out.bw_high = n(r.bw_high);
  if (n(r.zoom_bw) !== undefined) out.zoom_bw = n(r.zoom_bw);
  if (typeof r.cat === 'boolean') out.cat = r.cat;
  if (typeof r.tx === 'boolean') out.tx = r.tx;
  return out;
}

// ── Client ───────────────────────────────────────────────────────────────────

export class DecoderClient {
  private baseUrl: string;
  private uuid:    string;
  private ws:      WebSocket | null = null;
  private cb:      DecoderCallbacks;
  private active:  DecoderName | null = null;
  /** ★ SITOR-A: the last status the server sent (the status line is drawn from it and the 0x03 link state). */
  private sitorLast: SitorStatus | null = null;
  /** ★ PAGING: the last status (the line is drawn from it). */
  private pagingLast: PagingStatus | null = null;
  /** This server reports WEFAX phases (0x04) — set by the first one, cleared on every attach. */
  private wefaxPhases = false;
  private destroyed = false;
  private retries   = 0;

  // Per-decoder user settings
  rttySettings:  RttySettings = { ...RTTY_PRESETS.auto };
  /** ★ Set from the tuned frequency before attaching — see timeStationFor. MSF is the default
   *  only because something must be; it is overwritten on every start. */
  timeStation:   TimeStation = 'msf';
  wefaxLpm       = 120;
  whisperLang    = 'auto';
  morseQuality: MorseQuality = 'all';
  /** ★ V12 CW: SKIMMER (true, 'cwskim' over `cwWidth` Hz centred on the dial) or ONE SIGNAL (false, 'cw'). Changing
   *  either is a re-attach — a different decoder on the server (the caller calls start('cw') again). */
  cwSkim = false;
  cwWidth: number = CW_DEFAULT_WIDTH;
  /** ★ V12 PSK: SKIMMER ('pskskim' over `pskWidth` Hz) or ONE SIGNAL ('psk', the signal nearest `pskCentre` — the audio
   *  Hz of the passband's middle, set by the caller before start). Changing any is a re-attach, as CW's. */
  pskSkim = false;
  pskWidth: number = PSK_DEFAULT_WIDTH;
  pskCentre = 1500;
  /** ★ V12 DSC: the attach parameters for the dial and mode (dsc.ts dscParams) — set by the caller before start. */
  dscParams: Record<string, unknown> = { band: 'hf', center_frequency: 1700 };
  /** ★ V12 APRS: 1200 Bd (VHF, NFM) or 300 Bd (HF packet, USB) — a change is a re-attach (start('aprs') again). */
  aprsBaud: 1200 | 300 = 1200;
  /** ★ V12 PAGING: a `pagingModes` id from the server ('auto' first) — a change is a re-attach (start('paging') again). */
  pagingMode = 'auto';
  /** ★ V12 MFSK / OLIVIA / THOR: each family's chosen sub-mode (a server `<family>Modes` id, 'auto' first) — a change is a re-attach. */
  textMode: Record<TextFamily, string> = { mfsk: 'auto', olivia: 'auto', thor: 'auto' };
  private tmLast: TextModeStatus | null = null;

  constructor(baseUrl: string, uuid: string, callbacks: DecoderCallbacks, password?: string, authSuffix = '') {
    this.baseUrl  = baseUrl.replace(/\/+$/, '');
    this.uuid     = uuid;
    this.cb       = callbacks;
    this.password = password ?? null;
    this.authSuffix = /^&vs_/.test(authSuffix) ? authSuffix : (authSuffix ? '&' + authSuffix.replace(/^[?&]+/, '') : '');
  }
  private password: string | null = null;
  /** ★★★ THE PIN PROOF (`&vs_nonce=…&vs_auth=…`), as the spectrum and audio sockets carry it. A PIN-locked
   *  VibeServer gates /ws/dxcluster exactly like them (since 2026-07) — and this socket carried NOTHING, so on
   *  any PIN-protected radio every decoder was refused at the upgrade, retried every 2 s, and each refusal
   *  counted as a wrong PIN until the listener's own address was locked out (Stuart's HF+ with a per-radio
   *  PIN, 2026-10-04: "All of our decoders are fucked"). The web client always sent it (withAuth). */
  private authSuffix = '';

  /** ★★ V12 — WHICH VFO the decoder and the spots run on (decode on either VFO, 2026-10-27): set by the screen when it
   *  STARTS one, re-asserted with it on a reconnect and on a re-attach for new settings. 'B' goes on the wire as
   *  "vfo":"B"; A sends nothing — exactly what every older server understands. */
  vfo: 'A' | 'B' = 'A';
  spotsVfo: 'A' | 'B' = 'A';

  /** Start a decoder. Replaces any running one (server enforces one/session). */
  start(name: DecoderName) {
    this.active = name;
    this._idleCheck();                     // ★ wanted again: cancels a pending idle close
    if (this.ws?.readyState === WebSocket.OPEN) {
      this._attach();
    } else {
      this._open();
    }
  }

  /** Stop decoding. The socket stays warm for IDLE_CLOSE_MS (a quick switch to another decoder reuses it), then closes
   *  if nothing — decoder, spots, chat — still needs it. */
  stop() {
    this.active = null;
    if (this.ws?.readyState === WebSocket.OPEN) {
      try { this.ws.send(JSON.stringify({ type: 'audio_extension_detach' })); } catch {}
    }
    this._idleCheck();
  }

  /* ★★★ CLOSED WHEN NOTHING NEEDS IT (Stuart, 2026-10-10: "light everywhere — light on CPU on client and server and
   *  light on data"). The socket opened on demand but was then kept "warm" for the rest of the session — and on a
   *  shared dial the server MIRRORS a running decoder to every decoder socket, so a phone that had once tried RTTY
   *  went on receiving somebody else's decoder output; behind a Cloudflare tunnel an idle one is cut at ~100 s and
   *  retried. Same rule as the web client (web/client/src/decoders.ts). */
  private static readonly IDLE_CLOSE_MS = 8000;
  private idleTimer: ReturnType<typeof setTimeout> | null = null;
  private _needed() { return !!(this.active || this.spotsKind || this.chatSubscribed); }
  private _idleCheck() {
    if (this.destroyed) return;
    if (this._needed()) { if (this.idleTimer) { clearTimeout(this.idleTimer); this.idleTimer = null; } return; }
    if (!this.ws || this.idleTimer) return;
    this.idleTimer = setTimeout(() => {
      this.idleTimer = null;
      if (this._needed() || this.destroyed) return;
      const ws = this.ws;
      this.ws = null;
      if (ws) { ws.onopen = null; ws.onmessage = null; ws.onclose = null; ws.onerror = null; try { ws.close(); } catch { /* gone */ } }
    }, DecoderClient.IDLE_CLOSE_MS);
  }

  // ── Spots feed (shares this WS — brief follow-up #3) ───────────────────────
  private spotsKind: SpotsKind | null = null;

  /** ★ V12 digital spots: 'single' (the listener's audio — the button there always was) or 'band' (the band's IQ slice,
   *  every mode on its dial — docs/v12/DIGITAL-SPOTS.md). Switching scope while subscribed is one subscribe: the server
   *  keeps the slot (the web's setSpots). */
  private spotsScope: 'single' | 'band' = 'single';
  get spotsBandScope(): boolean { return !!this.spotsKind && this.spotsScope === 'band'; }

  startSpots(kind: SpotsKind, scope: 'single' | 'band' = 'single', vfo?: 'A' | 'B') {
    if (vfo) this.spotsVfo = vfo;
    this.spotsKind = kind;
    this.spotsScope = kind === 'digi' ? scope : 'single';
    this._idleCheck();
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
    this._idleCheck();
  }

  private _subscribeSpots() {
    if (!this.ws || !this.spotsKind) return;
    const v = this.spotsVfo === 'B' ? { vfo: 'B' } : {};
    this.ws.send(JSON.stringify(this.spotsKind === 'digi'
      ? (this.spotsScope === 'band' ? { type: 'subscribe_digital_spots', scope: 'band', ...v } : { type: 'subscribe_digital_spots', ...v })
      : { type: 'subscribe_cw_spots' }));
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
    this._idleCheck();
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

  /**
   * ★★ A FRESH SOCKET, SAME INTENT — for a full reconnect (Connection Refresh, the reconnect
   *  banners). The decoder, spots and chat choices live on THIS object, and onopen restates all
   *  three, so a new socket is everything a refresh needs; rebuilding the client would throw those
   *  choices away. The old socket's handlers are detached FIRST: its onclose would otherwise null
   *  out the new socket and schedule a retry of its own. Idle (nothing in use) → nothing is opened.
   */
  refresh() {
    if (this.destroyed) return;
    const old = this.ws;
    this.ws = null;
    if (old) {
      old.onopen = null; old.onmessage = null; old.onclose = null; old.onerror = null;
      try { old.close(); } catch { /* already dead */ }
    }
    this.retries = 0;
    if (this.active || this.spotsKind || this.chatSubscribed) this._open();
  }

  destroy() {
    this.destroyed = true;
    if (this.idleTimer) { clearTimeout(this.idleTimer); this.idleTimer = null; }
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
    if (this.idleTimer) { clearTimeout(this.idleTimer); this.idleTimer = null; }   // wanted again: keep it
    // ★ One socket at a time. A retry timer from an earlier close (or a start() while CONNECTING)
    //   would otherwise open a second one and orphan the first; the live one's onopen already
    //   restates the decoder, spots and chat.
    if (this.ws && (this.ws.readyState === WebSocket.CONNECTING || this.ws.readyState === WebSocket.OPEN)) return;
    const url = this.baseUrl.replace(/^http/, 'ws')
      + `/ws/dxcluster?user_session_id=${this.uuid}`
      + (this.password ? `&password=${encodeURIComponent(this.password)}` : '')
      + this.authSuffix;
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
    /* ★★ GUARDED PER MESSAGE (faultLog). The JSON branch was a single `catch {}` — a bad spot or
     *  chat frame vanished uncounted — and the binary branch was not guarded at all. */
    ws.onmessage = (e) => {
      if (e.data instanceof ArrayBuffer) {
        const u8 = new Uint8Array(e.data);
        guard('decoder', 'binary', () => this._handleBin(u8), `len=${u8.length}`);
      }
      // JSON traffic on this WS (DX spots, attach acks) — acks update status
      else if (typeof e.data === 'string') {
        guardJson('decoder', e.data, (m: any) => {
          if (m.type === 'audio_extension_attached') this.cb.onStatus('attached');
          // ★★ A VibeServer REFUSED the decoder — most often every decoder slot on the server is
          //    in use ("All 4 decoder slots on this server are in use — try again shortly"). The
          //    server's own words go in the status line verbatim (one definition, every client),
          //    and what was refused is FORGOTTEN so the reconnect in onopen does not ask again in
          //    a loop; the listener asks again when they choose to.
          else if (m.type === 'decoder_refused') {
            const msg = cleanText(m.message, 200) || 'The server could not start that decoder.';
            if (m.what === 'spots') this.spotsKind = null; else this.active = null;
            this.cb.onStatus(msg);
            this.cb.onDot('idle');
          }
          else if (m.type === 'audio_extension_error') {
            // Server field is `error` (audio_extension_manager.go sendErrorSafe)
            const msg = cleanText(m.error, 200) || cleanText(m.message, 200) || 'extension error';
            this.cb.onStatus('error: ' + msg);
            this.cb.onError?.(msg);
            this.cb.onDot('idle');
          } else if (m.type === 'digital_spot' && m.data) {
            const d = m.data;
            this.cb.onSpot?.({
              kind: 'digi',
              time: spotTime(d.timestamp),
              mode: cleanText(d.mode, 12).toUpperCase(),
              band: cleanText(d.band, 12),
              call: cleanText(d.callsign, 20),
              snr:  typeof d.snr === 'number' ? d.snr : undefined,
              freqHz: spotFreqHz(d.frequency),
              distKm: typeof d.distance_km === 'number' ? d.distance_km : undefined,
              grid: cleanText(d.grid, 8) || undefined,
              country: cleanText(d.country, 48),
              // ★ The wire key is NOT confirmed — the UberSDR web UI's column headings ("Message",
              // "Bearing") are not necessarily the field names. Accept the plausible spellings
              // rather than betting on one and silently showing nothing; the cost of the extra
              // `??`s is nil and the cost of guessing wrong is a feature that looks broken.
              msg: spotStr(d.message ?? d.msg ?? d.text),
              bearing: spotNum(d.bearing ?? d.bearing_deg ?? d.azimuth),
            });
          } else if ((m.type === 'aprs_list' || m.type === 'aprs_packet') && m.data) {
            // ★ V12 APRS: only while OUR APRS decoder is attached (a shared dial mirrors a running decoder).
            if (this.active !== 'aprs') return;
            if (m.type === 'aprs_list') this.cb.onAprsList?.(m.data); else this.cb.onAprsPacket?.(m.data);
          } else if (m.type === 'cw_signals' && m.data) {
            // ★ V12: only while OUR CW decoder is attached — on a shared dial the server mirrors a running decoder.
            if (this.active !== 'cw') return;
            const l = parseCwSignals(m.data);
            if (l) this.cb.onCwSignals?.(l);
          } else if (m.type === 'digital_spots_status' && m.data) {
            // ★ V12 Band governor — only while OUR Band subscription runs (a Single listener has no chips)
            if (!this.spotsBandScope) return;
            const st = parseSpotsStatus(m.data);
            if (st) this.cb.onSpotsStatus?.(st);
          } else if (m.type === 'psk_signals' && m.data) {
            // ★ V12 PSK: CW's list exactly, with the mode and IMD — the one list view draws both (the web's rule).
            if (this.active !== 'psk') return;
            const l = parseCwSignals(m.data, true);
            if (l) this.cb.onCwSignals?.(l);
          } else if (m.type === 'psk_spot' && m.data) {
            if (this.active !== 'psk') return;
            const sp = parseCwSpot(m.data, Date.now(), true);
            if (sp) this.cb.onCwSpot?.(sp);
          } else if (m.type === 'jtty_signals' && m.data) {
            if (this.active !== 'jtty') return;
            const l = parseJttySignals(m.data);
            if (l) this.cb.onJttySignals?.(l);
          } else if (m.type === 'jtty_spot' && m.data) {
            if (this.active !== 'jtty') return;
            const sp = parseJttySpot(m.data, Date.now());
            if (sp) this.cb.onJttySpot?.(sp);
          } else if (m.type === 'textmode_status' && m.data) {
            const fam = this.active;
            if (!isTextFamily(fam)) return;
            const st = parseTextModeStatus(m.data);
            if (st) { this.tmLast = st; this.cb.onStatus(textModeStatusText(fam, this.textMode[fam], st)); this.cb.onDot(st.detected ? 'active' : 'idle'); }
          } else if (m.type === 'hell_status' && m.data) {
            if (this.active !== 'hell') return;
            const st = parseHellStatus(m.data);
            if (st) { this.cb.onStatus(hellStatusText(st)); this.cb.onDot(st.signal ? 'rx' : 'idle'); }
          } else if (m.type === 'paging_status' && m.data) {
            if (this.active !== 'paging') return;
            const st = parsePagingStatus(m.data);
            if (st) { this.pagingLast = st; this.cb.onStatus(pagingStatusText(st)); this.cb.onDot(st.detected ? 'active' : 'idle'); }
          } else if (m.type === 'sitora_status' && m.data) {
            if (this.active !== 'sitora') return;
            const st = parseSitorStatus(m.data);
            if (st) { this.sitorLast = st; this.cb.onStatus(sitorStatusText(st)); }
          } else if (m.type === 'dsc_call' && m.data) {
            if (this.active !== 'dsc') return;
            const c = parseDscCall(m.data, Date.now());
            if (c) this.cb.onDscCall?.(c);
          } else if (m.type === 'cw_spot' && m.data && this.active === 'cw' && typeof m.data.callsign === 'string') {
            // ★ V12: a VibeServer's CW decoder names the call `callsign` (UberSDR's skimmer feed, below, `dx_call`).
            const sp = parseCwSpot(m.data, Date.now());
            if (sp) this.cb.onCwSpot?.(sp);
          } else if (m.type === 'cw_spot' && m.data) {
            const d = m.data;
            this.cb.onSpot?.({
              kind: 'cw',
              time: spotTime(d.time),
              mode: 'CW',
              band: cleanText(d.band, 12),
              call: cleanText(d.dx_call, 20),
              snr:  typeof d.snr === 'number' ? d.snr : undefined,
              wpm:  typeof d.wpm === 'number' ? d.wpm : undefined,
              freqHz: spotFreqHz(d.frequency),
              distKm: typeof d.distance_km === 'number' ? d.distance_km : undefined,
              country: cleanText(d.country, 48),
            });
          } else if (m.type === 'chat_message' && m.data) {
            const d = m.data;
            // ★ Someone else's typing, relayed by a server that may not be ours: cleaned on the way in.
            const user = cleanText(d.username, 32);
            const text = cleanText(d.message, 300);
            const ts   = cleanText(d.timestamp, 40);
            // Dedupe across buffer replays (server re-sends history on every
            // subscribe — reconnects must never re-notify)
            if (!this._chatSeenBefore(`m|${user}|${ts}|${text}`)) {
              this.cb.onChatMessage?.(user, text, ts, this._chatIsHistory());
            }
          } else if (m.type === 'chat_user_joined' && m.data) {
            const user = cleanText(m.data.username, 32);
            const ts   = cleanText(m.data.timestamp, 40);
            if (user && !this._chatSeenBefore(`j|${user}|${ts}`)) {
              this.cb.onChatJoined?.(user, this._chatIsHistory());
            }
          } else if (m.type === 'chat_user_left' && m.data) {
            const user = cleanText(m.data.username, 32);
            const ts   = cleanText(m.data.timestamp, 40);
            if (user && !this._chatSeenBefore(`l|${user}|${ts}`)) {
              this.cb.onChatLeft?.(user, this._chatIsHistory());
            }
          } else if (m.type === 'chat_active_users' && m.data) {
            const rows = Array.isArray(m.data.users) ? m.data.users : [];
            const count = Number(m.data.count ?? 0);
            this.cb.onChatUsers?.(
              rows.map(cleanChatUserRow).filter((u: ChatUserRow | null): u is ChatUserRow => !!u),
              Number.isFinite(count) && count >= 0 ? count : 0,
            );
          } else if (m.type === 'chat_user_update' && m.data) {
            const u = cleanChatUserRow(m.data);
            if (u) this.cb.onChatUserUpdate?.(u);
          } else if (m.type === 'chat_idle_updates' && Array.isArray(m.data?.users)) {
            for (const raw of m.data.users) {
              const u = cleanChatUserRow(raw);
              if (u) this.cb.onChatUserUpdate?.(u);
            }
          } else if (m.type === 'chat_error') {
            this.cb.onChatError?.(cleanText(m.error, 200) || 'chat error');
          }
        });
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
    this.wefaxPhases = false;
    if (!this.ws || !this.active) return;
    const { extension_name, params } = this._paramsFor(this.active);
    this.ws.send(JSON.stringify({
      type: 'audio_extension_attach', extension_name, params, ...(this.vfo === 'B' ? { vfo: 'B' } : {}),
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
          framing: rttyFraming(S),
          encoding: S.encoding,
          ...(S.auto && S.encoding === 'ITA2' ? { auto: true } : {}),
          ...(!S.auto && S.usos && S.encoding === 'ITA2' ? { usos: true } : {}),
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
      // ★ V12 CW: the skimmer's segment rides as `width` (the server reads 12–192 kHz; the box offers 24/48/96).
      // ★ V12 APRS: the baud rides as `baud_rate` (the server reads < 600 as HF 300 Bd), always sent, as the web does.
      case 'aprs':    return { extension_name: 'aprs', params: { baud_rate: this.aprsBaud } };
      case 'cw':      return this.cwSkim
        ? { extension_name: 'cwskim', params: { width: this.cwWidth } }
        : { extension_name: 'cw', params: {} };
      // ★ V12 PSK: the skimmer's segment as `width` (6/12/24 kHz); ONE SIGNAL's cursor as `centre` (the web's decParams).
      case 'psk':     return this.pskSkim
        ? { extension_name: 'pskskim', params: { width: this.pskWidth } }
        : { extension_name: 'psk', params: { centre: this.pskCentre } };
      // ★ V12 JTTY: every JTTY signal in the passband — nothing to set.
      case 'jtty':    return { extension_name: 'jtty', params: {} };
      // ★ V12 DSC: 'hf' with the tones' audio centre, or 'vhf' (channel 70 on NFM) — dsc.ts dscParams.
      case 'dsc':     return { extension_name: 'dsc', params: { ...this.dscParams } };
      // ★ V12 SITOR-A: the tones around 1 kHz (as RTTY); polarity, timing and each station's offset are found.
      case 'sitora':  this.sitorLast = null; return { extension_name: 'sitora', params: { center_frequency: 1000 } };
      // ★ V12 PAGING: the mode is one of the server's own `pagingModes` ids — the server falls back to AUTO for any other.
      // ★ V12 EAS / SAME: nothing to set (47 CFR § 11.31 fixes the tones and the rate).
      case 'same':    return { extension_name: 'same', params: {} };
      // ★ V12 SELCAL: nothing to set (ICAO's 32 tones, found by themselves).
      case 'selcal':  return { extension_name: 'selcal', params: {} };
      // ★ V12 MFSK / OLIVIA / THOR: "mode" is one of the server's own ids — the server falls back to AUTO for any other.
      case 'mfsk': case 'olivia': case 'thor': this.tmLast = null; return { extension_name: name, params: { mode: this.textMode[name] } };
      // ★ V12 FELD HELL: nothing to set — the tone is found by itself.
      case 'hell':    return { extension_name: 'hell', params: {} };
      case 'paging':  this.pagingLast = null; return { extension_name: 'paging', params: { mode: this.pagingMode } };
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
    // ★ V12 CW too: ONE SIGNAL's primary text is RTTY's 0x01 frame; the skimmer sends only its state (and JSON lists).
    // ★ V12 PSK the same (the primary signal's text); JTTY and DSC send each finished line too, but their boxes draw from
    //   the JSON — the line is the same text, so it is not shown twice (the web's onText).
    if (name === 'rtty' || name === 'navtex' || name === 'time' || name === 'cw' || name === 'psk' || name === 'jtty' || name === 'dsc' || name === 'sitora' || name === 'paging' || name === 'same' || name === 'selcal' || isTextFamily(name)) {
      if (t === 0x01) {
        if (u8.length < 13) return;
        const tl = v.getUint32(9, false);
        if (tl <= 0 || u8.length < 13 + tl) return;
        if (name !== 'jtty' && name !== 'dsc') this.cb.onText(utf8(u8.subarray(13, 13 + tl)));
        this.cb.onDot('active');
      } else if (isTextFamily(name) && t === 0x03) {
        if (u8.length < 2) return;
        if (!u8[1]) { this.tmLast = null; this.cb.onStatus(textModeStatusText(name, this.textMode[name], null)); }
        this.cb.onDot(u8[1] ? 'active' : 'idle');
      } else if (name === 'selcal' && t === 0x03) {
        if (u8.length < 2) return;
        this.cb.onStatus(selcalStateText(u8[1]));
        this.cb.onDot(u8[1] ? 'sync' : 'idle');
      } else if (name === 'same' && t === 0x03) {
        // ★ EAS / SAME: 0 listening · 1 reading a header burst
        if (u8.length < 2) return;
        this.cb.onStatus(sameStateText(u8[1]));
        this.cb.onDot(u8[1] ? 'sync' : 'idle');
      } else if (name === 'paging' && t === 0x03) {
        // ★ PAGING's lock: 0 hunting · 1 a batch lock at some rate (the status JSON names it)
        if (u8.length < 2) return;
        this.cb.onDot(u8[1] ? 'active' : 'idle');
        if (!u8[1] && this.pagingLast) { this.pagingLast.detected = ''; this.cb.onStatus(pagingStatusText(this.pagingLast)); }
      } else if (name === 'sitora' && t === 0x03) {
        // ★ SITOR-A's link state (0 no signal … 7 ended) — the status line keeps who is printing and the RQ count
        if (u8.length < 2) return;
        const s = u8[1];
        if (this.sitorLast) this.sitorLast.link = s;
        this.cb.onStatus(sitorStatusText(this.sitorLast, s));
        this.cb.onDot(s >= 2 && s <= 6 ? 'active' : s === 1 ? 'sync' : 'idle');
      } else if ((name === 'jtty' || name === 'dsc') && t === 0x03) {
        if (u8.length < 2) return;
        const s = u8[1];
        this.cb.onStatus(name === 'jtty' ? jttyStateText(s) : dscStateText(s));
        this.cb.onDot(s ? 'active' : 'idle');
      } else if ((name === 'cw' || name === 'psk') && t === 0x03) {
        // ★ CW's state: 0 nothing heard, 1 carriers, 3 decoding (vibe_cw_run.h).
        if (u8.length < 2) return;
        const s = u8[1];
        this.cb.onStatus(s >= 3 ? 'decoding' : s >= 1 ? 'carriers heard' : 'listening…');
        this.cb.onDot(s >= 3 ? 'active' : s >= 1 ? 'sync' : 'idle');
      } else if ((name === 'rtty' || name === 'navtex') && t === 0x03) {
        /* ★★ NAVTEX'S STATE IS 0x03 TOO (2026-10-05). The host sends every FSK decoder's state as 0x03
         *  (vibe_decoder_host.h, fsk_->onState); this waited for a 0x02 'sync' frame for NAVTEX that nothing
         *  sends, so the app's NAVTEX panel never said it had found a signal — the web client always read 0x03. */
        if (u8.length < 2) return;
        const s = u8[1];
        const words = name === 'navtex'
          ? ['no signal', 'searching', 'searching', 'decoding']   // NAVTEX hunts in one step: no 'sync 1/2' to tell apart
          : ['no signal', 'sync 1', 'sync 2', 'decoding'];
        this.cb.onStatus(words[s] ?? 'state ' + s);
        this.cb.onDot(s === 3 ? 'active' : s >= 1 ? 'sync' : 'idle');
        if (name === 'navtex') this.cb.onNavtexState?.(s);
      } else if (name === 'rtty' && t === 0x06 && u8.length >= 3) {
        this.cb.onTuneHint?.(v.getInt16(1, false));
      } else if (name === 'navtex' && t === 0x07 && u8.length >= 13) {
        // ★ A server older than 2026-10-05 never sends this; the box then keeps its '_'-derived "% lost".
        this.cb.onNavtexFec?.({ clean: v.getUint32(1, false), repaired: v.getUint32(5, false), lost: v.getUint32(9, false) });
      }

    } else if (name === 'wefax') {
      if (t === 0x01) {
        if (u8.length < 9) return;
        const ln = v.getUint32(1, false);
        const w  = v.getUint32(5, false);
        this.cb.onImageLine?.(ln, w, u8.subarray(9));
        // ★ Once the server reports phases (0x04), a line alone does not light the dot — noise draws lines too.
        if (!this.wefaxPhases) this.cb.onDot('rx');
      } else if (t === 0x04 && u8.length >= 2) {
        /* ★★ WHAT PART OF THE TRANSMISSION IS ARRIVING (Stuart, 2026-10-04): standing by when nothing is, green
         *  and named while it is — start tone, phasing, the chart — and the stop tone. wefax_decoder onPhase. */
        this.wefaxPhases = true;
        const p = u8[1];
        this.cb.onStatus(['standing by', 'start tone', 'phasing', 'receiving chart', 'stop tone'][p] ?? 'receiving');
        this.cb.onDot(p >= 1 && p <= 3 ? 'rx' : 'idle');
      } else if (t === 0x08) {
        /* ★★ WHY A CHART WAS OR WAS NOT PHASED (2026-10-07) — the decoder's own words, filed with the client's decisions
         *  so Export logs answers "the start tone was caught but the chart is not phased" after the fact. */
        noteDecision('wefax', utf8(u8.subarray(1)));
      } else if (t === 0x02) { this.cb.onStatus('START received'); this.cb.onDot('sync'); }
      else if (t === 0x03) {
        this.cb.onStatus('transmission complete');
        this.cb.onDot('active');
        this.cb.onImageDone?.();
      }

    } else if (name === 'hell') {
      // ★ FELD HELL: a strip's grey line, drawn at its own place (a re-sent line replaces itself); 0x02 = a fresh decoder,
      //   so the next line starts a new page (DecoderImageCanvas hellLine / hellStart).
      if (t === 0x01) {
        if (u8.length < 9) return;
        this.cb.onImageLine?.(v.getUint32(1, false), v.getUint32(5, false), u8.subarray(9));
      } else if (t === 0x02) {
        this.cb.onImageStart?.(0, 0);
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
        // ★ WPM to a WHOLE number (was one decimal): the estimate wanders in the tenths on every
        //   chunk, so the status string changed on nearly every message and each change re-rendered
        //   the whole receiver screen (audit 2026-10-03). Nobody keys at 18.3 WPM.
        this.cb.onStatus(`${Math.round(pitch)}Hz · ${Math.round(wpm)} WPM · ${confName}`);
      } else if (t === 0x11) {
        if (u8.length < 9) return;
        this.cb.onStatus(`${Math.round(v.getFloat32(1, false))}Hz · ${Math.round(v.getFloat32(5, false))} WPM`);
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
        guard('decoder', 'segments', () => {
          const segs = JSON.parse(utf8(u8.subarray(13, 13 + jlen)));
          if (Array.isArray(segs)) {
            for (const seg of segs) {
              if (seg?.completed && seg.text) this.cb.onText(seg.text + '\n');
            }
            this.cb.onDot('active');
          }
        }, `jlen=${jlen}`);
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
