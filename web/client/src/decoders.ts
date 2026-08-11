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
 *     {"type":"digital_spot","data":{mode,callsign,snr,frequency,band,grid,timestamp}}
 *
 * NB WEFAX and SSTV BOTH use 0x01 for a line, with different payloads — so
 * frames can only be parsed against the decoder currently attached. Never sniff.
 */

import { withAuth, type AuthState } from './auth';
import { wsBase } from './origin';

export type DecoderMode = 'rtty' | 'navtex' | 'wefax' | 'sstv' | 'rds' | 'time' | null;

export interface Spot {
  mode: 'FT8' | 'FT4';
  callsign: string;
  snr: number;
  frequency: number;   // RF Hz (dial + audio offset, computed server-side)
  band: string;        // e.g. "20m" — the shim works this out
  grid: string;        // Maidenhead, may be empty
  timestamp: number;   // ms
}

export interface DecoderCallbacks {
  /** Decoded characters (RTTY/NAVTEX), appended as they arrive. */
  onText?: (text: string) => void;
  /** Decoder lock/idle state (RTTY/NAVTEX). */
  onState?: (state: number) => void;
  /** A new image is starting. height is 0 for WEFAX (it grows without bound). */
  onImageStart?: (width: number, height: number) => void;
  /** One image line. `rgb` is true for SSTV (3 bytes/px), false for WEFAX (grey). */
  onImageLine?: (y: number, width: number, px: Uint8Array, rgb: boolean) => void;
  onImageDone?: () => void;
  /** SSTV mode name, e.g. "Martin 1". */
  onSstvMode?: (name: string) => void;
  onStatus?: (text: string) => void;
  /** An FT8/FT4 decode. */
  onSpot?: (spot: Spot) => void;
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
}

/** The shim's extension names. */
const EXT: Record<Exclude<DecoderMode, null>, string> = {
  rtty: 'fsk',
  // ★ Not an audio decoder — attaching 'rds' turns on the extended RDS stream. It goes
  // through the decoder path deliberately: selecting it IS the toggle, so the cost is paid
  // only while it is open (see BRIEF-rds-receiver.md).
  rds: 'rds',
  navtex: 'navtex',
  wefax: 'wefax',
  sstv: 'sstv',
  // ★ The time signals. One extension; WHICH station is a parameter, exactly as RTTY's shift and
  //   baud are — they share a decoder and differ only in how the seconds are marked.
  time: 'time',
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

  constructor(host: string, auth: AuthState, cb: DecoderCallbacks) {
    // ★ Same rule as every other URL: an https page cannot open a ws:// socket. See origin.ts.
    this.url = `${wsBase(host)}${withAuth('/ws/dxcluster', auth)}`;
    this.cb = cb;
  }

  connect() {
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
      if (this.spotsOn) this._send({ type: 'subscribe_digital_spots' });
    };
    ws.onmessage = (e) => {
      if (typeof e.data === 'string') this._handleText(e.data);
      else this._handleBinary(e.data as ArrayBuffer);
    };
    ws.onclose = () => {
      this.cb.onClose?.();
      if (!this.closedByUs) setTimeout(() => this.connect(), 3000);
    };
    ws.onerror = () => {};
  }

  close() {
    this.closedByUs = true;
    this.ws?.close();
    this.ws = null;
  }

  private _send(o: Record<string, unknown>) {
    const ws = this.ws;
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(o));
  }

  private _sendAttach(mode: Exclude<DecoderMode, null>, params: DecoderParams = {}) {
    this._send({ type: 'audio_extension_attach', extension_name: EXT[mode], ...params });
  }

  /** Attach a decoder (detaching whatever was running). */
  attach(mode: Exclude<DecoderMode, null>, params: DecoderParams = {}) {
    this.mode = mode;
    // ★ Remembered so a reconnect can re-assert the SAME decoder, not the default one.
    this.params = params;
    this._sendAttach(mode, params);
  }

  detach() {
    if (!this.mode) return;
    this.mode = null;
    this.params = {};
    this._send({ type: 'audio_extension_detach' });
  }

  /** FT8/FT4 spots run independently of the text/image decoders. */
  setSpots(on: boolean) {
    if (this.spotsOn === on) return;
    this.spotsOn = on;
    this._send({ type: on ? 'subscribe_digital_spots' : 'unsubscribe_digital_spots' });
  }

  get attached(): DecoderMode { return this.mode; }
  get spotsEnabled(): boolean { return this.spotsOn; }

  // ── Inbound ────────────────────────────────────────────────────────────────

  private _handleText(raw: string) {
    let msg: any;
    try { msg = JSON.parse(raw); } catch { return; }
    if (msg.type === 'digital_spot' && msg.data) {
      const d = msg.data;
      this.cb.onSpot?.({
        mode: d.mode === 'FT4' ? 'FT4' : 'FT8',
        callsign: d.callsign ?? '',
        snr: d.snr ?? 0,
        frequency: d.frequency ?? 0,
        band: d.band ?? '',
        grid: d.grid ?? '',
        timestamp: d.timestamp ?? Date.now(),
      });
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
        if (op === 0x01 && buf.byteLength >= 13) {
          const len = dv.getUint32(9, false);           // big-endian
          const text = new TextDecoder().decode(new Uint8Array(buf, 13, Math.min(len, buf.byteLength - 13)));
          if (text) this.cb.onText?.(text);
        } else if (op === 0x03 && buf.byteLength >= 2) {
          this.cb.onState?.(dv.getUint8(1));
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
            this.cb.onImageStart?.(0, 0);                // redraw of the current image
            return;
          default:
            return;                                      // 0x04 sync — nothing to show
        }

      default:
        return;   // nothing attached; spots arrive as text, not here
    }
  }
}
