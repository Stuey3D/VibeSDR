/**
 * Diagnostics report — what the user can choose to send us when something breaks.
 *
 * ★★★ WHY. Nine crashes were counted in App Store Connect and not one produced a
 * readable log: Apple's crash pipeline is opt-in and most people never opt in. The one
 * report we did get (iPhone 11, iOS 16.5.1, 9.0.1 build 8) was an uncaught ObjC
 * exception whose report contained NO `Last Exception Backtrace` — the throw site was
 * never recorded, so symbolicating it would have told us nothing. We had a crash we
 * could count and could not read.
 *
 * ★★ LOCAL ONLY, AND USER-SENT. Nothing here is transmitted by the app. The report is
 * assembled on demand, shown to the user, and handed to the SYSTEM SHARE SHEET so they
 * choose where it goes. The app makes no network call and we run no server — which is
 * why this needs no App Privacy declaration (data that never leaves the device to us is
 * not "collected") and why the privacy policy's "everything stays on your device"
 * remains true.
 *
 * ★ NEVER add a PIN, an admin password, a callsign, or a precise location. If a future
 * field cannot be shown to the user without embarrassment, it does not belong here.
 */
import { Platform, NativeModules } from 'react-native';
import { APP_VERSION } from '../constants/version';
import { getLastCrash } from './crashGuard';

const Vibe = (NativeModules as {
  VibePowerModule?: {
    getNativeCrash?: () => Promise<Record<string, unknown> | null>;
    clearNativeCrash?: () => void;
    getDeviceInfo?: () => Promise<{ model?: string; os?: string; systemName?: string }>;
  };
}).VibePowerModule;

const when = (ms?: number) => (ms ? new Date(ms).toISOString() : 'never');

/** The whole report as plain text — deliberately readable, because the user is shown it. */
export async function buildDiagnostics(extra?: Record<string, string | number | boolean>): Promise<string> {
  const lines: string[] = [];
  lines.push('VibeSDR diagnostics');
  lines.push('===================');
  lines.push(`generated : ${new Date().toISOString()}`);
  lines.push(`app       : ${APP_VERSION}`);
  lines.push(`platform  : ${Platform.OS} ${String(Platform.Version)}`);

  try {
    const d = await Vibe?.getDeviceInfo?.();
    if (d) lines.push(`device    : ${d.model ?? '?'} · ${d.systemName ?? ''} ${d.os ?? ''}`.trimEnd());
  } catch {}

  if (extra) for (const [k, v] of Object.entries(extra)) lines.push(`${k.padEnd(10)}: ${String(v)}`);

  // ── JS crash (crashGuard) ────────────────────────────────────────────────
  lines.push('', '--- last JS error ---');
  try {
    const c = await getLastCrash();
    if (!c) lines.push('none recorded');
    else {
      lines.push(`when   : ${when(c.ts)}`);
      lines.push(`screen : ${c.route ?? '?'}`);
      lines.push(`message: ${c.message}`);
      if (c.stack) lines.push('stack  :', c.stack.slice(0, 3000));
    }
  } catch { lines.push('unavailable'); }

  // ── Native uncaught ObjC exception (VibeCrashLog) ────────────────────────
  lines.push('', '--- last native exception ---');
  try {
    const n = await Vibe?.getNativeCrash?.();
    if (!n) lines.push('none recorded');
    else {
      lines.push(`when   : ${when(Number(n.ts))}`);
      lines.push(`name   : ${String(n.name ?? '?')}`);
      lines.push(`reason : ${String(n.reason ?? '')}`);
      lines.push(`os     : ${String(n.os ?? '?')} on ${String(n.model ?? '?')}`);
      if (n.stack) lines.push('stack  :', String(n.stack).slice(0, 3000));
    }
  } catch { lines.push('unavailable (Android, or module missing)'); }

  lines.push('', '(No PINs, passwords or location are included. Nothing is sent unless you send it.)');
  return lines.join('\n');
}

export function clearDiagnostics(): void {
  try { Vibe?.clearNativeCrash?.(); } catch {}
}
