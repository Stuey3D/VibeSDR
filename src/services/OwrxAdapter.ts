/**
 * OwrxAdapter — OpenWebRX / OpenWebRX+ behind the SDRBackend contract
 * (v3 brief §6). Single multiplexed WebSocket: JSON text control + binary FFT
 * (type 1) + binary audio (type 2/4). Verified against the OpenWebRX+ source in
 * reference/openwebrx-master (htdocs/openwebrx.js, lib/Demodulator.js).
 *
 * PHASE 2 MILESTONE 1 (this file): connect + handshake + config + waterfall, with
 * client-side view slicing, tune (offset within profile + auto profile-switch),
 * mode/bandwidth, profiles, and S-meter. AUDIO is intentionally NOT handled here —
 * per the brief the audio socket must move into the native engine so background
 * audio survives JS suspend (next phase). Type-2/4 frames are counted and ignored
 * for now; the waterfall is fully functional in TS.
 *
 * Architectural rule (brief §6.5): the FFT row always spans the WHOLE profile
 * (center_freq ± samp_rate/2). serverSideZoom=false → setView slices locally.
 */

import type { SDRMode, SDRStatus } from './UberSDRClient';
import type {
  SDRBackend, BackendCallbacks, BackendCapabilities, BackendKind, ProfileInfo, Aircraft,
} from './SDRBackend';
import { NativeModules } from 'react-native';
import { decodeOwrxFftFrame, OwrxAudioDecoder } from './imaAdpcm';
import { USER_AGENT } from '../constants/version';

const Vibe = NativeModules.VibePowerModule as {
  startExternalAudio?: (rate: number, pauseMode?: string) => void;
  pushExternalPcm?: (b64: string, rate: number, channels: number) => void;
  /** ★ Widen the player's cushion for a decoder that emits in blocks. 0 = analogue default. */
  setAudioBurstDepth?: (ms: number) => void;
  stopExternalAudio?: () => void;
} | undefined;

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
/** Minimal Uint8Array → base64 (no dep; Hermes has no btoa). */
function bytesToBase64(b: Uint8Array): string {
  let out = '', i = 0;
  const n = b.length;
  for (; i + 2 < n; i += 3) {
    const v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
    out += B64[(v >> 18) & 63] + B64[(v >> 12) & 63] + B64[(v >> 6) & 63] + B64[v & 63];
  }
  const rem = n - i;
  if (rem === 1) { const v = b[i] << 16; out += B64[(v >> 18) & 63] + B64[(v >> 12) & 63] + '=='; }
  else if (rem === 2) { const v = (b[i] << 16) | (b[i + 1] << 8); out += B64[(v >> 18) & 63] + B64[(v >> 12) & 63] + B64[(v >> 6) & 63] + '='; }
  return out;
}

/** Longest common leading substring across the strings (for deriving an SDR's
 *  name from its profiles' "{sdrName} {profileName}" labels). */
function commonPrefix(strs: string[]): string {
  if (!strs.length) return '';
  let pre = strs[0];
  for (let i = 1; i < strs.length; i++) {
    let k = 0;
    while (k < pre.length && k < strs[i].length && pre[k] === strs[i][k]) k++;
    pre = pre.slice(0, k);
    if (!pre) break;
  }
  return pre;
}

const B64_INV = (() => { const t = new Int8Array(128).fill(-1); for (let i = 0; i < B64.length; i++) t[B64.charCodeAt(i)] = i; return t; })();
/** Minimal base64 → Uint8Array (Hermes has no atob). Ignores '=' padding/whitespace. */
function base64ToBytes(s: string): Uint8Array {
  const out: number[] = [];
  let acc = 0, bits = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    const v = c < 128 ? B64_INV[c] : -1;
    if (v < 0) continue;                     // '=' / newline / stray char
    acc = (acc << 6) | v; bits += 6;
    if (bits >= 8) { bits -= 8; out.push((acc >> bits) & 0xff); }
  }
  return Uint8Array.from(out);
}

/** Unpack OWRX's WEFAX run-length-encoded scanline (MessagePanel.js port):
 *  byte<128 → literal run of (n+1) following bytes; byte>=128 → repeat the next
 *  byte (n-128+2) times. */
function faxRleDecode(rle: Uint8Array): Uint8Array {
  const out: number[] = [];
  for (let x = 0; x < rle.length; ) {
    const c = rle[x];
    if (c < 128) { for (let k = 0; k <= c; k++) out.push(rle[x + 1 + k] ?? 0); x += c + 2; }
    else { const b = rle[x + 1] ?? 0; for (let k = 0; k < c - 128 + 2; k++) out.push(b); x += 2; }
  }
  return Uint8Array.from(out);
}

const OWRX_AUDIO_RATE = 12000;   // output_rate (type-2)
const OWRX_HD_RATE = 48000;      // hd_output_rate (type-4 — WFM/wide)
// Fixed waterfall row width. Every emitted spectrum slice is resampled to this
// so the renderer's history texture never reallocates (a length change wipes
// the whole waterfall — server FFT frames jitter in size on digital profiles).
const OWRX_OUT_BINS = 1024;

/** Resample a dB FFT row to a fixed bin count. Down-samples with MAX pooling
 *  (peak-preserving — narrow carriers survive) and up-samples with linear
 *  interpolation (smooth). Returns the input untouched if already the right size. */
function resampleRow(src: Float32Array, outN: number): Float32Array {
  const n = src.length;
  if (n === outN || n === 0) return n === outN ? src : new Float32Array(outN);
  const out = new Float32Array(outN);
  if (n > outN) {
    // down: each output bin is the peak of its source span
    for (let i = 0; i < outN; i++) {
      const a = Math.floor((i * n) / outN);
      const b = Math.max(a + 1, Math.floor(((i + 1) * n) / outN));
      let m = -Infinity;
      for (let j = a; j < b && j < n; j++) if (src[j] > m) m = src[j];
      out[i] = m;
    }
  } else {
    // up: linear interpolation across source bins
    const step = (n - 1) / (outN - 1);
    for (let i = 0; i < outN; i++) {
      const p = i * step, a = Math.floor(p), t = p - a;
      const v0 = src[a], v1 = src[Math.min(n - 1, a + 1)];
      out[i] = v0 + (v1 - v0) * t;
    }
  }
  return out;
}

// Internal SDRMode → OWRX wire modulation. Gated on the server `modes` list at
// runtime; cwu/cwl collapse to 'cw' (offset convention handles the sideband).
const MODE_TO_WIRE: Record<SDRMode, string> = {
  usb: 'usb', lsb: 'lsb', am: 'am', sam: 'sam',
  fm: 'nfm', nfm: 'nfm', cwu: 'cw', cwl: 'cw', wfm: 'wfm',
};

const OWRX_CAPS: BackendCapabilities = {
  profiles:       true,
  serverSideZoom: false,    // FFT covers the whole profile; we slice client-side
  smeter:         'message',
  freqRange:      [0, 30_000_000],   // refined per profile from config
  chat:           false,
  serverNR:       false,
  // OWRX filters are wide for broadcast FM (server-controlled) and narrower for
  // SSB/CW; the UI clamps per mode. Generous default, WFM effectively server-side.
  maxBandwidth:   { default: 12000, fm: 96000, nfm: 12000 },
};

interface OwrxConfig {
  centerFreq: number;     // Hz, profile center
  sampRate: number;       // Hz, profile bandwidth (full FFT span)
  fftSize: number;
  fftCompression: 'none' | 'adpcm';
  audioCompression: 'none' | 'adpcm';
}

/** Great-circle distance, km. Aircraft send POSITION; range is ours to compute. */
function haversineKm(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const R = 6371;
  const dLat = (lat2 - lat1) * Math.PI / 180;
  const dLon = (lon2 - lon1) * Math.PI / 180;
  const a = Math.sin(dLat / 2) ** 2 +
            Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) *
            Math.sin(dLon / 2) ** 2;
  return Math.round(R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a)));
}

/** Initial bearing from the receiver to the aircraft, degrees. */
function bearingDeg(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const p1 = lat1 * Math.PI / 180, p2 = lat2 * Math.PI / 180;
  const dl = (lon2 - lon1) * Math.PI / 180;
  const y = Math.sin(dl) * Math.cos(p2);
  const x = Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl);
  return Math.round((Math.atan2(y, x) * 180 / Math.PI + 360) % 360);
}

export class OwrxAdapter implements SDRBackend {
  readonly kind: BackendKind = 'owrx';
  // Per-instance copy so freqRange can be refined to the active profile window
  // (OWRX profiles are anywhere — HF to VHF/UHF — not the UberSDR 0–30 MHz range).
  readonly caps: BackendCapabilities = { ...OWRX_CAPS, freqRange: [0, 30_000_000] };
  readonly uuid: string;

  private ws: WebSocket | null = null;
  private cb: BackendCallbacks;
  private wsUrl: string;
  private httpBase: string;   // http(s)://host:port — for /status.json polling
  private statusTimer: ReturnType<typeof setInterval> | null = null;
  // ── Incoming-rate readout ───────────────────────────────────────────────────
  // ★ OWRX showed no KB/s or fps at all. It has always measured frame TIMING for
  // the link bars, but never counted frames or bytes — so the connection readout
  // that every other backend fills in was simply blank here. It matters MORE on
  // OWRX than anywhere else: the rate cannot be turned down (fps/fft_fps/fft_size
  // are all ignored by the server), so knowing what it is costing you is the only
  // information available before deciding to move to the watch's own wifi.
  private rateTimer: ReturnType<typeof setInterval> | null = null;
  private fftFrames = 0;
  /** ALL inbound bytes on this socket — FFT *and* audio. See the case 2/4 note. */
  private fftBytes  = 0;

