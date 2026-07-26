import { useEffect, useRef } from 'react';
import { NativeModules, Platform } from 'react-native';
import { decodeVibeAdpcmFrame } from '../services/imaAdpcm';

// VibeSDR V4 — local-hardware / RTL-TCP audio.
//
// The on-device local-SDR shim serves demodulated PCM on its /ws/audio endpoint.
//  - Android: consumed NATIVELY by the foreground service (startLocalAudio) so
//    audio + the media card survive backgrounding (the service keeps the in-process
//    shim alive). Tune changes ride the same WS via sendLocalTune.
//  - iOS: no native local-audio pump (and no foreground service), so we read
//    /ws/audio here in JS and push the PCM through the SAME external-PCM engine
//    OWRX/Kiwi use (startExternalAudio / pushExternalPcm). Tune changes are sent
//    on the WS directly.

const Vibe = NativeModules.VibePowerModule as {
  startLocalAudio?:   (host: string, port: number, initialTune: string, authSuffix: string) => void;
  pushExternalOpus?:  (b64: string, sampleRate: number, channels: number) => void;
  sendLocalTune?:     (json: string) => void;
  stopLocalAudio?:    () => void;
  startExternalAudio?: (rate: number, pauseMode?: string) => void;
  pushExternalPcm?:   (b64: string, rate: number, channels?: number) => void;
  stopExternalAudio?: () => void;
  setInstanceName?:   (name: string) => void;
} | undefined;

const USE_NATIVE_PUMP = Platform.OS === 'android';

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function bytesToBase64(b: Uint8Array): string {
  let out = '';
  for (let i = 0; i < b.length; i += 3) {
    const n = (b[i] << 16) | ((i + 1 < b.length ? b[i + 1] : 0) << 8) | (i + 2 < b.length ? b[i + 2] : 0);
    out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] +
           (i + 1 < b.length ? B64[(n >> 6) & 63] : '=') +
           (i + 2 < b.length ? B64[n & 63] : '=');
  }
  return out;
}

export interface LocalAudioPlayerProps {
  port:          number | null;
  frequency:     number;
  mode:          string;
  bandwidthLow:  number;
  bandwidthHigh: number;
  instanceName?: string;
  // VibeServer (remote shim): a LAN host + PIN auth query suffix. Defaults keep
  // the local-hardware path on loopback with no auth.
  host?:         string;
  authSuffix?:   string;
  // The client's session id — MUST match the spectrum socket's user_session_id so
  // VibeServer treats the audio + spectrum sockets as ONE occupant. Without it the
  // audio socket is filed as an anonymous "anon:IP" occupant and collides with the
  // spectrum socket's real id → the second socket is refused as "in use" (2026-07-24).
  sessionId?:    string;
  /** Every inbound audio byte, so the connection readout can show the TRUE total.
   *  ★★ Without this the meter reported SPECTRUM ONLY — it showed 12 KB/s while
   *  the link was actually carrying 198, because uncompressed audio was 186 of
   *  it. A bandwidth figure that omits the largest consumer is not half a
   *  measurement, it is a misleading one: it gave false confidence exactly where
   *  the problem was, and hid it for months. */
  onBytes?:      (n: number) => void;
}

function tuneJson(frequency: number, mode: string, bandwidthLow: number, bandwidthHigh: number) {
  return JSON.stringify({ type: 'tune', frequency, mode, bandwidthLow, bandwidthHigh });
}

