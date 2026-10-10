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
import { APP_VERSION, RELEASE_LABEL } from '../constants/version';
import { getLastCrash } from './crashGuard';
import { audioPathDump, localListenPort } from './audioPathLog';
import { unhandledLog } from './protocolLog';
import { readCrumbs } from './crumbs';
import { faultSummary, faultTotal } from './faultLog';
import { getVibeServerStatus, getConnectedRadio } from './vibeServer';

const Vibe = (NativeModules as {
  VibePowerModule?: {
    getNativeCrash?: () => Promise<Record<string, unknown> | null>;
    clearNativeCrash?: () => void;
    getDeviceInfo?: () => Promise<{ model?: string; os?: string; systemName?: string;
                                    versionName?: string; versionCode?: string; packageId?: string;
                                    processStartMs?: number }>;
  };
}).VibePowerModule;

/* ★★★ THE REPORT'S OWN FORMAT VERSION (2026-10-07) — printed in the header, so a reader knows which sections
 *  that build COULD have written, and "none recorded" from a build that could not record is never read as
 *  "nothing happened". Bump it whenever a section is added.
 *    1 — JS error, boot crumbs, audio path, unhandled messages, contained faults (to V11 RC13).
 *    2 — + Android native crash capture: exit reason, tombstone, redacted log tail (RC14, 2026-10-05).
 *    3 — + full build identity (version, RC, build number, package), process uptime, the server section;
 *        Lite records native and JS crashes (2026-10-07). */
export const DIAG_FORMAT = 3;

const when = (ms?: number) => (ms ? new Date(ms).toISOString() : 'never');

/** ★ When this JS bundle was evaluated — the fallback "up since" where native cannot say (iOS). */
const jsStart = Date.now();

function dur(ms: number): string {
  const m = Math.max(0, Math.floor(ms / 60000));
  const d = Math.floor(m / 1440), h = Math.floor((m % 1440) / 60), mm = m % 60;
  return d ? `${d}d ${h}h ${mm}m` : h ? `${h}h ${mm}m` : `${mm}m`;
}

/* ★★★ THE SERVER HALF OF THE REPORT (2026-10-07). NickB's Pixel 6 hosts a VibeServer and keeps crashing;
 *  the report he sent said nothing about the server at all — which version, which radio, how long it had
 *  been up. On Android the server runs INSIDE the app process (no android:process), so the native crash
 *  section below already covers it; this section says what it was doing.
 *  ★ PRIVATE, like the rest: no address (the status's `ip`/`clientAddr` are deliberately NOT printed), no
 *    PIN, no admin password, no location, no owner's radio name. Version comes from the server's own
 *    /vibeserver.json on loopback — the same identity every client reads — and only its version fields.
 *  ★ Android only: iOS has no server mode, and vibeServer's status call shouts when its bridge is absent. */
/** ★★ THE RADIO'S HEALTH, from the server's own counters in /vibeserver.json: IQ the DSP never got, IQ the radio
 *  library lost before we saw it (USB), and listener threads that fell behind. One line, and only what the server
 *  says — a stalling radio (an Airspy HF+ on a tablet's USB, 2026-10-10) shows here as drops with a recent "last". */
export function radioHealthLine(j: any): string | null {
  if (!j || typeof j !== 'object' || j.iqDrops == null) return null;
  const ago = (s: any) => (typeof s === 'number' && s >= 0 ? `, last ${s < 120 ? `${s}s` : `${Math.round(s / 60)}m`} ago` : '');
  const parts = [`IQ drops ${j.iqDrops}${ago(j.iqDropAgo)}`];
  if (j.usbDrops != null) parts.push(`USB drops ${j.usbDrops}${j.usbDropSamples ? ` (${j.usbDropSamples} samples)` : ''}${ago(j.usbDropAgo)}`);
  if (j.chanDrops != null) parts.push(`listener drops ${j.chanDrops}`);
  if (typeof j.dspCpu === 'number') parts.push(`DSP ${Math.round(j.dspCpu)}%`);
  return `radio     : ${parts.join(' · ')}`;
}

