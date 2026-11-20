/* ★★ THE LOCAL HARDWARE BENCHMARK RUN — one run at a time, owned HERE rather than by the card that started it.
 *
 *  The card lives in LocalHardwarePanel, a Modal whose children unmount when it is closed. A run takes minutes, and
 *  closing the sheet half-way must not lose the resume: the waterfall was paused for the run (the radio is released for
 *  the measurement — nativeRunBenchmark `ownRadio`) and has to come back whatever happens. So the run, its progress and
 *  its result are module state, the card only subscribes, and `resume` is called in `finally`.
 *  ★ Android only: VibeLocalSDR.runBenchmarkOwnRadio exists in the Android app alone; anywhere else the card is not drawn. */
import { NativeModules } from 'react-native';
import type { BenchResult } from './localBench';

export interface LocalBenchState {
  busy: boolean;
  /** {step, steps, label} while the engine measures — polled, as the server screen does. null before it starts
   *  (the test recordings are fetched first) and after. */
  prog: { step: number; steps: number; label: string } | null;
  /** The device's last result (shared with the server screen — benchmark.json). null = never measured here. */
  result: BenchResult | null;
  /** Why the last run did not finish, in the engine's words; '' after a good run. */
  error: string;
  loaded: boolean;
}

let state: LocalBenchState = { busy: false, prog: null, result: null, error: '', loaded: false };
const subs = new Set<() => void>();
const set = (p: Partial<LocalBenchState>) => { state = { ...state, ...p }; subs.forEach((f) => f()); };

export function localBenchState(): LocalBenchState { return state; }
export function subscribeLocalBench(f: () => void): () => void { subs.add(f); return () => { subs.delete(f); }; }

const mod = () => (NativeModules as any).VibeLocalSDR as
  { lastBenchmark?: () => Promise<string | null>; runBenchmarkOwnRadio?: (wantDab: boolean) => Promise<string>;
    benchProgress?: () => Promise<string> } | undefined;

/** The engine can measure from Local Hardware (an Android build with runBenchmarkOwnRadio). */
export function localBenchAvailable(): boolean { return !!mod()?.runBenchmarkOwnRadio; }

/** ★ The last result — read every time the card mounts, so a run made from the SERVER screen since shows here too. */
export async function loadLocalBench(): Promise<void> {
  const m = mod();
  if (!m?.lastBenchmark || state.busy) { set({ loaded: true }); return; }
  try {
    const s = await m.lastBenchmark();
    let r: BenchResult | null = null;
    if (s) { try { r = JSON.parse(s); } catch { r = null; } }
    set({ result: r ?? state.result, loaded: true });
  } catch { set({ loaded: true }); }
}

/** ★★★ RUN IT. `pause` stops this app's own listening (the waterfall) before the engine releases the radio; `resume`
 *  brings it back after — ALWAYS, in `finally`, success or not. `wantDab`: fetch the DAB recording (~18 MB, once). */
export async function runLocalBench(h: { pause: () => void; resume: () => void }, wantDab: boolean): Promise<void> {
  const m = mod();
  if (state.busy) return;
  if (!m?.runBenchmarkOwnRadio) { set({ error: 'This build cannot measure itself.' }); return; }
  set({ busy: true, error: '', prog: null });
  try { h.pause(); } catch { /* nothing to pause is not a reason to refuse */ }
  /* ★ POLLED, not pushed — the server screen's reason: an event per step would cross onto the JS thread in the middle of
   *  the thing being measured; a poll every 700 ms cannot perturb it. */
  const timer = setInterval(() => {
    m.benchProgress?.().then((s) => {
      try {
        const p = JSON.parse(s);
        set({ prog: p?.running ? { step: p.step | 0, steps: p.steps | 0, label: String(p.label || '') } : null });
      } catch { /* a malformed reading leaves the bar where it was */ }
    }).catch(() => {});
  }, 700);
  try {
    const j = JSON.parse(await m.runBenchmarkOwnRadio(wantDab));
    set({ result: j, error: '' });
  } catch (e: any) {
    set({ error: 'The measurement did not finish: ' + (e?.message || String(e)) });
  } finally {
    clearInterval(timer);
    try { h.resume(); } catch { /* the screen has gone — its next session starts the radio afresh */ }
    set({ busy: false, prog: null });
  }
}