export default function LocalAudioPlayer(
  { port, frequency, mode, bandwidthLow, bandwidthHigh, instanceName,
    host = '127.0.0.1', authSuffix = '', sessionId = '', onBytes }: LocalAudioPlayerProps,
) {
  const started = useRef(false);
  const ws      = useRef<WebSocket | null>(null);
  const extStarted = useRef(false);

  // Latest tune, so start can send it immediately without a stale closure.
  const tune = useRef({ frequency, mode, bandwidthLow, bandwidthHigh });
  tune.current = { frequency, mode, bandwidthLow, bandwidthHigh };

  useEffect(() => {
    if (port == null) return;
    const { frequency: f, mode: m, bandwidthLow: bl, bandwidthHigh: bh } = tune.current;
    Vibe?.setInstanceName?.(instanceName ?? 'Local Hardware');

    // Carry the session id on the audio socket so VibeServer matches it to the spectrum socket and
    // counts the pair as ONE occupant (else the audio socket is an anonymous occupant and the second
    // socket is refused "in use"). Both platforms turn a "&key=val…" suffix into "?key=val…", so fold
    // the id in front of the PIN suffix and reuse the same string for the native pump and the JS WS.
    // ★★ ASK FOR COMPRESSED AUDIO. Without `codec=opus` VibeServer falls back to
    // raw PCM — 48 kHz stereo int16 is 187 KB/s, and it was measured at 186 on
    // the wire while the spectrum beside it got 8. The shim sends both sockets
    // under one blocking mutex, so that torrent also starved the spectrum
    // emitter down to ~58% of its configured frame rate. Opus is ~4x lighter
    // again than the ADPCM it replaced, and the decoder is already on the native
    // side (pushExternalOpus).
    const combinedSuffix = (sessionId ? `&user_session_id=${encodeURIComponent(sessionId)}` : '')
      + '&codec=opus' + authSuffix;

    if (USE_NATIVE_PUMP) {
      Vibe?.startLocalAudio?.(host, port, tuneJson(f, m, bl, bh), combinedSuffix);
      started.current = true;
      return () => { if (started.current) { Vibe?.stopLocalAudio?.(); started.current = false; } };
    }

    // iOS: read /ws/audio in JS, push PCM through the external-PCM engine. For a
    // VibeServer the URL points at a LAN host and carries the PIN auth suffix.
    let closed = false;
    // combinedSuffix is "&user_session_id=…&vs_nonce=…&vs_auth=…" (built to append to an existing
    // query). /ws/audio has no query, so it needs a leading "?" instead of "&".
    const authQ = combinedSuffix ? '?' + combinedSuffix.replace(/^&/, '') : '';
    const sock = new WebSocket(`ws://${host}:${port}/ws/audio${authQ}`);
    sock.binaryType = 'arraybuffer';
    ws.current = sock;
    started.current = true;
    sock.onopen = () => { if (!closed) sock.send(tuneJson(f, m, bl, bh)); };
    sock.onmessage = (e) => {
      if (closed || !(e.data instanceof ArrayBuffer)) return;
      const buf = e.data as ArrayBuffer;
      onBytes?.(buf.byteLength);   // count BEFORE any early return — every byte crossed the link
      if (buf.byteLength <= 6) return;
      const dv = new DataView(buf);
      const format = dv.getUint8(1);
      let rate: number, channels: number, pcmBytes: Uint8Array;
      if (format === 3) {
        // OPUS — hand the packet straight to the native decoder. The base64 hop
        // was already happening for raw PCM; an Opus packet is ~20x smaller, so
        // this is LESS bridge traffic than before, not more.
        const rate3 = dv.getUint32(2, true);
        const ch3   = dv.getUint8(0);
        if (!extStarted.current) { Vibe?.startExternalAudio?.(rate3, 'resume'); extStarted.current = true; }
        Vibe?.pushExternalOpus?.(bytesToBase64(new Uint8Array(buf, 6)), rate3, ch3 === 2 ? 2 : 1);
        return;
      }
      if (format === 1 || format === 2) {
        // VibeServer compressed audio (IMA-ADPCM). Decode to int16 PCM bytes.
        const d = decodeVibeAdpcmFrame(buf);
        rate = d.rate; channels = d.channels;
        pcmBytes = new Uint8Array(d.pcm.buffer, d.pcm.byteOffset, d.pcm.byteLength);
      } else {
        rate = dv.getUint32(2, true);
        channels = dv.getUint8(0);
        pcmBytes = new Uint8Array(buf, 6);
      }
      if (!extStarted.current) { Vibe?.startExternalAudio?.(rate, 'resume'); extStarted.current = true; }
      Vibe?.pushExternalPcm?.(bytesToBase64(pcmBytes), rate, channels === 2 ? 2 : 1);
    };
    return () => {
      closed = true;
      try { sock.close(); } catch {}
      ws.current = null;
      if (extStarted.current) { Vibe?.stopExternalAudio?.(); extStarted.current = false; }
      started.current = false;
    };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [port]);

  // Forward tune/mode/bandwidth changes (native sends on the WS for Android; iOS
  // sends directly on the JS WS).
  useEffect(() => {
    if (!started.current) return;
    if (USE_NATIVE_PUMP) {
      Vibe?.sendLocalTune?.(tuneJson(frequency, mode, bandwidthLow, bandwidthHigh));
    } else {
      const sock = ws.current;
      if (sock && sock.readyState === WebSocket.OPEN) {
        sock.send(tuneJson(frequency, mode, bandwidthLow, bandwidthHigh));
      }
    }
  }, [frequency, mode, bandwidthLow, bandwidthHigh]);

  useEffect(() => {
    Vibe?.setInstanceName?.(instanceName ?? 'Local Hardware');
  }, [instanceName]);

  return null;
}