  // server/profile state
  private cfg: OwrxConfig | null = null;
  private profiles: ProfileInfo[] = [];
  private modes: string[] | null = null;     // server-reported mode ids (gating)
  private serverModes: { id: string; name: string; digital: boolean; type?: string; underlying?: string[]; ifRate?: number; bandpass?: { low_cut: number; high_cut: number } }[] = [];
  private serverVersion = '';

  // tuned state (absolute Hz / internal model)
  private freq = 0;
  private mode: SDRMode = 'am';
  // Active secondary decoder (SSTV/Fax/… = OWRX 'digimode') running ON TOP of the
  // analog carrier in `mode`. null = none. Kept separate so the carrier demod
  // stays user-visible/selectable and audio is never on the wrong sideband.
  private secondaryDecoder: string | null = null;
  // Where in the audio passband the secondary decoder listens (Hz). OWRX defaults
  // to 1000 — that's the standard RTTY/digimode audio centre, so tuning the VFO
  // to the carrier (e.g. 4582 kHz USB for DWD) decodes without a passband tuner.
  private secondaryOffset = 1000;
  private squelchLevel = -150;   // dB; -150 = off (open). dspcontrol squelch_level.
  private bwLow = -4000;
  private bwHigh = 4000;
  private audioServiceId = 0;                 // DAB: selected programme within ensemble
  private dabProgrammes: { id: number; name: string }[] = [];
  private dabEnsemble = '';                    // DAB: ensemble (multiplex) label, cached
  private lastDabSig = '';                      // dedupe key for per-second DAB metadata
  private lastVoiceSpeaker = '';                 // dedupe key for digital-voice callers
  private rdsPs = '';                            // cached RDS programme-service name
  private owrxBookmarks: { name: string; frequency: number; mode?: string; repeater?: boolean }[] = [];
  private owrxDials: { name: string; frequency: number; mode?: string; repeater?: boolean }[] = [];
  private voiceTimer: ReturnType<typeof setTimeout> | null = null;  // digital-voice idle clear
  private dabRateScale = 1;                      // DAB speed-correction factor (1 = off)

  // current view (client-side); defaults to whole profile on first config
  private viewCenter = 0;
  private viewBw = 0;
  private viewInit = false;
  private followVfo = true;               // VFO lock (true = view follows VFO)

  // last full FFT row (full profile span) — re-sliced on local view changes
  private lastRow: Float32Array | null = null;

  // Link-quality (connection signal meter): OWRX has no ping/RTT, so we score
  // the connection from FFT frame inter-arrival timing — steady frames = strong,
  // stalls/gaps = weak. Mirrors the UberSDR 0–3 bar indicator.
  private gapHist: number[] = [];
  private lastFrameAt = 0;
  private connectedAt = 0;
  private lastLink: -1 | 0 | 1 | 2 | 3 = -1;

  private started = false;
  private specPaused = false;   // background/lock: skip FFT processing, keep audio
  private lonSent = false;
  private receiverPos: { lat: number; lon: number } | null = null;      // receiver longitude emitted once
  private dspStarted = false;     // dspcontrol start re-asserted after demod (web-client order)
  private audioStarted = false;
  private audioDec = new OwrxAudioDecoder();    // type-2 (output_rate, 12k)
  private hdAudioDec = new OwrxAudioDecoder();   // type-4 (hd_output_rate, 48k — WFM)

  constructor(baseUrl: string, uuid: string, callbacks: BackendCallbacks) {
    this.uuid = uuid;
    this.cb = callbacks;
    this.wsUrl = OwrxAdapter.toWsUrl(baseUrl);
    this.httpBase = OwrxAdapter.toHttpBase(baseUrl);
  }

