// Crash guard — OWRX (and other) servers are notoriously flaky; a server that
// restarts mid-session can push our JS into a state where a timer callback,
// unhandled promise rejection, or render throws. In a release build RN's
// default fatal handler calls abort() → the whole app dies (seen in the
// 2026-06-15 field crashes: RCTExceptionsManager.reportFatal → abort).
//
// Instead we install a global handler that LOGS the error (persisted so the
// next launch — or a USB pull — can read the exact message + stack) and
// RECOVERS to the instance picker rather than aborting. The companion
// CrashBoundary (components/CrashBoundary.tsx) does the same for render errors.

import { Alert } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';
import type { NavigationContainerRef } from '@react-navigation/native';
import type { RootStackParamList } from '../../App';

const KEY = 'vibe.lastCrash';
let recovering = false;

export type CrashInfo = { ts: number; message: string; stack?: string; route?: string };

export async function getLastCrash(): Promise<CrashInfo | null> {
  try { const s = await AsyncStorage.getItem(KEY); return s ? JSON.parse(s) : null; } catch { return null; }
}
export async function clearLastCrash(): Promise<void> {
  try { await AsyncStorage.removeItem(KEY); } catch {}
}

/** Persist + log a caught error. Exported so CrashBoundary can reuse it. */
export function recordCrash(error: any, route?: string): CrashInfo {
  const info: CrashInfo = {
    ts: Date.now(),
    message: String(error?.message ?? error),
    stack: typeof error?.stack === 'string' ? error.stack.slice(0, 4000) : undefined,
    route,
  };
  try { AsyncStorage.setItem(KEY, JSON.stringify(info)); } catch {}
  // NSLog via console so a live `idevicesyslog` capture also sees it.
  console.log('[CrashGuard]', route ?? '?', info.message, '\n', info.stack ?? '');
  return info;
}

// ── Whose fault was it? ───────────────────────────────────────────────────────
//
// ★ This used to be assumed rather than decided: EVERY crash was reported as "the
// SDR server stopped responding... this is a server issue, not a problem with
// VibeSDR". That is wrong in both directions and the app must not do either.
//
//  - Blaming the server for OUR exception sends people off restarting a receiver
//    that was perfectly healthy, and does it in OUR voice, on someone else's
//    equipment. (Observed 2026-07-25: a TypeError in the menu produced exactly
//    that message against a working UberSDR.)
//  - Equally, taking the blame for a genuine server fault or policy — a restart, a
//    dropped socket, an enforced session limit — is dishonest the other way and
//    makes us look broken when we are not.
//
// So classify from evidence, and where the evidence is thin, say so plainly rather
// than picking a culprit.
export type FaultOwner = 'server' | 'app' | 'unknown';

export function classifyFault(error: any): FaultOwner {
  const name = String(error?.name ?? '');
  const text = `${name} ${String(error?.message ?? error ?? '')}`.toLowerCase();

  // Unmistakably our own code — a programming error, whatever the server did.
  // Checked FIRST: a JS exception can easily mention a socket in passing.
  if (/typeerror|referenceerror|rangeerror|syntaxerror/.test(text)
      || /is not a function|is not an object|undefined is not|null is not/.test(text)
      || /property '[^']*' doesn'?t exist|cannot read propert/.test(text)) return 'app';

  // Genuine transport or far-end failures.
  if (/websocket|socket|network request failed|network error|timed ?out|timeout/.test(text)
      || /econnreset|econnrefused|enotfound|etimedout|dns|tls|ssl|handshake/.test(text)
      || /connection (closed|lost|refused|reset|failed)|disconnect/.test(text)
      || /http [45]\d\d/.test(text)) return 'server';

  return 'unknown';
}

/** Title + body for a recovered crash, attributed honestly. */
export function faultMessage(error: any, detail: string): { title: string; body: string } {
  const who = classifyFault(error);
  const tail = `\n\n(detail: ${detail})`;
  if (who === 'app') {
    return {
      title: 'VibeSDR hit a problem',
      body: 'Something went wrong inside VibeSDR — this one is ours, not the '
          + 'server\u2019s. You\u2019ve been returned to the server list. If it keeps '
          + 'happening, the detail below is what to report.' + tail,
    };
  }
  if (who === 'server') {
    return {
      title: 'Server connection lost',
      body: 'The SDR server stopped responding — SDR servers (OpenWebRX especially) '
          + 'restart from time to time, and some limit how long you may stay '
          + 'connected. You\u2019ve been returned to the server list.' + tail,
    };
  }
  return {
    title: 'Connection ended',
    body: 'VibeSDR lost the receiver and returned you to the server list. There '
        + 'isn\u2019t enough detail to say whether that came from the server or from '
        + 'VibeSDR — if it only happens on one receiver it is likely that server, and '
        + 'if it happens everywhere it is likely us.' + tail,
  };
}

export function installCrashGuard(navRef: NavigationContainerRef<RootStackParamList>): void {
  const EU = (globalThis as any).ErrorUtils;
  if (!EU?.setGlobalHandler) return;
  const prev = EU.getGlobalHandler?.();

  EU.setGlobalHandler((error: any, isFatal?: boolean) => {
    const route = navRef.isReady() ? navRef.getCurrentRoute()?.name : undefined;
    const info = recordCrash(error, route);

    // Non-fatal: just log (keep the dev redbox in __DEV__).
    if (!isFatal) { if (__DEV__ && prev) prev(error, isFatal); return; }

    // Fatal: recover instead of letting RN abort the process.
    if (!recovering) {
      recovering = true;
      try {
        if (navRef.isReady() && navRef.getCurrentRoute()?.name !== 'InstancePicker') {
          navRef.reset({ index: 0, routes: [{ name: 'InstancePicker' }] });
        }
      } catch {}
      setTimeout(() => {
        recovering = false;
        const m = faultMessage(error, info.message);
        Alert.alert(m.title, m.body);
      }, 350);
    }

    // In dev, still surface the real error so genuine bugs aren't masked.
    if (__DEV__ && prev) prev(error, isFatal);
  });
}
