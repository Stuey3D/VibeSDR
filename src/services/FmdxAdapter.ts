/**
 * FmdxAdapter — FM-DX Webserver (TEF668x / XDR-F1HD hardware tuners) behind the
 * SDRBackend contract. v7. Protocol per BRIEF-fmdx-backend-adapter.md, verified
 * against NoobishSVK/fm-dx-webserver.
 *
 * Model: a SINGLE SHARED hardware tuner — every user hears the same frequency,
 * tuning is global (see plan §2g). Server does demod + RDS decode; there is NO
 * waterfall/FFT and no client DSP. Control + whole-state JSON over `wss://…/text`;
 * audio is MP3-over-WS decoded natively (VibePowerModule.startFmdxAudio → the
 * FmdxMp3Decoder path). Because there's no spectrum, the waterfall side of the
 * SDRBackend surface is stubbed; the tuner screen consumes onFmdxState.
 */

import type { SDRMode, SDRStatus } from './UberSDRClient';
import type {
  SDRBackend, BackendCallbacks, BackendCapabilities, BackendKind, FmdxState,
} from './SDRBackend';
import { NativeModules } from 'react-native';
import { FMDX_TUNE_LO, FMDX_TUNE_HI } from '../constants/fmBand';

const Vibe = NativeModules.VibePowerModule as {
  startFmdxAudio?: (baseUrl: string) => void;
  stopFmdxAudio?: () => void;
} | undefined;

const FMDX_CAPS: BackendCapabilities = {
  profiles:       false,
  serverSideZoom: false,   // no waterfall at all
  smeter:         'message',
  // The receiver's full sweep, NOT the ITU-R1 band. An FM-DX server exposes its
  // tuning range nowhere machine-readable (see constants/fmBand.ts — it is not in
  // /static_data, /api or the /text payload), so the old 87.5 floor here was a
  // guess that locked users out of OIRT, the Japanese band and any server parked
  // below 87.5. Overshooting an owner's limit is safe: the server drops the
  // command silently rather than retuning.
  freqRange:      [FMDX_TUNE_LO, FMDX_TUNE_HI],
  chat:           true,    // /chat WS — vital on a shared tuner ("can I retune?")
  serverNR:       false,
  maxBandwidth:   { default: 100_000, wfm: 100_000 },
};

/** ws(s) URL for a given path from an http(s) base. */
function wsUrl(base: string, path: string): string {
  let s = base.trim();
  if (s.startsWith('https://'))      s = 'wss://' + s.slice(8);
  else if (s.startsWith('http://'))  s = 'ws://'  + s.slice(7);
  else                               s = 'ws://'  + s;
  s = s.replace(/\/+$/, '');
  return s + path;
}

export class FmdxAdapter implements SDRBackend {
  readonly kind: BackendKind = 'fmdx';
  readonly caps: BackendCapabilities = FMDX_CAPS;
  readonly uuid: string;

  private base: string;
  private cb: BackendCallbacks;
  private ws: WebSocket | null = null;
  private chatWs: WebSocket | null = null;
  private audioStarted = false;
  private destroyed = false;

  // Latest known state (server is authoritative on a shared tuner).
  private freq = 95_000_000;         // Hz — adopted from the server's first frame
  private lastState: FmdxState | null = null;
  private eqOn = false;
  private imsOn = false;
  private textGen = 0;
  private textReconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private paused = false;            // power-saving pause — suppress auto-reconnect
  private deepLinkFreq?: number;

  constructor(baseUrl: string, uuid: string, callbacks: BackendCallbacks) {
    this.base = baseUrl.replace(/\/+$/, '');
    this.uuid = uuid;
    this.cb = callbacks;
  }

  connect(frequency?: number, _mode?: SDRMode): Promise<void> {
    // NB: we do NOT auto-tune on connect — the tuner is shared; we adopt the
    // server's current frequency and only retune on explicit user action.
    // `frequency` (e.g. from a deep link) is applied after the first state frame.
    this.deepLinkFreq = frequency;
    return new Promise((resolve, reject) => this.openTextWs(resolve, reject));
  }