  /** ws(s)/http(s)://host:port[/path] → http(s)://host:port (for /status.json). */
  static toHttpBase(baseUrl: string): string {
    let u = baseUrl.trim().replace(/\/+$/, '');
    if (u.startsWith('wss://'))        u = 'https://' + u.slice('wss://'.length);
    else if (u.startsWith('ws://'))    u = 'http://'  + u.slice('ws://'.length);
    else if (!/^https?:\/\//.test(u))  u = 'http://'  + u;
    return u.replace(/\/ws\/?$/, '');
  }

  /** Poll /status.json (public, no admin) and build per-SDR usage keyed by sdrId.
   *  An SDR appears in status.json only when ACTIVE (a live user or a background
   *  task is on it), so presence = in-use. Profile ids are `sdrId|profileId` and
   *  WS profile names are `"{sdrName} {profileName}"`, so the SDR name is the
   *  status.json source name that prefixes a group's profile names. */
  private async pollStatus(): Promise<void> {
    try {
      const res = await fetch(this.httpBase + '/status.json');
      if (!res.ok) return;
      const j: any = await res.json();
      // Receiver location → ITU region (MW 9/10 kHz). Emit once.
      const lon = j?.receiver?.gps?.lon;
      if (typeof lon === 'number' && !this.lonSent) { this.lonSent = true; this.cb.onReceiverLon?.(lon); }
      // Keep the full position: ADS-B sends aircraft POSITIONS, not distances, so
      // the range from the receiver has to be computed here.
      const rlat = j?.receiver?.gps?.lat;
      if (typeof rlat === 'number' && typeof lon === 'number') {
        this.receiverPos = { lat: rlat, lon };
      }
      const sdrs: any[] = Array.isArray(j?.sdrs) ? j.sdrs : [];

      // Group the WS profiles by sdrId (the id prefix).
      const groups: Record<string, ProfileInfo[]> = {};
      for (const p of this.profiles) {
        const sid = p.id.includes('|') ? p.id.split('|')[0] : p.id;
        (groups[sid] ??= []).push(p);
      }
      const map: Record<string, { name: string; inUse: boolean; activeProfileId?: string }> = {};
      for (const [sid, items] of Object.entries(groups)) {
        // Match this group to a status.json source: the SDR is the one whose
        // name prefixes the group's "{sdrName} {profileName}" entries (longest
        // wins). Present in status.json = active/in-use.
        let match: any = null; let best = '';
        for (const s of sdrs) {
          const n = String(s?.name ?? '').trim();
          if (n && items[0].name.startsWith(n + ' ') && n.length > best.length) { best = n; match = s; }
        }
        const name = best || commonPrefix(items.map((i) => i.name)).replace(/\s+\S*$/, '').trim() || sid;
        // The currently-tuned profile per SDR isn't in upstream status.json, but
        // our patch (contrib) adds `active_profile` (the profile's display name).
        // Map it back to the WS profile id: full name = "{sdrName} {activeName}".
        let activeProfileId: string | undefined;
        const activeName = match ? String(match.active_profile ?? match.activeProfile ?? '').trim() : '';
        if (activeName) activeProfileId = items.find((i) => i.name === `${name} ${activeName}` || i.name === activeName)?.id;
        map[sid] = { name, inUse: !!match, activeProfileId };
      }
      this.cb.onSdrUsage?.(map);
    } catch { /* server may not expose it / be offline — leave usage unknown */ }
  }

  /** 1 Hz frames/bytes readout. Rung 1 and never settling: OWRX has no rate
   *  ladder to climb (see LADDERS — it is omitted deliberately), so it must not
   *  clamp the link bars, which take the WORST of gap quality and rung. */
  private startRateMeter(): void {
    if (this.rateTimer) return;
    this.rateTimer = setInterval(() => {
      const fps  = this.fftFrames;
      const kbps = this.fftBytes / 1024;
      this.fftFrames = 0; this.fftBytes = 0;
      this.cb.onLinkRate?.(1, false, fps, kbps);
    }, 1000);
  }
  private stopRateMeter(): void {
    if (this.rateTimer) { clearInterval(this.rateTimer); this.rateTimer = null; }
    this.fftFrames = 0; this.fftBytes = 0;
  }

  private startStatusPoll(): void {
    if (this.statusTimer) return;
    this.pollStatus();
    this.statusTimer = setInterval(() => this.pollStatus(), 15000);
  }
  private stopStatusPoll(): void {
    if (this.statusTimer) { clearInterval(this.statusTimer); this.statusTimer = null; }
    this.stopRateMeter();
  }

  /** http(s)://host:port[/path] → ws(s)://host:port/ws/ (trailing slash required;
   *  the StaticRoute is exactly "/ws/", a bare /ws 404s). */
  static toWsUrl(baseUrl: string): string {
    let u = baseUrl.trim().replace(/\/+$/, '');
    if (u.startsWith('https://'))      u = 'wss://' + u.slice('https://'.length);
    else if (u.startsWith('http://'))  u = 'ws://'  + u.slice('http://'.length);
    else if (!/^wss?:\/\//.test(u))    u = 'ws://'  + u;
    return u + '/ws/';
  }

  // ── lifecycle ────────────────────────────────────────────────────────────
  connect(frequency?: number, mode?: SDRMode): Promise<void> {
    if (frequency != null) this.freq = frequency;
    if (mode) this.mode = mode;
    return new Promise((resolve, reject) => {
      let settled = false;
      let ws: WebSocket;
      // ★ Identify ourselves to somebody else's receiver — see USER_AGENT.
      try { ws = new (WebSocket as any)(this.wsUrl, null,
                                        { headers: { 'User-Agent': USER_AGENT } }) as WebSocket; }
      catch (e) { reject(e); return; }
      this.ws = ws;
      ws.binaryType = 'arraybuffer';

      ws.onopen = () => {
        this.dbg('WS open ' + this.wsUrl);
        ws.send('SERVER DE CLIENT client=vibesdr type=receiver');
        // connectionproperties + start are sent on the CLIENT DE SERVER ack.
      };
      ws.onmessage = (e) => {
        try {
          if (typeof e.data === 'string') this.onText(e.data, () => { if (!settled) { settled = true; resolve(); } });
          else this.onBinary(new Uint8Array(e.data as ArrayBuffer));
        } catch (err: any) { this.dbg('msg err: ' + (err?.message ?? err)); }
      };
      ws.onerror = () => { this.dbg('WS error'); if (!settled) { settled = true; reject(new Error('OpenWebRX WebSocket error')); } };
      ws.onclose = (ev) => {
        this.dbg('WS close ' + ev.code);
        this.lastLink = 0; this.cb.onLink?.(0);   // drop the connection signal meter
        this.stopStatusPoll();                     // stop polling a dead server
        // This handler is nulled before our own intentional close()s (destroy /
        // disconnectSocket), so reaching here ALWAYS means an unexpected drop —
        // i.e. the OWRX server crashed/restarted. Tell the UI to hold + warn.
        this.cb.onDisconnect();
        this.cb.onServerLost?.();
        if (!settled) { settled = true; reject(new Error('OpenWebRX closed (' + ev.code + ')')); }
      };
    });
  }

  destroy(): void {
    this.started = false;
    this.stopStatusPoll();
    if (this.audioStarted) { Vibe?.stopExternalAudio?.(); this.audioStarted = false; }
    const ws = this.ws; this.ws = null;
    if (ws) { try { ws.onclose = null; ws.close(); } catch {} }
  }

  /** Pause-disconnect: close the WS to free the server slot, but DON'T tear down
   *  the native audio session — the native engine keeps showing the paused /
   *  disconnected card (disconnectForPause manages it). A fresh adapter is built
   *  on resume via the JS fullReconnect, which is what stops the native engine.
   *  (destroy() here would call stopExternalAudio and drop the lock-screen card.) */
  disconnectSocket(): void {
    this.started = false;
    this.stopStatusPoll();
    const ws = this.ws; this.ws = null;
    if (ws) { try { ws.onclose = null; ws.close(); } catch {} }
  }

  // ── inbound: text/JSON ───────────────────────────────────────────────────
  private onText(data: string, onReady: () => void): void {
    if (data.startsWith('CLIENT DE SERVER')) {
      const params = Object.fromEntries(
        data.slice(17).split(' ').map((p) => { const a = p.split('='); return [a[0], a.slice(1).join('=')]; }),
      );
      this.serverVersion = (params['version'] || '').replace(/^v/i, '');   // "v1.2.116" → "1.2.116"
      this.dbg('server ack: ' + (params['server'] || '?') + ' ' + this.serverVersion);
      // The ack's `server` is "openwebrx" even on the + fork, so sniff the landing
      // page for the "OpenWebRX+" marker to label it correctly in the menu footer.
      this.detectServerName();
      // Negotiate, then start the DSP.
      this.send({ type: 'connectionproperties', params: { output_rate: 12000, hd_output_rate: 48000 } });
      this.send({ type: 'dspcontrol', action: 'start' });
      this.connectedAt = Date.now();
      this.cb.onConnect();
      this.evalLink();          // seed the connection signal meter (tentative bars)
      this.startStatusPoll();   // begin polling which SDRs are in use
      onReady();
      return;
    }
    let json: any;
    try { json = JSON.parse(data); } catch { return; }
    switch (json.type) {
      case 'config':   this.onConfig(json.value || {}); break;
      case 'profiles': this.onProfiles(json.value || []); break;
      case 'clients': { const n = Number(json.value); if (Number.isFinite(n)) this.cb.onClients?.(n); break; }
      case 'chat_message': this.cb.onChatMessage?.(String(json.name ?? '?'), String(json.text ?? ''), json.color); break;
      case 'modes': {
        const arr = (json.value || []) as any[];
        this.serverModes = arr.map((m) => ({
          id: m.modulation ?? m.id ?? String(m),
          name: m.name ?? String(m.modulation ?? m.id ?? m).toUpperCase(),
          digital: (m.type ?? 'analog') !== 'analog',
          // 'digimode' = a SECONDARY decoder (SSTV/Fax/Packet/…) that runs on top
          // of an analog `underlying` mode via secondary_mod — not a primary demod.
          type: m.type,
          underlying: Array.isArray(m.underlying) ? m.underlying.map(String) : undefined,
          ifRate: typeof m.ifRate === 'number' ? m.ifRate : undefined,
          bandpass: m.bandpass,
        }));
        this.modes = this.serverModes.map((m) => m.id);
        this.buildBandwidthCaps();   // per-mode slider caps from each mode's bandpass
        this.cb.onModes?.(this.serverModes.map((m) => ({ id: m.id, label: m.name, digital: m.digital })));
        // If the modes list lands after config (so the start_mod passband wasn't
        // known yet), apply it now and resend so the filter widens to match.
        //
        // AND finish setting up a start_mod DIGIMODE, which needs this list to know
        // it must run as a secondary decoder. Without this the ADS-B profile loaded,
        // said "ADSB", and decoded nothing at all until you opened the demodulator
        // picker and re-selected it by hand.
        if (this.pendingStartMod) this.applyStartMod(this.pendingStartMod);
        if (this.started) {
          this.applyModeBandpass();
          this.sendDemod();
          this.cb.onStatus(this.getStatus());
        }
        break;
      }
      case 'smeter':
        // The wire value is LINEAR power (from csdr squelch_and_smeter_cc); the
        // OWRX UI shows 10·log10(value) dB. Convert here so onSMeter is in dB.
        if (typeof json.value === 'number' && json.value > 0) {
          this.cb.onSMeter?.(10 * Math.log10(json.value));
        }
        break;
      case 'metadata': this.onMetadata(json.value || {}); break;
      case 'secondary_demod': this.onSecondaryDemod(json.value); break;
      case 'bookmarks':        // server-configured named bookmarks
        this.owrxBookmarks = ((json.value || []) as any[])
          .filter((b) => b && typeof b.frequency === 'number')
          // Repeater-DB entries carry an auto-generated "On-air, Nkm away. Last
          // updated …" description — use that signature to tag them vs user bookmarks.
          .map((b) => ({ name: String(b.name ?? b.modulation ?? b.frequency), frequency: b.frequency, mode: b.modulation,
                         repeater: /\bkm away\b|on[- ]air|repeater/i.test(String(b.description ?? '')) }));
        this.emitBookmarks();
        break;
      case 'dial_frequencies': // auto-detected mode markers (FT8 etc.) — also tunable
        this.owrxDials = ((json.value || []) as any[])
          .filter((d) => d && typeof d.frequency === 'number')
          .map((d) => ({ name: String(d.mode ?? d.frequency).toUpperCase(), frequency: d.frequency, mode: d.mode }));
        this.emitBookmarks();
        break;
      case 'sdr_error':
      case 'demodulator_error': this.cb.onError(String(json.value ?? 'OpenWebRX error')); break;
      case 'backoff': this.dbg('server backoff ' + json.value); break;
      default: this.dbg('unhandled msg type: ' + json.type);  // ignore-with-log = forward compat
    }
  }

  /** Adopt a profile's default mode — e.g. the ADSB profile starts on `adsb`.
   *
   *  A digimode has to be set up as a SECONDARY decoder or the server decodes
   *  nothing, and knowing that requires `serverModes` … which arrives AFTER `config`
   *  on connect. So this used to silently fall through to "just set the mode": the
   *  ADS-B profile loaded, showed ADSB, and produced no aircraft at all until you
   *  went into the demodulator picker and re-selected it (by which time the modes
   *  list had arrived). Remember the mode and re-apply it when the list lands. */
  private pendingStartMod: string | null = null;

  private applyStartMod(id: string): void {
    const sm = this.serverModes.find((m) => m.id === id);
    if (!sm) {
      // Modes haven't arrived yet — replay this the moment they do.
      this.pendingStartMod = id;
      this.mode = id as SDRMode;
      return;
    }
    this.pendingStartMod = null;
    if (sm.type === 'digimode' && sm.underlying?.length) {
      this.secondaryDecoder = id;
      const real = sm.underlying.filter((u) => u !== 'empty');
      this.mode = (real[0] ?? id) as SDRMode;
    } else {
      this.mode = id as SDRMode;
    }
  }

  private onConfig(c: any): void {
    const profileSwitch = 'sdr_id' in c || 'profile_id' in c;
    if ('allow_chat' in c) this.cb.onChatEnabled?.(!!c.allow_chat);
    if (!this.cfg) this.cfg = { centerFreq: 0, sampRate: 0, fftSize: 1024, fftCompression: 'none', audioCompression: 'none' };
    if ('center_freq' in c)       this.cfg.centerFreq = c.center_freq;
    if ('samp_rate' in c)         this.cfg.sampRate = c.samp_rate;
    if ('fft_size' in c)          this.cfg.fftSize = c.fft_size;
    if ('fft_compression' in c)   this.cfg.fftCompression = c.fft_compression;
    if ('audio_compression' in c) this.cfg.audioCompression = c.audio_compression;
    const startMod = ('start_mod' in c && c.start_mod) ? String(c.start_mod) : null;
    if (startMod) this.applyStartMod(startMod);
    if ('start_offset_freq' in c && this.cfg.centerFreq) this.freq = this.cfg.centerFreq + c.start_offset_freq;

    // Server/profile preset DSP defaults (mirrors the OWRX web client: a profile
    // can ship a default squelch and an initial NR level that auto-apply on load —
    // e.g. a 2 m NFM profile with a −65 dB squelch). We honour them on connect and
    // every profile switch so the user lands on the owner's intended settings
    // instead of a stale/off value. The squelch rides out on the sendDemod() below;
    // NR goes via its own connectionproperties message. The UI sliders are synced
    // via onServerDspDefaults so they reflect the preset.
    const dspDefaults: { squelchDb?: number; nrEnabled?: boolean; nrThreshold?: number } = {};
    if ('initial_squelch_level' in c) {
      // Integer = preset level (dB); anything else (e.g. null) = squelch off.
      this.squelchLevel = Number.isInteger(c.initial_squelch_level) ? c.initial_squelch_level : -150;
      dspDefaults.squelchDb = this.squelchLevel;
    }
    if ('initial_nr_level' in c) {
      const on = Number.isInteger(c.initial_nr_level);
      const threshold = on ? Math.max(0, Math.round(c.initial_nr_level)) : 0;
      this.setNr(threshold);
      dspDefaults.nrEnabled = on;
      dspDefaults.nrThreshold = threshold;
    }
    if (dspDefaults.squelchDb !== undefined || dspDefaults.nrEnabled !== undefined) {
      this.cb.onServerDspDefaults?.(dspDefaults);
    }
    // Ensure we have a tuned frequency inside the profile before centring the view.
    if ((this.freq === 0 || Math.abs(this.freq - this.cfg.centerFreq) > this.cfg.sampRate / 2) && this.cfg.centerFreq) {
      this.freq = this.cfg.centerFreq;
    }

    // Centre the view on the VFO (a sensible default span), or re-centre on a switch.
    if (!this.viewInit || profileSwitch) {
      this.viewCenter = this.freq || this.cfg.centerFreq;
      this.viewBw = this.cfg.sampRate;
      this.viewInit = true;
      this.lastRow = null;   // clear stale waterfall on profile change
      if (profileSwitch) {
        this.audioDec.reset(); this.hdAudioDec.reset();   // ADPCM restarts per profile
        this.gapHist = []; this.lastFrameAt = 0;          // frame rate may change — reset link timing
        this.audioServiceId = 0; this.dabProgrammes = []; this.dabEnsemble = ''; this.lastDabSig = ''; this.rdsPs = ''; this.lastVoiceSpeaker = '';  // new ensemble/station
        // A decoder you turned on BY HAND doesn't carry across profiles.
        //
        // But this ran AFTER the new profile's start_mod was adopted and BEFORE the
        // demod was sent — so it also wiped the profile's OWN default decoder. The
        // ADS-B profile therefore loaded, said "ADSB", and sent secondary_mod=false:
        // the server decoded nothing at all until you opened the demodulator picker
        // and re-selected it by hand (which sets it again, with nothing left to clear
        // it). Clear the carried-over decoder, then re-assert the profile's own.
        this.secondaryDecoder = null;
        if (startMod) this.applyStartMod(startMod);
        this.cb.onMetadata?.({ programmes: [] });          // clear stale RDS/DAB labels + picker
      }
    }

    // Refine the tunable range to the active profile window so the UI's clamps
    // (which read caps.freqRange) don't peg VHF/UHF tunes to a 30 MHz ceiling.
    if (this.cfg.sampRate) {
      this.caps.freqRange = [this.cfg.centerFreq - this.cfg.sampRate / 2, this.cfg.centerFreq + this.cfg.sampRate / 2];
      this.buildBandwidthCaps();
    }
    this.dbg(`cfg cf=${this.cfg.centerFreq} sr=${this.cfg.sampRate} fft=${this.cfg.fftSize} freq=${this.freq} fftcomp=${this.cfg.fftCompression}`);
    // Profile's start_mod may be a wide mode (broadcast-FM profile → WFM): adopt
    // its server passband so the filter isn't stuck narrow until a manual re-tap.
    this.applyModeBandpass();
    // Send (or resend) the demod params now we know the profile window.
    this.started = true;
    this.sendDemod();
    // Match the OWRX web client: it sends the demod params THEN dspcontrol start
    // (Demodulator.start = set()+start), and re-runs that on every profile select.
    // Our single start at connect fires before the profile/mod exist, so the DSP
    // chain isn't (re)built around the profile's demod — fatal for DAB, where the
    // dablin chain must be assembled with mod=dab. Re-assert start after the demod
    // on first config and every profile switch.
    if (profileSwitch || !this.dspStarted) {
      this.send({ type: 'dspcontrol', action: 'start' });
      this.dspStarted = true;
      this.dbg('dspcontrol start (after demod)');
    }
    this.cb.onStatus(this.getStatus());
  }

  /** Extract the active caller from a digital-voice metadata message, or undefined
   *  if the message isn't a digital-voice protocol. Returns '' when the protocol
   *  IS digital voice but nobody is transmitting (idle), so the dedupe resets. */
  private digiVoiceState(v: any): { active: boolean; caller?: string } | null {
    const cs = (x: any) => (typeof x === 'string' && x.trim() ? x.trim() : undefined);
    const pair = (c?: string, t?: string) => (c ? (t ? `${c} → ${t}` : c) : undefined);
    switch (v?.protocol) {
      case 'DMR': {
        const caller = pair(cs(v.additional?.callsign) ?? cs(v.talkeralias) ?? cs(v.source), cs(v.target));
        return { active: v.sync === 'voice' || !!caller, caller };
      }
      case 'YSF': {
        const active = !!v.mode;
        return { active, caller: active ? cs(v.source) : undefined };
      }
      case 'DStar': case 'D-Star': {
        const caller = pair(cs(v.ourcall), cs(v.yourcall));
        return { active: v.sync === 'voice' || !!caller, caller };
      }
      case 'NXDN': case 'M17': {
        const caller = pair(cs(v.source), cs(v.target));
        return { active: !!caller, caller };
      }
      default: return null;
    }
  }

  /** Drop held live metadata (RDS name / digital-voice caller) and tell the UI to
   *  clear it — on retune (the old station no longer applies) and on voice idle.
   *  Only emits when something was cached, so it doesn't spam on every tune tick. */
  private clearLiveMeta(): void {
    if (this.voiceTimer) { clearTimeout(this.voiceTimer); this.voiceTimer = null; }
    if (this.rdsPs || this.lastVoiceSpeaker) {
      this.rdsPs = '';
      this.lastVoiceSpeaker = '';
      this.cb.onMetadata?.({});
    }
  }

  /** Merge server bookmarks + dial-frequency markers and push to the UI. */
  private emitBookmarks(): void {
    this.cb.onBookmarks?.([...this.owrxBookmarks, ...this.owrxDials]);
  }

  private onProfiles(list: any[]): void {
    this.profiles = list.map((p) => ({ id: String(p.id ?? p), name: String(p.name ?? p.id ?? p) }));
    this.cb.onProfiles?.(this.profiles);
    if (this.statusTimer) this.pollStatus();   // now we can map sdrId → name/usage
  }

  /** RDS (broadcast FM) and DAB ensemble/programme metadata → normalised
   *  StationMeta. Both arrive on the same `metadata` message; RDS is keyed by
   *  protocol:'WFM' (ps = station name, radiotext = scrolling text), DAB by
   *  mode:'DAB' (ensemble_label + programmes map). */
  private onMetadata(v: any): void {
    // Digital voice (DMR/YSF/D-Star/NXDN/M17): surface the active caller's
    // callsign (→ talkgroup/target) as the station name, so the VTS pops on each
    // NEW caller — uniform with RDS/DAB. Deduped on the speaker string.
    const dv = v ? this.digiVoiceState(v) : null;
    if (dv) {                          // a digital-voice protocol message
      if (dv.caller && dv.caller !== this.lastVoiceSpeaker) {
        this.lastVoiceSpeaker = dv.caller;
        const badge = String(v.protocol).toUpperCase().replace('DSTAR', 'D-STAR');
        this.cb.onMetadata?.({ stationName: dv.caller, badge });   // pop on each new caller
      }
      // Keep the caller shown for the WHOLE transmission. The callsign rides only
      // on SOME voice frames, but the slot stays `active` throughout, so re-arm
      // the idle-clear timer on any active frame (not just callsign-bearing ones)
      // — that stops the caller dropping to the bookmark mid-over. The grace
      // (3.5s) then clears it shortly after the transmission actually ends.
      if ((dv.active || dv.caller) && this.lastVoiceSpeaker) {
        if (this.voiceTimer) clearTimeout(this.voiceTimer);
        this.voiceTimer = setTimeout(() => { this.voiceTimer = null; this.clearLiveMeta(); }, 3500);
      }
      return;
    }
    if (v && v.mode === 'DAB') {
      // DAB metadata is INCREMENTAL — the server sends partial updates (a
      // timestamp every second, programmes once, ensemble once). Cache the
      // programme list + ensemble so a timestamp-only message doesn't wipe them.
      if (v.programmes && typeof v.programmes === 'object') {
        this.dabProgrammes = Object.entries(v.programmes).map(([id, name]) => ({ id: Number(id), name: String(name) }));
        // OWRX does NOT auto-play a DAB service — the web client selects the first
        // programme once the list lands (MetaPanel.js:707). Without an
        // audio_service_id the server outputs NO audio, so adopt the first and
        // RE-SEND the demod to actually start it.
        if (this.dabProgrammes.length && !this.dabProgrammes.some((p) => p.id === this.audioServiceId)) {
          this.audioServiceId = this.dabProgrammes[0].id;
          this.dbg('DAB adopt service ' + this.audioServiceId + ' (' + this.dabProgrammes[0].name + ')');
          this.audioDec.reset(); this.hdAudioDec.reset();
          this.sendDemod();
        }
      }
      if (typeof v.ensemble_label === 'string') this.dabEnsemble = v.ensemble_label;
      // The selected programme's label is the "station name"; fall back to ensemble.
      const selName = this.dabProgrammes.find((p) => p.id === this.audioServiceId)?.name;
      // Dedupe — DAB resends metadata every second (timestamp ticks); only emit
      // when the parts the UI cares about actually change, to avoid per-second
      // React churn (the picker + station readout re-rendering needlessly).
      const sig = (selName ?? '') + '|' + this.dabEnsemble + '|' + this.dabProgrammes.map((p) => p.id + ':' + p.name).join(',');
      if (sig !== this.lastDabSig) {
        this.lastDabSig = sig;
        this.cb.onMetadata?.({
          ensemble: this.dabEnsemble || undefined,
          stationName: selName,
          programmes: this.dabProgrammes,   // always the full cached list
          badge: selName ? 'RDS' : undefined,  // live broadcast-data mark (same as FM RDS)
        });
      }
      return;
    }
    // RDS (WFM). ps = programme service name (8 chars), radiotext = scrolling.
    // Messages are incremental (ps and radiotext arrive separately), so cache ps
    // — a radiotext-only update must not blank the station name (which would
    // dismiss the held popup). Cleared on mode/profile change.
    if (v && (v.protocol === 'WFM' || 'ps' in v || 'radiotext' in v)) {
      if (typeof v.ps === 'string' && v.ps.trim()) this.rdsPs = v.ps.trim();
      this.cb.onMetadata?.({
        stationName: this.rdsPs || undefined,
        text: typeof v.radiotext === 'string' ? v.radiotext.trim() || undefined : undefined,
        badge: this.rdsPs ? 'RDS' : undefined,
      });
    }
  }

  /** Secondary-demod output. OWRX decodes server-side and streams the result over
   *  the SAME WS once secondary_mod is set. Two shapes:
   *   • IMAGES (SSTV/Fax) — a size header, per-scanline pixel messages, then a Fax
   *     end marker → translated to the DecoderImageCanvas contract (onDecoderImage).
   *   • TEXT RECORDS (Packet/POCSAG/FLEX/DSC/ISM/HFDL/ACARS/ADSB/WSJT) — one JSON
   *     record per decode, keyed by `mode` → formatted to a line (onDecoderText). */
  private onSecondaryDemod(v: any): void {
    // Character-stream decoders (RTTY, CW skimmer, BPSK/PSK, etc.) send the
    // decoded output as a plain STRING (OWRX secondary_demod_push_data), not a
    // record. Append it raw (printable + newlines only, like the web client).
    if (typeof v === 'string') {
      const txt = v.replace(/[^\n\x20-\x7e]/g, '');
      if (txt) this.cb.onDecoderText?.(txt, false);
      return;
    }
    if (!v || typeof v !== 'object') return;
    const kind: 'sstv' | 'fax' | null = v.mode === 'SSTV' ? 'sstv' : v.mode === 'Fax' ? 'fax' : null;
    if (!kind) {
      // NB: text records (POCSAG/FLEX/packet) legitimately carry a `message`
      // field (the page/comment text) — do NOT treat that as a debug skip.
      const rec = this.secondaryRecordToText(v);
      if (rec) this.cb.onDecoderText?.(rec.replace ? rec.text : rec.text + '\n', rec.replace);
      return;
    }
    // SSTV/Fax debug messages carry a `message` and no pixels — skip those only.
    if ('message' in v && v.width == null && v.line == null) return;
    // Header: dimensions, no scanline → start a fresh image.
    if (v.width > 0 && v.height > 0 && v.line == null) {
      this.cb.onDecoderImage?.({ phase: 'start', kind, width: v.width, height: v.height });
      return;
    }
    // Fax end-of-image (crop to received lines).
    if (v.line >= 0 && v.ended && v.pixels == null) {
      this.cb.onDecoderImage?.({ phase: 'done', kind });
      return;
    }
    // Scanline.
    if (v.line >= 0 && v.width > 0 && typeof v.pixels === 'string') {
      let bytes = base64ToBytes(v.pixels);
      let px: Uint8Array;
      if (kind === 'sstv') {
        // OWRX sends BMP BGR triplets; the canvas wants RGB triplets.
        const w = v.width as number;
        px = new Uint8Array(w * 3);
        for (let x = 0; x < w; x++) {
          px[x * 3]     = bytes[x * 3 + 2] ?? 0;
          px[x * 3 + 1] = bytes[x * 3 + 1] ?? 0;
          px[x * 3 + 2] = bytes[x * 3]     ?? 0;
        }
      } else {
        // Fax: greyscale, optionally RLE-compressed → one byte per pixel.
        px = v.rle ? faxRleDecode(bytes) : bytes;
      }
      this.cb.onDecoderImage?.({ phase: 'line', kind, line: v.line, width: v.width, pixels: px });
    }
  }

  /** Format a text-decoder `secondary_demod` record into one readable line (or a
   *  replacing snapshot for the ADS-B aircraft list). Mirrors the fields the OWRX
   *  web MessagePanels show, condensed to a single line. Returns null to skip. */
  private secondaryRecordToText(v: any): { text: string; replace?: boolean } | null {
    const mode = v?.mode;
    const hhmmss = (ts: any): string => {
      // OWRX timestamps are MILLISECOND epochs (e.g. 1781472474000) — the old
      // code multiplied by 1000 → garbage date. Accept ms or sec epoch + ISO str.
      let d: Date | null = null;
      if (typeof ts === 'number' && ts > 0) d = new Date(ts > 1e12 ? ts : ts * 1000);
      else if (typeof ts === 'string' && ts) { const p = new Date(ts); d = isNaN(p.getTime()) ? null : p; }
      return d && !isNaN(d.getTime()) ? d.toISOString().slice(11, 19) : '';
    };
    const j = (...xs: any[]) => xs.filter((x) => x != null && x !== '').join(' ');
    switch (mode) {
      // Packet: APRS / AIS / SONDE
      case 'APRS': case 'AIS': case 'SONDE': {
        let m = v;
        if (m.type === 'thirdparty' && m.data) m = m.data;
        if (m.type === 'nmea') return null;                       // raw AIS NMEA — skip
        const src = m.source ?? (m.type === 'item' ? m.item : undefined) ?? m.object;
        const coord = (m.lat != null && m.lon != null) ? `(${(+m.lat).toFixed(3)},${(+m.lon).toFixed(3)})` : '';
        return { text: j(src, coord, m.comment ?? m.message) };
      }
      case 'Pocsag':                                              // PocsagMessagePanel
        return { text: j((v.address ?? '') + ':', v.message) };
      case 'FLEX': case 'POCSAG': {                               // PageMessagePanel (paging)
        const proto = (v.mode ?? '') + (v.baud ?? '') + (v.channel != null ? '/' + v.channel : '');
        return { text: j(hhmmss(v.timestamp), v.address, proto, v.message) };
      }
      case 'DSC':
        return { text: j(hhmmss(v.time ?? v.timestamp), v.src, v.dst ? '→ ' + v.dst : '', v.format, v.category, v.data) };
      case 'ISM': case 'WMBUS': {
        // ISM = sensor telemetry (TPMS tyre sensors, weather stations, …). Show
        // the device + its readings, like the desktop's attribute table but on a
        // line: append every extra scalar field (pressure_kPa, temperature_C, …).
        const skip = new Set(['mode', 'id', 'model', 'timestamp', 'mic', 'flags', 'channel']);
        const attrs = Object.entries(v)
          .filter(([k, val]) => !skip.has(k) && (typeof val === 'string' || typeof val === 'number' || typeof val === 'boolean') && val !== '')
          .map(([k, val]) => `${k}=${val}`);
        return { text: j(hhmmss(v.timestamp), v.id ?? '???', v.model, ...attrs) };
      }
      case 'HFDL': case 'VDL2': case 'ACARS': case 'UAT': case 'ADSB':
        return { text: j(hhmmss(v.timestamp), v.flight ?? v.aircraft ?? v.icao, v.type, v.message ?? v.data) };
      case 'ADSB-LIST': {                                          // live aircraft table → replace
        if (!Array.isArray(v.aircraft)) return null;

        // Emit the STRUCTURED list too. Flattening these to text on arrival is why
        // nothing but the decoder panel could ever use them — and the records are
        // rich: registry country, altitude, vertical rate, position, squawk.
        // Distance/bearing are ours to compute: the server sends POSITION, not range.
        this.cb.onAircraft?.(v.aircraft.map((a: any): Aircraft => {
          const lat = typeof a.lat === 'number' ? a.lat : undefined;
          const lon = typeof a.lon === 'number' ? a.lon : undefined;
          const rp = this.receiverPos;
          const hasPos = lat != null && lon != null && rp != null;
          return {
            icao:     String(a.icao ?? ''),
            flight:   a.flight ? String(a.flight).trim() : undefined,
            reg:      a.aircraft ? String(a.aircraft).trim() : undefined,
            ccode:    a.ccode ? String(a.ccode) : undefined,
            country:  a.country ? String(a.country) : undefined,
            altitude: typeof a.altitude === 'number' ? a.altitude : undefined,
            speed:    typeof a.speed === 'number' ? a.speed : undefined,
            vspeed:   typeof a.vspeed === 'number' ? a.vspeed : undefined,
            course:   typeof a.course === 'number' ? a.course : undefined,
            squawk:   a.squawk ? String(a.squawk) : undefined,
            rssi:     typeof a.rssi === 'number' ? a.rssi : undefined,
            msgs:     typeof a.msgs === 'number' ? a.msgs : undefined,
            lat, lon,
            distKm:  hasPos ? haversineKm(rp!.lat, rp!.lon, lat!, lon!) : undefined,
            bearing: hasPos ? bearingDeg(rp!.lat, rp!.lon, lat!, lon!) : undefined,
          };
        }));

        const rows = v.aircraft.map((a: any) =>
          j(a.flight ?? a.aircraft ?? a.icao ?? '?',
            a.altitude != null ? a.altitude + 'ft' : '',
            a.speed != null ? a.speed + 'kt' : '',
            a.rssi != null ? a.rssi + 'dB' : ''));
        return { text: `── ADS-B aircraft (${v.aircraft.length}) ──\n${rows.join('\n')}\n`, replace: true };
      }
      default: {
        // WSJT family (FT8/FT4/JT65/JT9/WSPR/…): { mode, msg, db, dt, freq, timestamp }
        if (typeof v?.msg === 'string') {
          return { text: j(hhmmss(v.timestamp), v.db != null ? v.db + 'dB' : '', v.freq != null ? v.freq + 'Hz' : '', v.msg) };
        }
        // Unmapped record shape — show its scalar fields so the decoder never
        // reads blank (and the real field names are visible for refinement).
        if (typeof mode === 'string') {
          const fields = Object.entries(v)
            .filter(([k, val]) => k !== 'mode' && k !== 'timestamp' && (typeof val === 'string' || typeof val === 'number') && val !== '')
            .map(([k, val]) => `${k}=${val}`);
          if (fields.length) return { text: j(hhmmss(v.timestamp), mode, ...fields) };
        }
        return null;
      }
    }
  }

  // ── inbound: binary frames ───────────────────────────────────────────────
  private onBinary(buf: Uint8Array): void {
    const type = buf[0];
    const payload = buf.subarray(1);
    switch (type) {
      case 1:
        this.fftFrames++; this.fftBytes += buf.length;
        this.startRateMeter();
        this.onFft(payload);
        break;
      // ★ AUDIO COUNTS TOO, and only OWRX can do this honestly. Everywhere else
      // the phone decodes audio NATIVELY, so JS never sees those bytes and the
      // readout is spectrum-only. OWRX's audio arrives on this same socket and is
      // decoded here — and it is not a rounding error: WFM/DAB is ~23.8 KB/s, it
      // CANNOT be lowered, and on a heavy profile it is roughly half the total.
      // Reporting spectrum alone would halve the very number that decides whether
      // this server is viable on a given link.
      case 2:
        this.fftBytes += buf.length;
        this.onAudio(payload, OWRX_AUDIO_RATE, this.audioDec); break;   // primary (12k)
      case 4:
        this.fftBytes += buf.length;
        this.onAudio(payload, OWRX_HD_RATE, this.hdAudioDec); break;     // HD / WFM (48k)
      case 3: break;                                    // secondary FFT — ignored in v3
      default: this.dbg('unknown binary type ' + type);
    }
  }

  /** Decode an audio frame (ADPCM or raw s16 LE) and push the PCM to the native
   *  player at its rate. Foreground-first: socket+decode live here in JS, the
   *  native engine just plays — audio stops if JS suspends (native engine later).
   *  type-2 = output_rate (12k); type-4 = hd_output_rate (48k), used for WFM. */
  private onAudio(payload: Uint8Array, rate: number, dec: OwrxAudioDecoder): void {
    // DAB speed correction (the dablin/OWRX chipmunk: UK DAB+ stations whose true
    // sample rate is misread, played 1.5×/1.07× fast). We can't know the true rate
    // from the headerless type-4 stream, so the user picks a ratio; we under-state
    // the PCM rate by that factor and let the native resampler stretch it back to
    // correct speed + pitch. Only DAB (type-4) is scaled; WFM (also type-4) isn't.
    const playRate = String(this.mode) === 'dab' ? Math.round(rate * this.dabRateScale) : rate;
    if (!this.audioStarted) { Vibe?.startExternalAudio?.(playRate, 'release'); this.audioStarted = true; }
    let pcm: Int16Array;
    if (this.cfg?.audioCompression === 'adpcm') {
      pcm = dec.decode(payload);
    } else {
      const m = payload.byteLength & ~1;
      pcm = new Int16Array(m / 2);
      const dv = new DataView(payload.buffer, payload.byteOffset, m);
      for (let i = 0; i < pcm.length; i++) pcm[i] = dv.getInt16(i * 2, true);
    }
    if (!pcm.length) return;
    Vibe?.pushExternalPcm?.(bytesToBase64(new Uint8Array(pcm.buffer, pcm.byteOffset, pcm.byteLength)), playRate, 1);
  }

  /** DAB speed correction factor (1 = off). e.g. 0.6667 plays a 32 kHz-as-48 kHz
   *  station at correct speed; 0.9375 fixes the DAB+ 960-vs-1024 framing drift. */
  setDabAudioScale(scale: number): void {
    this.dabRateScale = scale > 0 ? scale : 1;
  }

  private onFft(payload: Uint8Array): void {
    if (!this.cfg) return;
    // Backgrounded/locked: skip the per-frame FFT ADPCM decode + resample + emit
    // entirely — it's the bulk of OWRX's CPU and useless with the screen off.
    // Audio frames (type 2/4) still process so background audio keeps playing.
    if (this.specPaused) return;
    let row: Float32Array;
    if (this.cfg.fftCompression === 'adpcm') {
      row = decodeOwrxFftFrame(payload);
    } else {
      // f32 dB row, little-endian; copy into an aligned Float32Array
      row = new Float32Array(payload.byteLength / 4);
      const dv = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
      for (let i = 0; i < row.length; i++) row[i] = dv.getFloat32(i * 4, true);
    }
    this.lastRow = row;
    // Track frame inter-arrival for the connection signal meter.
    const now = Date.now();
    if (this.lastFrameAt > 0) {
      this.gapHist.push(now - this.lastFrameAt);
      if (this.gapHist.length > 40) this.gapHist.shift();
    }
    this.lastFrameAt = now;
    this.evalLink();
    this.emitSlice(row);
  }

  /** Score the connection like a phone signal indicator (0–3 bars) from FFT
   *  frame timing. Stalls are judged against the MEDIAN gap so a profile's
   *  natural frame rate (whatever it is) reads as full bars when steady. */
  private evalLink(): void {
    let q: 0 | 1 | 2 | 3;
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      q = 0;
    } else {
      const now = Date.now();
      const h = this.gapHist;
      let med = 100;
      if (h.length >= 5) { const s = [...h].sort((a, b) => a - b); med = s[s.length >> 1]; }
      let stalls = 0;
      for (let i = 0; i < h.length; i++) if (h[i] > med * 2.5 + 50) stalls++;
      const starving = this.lastFrameAt > 0 && now - this.lastFrameAt > Math.max(2000, med * 4);
      // Just (re)connected and few frames yet — show a tentative two bars.
      if (now - this.connectedAt < 4000 && h.length < 5) q = 2;
      else if (stalls >= 3 || starving) q = 1;
      else if (stalls >= 1) q = 2;
      else q = 3;
    }
    if (q !== this.lastLink) { this.lastLink = q; this.cb.onLink?.(q); }
  }

  /** Slice the full-profile FFT row to the current view window and emit. The
   *  window is SHIFTED (not clamped) to stay inside the profile, so a VFO near a
   *  band edge keeps a full-width view and the marker simply moves toward the edge
   *  — clamping the edges instead pinned the VFO to the left edge and stuck it. */
  /** The shifted view window [lo,hi] in absolute Hz — shared by the waterfall
   *  slice and the status snapshot so the VFO overlay and waterfall agree. */
  private window(): { lo: number; hi: number } {
    const cf = this.cfg?.centerFreq ?? this.freq, sr = this.cfg?.sampRate ?? 0;
    if (!sr) return { lo: cf, hi: cf };
    const fullLo = cf - sr / 2, fullHi = cf + sr / 2;
    const bw = Math.min(this.viewBw || sr, sr);
    let lo = this.viewCenter - bw / 2, hi = this.viewCenter + bw / 2;
    if (lo < fullLo) { lo = fullLo; hi = fullLo + bw; }
    if (hi > fullHi) { hi = fullHi; lo = fullHi - bw; }
    return { lo, hi };
  }

  private emitSlice(full: Float32Array): void {
    if (!this.cfg || !this.cfg.sampRate) return;
    const cf = this.cfg.centerFreq, sr = this.cfg.sampRate;
    const fullLo = cf - sr / 2;
    const { lo, hi } = this.window();
    const n = full.length;
    const loIdx = Math.max(0, Math.min(n, Math.round(((lo - fullLo) / sr) * n)));
    const hiIdx = Math.max(loIdx, Math.min(n, Math.round(((hi - fullLo) / sr) * n)));
    const win = (loIdx === 0 && hiIdx === n) ? full : full.subarray(loIdx, hiIdx);
    // ALWAYS emit a FIXED bin count. The waterfall reallocates (and wipes) its
    // history texture whenever the row length changes — and the server's FFT
    // frames jitter in length on some (esp. digital/DAB) profiles, which wiped
    // the waterfall every few seconds. Resampling the window to OUT_BINS keeps
    // the texture width constant so history is continuous across both that
    // jitter AND zoom (old rows scroll off instead of a hard reset).
    const out = resampleRow(win, OWRX_OUT_BINS);
    this.cb.onSpectrum(out, this.statusForSlice(OWRX_OUT_BINS, lo, hi));
  }

  private statusForSlice(binCount: number, viewLo: number, viewHi: number): SDRStatus {
    const bw = Math.max(1, viewHi - viewLo);
    return {
      frequency: this.freq, mode: this.mode,
      bandwidthLow: this.bwLow, bandwidthHigh: this.bwHigh,
      binCount, binBandwidth: bw / Math.max(1, binCount),
      centerHz: (viewLo + viewHi) / 2, bwHz: bw,
    };
  }

  // ── outbound: control plane ──────────────────────────────────────────────
  private send(obj: any): void { try { this.ws?.send(JSON.stringify(obj)); } catch {} }

  private sendDemod(): void {
    if (!this.started || !this.cfg) return;
    const offset = this.freq - this.cfg.centerFreq;
    // The mode may be a known SDRMode (mapped) or a server modulation passed
    // straight through (wfm, dmr, dab, …) selected from the gated picker.
    let mod = MODE_TO_WIRE[this.mode] ?? String(this.mode);
    // this.mode is always the analog carrier; the secondary decoder (SSTV/Fax/…)
    // rides on top via secondary_mod (OWRX runs it on the carrier's audio).
    const secondaryMod: string | false = this.secondaryDecoder ?? false;
    if (this.modes && !this.modes.includes(mod)) {
      if (mod === 'sam') mod = 'am';                 // clamp per brief §4
      else if (mod === 'nfm' && this.modes.includes('fm')) mod = 'fm';
    }
    const params: Record<string, unknown> = {
      offset_freq: Math.round(offset), mod, squelch_level: Math.round(this.squelchLevel),
      secondary_mod: secondaryMod,
    };
    if (secondaryMod) params.secondary_offset_freq = this.secondaryOffset;
    // RAW-IF decoders must get NULL cuts — sending numeric low_cut/high_cut inserts
    // a bandpass/resampler that STARVES a fixed-rate decoder (no decode/audio). This
    // covers DAB/DRM (dablin/dream on the full IF) AND raw-IF secondary decoders:
    // ADSB (2.4 MHz IF) and ISM, identified by an "empty" underlying or an ifRate.
    // SSB/AM-carried decoders (RTTY/Fax/Packet) DO want their carrier's cuts.
    const decDef = this.secondaryDecoder ? this.serverModes.find((m) => m.id === this.secondaryDecoder) : undefined;
    const rawIf = ['dab', 'drm'].includes(mod)
      || !!(decDef && (decDef.underlying?.includes('empty') || (decDef.ifRate ?? 0) > 0));
    params.low_cut = rawIf ? null : this.bwLow;
    params.high_cut = rawIf ? null : this.bwHigh;
    // DAB: which programme (audio service) within the ensemble to decode.
    if (mod === 'dab') params.audio_service_id = this.audioServiceId;
    this.send({ type: 'dspcontrol', params });
    this.applyAudioBurstDepth(mod);
  }

  /* ★★★ DECODED MODES DELIVER AUDIO IN BLOCKS, AND THE PLAYER HAS TO BE TOLD.
   *
   *  ★★★ THE FAULT, from issue #22. Analogue audio trickles in continuously, so both players are
   *      tuned for that: iOS drops anything above a 0.4 s live-edge bound, Android's writer polls
   *      for 250 ms and writes nothing if it finds nothing. A DECODER does not trickle — DRM emits
   *      a 400 ms frame in one go, so on iOS the tail of nearly every frame was discarded (0.22 s
   *      of pre-roll plus a 0.4 s burst is over the bound) and on Android the track starved between
   *      frames. Michael (DL8LDN): "using DRM have audio drop outs. Using OWRX in Browser works ok
   *      to the same time and signal." The browser buffers deeply and rides through it; the stream
   *      was never the problem.
   *
   *  ★★★ AND IT IS NOT ONLY DRM, which is why this is a LIST and not an `if`. Stuart: "make sure
   *      we arent just fixing it for 1 mode and we fix for all digitmodes that need it." Taken
   *      from his own OWRX+ /api/features, which has every codec installed — the audio-producing
   *      decoders there are drm (dream-2-2), dab, hdradio, tetra, digital_voice_digiham
   *      (DMR/D-Star/NXDN/YSF), digital_voice_freedv, digital_voice_m17 and digital_voice_rade.
   *      Everything else available on that box — adsb, acars, hfdl, ism, pocsag, page, sonde,
   *      vdl2, wsjt, js8call, packet — produces TEXT and never reaches the audio path at all.
   *      ★ Digital voice bursts differently but for the same reason: nothing at all between
   *        overs, then a transmission — so the first frames after each over have no cushion.
   *
   *  ★★ ANALOGUE IS LEFT EXACTLY AS IT IS. Stuart: "only engage the larger buffer if DRM is
   *     actively selected" — generalised to the modes that need it, and to nothing else, so a fix
   *     for modes almost nobody can test cannot cost latency on the ones everybody uses.
   *  ★ Depth by how long a block is: DRM's 400 ms frame is the longest, HD Radio's is comparable;
   *    DAB and digital voice are shorter. Names are matched loosely because OWRX+ spells them
   *    differently across builds (dstar/d-star, hdr/hdradio). */
  private static readonly BURST_MS: ReadonlyArray<[RegExp, number]> = [
    [/^(drm)$/i, 800],
    [/^(hdr|hdradio|nrsc5)$/i, 800],
    [/^(dab)$/i, 500],
    [/^(dmr|dstar|d-star|nxdn|ysf|m17|freedv|rade|tetra)$/i, 400],
  ];

  private lastBurstMs = -1;

  private applyAudioBurstDepth(mod: string): void {
    let ms = 0;
    for (const [re, v] of OwrxAdapter.BURST_MS) if (re.test(mod)) { ms = v; break; }
    if (ms === this.lastBurstMs) return;
    this.lastBurstMs = ms;
    Vibe?.setAudioBurstDepth?.(ms);
  }

  /** DAB: pick a programme within the tuned ensemble (re-sends the demod). */
  setAudioServiceId(id: number): void {
    if (id === this.audioServiceId) return;
    this.audioServiceId = id;
    this.audioDec.reset(); this.hdAudioDec.reset();   // audio service swap = new stream
    this.sendDemod();
    // Re-emit metadata so the UI's station name follows the new selection. Carry
    // the live-data badge too, else switching programme via the picker clears it.
    const selName = this.dabProgrammes.find((p) => p.id === id)?.name;
    if (selName) this.cb.onMetadata?.({ stationName: selName, programmes: this.dabProgrammes, badge: 'RDS' });
  }

  tune(frequency: number, mode?: SDRMode, opts?: { recenter?: boolean }): void {
    if (mode) this.mode = mode;
    const recenter = this.followVfo || !!opts?.recenter;
    // Retune to a different frequency = a different station/signal → drop the held
    // RDS name / digital-voice caller so the VTS falls back to bookmarks for the
    // new spot (was sticking on the old station's RDS/callsign).
    if (frequency !== this.freq) this.clearLiveMeta();
    if (!this.cfg) { this.freq = frequency; return; }
    const half = this.cfg.sampRate / 2;
    const offset = frequency - this.cfg.centerFreq;
    this.dbg(`tune in=${frequency} off=${Math.round(offset)} half=${half} ${Math.abs(offset) <= half ? 'IN' : 'OUT'}`);
    if (Math.abs(offset) <= half) {
      this.freq = frequency;
      if (recenter) this.viewCenter = frequency;   // locked: view follows VFO; unlocked: leave the pan
      this.sendDemod();
      if (this.lastRow) this.emitSlice(this.lastRow);
      this.cb.onStatus(this.getStatus());
      return;
    }
    // Outside the current profile window — CLAMP to the edge. We do NOT auto-switch profiles:
    // an OWRX profile change retunes the SHARED SDR for every listener, so switching is an
    // EXPLICIT user action only (Stuart's rule from when OWRX was added — etiquette). Tuning past
    // the edge pulses the drum at the band edge; the user picks another profile deliberately.
    this.freq = this.cfg.centerFreq + Math.sign(offset) * half;
    this.viewCenter = this.freq;
    this.sendDemod();
    if (this.lastRow) this.emitSlice(this.lastRow);
    this.cb.onStatus(this.getStatus());
  }

  syncFrequency(frequency: number, mode?: SDRMode): void {
    this.freq = frequency; if (mode) this.mode = mode;
  }

  setFollowMode(follow: boolean): void { this.followVfo = follow; }

  /** Active profile span — hard walls; pan never auto-switches profile. */
  panSpan(): { loHz: number; hiHz: number; movable: boolean } {
    return { loHz: this.caps.freqRange[0], hiHz: this.caps.freqRange[1], movable: false };
  }

  /** Per-mode bandwidth-slider caps (per-edge half-width) so the slider is
   *  DEMODULATOR-AWARE: USB tops out ~7 kHz (fine control around 2.7 kHz) instead
   *  of the whole multi-MHz IF. Derived from each mode's server bandpass with
   *  headroom; DAB/DRM stay wide (server-controlled). Was sampRate/2 for all. */
  private buildBandwidthCaps(): void {
    const sr = this.cfg?.sampRate ?? 0;
    const caps: Record<string, number> = {};
    for (const m of this.serverModes) {
      const id = m.id.toLowerCase();
      if (id === 'dab' || id === 'drm') { caps[m.id] = sr ? Math.floor(sr / 2) : 1_000_000; continue; }
      if (m.ifRate) { caps[m.id] = Math.floor(m.ifRate / 2); continue; }   // raw-IF (ADSB 1.2MHz, ISM 125k)
      if (m.bandpass) {
        const half = Math.max(Math.abs(m.bandpass.low_cut), Math.abs(m.bandpass.high_cut));
        caps[m.id] = Math.max(3000, Math.round(half * 2.5));   // headroom for fine tuning
      } else if (id.includes('wfm')) caps[m.id] = 150_000;
      else if (id === 'usb' || id === 'lsb' || id === 'cw' || id.startsWith('cw')) caps[m.id] = 6000;
      else caps[m.id] = 8000;                                   // adsb/ism/raw — passband n/a
    }
    this.caps.maxBandwidth = { default: 8000, ...(caps as Partial<Record<SDRMode, number>>) };
  }

  /** Adopt the server's default passband for the current demodulator, if known. */
  private applyModeBandpass(): void {
    const info = this.serverModes.find((m) => m.id === (this.mode as string));
    if (info?.bandpass) { this.bwLow = info.bandpass.low_cut; this.bwHigh = info.bandpass.high_cut; }
    else if (String(this.mode) === 'dab') {
      // DAB has no mode bandpass (server-controlled, ~1.536 MHz channel). Show a
      // wide span so the UI/VFO overlay matches what's actually decoded — the
      // cuts aren't sent (see sendDemod), this is display-only.
      this.bwLow = -768_000; this.bwHigh = 768_000;
    }
  }

  setMode(mode: SDRMode): void {
    // Leaving ADS-B: clear the aircraft table. The watch routes on the presence of
    // that table (the wire mode is `empty`+secondary, so the mode string can't be
    // trusted), so a stale list would strand it on the aircraft screen.
    if (String(mode) !== 'adsb' && String(this.mode) === 'adsb') this.cb.onAircraft?.([]);

    const sel = this.serverModes.find((m) => m.id === String(mode));
    // ALL digimodes are SECONDARY demods (DIG dropdown) — verified on the wire:
    // ADSB needs mod=empty + secondary_mod=adsb (mod=adsb alone decodes nothing).
    // RTTY/WEFAX/SSTV ride usb/lsb, Packet/Page ride nfm, ACARS rides am, and
    // ADSB/ISM ride the special "empty" (raw-IF) carrier. So always set
    // secondary_mod; only auto-pick a REAL carrier sideband (never "empty" — for
    // ADSB we keep the profile's mode, which works as mod=adsb + secondary_mod=adsb).
    const realUnderlying = (sel?.underlying ?? []).filter((u) => u !== 'empty');
    if (sel?.type === 'digimode' && sel.underlying?.length) {
      if (realUnderlying.length) {
        // CARRIED digimode (RTTY/SSTV/WEFAX/Packet/ACARS): a secondary decoder on
        // an analog carrier — toggle on/off and auto-pick a real sideband.
        this.secondaryDecoder = this.secondaryDecoder === sel.id ? null : sel.id;
        if (this.secondaryDecoder && !realUnderlying.includes(String(this.mode))) {
          this.mode = realUnderlying[0] as SDRMode;   // RTTY→usb, Page→nfm, …
        }
      } else {
        // RAW-IF STANDALONE digimode (ADSB/ISM/Meshtastic/Meshcore/LoRa-*): it IS
        // the primary mod, but OWRX needs mod=id AND secondary_mod=id — mod alone
        // decodes nothing. Manually picking it must set BOTH (the toggle path used
        // to leave mode on the previous carrier, so the server kept decoding LoRa).
        this.secondaryDecoder = sel.id;
        this.mode = sel.id as SDRMode;
      }
      this.applyModeBandpass();
      this.audioDec.reset(); this.hdAudioDec.reset();
      this.sendDemod();
      this.cb.onStatus(this.getStatus());
      return;
    }
    // Normal demod pick = set the carrier/primary mode (analog, digital voice,
    // DAB/WFM, OR a standalone digimode like ADSB that has no underlying).
    this.mode = mode;
    // Keep a running secondary decoder if the new carrier is a sideband it
    // supports — e.g. RTTY active, user taps USB per the advisory → RTTY stays
    // and now decodes. Switching to an incompatible mode turns the decoder off.
    if (this.secondaryDecoder) {
      const dec = this.serverModes.find((m) => m.id === this.secondaryDecoder);
      if (!dec?.underlying?.includes(String(mode))) this.secondaryDecoder = null;
    }
    // Leaving DAB/WFM: the RDS/DAB labels no longer apply, clear them.
    if (String(mode) !== 'dab' && String(mode) !== 'wfm') {
      this.dabProgrammes = []; this.dabEnsemble = ''; this.lastDabSig = ''; this.rdsPs = ''; this.lastVoiceSpeaker = '';
      this.cb.onMetadata?.({ programmes: [] });
    }
    this.applyModeBandpass();
    // The audio stream restarts on a mode change (e.g. NFM type-2 ↔ WFM type-4) —
    // reset both ADPCM decoders so stale state doesn't corrupt the new stream.
    this.audioDec.reset(); this.hdAudioDec.reset();
    this.sendDemod();
    this.cb.onStatus(this.getStatus());
  }

  /** The active decoder id (for the UI decoder panel + highlight): the secondary
   *  decoder if one rides on a carrier, else the primary mode itself when it's a
   *  standalone digimode (ADSB/POCSAG/…). Null for plain analog/voice/DAB modes. */
  getSecondaryDecoder(): string | null {
    if (this.secondaryDecoder) return this.secondaryDecoder;
    const sel = this.serverModes.find((m) => m.id === String(this.mode));
    return sel?.type === 'digimode' ? sel.id : null;
  }

  setBandwidth(low: number, high: number): void {
    // DAB's passband is server-controlled and fixed-wide — ignore UI bandwidth
    // edits so the displayed span (and the unsent cuts) stay correct.
    if (String(this.mode) === 'dab') return;
    this.bwLow = low; this.bwHigh = high; this.sendDemod(); this.cb.onStatus(this.getStatus());
  }

  // ── view (client-side slicing) ───────────────────────────────────────────
  zoom(frequency: number, binBandwidth: number): void {
    // binBandwidth is Hz/bin for the requested view; reconstruct total view width.
    // The view is quantised to OWRX_OUT_BINS bins (the fixed waterfall row width),
    // so reconstruct against that — NOT the server's fftSize, which would mismatch
    // the bin count the UI sees and make zoom over/under-shoot.
    const sr = this.cfg?.sampRate ?? this.viewBw;
    // Max-zoom floor: cap how far we can zoom in. We slice the server's FFT
    // (fftSize bins over the full span) and resample to OUT_BINS. Zoom past the
    // point where the window holds OUT_BINS/MAX_UPSAMPLE real bins and there's
    // nothing left to show but stretched noise — that degenerate over-zoom is
    // what broke pan/levels on UberSDR. nativeBinHz = sampRate/fftSize.
    const fftSize = this.cfg?.fftSize ?? OWRX_OUT_BINS;
    const MAX_UPSAMPLE = 8;   // allow up to 8× interpolation, then stop zooming
    // Floor at 6 kHz (one SSB channel each side), matching Uber/Kiwi: zooming past a single
    // contact leaves nothing to show but stretched noise, and auto-contrast floods it white.
    // Keep whichever floor is WIDER (the bin-based one guards over-upsampling), but never let
    // the visible span fall below 6 kHz — and never exceed the source rate on a tiny server.
    const minViewBw = Math.min(sr, Math.max(6_000, (OWRX_OUT_BINS / MAX_UPSAMPLE) * (sr / fftSize)));
    this.viewCenter = frequency;
    this.viewBw = Math.max(minViewBw, Math.min(sr, binBandwidth * OWRX_OUT_BINS));
    if (this.lastRow) this.emitSlice(this.lastRow);
  }
  pan(frequency: number): void { this.viewCenter = frequency; if (this.lastRow) this.emitSlice(this.lastRow); }
  resetView(): void {
    if (this.cfg) { this.viewCenter = this.cfg.centerFreq; this.viewBw = this.cfg.sampRate; }
    if (this.lastRow) this.emitSlice(this.lastRow);
  }

  // OWRX pushes one row per FFT; no client rate control / pause needed.
  setRate(_divisor: number): void {}
  pauseSpectrum(): void { this.specPaused = true; }
  resumeSpectrum(): void {
    this.specPaused = false;
    if (this.lastRow) this.emitSlice(this.lastRow);   // repaint immediately, no wait for next frame
  }

  // ── profiles ─────────────────────────────────────────────────────────────
  getProfiles(): ProfileInfo[] { return this.profiles; }
  selectProfile(id: string): void { this.send({ type: 'selectprofile', params: { profile: id } }); }

  /** Fetch the landing page once to label the server OpenWebRX vs OpenWebRX+
   *  (the WS ack can't tell them apart) and report name + version to the UI. */
  private async detectServerName(): Promise<void> {
    let plus = false;
    try {
      const res = await fetch(this.httpBase + '/');
      if (res.ok) { const html = await res.text(); plus = /openwebrx\+/i.test(html); }
    } catch { /* offline — fall back to plain OpenWebRX */ }
    this.cb.onServerInfo?.({ name: plus ? 'OpenWebRX+' : 'OpenWebRX', version: this.serverVersion });
  }

  /** Squelch level in dB (−150 = off/open). Re-sends the demod. */
  setSquelch(level: number): void {
    this.squelchLevel = level;
    this.sendDemod();
  }

  /** Noise reduction. threshold ≤ 0 = off (slider fully open); higher = more NR.
   *  OWRX takes nr via connectionproperties {nr_enabled, nr_threshold(dB)}. */
  setNr(threshold: number): void {
    const enabled = threshold > 0;
    this.send({ type: 'connectionproperties', params: { nr_enabled: enabled, nr_threshold: Math.max(0, Math.round(threshold)) } });
  }

  /** Basic text chat on the main WS. The server broadcasts to all clients incl.
   *  us (our own message echoes back as a chat_message). */
  sendChat(text: string, name: string): void {
    const t = text.trim(); if (!t) return;
    this.send({ type: 'sendmessage', text: t, name });
  }

  // ── status ───────────────────────────────────────────────────────────────
  getStatus(): SDRStatus {
    const sr = this.cfg?.sampRate ?? 0;
    const { lo, hi } = this.window();
    const bw = Math.max(1, hi - lo);
    // The view is always resampled to OWRX_OUT_BINS bins (emitSlice), so report
    // that as the bin count — the UI's zoom maths must agree with what's drawn.
    const bins = this.viewInit ? OWRX_OUT_BINS : 0;
    return {
      frequency: this.freq, mode: this.mode,
      bandwidthLow: this.bwLow, bandwidthHigh: this.bwHigh,
      binCount: bins, binBandwidth: bw / Math.max(1, bins),
      centerHz: this.viewInit ? (lo + hi) / 2 : (this.cfg?.centerFreq ?? this.freq),
      bwHz: this.viewInit ? bw : sr,
    };
  }
  getView(): SDRStatus { return this.getStatus(); }

  private dbg(m: string): void { this.cb.onDbg?.('[owrx] ' + m); }
}