/** ★★ LOCAL LISTEN RUNS THE SAME SERVER, on loopback, without "serving" — so the section above was skipped and a
 *  local-listen report said nothing about the radio at all (the HF+ report, 2026-10-10: five audio drops, no way to
 *  see why). Asked of the loopback server the app itself is listening to. */
async function localListenSection(port: number): Promise<string[]> {
  if (Platform.OS !== 'android' || !(port > 0)) return [];
  try {
    const ac = new AbortController();
    const t = setTimeout(() => ac.abort(), 2000);
    const j = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: ac.signal }).then((x) => x.json());
    clearTimeout(t);
    const health = radioHealthLine(j);
    return ['', '--- this device\'s own receiver (local listen) ---',
            `version   : ${j?.version ?? 'unknown'}`, ...(health ? [health] : [])];
  } catch { return []; }
}

async function serverSection(always: boolean): Promise<{ lines: string[]; version: string | null }> {
  const lines: string[] = [];
  let version: string | null = null;
  if (Platform.OS !== 'android' || !(NativeModules as any).VibeLocalSDR) return { lines, version };
  let st = null as Awaited<ReturnType<typeof getVibeServerStatus>>;
  try { st = await getVibeServerStatus(); } catch {}
  // From About / the home screen: only when this device IS serving.
  if (!always && !st?.running) return { lines, version };
  lines.push('', '--- this device as a VibeServer ---');
  lines.push(`running   : ${st ? (st.running ? 'yes' : 'no') : 'unknown (no status)'}`);
  try {
    const r = await getConnectedRadio();
    lines.push(`radio     : ${r ? `${r.model} (driver ${r.driver})` : 'none detected on USB'}`);
  } catch { lines.push('radio     : unavailable'); }
  if (st?.running) {
    try {
      const ac = new AbortController();
      const t = setTimeout(() => ac.abort(), 2500);
      const j = await fetch(`http://127.0.0.1:${st.port}/vibeserver.json`, { signal: ac.signal }).then((x) => x.json());
      clearTimeout(t);
      if (j?.version) version = String(j.version);
      lines.push(`version   : ${j?.version ?? 'unknown'}${j?.host ? ` — hosted by ${j.host}` : ''}`
               + `${j?.proto != null ? ` · proto ${j.proto}` : ''}`);
      const health = radioHealthLine(j);
      if (health) lines.push(health);
    } catch { lines.push('version   : unavailable (the server did not answer on loopback)'); }
    lines.push(`listeners : ${st.listeners} of ${st.maxUsers}`);
    if (st.sampleRate) lines.push(`rate      : ${(st.sampleRate / 1e6).toFixed(3)} MS/s`);
    if (st.cpu) lines.push(`cpu       : ${st.cpu.toFixed(0)}% of 1 core (of ${st.cores || '?'})`);
    if (st.radioProblem) lines.push(`problem   : ${st.radioProblem}`);
  }
  return { lines, version };
}

/** The whole report as plain text — deliberately readable, because the user is shown it. */
/** ★ `server`: the report is being exported FROM the server screen, so the server section is printed
 *  even when the server is stopped (it has just crashed, most likely). Elsewhere it appears only while
 *  this device is serving. */