  /** Open (or reopen) the /text control socket. Transient drops — backgrounding,
   *  audio route changes (AirPods removed), network blips — auto-reconnect
   *  silently rather than surfacing a "server stopped responding" banner. */
  private openTextWs(onOpen?: () => void, onErr?: (e: Error) => void): void {
    this.textGen++;
    const gen = this.textGen;
    let ws: WebSocket;
    try { ws = new WebSocket(wsUrl(this.base, '/text')); }
    catch (e) { onErr?.(e as Error); return; }
    this.ws = ws;

    ws.onopen = () => {
      if (this.textGen !== gen) return;
      this.cb.onConnect();
      this.cb.onLink?.(3);
      this.cb.onChatEnabled?.(true);
      this.openChatWs();
      this.fetchStaticData();
      // Audio is a separate native WS opened from the http base.
      if (!this.audioStarted) { Vibe?.startFmdxAudio?.(this.base); this.audioStarted = true; }
      // Optional deep-link retune, first connect only (shared-tuner: retunes all).
      if (this.deepLinkFreq != null) {
        const f = this.deepLinkFreq; this.deepLinkFreq = undefined;
        setTimeout(() => { if (!this.destroyed) this.tune(f); }, 400);
      }
      onOpen?.();
    };
    ws.onmessage = (e) => {
      if (typeof e.data !== 'string') return;   // server pushes JSON text
      try { this.onFrame(JSON.parse(e.data)); }
      catch { /* non-JSON keepalive — ignore */ }
    };
    ws.onerror = () => { onErr?.(new Error('FM-DX WebSocket error')); };
    ws.onclose = (ev) => {
      if (this.textGen !== gen || this.ws !== ws) return;   // superseded
      this.cb.onLink?.(0);
      this.cb.onDisconnect();
      onErr?.(new Error('FM-DX closed (' + ev.code + ')'));
      if (!this.destroyed) this.scheduleTextReconnect();     // silent auto-reconnect
    };
  }

  private scheduleTextReconnect(): void {
    if (this.textReconnectTimer || this.destroyed || this.paused) return;
    this.textReconnectTimer = setTimeout(() => {
      this.textReconnectTimer = null;
      if (!this.destroyed) this.openTextWs();
    }, 2000);
  }

  destroy(): void {
    this.destroyed = true;
    this.textGen++;                                          // supersede any in-flight socket
    if (this.textReconnectTimer) { clearTimeout(this.textReconnectTimer); this.textReconnectTimer = null; }
    if (this.audioStarted) { Vibe?.stopFmdxAudio?.(); this.audioStarted = false; }
    const ws = this.ws; this.ws = null;
    if (ws) { try { ws.onclose = null; ws.close(); } catch {} }
    const cw = this.chatWs; this.chatWs = null;
    if (cw) { try { cw.onclose = null; cw.close(); } catch {} }
  }

  // ── Chat (/chat WS) — shared-tuner coordination ─────────────────────────────
  private openChatWs(): void {
    if (this.chatWs) return;
    let cw: WebSocket;
    try { cw = new WebSocket(wsUrl(this.base, '/chat')); }
    catch { return; }
    this.chatWs = cw;
    cw.onmessage = (e) => {
      if (typeof e.data !== 'string') return;
      try {
        const j = JSON.parse(e.data);
        // The server sends a 'clientIp' control frame + chat frames {nickname,
        // message, time, admin?, history?}. Render only ones with a message.
        if (j?.type === 'clientIp') return;
        if (j?.message != null) {
          this.cb.onChatMessage?.(String(j.nickname ?? '?'), String(j.message));
        }
      } catch { /* ignore */ }
    };
    cw.onclose = () => { this.chatWs = null; };
    cw.onerror = () => {};
  }

  /** ChatDrawer onSend → post to the shared server chat. FM-DX needs no join;
   *  the nickname rides every message (≤32 chars, message ≤255). */
  sendChat(text: string, name: string): void {
    const cw = this.chatWs;
    if (!cw || cw.readyState !== 1) return;
    const payload = JSON.stringify({
      nickname: (name || 'VibeSDR').slice(0, 32),
      message:  String(text).slice(0, 255),
    });
    try { cw.send(payload); } catch {}
  }

