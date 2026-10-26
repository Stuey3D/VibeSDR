/**
 * ★★ V12 THE SCANNER'S HOST — the one place the pure engine (services/scanner.ts) meets the app: the backend's tune, the
 * native audio gate, the recorder, VoiceOver, the clock (docs/v12/SCANNER.md).
 *
 * ★★★ THE CONTROLLER LIVES IN A REF, created once per screen, so the session tag list and the found list survive
 *     backgrounding and re-renders (brief §10.2: "keep them in a ref/context, not derived state") and die with the app.
 * ★★ Everything the host reads is a GETTER handed in by the screen, read at the moment it is needed — the reach above
 *    all: the scanner must never act on a span it cached (§4.4).
 * ★ Re-renders are coalesced: the engine reports a change on every hop; the panel is redrawn at most ~8 times a second.
 */
import { useCallback, useEffect, useRef, useState } from 'react';
import { AccessibilityInfo, NativeModules } from 'react-native';
import * as FileSystem from 'expo-file-system/legacy';
import {
  ScannerController, type ScanReach, type ScanTarget, type MeterMode, type ScanTiming, type ScanSample,
} from '../services/scanner';
import type { SDRBackend } from '../services/SDRBackend';
import type { SDRMode } from '../services/sdrProtocol';
import { noteRecordingServer } from '../services/recordingsIndex';

export interface ScannerHostDeps {
  client: () => SDRBackend | null;
  reach: () => ScanReach;
  meterMode: () => MeterMode;
  timing: () => ScanTiming;
  /** The native audio gate, combined by the screen with any other gate it runs (a Kiwi's client squelch). */
  setGate: (open: boolean) => void;
  /** After the scanner tunes: the screen's own readout (setStatus from the backend's status). */
  onTuned: (t: ScanTarget) => void;
  manualRecording: () => boolean;
  /** The receiver's name for the Recordings list (recordingsIndex). */
  serverName: () => string;
  nameFor: (hz: number, tolHz: number) => string | undefined;
}

type RecModule = {
  startRecording?: (freq: number, mode: string) => Promise<unknown>;
  stopRecording?: () => Promise<string | null>;
};

/** VibeSDR_<time>_<freq>_<MODE>.m4a → VibeSDR_<time>_<freq>_<MODE>_scan.m4a: the Recordings list's parser reads the first
 *  four fields, so the suffix marks it without breaking the list (RecordingsOverlay parseRec). */
export function scanRecordingName(path: string): string {
  return /_scan\.m4a$/i.test(path) ? path : path.replace(/\.m4a$/i, '_scan.m4a');
}

export function useScanner(d: ScannerHostDeps) {
  const deps = useRef(d); deps.current = d;
  const [rev, setRev] = useState(0);
  const pending = useRef<ReturnType<typeof setTimeout> | null>(null);
  const bump = useCallback(() => {
    if (pending.current) return;
    pending.current = setTimeout(() => { pending.current = null; setRev((n) => n + 1); }, 120);
  }, []);
  const recChain = useRef<Promise<unknown>>(Promise.resolve());

  const ctlRef = useRef<ScannerController | null>(null);
  if (!ctlRef.current) {
    const Rec = NativeModules.VibePowerModule as RecModule | undefined;
    ctlRef.current = new ScannerController({
      now: () => Date.now(),
      reach: () => deps.current.reach(),
      tune: (t: ScanTarget) => {
        const c = deps.current.client(); if (!c) return;
        // ★★ recenter:false always (brief §3 — the scanner never moves the view); then the tag's own passband.
        c.tune(t.frequency, t.mode as SDRMode, { recenter: false });
        c.setBandwidth(t.bwLow, t.bwHigh);
        deps.current.onTuned(t);
      },
      setGate: (open: boolean) => deps.current.setGate(open),
      meterMode: () => deps.current.meterMode(),
      manualRecording: () => deps.current.manualRecording(),
      startRecording: (t: ScanTarget) => {
        // ★ Serialised: a stop and the next start must reach the recorder in order, never overlapping.
        recChain.current = recChain.current.then(() => Rec?.startRecording?.(Math.round(t.frequency), t.mode)).catch(() => {});
      },
      stopRecording: () => {
        recChain.current = recChain.current.then(async () => {
          const path = await Rec?.stopRecording?.();
          if (!path) return;
          const to = scanRecordingName(path);
          try {
            await FileSystem.moveAsync({ from: path.startsWith('file://') ? path : `file://${path}`, to: to.startsWith('file://') ? to : `file://${to}` });
            void noteRecordingServer(to, deps.current.serverName());
          } catch { void noteRecordingServer(path, deps.current.serverName()); }
        }).catch(() => {});
      },
      announce: (text: string) => { AccessibilityInfo.announceForAccessibility(text); },
      nameFor: (hz: number, tol: number) => deps.current.nameFor(hz, tol),
      changed: bump,
    }, d.timing());
  }
  const ctl = ctlRef.current;
  ctl.timing = d.timing();   // ★ the backend can change under the same screen (a reconnect, a radio switch)

  // ★ The clock: settle / sample windows, hang, time limit, priority probes, the reach check. 100 ms while engaged.
  const [engaged, setEngaged] = useState(false);
  useEffect(() => { setEngaged(ctl.running()); }, [rev, ctl]);
  useEffect(() => {
    if (!engaged) return;
    const iv = setInterval(() => ctl.tick(), 100);
    return () => clearInterval(iv);
  }, [engaged, ctl]);
  // ★★ Never leave the gate shut (brief §7.4 / acceptance 11): stop on unmount, which also opens it.
  useEffect(() => () => { ctl.stop(); if (pending.current) clearTimeout(pending.current); }, [ctl]);

  /** Every meter frame. Cheap when idle (the engine returns at once). */
  const feed = useCallback((s: ScanSample) => { if (ctl.running()) { ctl.feed(s); ctl.tick(); } }, [ctl]);

  /** Resolves when the scanner's recorder work (a stop + rename) has finished — a manual REC waits for it. */
  const recIdle = useCallback(() => recChain.current, []);

  return { ctl, rev, engaged, feed, recIdle };
}
