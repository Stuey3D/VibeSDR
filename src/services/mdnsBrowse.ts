// mdnsBrowse — the LAN discovery list's state, with the platform taken out (no react-native import), so
// scripts/test_mdns_browse.ts can drive it with a fake native module and a fake clock.
//
// ★★★ A BROWSE THAT IS NEVER RESTARTED GOES STALE (Stuart, 2026-10-20: "the app needs a refresh button on the
//     discoverable tab as sometimes it wont find a server until I close and open again"). The native browse was started
//     when the server list gained focus and never again: not when the app came back from the background (iOS tears down
//     a suspended app's DNS-SD connections, so the browser was dead while the list looked fine), not after the phone
//     changed networks, and on Android a resolve that never called back held the whole queue for ever. Closing and
//     reopening the app was the only restart there was. Now:
//       • refresh() RESTARTS the native browse — stop, start — and re-runs the hotspot sweep. It is what the
//         SEARCH AGAIN row calls, and what coming back to the foreground calls by itself;
//       • STALE ENTRIES GO: every row is marked at the restart and one not heard from again within GRACE_MS is dropped
//         (a server that moved network, or was switched off without a goodbye). Rows that are still there stay put
//         while the browse re-finds them, so a refresh never blanks the list it is about to show again;
//       • results from a browse or sweep started BEFORE a refresh cannot add a row after it.

export type ServerProto = 'rtltcp' | 'vibeserver';
export type DiscoveredServer = {
  name: string; host: string; port: number;
  proto: ServerProto;   // which protocol the advertised server speaks
  pin: boolean;         // VibeServer only: whether it requires a PIN
};

export type Sub = { remove(): void };
export type BrowseDeps = {
  /** Native browse control (NWBrowser on iOS, NsdManager on Android). */
  start(): void;
  stop(): void;
  /** Subscribe to the native module's VibeMdnsFound / VibeMdnsLost events. */
  listen(event: 'VibeMdnsFound' | 'VibeMdnsLost', fn: (e: any) => void): Sub;
  /** Optional probe of addresses Bonjour cannot reach (the iPhone hotspot): every server it finds, or null to skip. */
  sweep?: () => Promise<DiscoveredServer[] | null>;
  /** Called with a function to run when the app returns to the foreground from the background. */
  onForeground?: (fn: () => void) => Sub;
  setTimeout: (fn: () => void, ms: number) => any;
  clearTimeout: (t: any) => void;
  setInterval: (fn: () => void, ms: number) => any;
  clearInterval: (t: any) => void;
};

/** How long a row survives a restart without being found again. */
export const GRACE_MS = 6000;
/** How often the hotspot sweep repeats while the list is open. */
export const SWEEP_MS = 20000;

/** The stop function every caller already uses, with refresh() on it — so existing call sites need no change. */
export type MdnsBrowse = (() => void) & {
  /** Restart the browse: native stop + start, the sweep again, rows not re-found within GRACE_MS dropped. */
  refresh(): void;
  /** True from a (re)start until GRACE_MS later — "searching…". */
  searching(): boolean;
};

const key = (host: string, port: number) => `${host}:${port}`;

export function createMdnsBrowse(
  d: BrowseDeps,
  onChange: (servers: DiscoveredServer[]) => void,
  onSearching?: (busy: boolean) => void,
): MdnsBrowse {
  const byKey = new Map<string, DiscoveredServer>();
  const nameToKey = new Map<string, string>();
  const swept = new Set<string>();            // rows that came from the sweep, not from Bonjour
  const seen = new Set<string>();             // rows heard from since the last (re)start
  let gen = 0;                                // bumped on every (re)start: older results are ignored
  let dead = false;
  let graceTimer: any = null;
  let busy = false;

  const push = () => { if (!dead) onChange(Array.from(byKey.values())); };
  const setBusy = (b: boolean) => { if (busy !== b) { busy = b; onSearching?.(b); } };

  const foundSub = d.listen('VibeMdnsFound', (e: any) => {
    if (dead) return;
    const host = String(e?.host ?? '').trim();
    const port = Number(e?.port);
    if (!host || !Number.isFinite(port) || port <= 0) return;
    const name = String(e?.name ?? '').trim() || `${host}:${port}`;
    const proto: ServerProto = e?.proto === 'vibeserver' ? 'vibeserver' : 'rtltcp';
    const pin = !!e?.pin;
    const k = key(host, port);
    // ★ A server that MOVED (same name, new address — the Portable switching networks) replaces its old row.
    const svc = e?.name ? String(e.name) : '';
    const was = svc ? nameToKey.get(svc) : undefined;
    if (was && was !== k) { byKey.delete(was); seen.delete(was); swept.delete(was); }
    byKey.set(k, { name, host, port, proto, pin });
    swept.delete(k);
    seen.add(k);
    if (svc) nameToKey.set(svc, k);
    push();
  });

  // Removal events carry only the service name (host/port aren't re-resolved on teardown), so map it back.
  const lostSub = d.listen('VibeMdnsLost', (e: any) => {
    if (dead) return;
    const svcName = String(e?.name ?? '');
    const k = nameToKey.get(svcName);
    if (k && byKey.delete(k)) {
      nameToKey.delete(svcName);
      seen.delete(k);
      push();
    }
  });

  const sweep = async () => {
    if (!d.sweep || dead) return;
    const myGen = gen;
    const found = await d.sweep();
    if (dead || myGen !== gen || !found) return;
    let changed = false;
    const now = new Set<string>();
    for (const s of found) {
      const k = key(s.host, s.port);
      now.add(k);
      seen.add(k);
      if (!byKey.has(k)) { byKey.set(k, s); swept.add(k); changed = true; }
    }
    for (const k of Array.from(swept)) {
      if (!now.has(k)) { swept.delete(k); byKey.delete(k); changed = true; }
    }
    if (changed) push();
  };

  const start = () => {
    gen++;
    seen.clear();
    try { d.stop(); } catch {}
    try { d.start(); } catch {}
    setBusy(true);
    if (graceTimer) d.clearTimeout(graceTimer);
    graceTimer = d.setTimeout(() => {
      graceTimer = null;
      if (dead) return;
      // ★ Mark-and-sweep: whatever was not heard from since the restart is gone.
      let changed = false;
      for (const k of Array.from(byKey.keys())) {
        if (!seen.has(k)) {
          byKey.delete(k);
          swept.delete(k);
          for (const [n, v] of Array.from(nameToKey.entries())) if (v === k) nameToKey.delete(n);
          changed = true;
        }
      }
      if (changed) push();
      setBusy(false);
    }, GRACE_MS);
    void sweep();
  };

  const sweepTimer = d.setInterval(() => { void sweep(); }, SWEEP_MS);
  const fgSub = d.onForeground?.(() => { if (!dead) start(); });
  start();

  const stop = (() => {
    if (dead) return;
    dead = true;
    d.clearInterval(sweepTimer);
    if (graceTimer) d.clearTimeout(graceTimer);
    try { d.stop(); } catch {}
    foundSub.remove();
    lostSub.remove();
    fgSub?.remove();
  }) as MdnsBrowse;
  stop.refresh = () => { if (!dead) start(); };
  stop.searching = () => busy;
  return stop;
}