  disconnectSocket(): void {
    // Pause: drop the /text socket but leave the native audio session (lock
    // screen card) — matches the OWRX/Kiwi "release" behaviour.
    const ws = this.ws; this.ws = null;
    if (ws) { try { ws.onclose = null; ws.close(); } catch {} }
  }

  /** Power-saving PAUSE — the user stopped listening (lock-screen pause, AirPods
   *  out, Bluetooth off). Fully drop the /text + /chat control sockets (freezes
   *  SNR/RDS) and suppress auto-reconnect, so nothing keeps streaming in the
   *  background draining battery. The native side stops the /audio stream in
   *  parallel. resumeFromPower() (on ▶) reopens everything. This makes FM-DX a
   *  true disconnect-on-pause, like UberSDR — not a mute-in-place. */
  pauseForPower(): void {
    if (this.paused) return;
    this.paused = true;
    this.textGen++;                                        // supersede any in-flight socket
    if (this.textReconnectTimer) { clearTimeout(this.textReconnectTimer); this.textReconnectTimer = null; }
    const ws = this.ws; this.ws = null;
    if (ws) { try { ws.onclose = null; ws.close(); } catch {} }
    const cw = this.chatWs; this.chatWs = null;
    if (cw) { try { cw.onclose = null; cw.close(); } catch {} }
    this.cb.onLink?.(0);
    this.cb.onChatEnabled?.(false);
    this.cb.onDisconnect();
  }

  resumeFromPower(): void {
    if (!this.paused) return;
    this.paused = false;
    if (!this.destroyed) this.openTextWs();
  }

  // ── Incoming whole-state frame ──────────────────────────────────────────────
  private onFrame(j: any): void {
    // Some frames are for other channels (e.g. plugin data) — require a freq.
    const mhz = parseFloat(j?.freq);
    if (!Number.isFinite(mhz)) return;
    const freqHz = Math.round(mhz * 1e6);
    this.freq = freqHz;

    const rtFlag = String(j?.rt_flag ?? '0') === '1';
    const rt = String((rtFlag ? j?.rt1 : j?.rt0) ?? '').trim();
    const ps = String(j?.ps ?? '').trim();
    const tx = j?.txInfo && typeof j.txInfo === 'object' ? {
      tx:   j.txInfo.tx   != null ? String(j.txInfo.tx)   : undefined,
      city: j.txInfo.city != null ? String(j.txInfo.city) : undefined,
      itu:  j.txInfo.itu  != null ? String(j.txInfo.itu)  : undefined,
      erp:  Number(j.txInfo.erp),
      pol:  j.txInfo.pol  != null ? String(j.txInfo.pol)  : undefined,
      dist: Number(j.txInfo.dist),
      azi:  Number(j.txInfo.azi),
    } : undefined;

    const state: FmdxState = {
      freqHz,
      sig:    Number(j?.sig) || 0,
      stereo: !!j?.st,
      rds:    !!j?.rds,
      pi:     String(j?.pi ?? '').replace(/\?/g, '').trim(),
      ps,
      rt,
      pty:    Number(j?.pty) || 0,
      tp:     !!j?.tp,
      ta:     !!j?.ta,
      af:     Array.isArray(j?.af) ? j.af.map((k: any) => Math.round(Number(k) * 1000)).filter((n: number) => Number.isFinite(n)) : [],
      users:  Number(j?.users) || 0,
      tx,
      countryIso: j?.country_iso ? String(j.country_iso).trim() : undefined,
      eq:  Number(j?.eq)  === 1,
      ims: Number(j?.ims) === 1,
      ant: Number.isFinite(Number(j?.ant)) ? Number(j.ant) : undefined,
    };
    this.eqOn  = state.eq ?? this.eqOn;
    this.imsOn = state.ims ?? this.imsOn;
    this.lastState = state;

    this.cb.onFmdxState?.(state);
    this.cb.onStatus(this.getStatus());
    if (state.sig) this.cb.onSMeter?.(state.sig);   // dBf (tuner labels accordingly)
    // Feed the shared station-name path too (now-playing / VTS).
    this.cb.onMetadata?.({ stationName: ps || undefined, text: rt || undefined, badge: 'RDS', stereo: state.stereo });
  }