export async function buildDiagnostics(extra?: Record<string, string | number | boolean>,
                                       opts?: { server?: boolean }): Promise<string> {
  const lines: string[] = [];
  lines.push('VibeSDR diagnostics');
  lines.push('===================');
  lines.push(`generated : ${new Date().toISOString()}`);

  let d: Awaited<ReturnType<NonNullable<NonNullable<typeof Vibe>['getDeviceInfo']>>> | undefined;
  try { d = await Vibe?.getDeviceInfo?.(); } catch {}
  let srv: { lines: string[]; version: string | null } = { lines: [], version: null };
  try { srv = await serverSection(!!opts?.server); } catch {}
  // ★ Not serving, but listening to this device's own radio: the same server, on the port the audio path used.
  let local: string[] = [];
  if (!srv.lines.length) {
    const lp = localListenPort();
    try { local = await localListenSection(lp); } catch {}
  }

  /* ★★★ THE FULL IDENTITY (2026-10-07). "app : 11.0" could not tell a pre-RC14 build (no crash capture)
   *  from RC26 — Nick's Pixel 6 report did exactly that. Now: name, version, RC, build number and package,
   *  from NATIVE where it can say (the build that is actually running), plus the report format.
   *  ★ Lite bundles the main app's version.ts, so its line comes wholly from native: Lite's versionName
   *    already carries the server version and the rc ("11.0.0-rc26-lite"). */
  const pkg = d?.packageId ?? '';
  const isLite = pkg === 'com.vibesdr.serverlite';
  const code = d?.versionCode ? ` (${d.versionCode})` : ' (build unknown)';
  const ident = isLite
    ? `VibeServer Lite ${d?.versionName || '?'}${code}`
    : `VibeSDR ${APP_VERSION}${RELEASE_LABEL ? ' ' + RELEASE_LABEL : ''}${code}`;
  lines.push(`app       : ${ident}${pkg ? ` · ${pkg}` : ''}`);
  // ★ A native versionName that disagrees with version.ts is itself a finding (the drift version.ts records).
  if (!isLite && d?.versionName && d.versionName !== APP_VERSION) lines.push(`native ver: ${d.versionName} (JS says ${APP_VERSION})`);
  if (srv.version) lines.push(`vibeserver: ${srv.version} (serving from this device)`);
  lines.push(`report    : format ${DIAG_FORMAT}`);
  lines.push(`platform  : ${Platform.OS} ${String(Platform.Version)}`);
  if (d) lines.push(`device    : ${d.model ?? '?'} · ${d.systemName ?? ''} ${d.os ?? ''}`.trimEnd());
  // ★ The process start — on Android also the hosted server's last restart (it runs in this process).
  const up = d?.processStartMs ? Number(d.processStartMs) : 0;
  lines.push(up ? `up since  : ${when(up)} (${dur(Date.now() - up)}) — app process, and any server in it`
                : `up since  : ${when(jsStart)} (${dur(Date.now() - jsStart)}) — app screen`);

  if (extra) for (const [k, v] of Object.entries(extra)) lines.push(`${k.padEnd(10)}: ${String(v)}`);

  lines.push(...srv.lines);
  lines.push(...local);

  // ── The audio path ───────────────────────────────────────────────────────
  // ★★ In this app the audio socket carries the TUNE and every control message — the spectrum
  //    socket is display-only — so one refused audio connection takes sound, tuning and the
  //    hardware panel with it, and reads as "the radio is ignoring me". This section names which
  //    component owns audio, what its gates evaluated to, and what the socket did.
  lines.push('', '--- audio path ---');
  for (const l of audioPathDump()) lines.push(l);

  // ── Server messages we did not handle ────────────────────────────────────
  // ★★ The single most useful thing in this report when a receiver misbehaves: a server says
  // something, we ignore it, and the symptom turns up somewhere unrelated. See protocolLog.ts.
  /* ★ The ring now also carries DECISIONS the client made (protocolLog.noteDecision) — the
   *  shared-dial adopt/decline above all — so the heading says both. A report that names only
   *  half its contents gets read as if the other half is not there. */
  lines.push('', '--- unhandled server messages + client decisions ---');
  {
    const u = unhandledLog();
    if (!u.length) lines.push('none');
    else for (const l of u) lines.push(`${new Date(l.ts).toISOString().slice(11, 19)} ${l.backend.padEnd(8)} ${l.text}`);
  }

  // ── Contained faults (faultLog) ──────────────────────────────────────────
  /* ★★★ A BAD MESSAGE OR A PANEL THAT THREW, CONTAINED. Each of these would once have taken far more
   *  than itself with it (a bad RDS packet, an AdvRdsPanel render throw that unmounted the screen).
   *  Now the message is dropped / the panel closed — and this is where the fact that it happened
   *  survives, counted per source and type, so a type that fails every frame reads as such. */
  lines.push('', `--- contained faults (dropped messages, closed panels): ${faultTotal()} ---`);
  {
    const f = faultSummary();
    if (!f.length) lines.push('none');
    else for (const e of f.slice(0, 30)) {
      lines.push(`${e.source} ${e.kind} x${e.count}  first ${new Date(e.firstTs).toISOString().slice(11, 19)}`
        + ` last ${new Date(e.lastTs).toISOString().slice(11, 19)}`);
      lines.push(`    ${e.firstError}${e.lastError !== e.firstError ? `  | latest: ${e.lastError}` : ''}`);
    }
  }

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
      // ★ The part that actually names the component — see CrashInfo.componentStack.
      if (c.componentStack) lines.push('components:', c.componentStack.slice(0, 2000));
    }
  } catch { lines.push('unavailable'); }

  // ── Native crash: iOS uncaught ObjC exception (VibeCrashLog) / Android exit record (VibeExitInfo) ──
  /* ★★ ONE SECTION, BOTH PLATFORMS (2026-10-05). Android had no getNativeCrash at all, so a crash in the
   *  C++ engine (VibeServer, libairspyhf, the DSP) printed "none recorded" — Nick's overnight crash on a
   *  Pixel hosting VibeServer left nothing to read. Android now answers with the system's own record of
   *  how the app last died: reason, signal, the decoded native backtrace / ANR stacks, and a history.
   *  The extra fields are Android-only and print only when present, so the iOS lines are unchanged.
   * ★ `name` absent with a `history` present = no abnormal exit, but here is how the app last ended. */
  lines.push('', '--- last native crash ---');
  try {
    const n = await Vibe?.getNativeCrash?.();
    if (!n || !n.name) lines.push('none recorded');
    else {
      lines.push(`when   : ${when(Number(n.ts))}`);
      lines.push(`name   : ${String(n.name ?? '?')}`);
      lines.push(`reason : ${String(n.reason ?? '')}`);
      lines.push(`os     : ${String(n.os ?? '?')} on ${String(n.model ?? '?')}`);
      for (const k of ['version', 'process', 'status', 'importance', 'memory'] as const) {
        if (n[k] != null) lines.push(`${k.padEnd(7)}: ${String(n[k])}`);
      }
      if (n.stack) lines.push('stack  :', String(n.stack).slice(0, 8000));
      /* ★ Android: the last log lines the tombstone kept (2026-10-05, Stuart approved) — REDACTED on the device
       *  (TombstoneReader.redact: addresses, PINs/keys/tokens, email, callsign, blobs → <placeholders>). The TAIL
       *  is kept under the cap: the lines nearest the crash are the ones that matter. */
      if (n.log) {
        const log = String(n.log);
        lines.push('log (last lines before the crash, private parts removed):', log.length > 6000 ? '…' + log.slice(-6000) : log);
      }
    }
    if (n?.history) lines.push('how the app\'s processes last ended (newest first):', String(n.history));
  } catch { lines.push('unavailable on this device'); }

  /* ★★ BOOT BREADCRUMBS — the launch timeline, native and JS interleaved. Primary route is the
   *  file itself (`Documents/boot-crumbs.log`, pulled with devicectl off a TestFlight build), but
   *  a tester with the phone and no Mac must still be able to send it, and this is the existing
   *  "copy the diagnostics" button they already know.
   * ★ TAIL, not head, and capped: the file holds several launches and the one being reported is
   *  the last. The whole file is still on the device for anyone who can reach it.
   * ★ No PINs pass through here — crumbs record URLs and outcomes, never the PIN itself. Checked
   *  against every crumb added, and it is why the connect crumbs log `${baseUrl}` and not `pin`. */
  lines.push('', '--- boot breadcrumbs (most recent) ---');
  try {
    const c = await readCrumbs();
    lines.push(c ? c.slice(-6000) : 'none recorded');
  } catch { lines.push('unavailable'); }

  lines.push('', '(No PINs, passwords or location are included. Nothing is sent unless you send it.)');
  return lines.join('\n');
}

export function clearDiagnostics(): void {
  try { Vibe?.clearNativeCrash?.(); } catch {}
}
