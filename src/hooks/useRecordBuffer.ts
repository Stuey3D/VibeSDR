import { useCallback, useEffect, useRef, useState } from 'react';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { VibePowerModule } from '../components/AudioPlayer';
import { noteAudioEvent } from '../services/audioPathLog';
import {
  RecordBufferAnchor, RECORD_BUFFER_KEY, parseRecordBufferSeconds, type RecordBufferSeconds,
} from '../services/recordBuffer';

type RecBufNative = { setRecordBuffer?: (s: number) => void; clearRecordBuffer?: () => void };

/**
 * ★★ V12 RECORD BUFFER — the setting (app-wide, persisted) and the retune rule wired to the native ring.
 *  SDRScreen and TunerScreen each call this with where the listener is; the rule itself is
 *  services/recordBuffer.ts (unit-tested), the ring is native (VibePowerModule.swift / VibeStreamService.kt).
 *  ★ A native build without the methods simply has no buffer: recordings start empty, exactly as before.
 *  ★ The native ring outlives a screen, so the first update on a new screen clears it — what the last screen
 *    left in it was another server's audio.
 */
export function useRecordBuffer(dial: { freqHz: number; mode: string; source: string }):
  [RecordBufferSeconds, (s: RecordBufferSeconds) => void] {
  const [seconds, setSecondsState] = useState<RecordBufferSeconds>(0);
  const anchor = useRef<RecordBufferAnchor | null>(null);
  if (!anchor.current) anchor.current = new RecordBufferAnchor(0);
  const native = VibePowerModule as unknown as RecBufNative | undefined;

  useEffect(() => {
    AsyncStorage.getItem(RECORD_BUFFER_KEY)
      .then((v) => setSecondsState(parseRecordBufferSeconds(v)))
      .catch(() => {});
  }, []);

  const setSeconds = useCallback((s: RecordBufferSeconds) => {
    setSecondsState(s);
    AsyncStorage.setItem(RECORD_BUFFER_KEY, String(s)).catch(() => {});
  }, []);

  // The setting → native. Off frees the ring there; 30 ↔ 60 keeps what it holds.
  useEffect(() => {
    try { native?.setRecordBuffer?.(seconds); } catch {}
    anchor.current!.setWindow(seconds);
    if (seconds > 0) noteAudioEvent(`record buffer ${seconds} s`);
  }, [seconds, native]);

  // The retune rule: a move past the mode's allowance from the anchor, a mode change, a source change → clear.
  const { freqHz, mode, source } = dial;
  useEffect(() => {
    if (seconds <= 0 || !(freqHz > 0)) return;
    if (anchor.current!.update({ freqHz, mode, source }, Date.now())) {
      try { native?.clearRecordBuffer?.(); } catch {}
    }
  }, [seconds, freqHz, mode, source, native]);

  // Turning it on starts afresh here: whatever happens next is the first thing in the buffer.
  useEffect(() => {
    if (seconds <= 0) anchor.current = new RecordBufferAnchor(0);
  }, [seconds]);

  return [seconds, setSeconds];
}

/** startRecording resolves `{ path, bufferedSeconds }` on builds with the buffer, a bare path on older ones. */
export function bufferedSecondsOf(result: unknown): number {
  if (result && typeof result === 'object') {
    const s = Number((result as { bufferedSeconds?: unknown }).bufferedSeconds);
    return isFinite(s) && s > 0 ? s : 0;
  }
  return 0;
}
