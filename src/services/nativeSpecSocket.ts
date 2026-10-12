/**
 * nativeSpecSocket — the spectrum WebSocket, received natively (Android). docs/NATIVE-SPECTRUM-ENGINE.md, stage 2.
 *
 * ★★★ WHY. React Native's WebSocket delivers every message ON the JS thread, so whenever JS was busy (React renders,
 *   GC, the DAB list's live text) spectrum frames reached the waterfall late and then in a burst — on the XCover the
 *   waterfall "renders smoothly then crawls for a bit" (Stuart, 2026-10-12). Here the socket is OkHttp
 *   (VibeSpecSocketModule.kt, handshake identical to RN's) and every message lands natively first: spectrum frames are
 *   decoded and fed to the waterfall engine at once, on the socket's own thread.
 * ★★ JS STILL GETS EVERY MESSAGE, IN ORDER — the meters' smoothing, the squelch baseline and the peak hold step once
 *   per frame, so nothing may be coalesced. The native side queues each message; JS drains the queue when it is free,
 *   exactly as RN's WebSocket delivered a backlog. A spectrum frame arrives ALREADY DECODED (onspec), so the client
 *   skips its own parse; text, gzip control and anything the client would count as a fault arrive as before
 *   (onmessage).
 * ★ The shape is RN's WebSocket — readyState, onopen/onmessage/onclose/onerror, send(), close() — and its event order:
 *   a failure is CLOSED → error → close(1006, message); the queue is drained BEFORE a close is delivered, so no
 *   message ever lands after its socket closed.
 */
import { NativeEventEmitter, NativeModules, Platform } from 'react-native';

type Drained =
  | { k: 0; text: string }
  | { k: 1; data: ArrayBuffer }
  | { k: 2; bins: Float32Array; freq: number; bytes: number };

type Mod = {
  connect(id: number, url: string, headers: Record<string, string> | null, vibeServer: boolean): void;
  send(id: number, text: string): void;
  close(id: number, code: number, reason: string): void;
  destroy(id: number): void;
};

const mod = NativeModules.VibeSpecSocket as Mod | undefined;
const g = globalThis as unknown as {
  __vibeSpecSockDrain?: (id: number) => Drained[];
  __vibeSpecSockMirror?: (id: number, inFlight: boolean, centerHz: number, binBw: number, shared: boolean) => void;
  __vibeSpectrumCreate?: unknown;
};

let emitter: NativeEventEmitter | null = null;
let nextId = 1;

/** True when the native socket can be used: Android, the module present, the engine library installed (its kill
 *  switch, debug.vibesdr.spectrumengine=0, leaves these globals absent → the JS WebSocket, as before). */
export function nativeSpecSocketAvailable(): boolean {
  if (Platform.OS !== 'android' || !mod) return false;
  if (!g.__vibeSpecSockDrain) {
    try { (NativeModules.VibePowerModule as { installSpectrumEngine?: () => boolean } | undefined)?.installSpectrumEngine?.(); }
    catch { /* stays unavailable */ }
  }
  return !!g.__vibeSpecSockDrain && !!g.__vibeSpecSockMirror;
}

type Handler<E> = ((ev: E) => void) | null;

export class NativeSpecSocket {
  static CONNECTING = 0; static OPEN = 1; static CLOSING = 2; static CLOSED = 3;
  CONNECTING = 0; OPEN = 1; CLOSING = 2; CLOSED = 3;
  readyState = 0;
  binaryType = 'arraybuffer';
  onopen: Handler<unknown> = null;
  onmessage: Handler<{ data: string | ArrayBuffer }> = null;
  onclose: Handler<{ code: number; reason: string }> = null;
  onerror: Handler<{ message: string }> = null;
  /** A decoded spectrum frame (bins in SERVER order, frequency Hz, its size on the wire). */
  onspec: ((bins: Float32Array, freq: number, wireBytes: number) => void) | null = null;
  /** After each drained batch — the client re-mirrors its view state then. */
  ondrained: (() => void) | null = null;

  private readonly id = nextId++;
  private sub: { remove(): void } | null;
  private done = false;

  constructor(url: string, _protocols: undefined, opts: { headers?: Record<string, string> } | undefined, vibeServer: boolean) {
    if (!emitter) emitter = new NativeEventEmitter(NativeModules.VibeSpecSocket);
    this.sub = emitter.addListener('vibeSpecSocket', (ev: { id: number; type: string; code?: number; reason?: string; message?: string }) => {
      if (ev.id !== this.id) return;
      switch (ev.type) {
        case 'pending': this.drain(); break;
        case 'open':
          this.readyState = this.OPEN;
          this.onopen?.({});
          break;
        case 'closed':
          this.drain();
          this.readyState = this.CLOSED;
          this.onclose?.({ code: ev.code ?? 1000, reason: ev.reason ?? '' });
          this.finish();
          break;
        case 'failed':
          this.drain();
          this.readyState = this.CLOSED;
          this.onerror?.({ message: ev.message ?? '' });
          this.onclose?.({ code: 1006, reason: ev.message ?? '' });   // RN: CLOSE_ABNORMAL
          this.finish();
          break;
      }
    });
    mod!.connect(this.id, url, opts?.headers ?? null, vibeServer);
  }

  private drain() {
    if (this.done) return;
    const items = g.__vibeSpecSockDrain?.(this.id) ?? [];
    for (let i = 0; i < items.length; i++) {
      const it = items[i];
      if (it.k === 2) this.onspec?.(it.bins, it.freq, it.bytes);
      else if (it.k === 1) this.onmessage?.({ data: it.data });
      else this.onmessage?.({ data: it.text });
    }
    if (items.length) this.ondrained?.();
  }

  /** The client's view state, for the native side's frame geometry (see _emitSpectrum). */
  mirror(inFlight: boolean, centerHz: number, binBw: number, shared: boolean) {
    if (!this.done) g.__vibeSpecSockMirror?.(this.id, inFlight, centerHz, binBw, shared);
  }

  send(data: string) {
    if (this.readyState === this.CONNECTING) throw new Error('INVALID_STATE_ERR');   // as RN
    mod!.send(this.id, data);
  }

  close(code?: number, reason?: string) {
    if (this.readyState === this.CLOSING || this.readyState === this.CLOSED) return;
    this.readyState = this.CLOSING;
    mod!.close(this.id, typeof code === 'number' ? code : 1000, typeof reason === 'string' ? reason : '');
  }

  private finish() {
    if (this.done) return;
    this.done = true;
    this.sub?.remove(); this.sub = null;
    mod!.destroy(this.id);
  }
}
