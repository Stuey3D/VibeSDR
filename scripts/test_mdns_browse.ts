/**
 * LAN DISCOVERY LIST (src/services/mdnsBrowse.ts) — the refresh, the foreground restart and stale rows.
 *
 * Stuart, 2026-10-20: "the app needs a refresh button on the discoverable tab as sometimes it wont find a server until
 * I close and open again". The native browse was started once, on focus, and never again.
 *
 * Proves:
 *   • refresh() really RESTARTS the native browse (stop then start) — it is not a re-render;
 *   • coming back from the background restarts it by itself;
 *   • a row not heard from within GRACE_MS of a restart is dropped; a row re-found stays and never blinks out;
 *   • a server that moves (same service name, new address) replaces its old row;
 *   • a sweep or a find started before a refresh cannot resurrect a row after it; nothing is pushed after stop();
 *   • "searching" goes true at every (re)start and false when the grace period ends.
 *
 * Run: node --no-warnings scripts/test_mdns_browse.ts
 */
import { createMdnsBrowse, GRACE_MS, type DiscoveredServer, type BrowseDeps } from '../src/services/mdnsBrowse.ts';

let fails = 0;
const check = (ok: boolean, what: string) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) fails++; };

// ── a manual clock ──────────────────────────────────────────────────────────────────────────────────────────────
type T = { at: number; fn: () => void; id: number; every?: number };
let now = 0, nextId = 1;
let timers: T[] = [];
const advance = (ms: number) => {
  const end = now + ms;
  for (;;) {
    timers.sort((a, b) => a.at - b.at);
    const t = timers[0];
    if (!t || t.at > end) break;
    now = t.at;
    if (t.every) t.at += t.every; else timers.shift();
    t.fn();
  }
  now = end;
};
const flush = () => new Promise<void>((r) => setImmediate(r));

// ── a fake native module ────────────────────────────────────────────────────────────────────────────────────────
function rig(opts: { sweep?: () => Promise<DiscoveredServer[] | null> } = {}) {
  const calls: string[] = [];
  const subs: Record<string, ((e: any) => void)[]> = { VibeMdnsFound: [], VibeMdnsLost: [] };
  let fg: (() => void) | null = null;
  const lists: DiscoveredServer[][] = [];
  const busy: boolean[] = [];
  const deps: BrowseDeps = {
    start: () => calls.push('start'),
    stop: () => calls.push('stop'),
    listen: (ev, fn) => { subs[ev].push(fn); return { remove: () => { subs[ev] = subs[ev].filter((f) => f !== fn); } }; },
    sweep: opts.sweep,
    onForeground: (fn) => { fg = fn; return { remove: () => { fg = null; } }; },
    setTimeout: (fn, ms) => { const t = { at: now + ms, fn, id: nextId++ }; timers.push(t); return t.id; },
    clearTimeout: (id) => { timers = timers.filter((t) => t.id !== id); },
    setInterval: (fn, ms) => { const t = { at: now + ms, fn, id: nextId++, every: ms }; timers.push(t); return t.id; },
    clearInterval: (id) => { timers = timers.filter((t) => t.id !== id); },
  };
  const browse = createMdnsBrowse(deps, (l) => lists.push(l), (b) => busy.push(b));
  const emit = (ev: string, e: any) => subs[ev].forEach((f) => f(e));
  const found = (name: string, host: string, port = 48000) =>
    emit('VibeMdnsFound', { name, host, port, proto: 'vibeserver', pin: false });
  const last = () => lists[lists.length - 1] ?? [];
  return { browse, calls, found, emit, last, lists, busy, foreground: () => fg?.(), hasFg: () => !!fg, subs };
}