  // ── Control ─────────────────────────────────────────────────────────────────
  private send(cmd: string): void {
    const ws = this.ws;
    if (ws && ws.readyState === 1) { try { ws.send(cmd); } catch {} }
  }

  tune(frequency: number, _mode?: SDRMode, _opts?: { recenter?: boolean }): void {
    // T<kHz>. The server clamps to its tuning range and (on a locked tuner)
    // silently ignores — the next state frame reveals the actual frequency.
    const khz = Math.round(frequency / 1000);
    this.freq = frequency;                 // optimistic; server frame confirms
    this.send('T' + khz);
  }

  syncFrequency(frequency: number, _mode?: SDRMode): void { this.freq = frequency; }

  /** Force mono (B1) vs stereo/auto (B0) — the FM-DX "st" button. Helps pull
   *  weak/DX stations out of stereo hiss. */
  forceMono(mono: boolean): void { this.send(mono ? 'B1' : 'B0'); }

  /** cEQ / iMS filters — `G<eq><ims>` two digits, sent together. */
  setEq(on: boolean):  void { this.eqOn = on;  this.send(`G${on ? 1 : 0}${this.imsOn ? 1 : 0}`); }
  setIms(on: boolean): void { this.imsOn = on; this.send(`G${this.eqOn ? 1 : 0}${on ? 1 : 0}`); }
  /** Antenna select — `Z<n>` (only when the server advertises multiple). */
  setAntenna(id: number): void { this.send('Z' + id); }

  /** One-shot /static_data fetch → antenna list + bw-switch capability. */
  private fetchStaticData(): void {
    const base = this.base.replace(/\/+$/, '');
    const url = /^https?:\/\//.test(base) ? base + '/static_data' : 'http://' + base + '/static_data';
    fetch(url).then((r) => r.json()).then((j) => {
      if (this.destroyed) return;
      // ant = { enabled, ant1:{enabled,name}, ant2:{...}, … }. Only expose the
      // switch when ant.enabled, and only the individual antennas marked enabled.
      // Keys are antN (1-based); the Z command / `ant` state are 0-based → id=N-1.
      const antennas: { id: number; name: string }[] = [];
      const ant = j?.ant;
      if (ant && typeof ant === 'object' && ant.enabled) {
        for (const k of Object.keys(ant)) {
          if (k === 'enabled') continue;
          const v = (ant as any)[k];
          if (!v || typeof v !== 'object' || v.enabled !== true) continue;
          const m = /(\d+)/.exec(k);
          const id = m ? parseInt(m[1], 10) - 1 : antennas.length;
          antennas.push({ id, name: String(v.name ?? k) });
        }
      }
      // tunerName = where the RECEIVER is. Every txInfo distance is measured from
      // here, so without it "46 km" is a number with no origin.
      const tunerName = j?.tunerName != null ? String(j.tunerName).trim() : undefined;
      this.cb.onFmdxInfo?.({ antennas, bwSwitch: !!j?.bwSwitch, tunerName });
    }).catch(() => {});
  }

  getStatus(): SDRStatus {
    return {
      frequency:     this.freq,
      mode:          'wfm',
      bandwidthLow:  -100_000,
      bandwidthHigh:  100_000,
      binCount:       0,
      binBandwidth:   0,
      centerHz:       this.freq,
      bwHz:           200_000,
    };
  }
  getView(): SDRStatus { return this.getStatus(); }

  /** Latest parsed state, for a screen that mounts after the first frame. */
  getFmdxState(): FmdxState | null { return this.lastState; }

  // ── No-ops: FM-DX has no waterfall / zoom / pan / mode / bandwidth control ──
  setFollowMode(_follow: boolean): void {}
  panSpan(): { loHz: number; hiHz: number; movable: boolean } {
    return { loHz: this.freq, hiHz: this.freq, movable: false };
  }
  setMode(_mode: SDRMode): void {}
  setBandwidth(_low: number, _high: number): void {}
  zoom(_frequency: number, _binBandwidth: number): void {}
  pan(_frequency: number): void {}
  resetView(): void {}
  setRate(_divisor: number): void {}
  pauseSpectrum(): void {}
  resumeSpectrum(): void {}
}