async function main() {
  // 1) start, find, refresh = native stop + start; a re-found row never blinks out
  {
    timers = []; now = 0;
    const r = rig();
    check(r.calls.join(',') === 'stop,start', '1) start: native browse (re)started');
    r.found('Pi 500', '192.168.86.50');
    r.found('Portable', '192.168.86.158');
    check(r.last().length === 2, '1) two servers listed');
    const pushesBefore = r.lists.length;
    r.browse.refresh();
    check(r.calls.join(',') === 'stop,start,stop,start', '1) refresh: native stop THEN start (a real restart)');
    check(r.lists.length === pushesBefore, '1) refresh does not blank the list it is about to show again');
    advance(1000);
    r.found('Pi 500', '192.168.86.50');
    r.found('Portable', '192.168.86.158');
    advance(GRACE_MS);
    check(r.last().length === 2, '1) both re-found: both still listed after the grace period');
    r.browse();
  }
  // 2) a row not re-found within the grace period is dropped (the stale entry)
  {
    timers = []; now = 0;
    const r = rig();
    r.found('Pi 500', '192.168.86.50');
    r.found('Gone', '192.168.86.77');
    r.browse.refresh();
    advance(500);
    r.found('Pi 500', '192.168.86.50');
    advance(GRACE_MS - 600);
    check(r.last().length === 2, '2) before the grace period ends, nothing is dropped');
    advance(200);
    check(r.last().length === 1 && r.last()[0].host === '192.168.86.50', '2) after it, the silent one is gone');
    r.browse();
  }
  // 3) back from the background restarts the browse by itself
  {
    timers = []; now = 0;
    const r = rig();
    check(r.hasFg(), '3) foreground hook registered');
    r.found('Portable', '10.42.0.1');
    r.foreground();
    check(r.calls.join(',') === 'stop,start,stop,start', '3) foreground: native stop + start');
    advance(2000);
    r.found('Portable', '192.168.86.158');      // the Portable switched from its hotspot to the home Wi-Fi
    advance(GRACE_MS);
    check(r.last().length === 1 && r.last()[0].host === '192.168.86.158', '3) moved server: one row, the new address');
    r.browse();
    check(!r.hasFg(), '3) stop() removes the foreground hook');
  }
  // 4) a server that moves replaces its row at once (same service name, new address)
  {
    timers = []; now = 0;
    const r = rig();
    r.found('Portable', '10.42.0.1');
    r.found('Portable', '192.168.86.158');
    check(r.last().length === 1 && r.last()[0].host === '192.168.86.158', '4) moved: the old address row is replaced');
    r.emit('VibeMdnsLost', { name: 'Portable' });
    check(r.last().length === 0, '4) lost by name removes the current row');
    r.browse();
  }
  // 5) searching: true at each (re)start, false at the end of its grace period
  {
    timers = []; now = 0;
    const r = rig();
    check(r.browse.searching() && r.busy.join() === 'true', '5) searching at start');
    advance(GRACE_MS);
    check(!r.browse.searching() && r.busy.join() === 'true,false', '5) done after the grace period');
    r.browse.refresh();
    check(r.browse.searching(), '5) searching again on refresh');
    advance(GRACE_MS / 2);
    r.browse.refresh();                           // a second press restarts the grace period
    advance(GRACE_MS - 1);
    check(r.browse.searching(), '5) a second refresh restarts the grace period');
    advance(2);
    check(!r.browse.searching(), '5) and it ends');
    r.browse();
  }
  // 6) after stop(): native stopped, events and timers do nothing
  {
    timers = []; now = 0;
    const r = rig();
    r.found('Pi 500', '192.168.86.50');
    const n = r.lists.length;
    r.browse();
    check(r.calls[r.calls.length - 1] === 'stop', '6) stop(): native browse stopped');
    check(r.subs.VibeMdnsFound.length === 0 && r.subs.VibeMdnsLost.length === 0, '6) listeners removed');
    r.browse.refresh();
    advance(GRACE_MS * 3);
    check(r.lists.length === n && r.calls.filter((c) => c === 'start').length === 1, '6) nothing after stop');
  }
  // 7) the hotspot sweep: merged, removed when gone, and a sweep from before a refresh cannot add a row after it
  {
    timers = []; now = 0;
    let answer: DiscoveredServer[] = [{ name: 'VibeServer Portable (172.20.10.3)', host: '172.20.10.3', port: 48000, proto: 'vibeserver', pin: false }];
    let gate: (() => void) | null = null;
    let slow = false;
    const r = rig({ sweep: () => new Promise((res) => { if (slow) gate = () => res(answer); else res(answer); }) });
    await flush();
    check(r.last().some((s) => s.host === '172.20.10.3'), '7) sweep: hotspot server listed');
    answer = [];
    advance(20000);
    await flush();
    check(!r.last().some((s) => s.host === '172.20.10.3'), '7) sweep: gone when it no longer answers');
    slow = true;
    answer = [{ name: 'Old', host: '172.20.10.9', port: 48000, proto: 'vibeserver', pin: false }];
    advance(20000);                                // a sweep starts and hangs
    const late = gate as (() => void) | null;
    r.browse.refresh();                            // the person presses SEARCH AGAIN meanwhile
    late?.();                                      // the old sweep finally answers
    await flush();
    check(!r.last().some((s) => s.host === '172.20.10.9'), '7) a sweep from before the refresh adds nothing');
    r.browse();
  }

  console.log(fails ? `${fails} FAILED` : 'all passed');
  process.exit(fails ? 1 : 0);
}
void main();
