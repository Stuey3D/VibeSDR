/* ★★★ ONE SOURCE FOR "AM I SERVING?", because three things need the answer and they must never
 *     disagree: the pill on the picker, the pill while listening, and the loopback rewrite that
 *     decides whether a connection is to ourselves.
 *
 *  This is the project's "one rule, two readers" shape pointed at deliberately. Each of those
 *  could have polled `getVibeServerStatus()` for itself; then the pill could say SERVING while the
 *  connect path still dialled the LAN address, and the disagreement would only show up as a
 *  round trip through the Wi-Fi router that nobody can see.
 *
 *  ★ ONE timer for any number of subscribers, running only while something is actually watching.
 */
import { useEffect, useState } from 'react';
import { Platform } from 'react-native';
import { getVibeServerStatus, vibeServerSupported } from './vibeServer';

export type Serving = {
  running: boolean;
  listeners: number;
  /** LAN address the server is on — the one mDNS discovery hands out. */
  ip: string;
  port: number;
  /** Slots the owner configured. 1 = single-user, where any arrival displaces the incumbent. */
  maxUsers: number;
};

const IDLE: Serving = { running: false, listeners: 0, ip: '', port: 0, maxUsers: 1 };

/* ★★ The cache is read SYNCHRONOUSLY by loopbackIfSelf() at connect time. It is warm because the
 *    picker subscribes while it is on screen, which is necessarily true of any tap that reaches
 *    the connect path from it. If it is ever cold the worst case is the old behaviour — the LAN
 *    address — never a wrong address. */
let cache: Serving = IDLE;
const subs = new Set<(s: Serving) => void>();
let timer: ReturnType<typeof setInterval> | null = null;

/** Only Android serves, and only where the native shim exists. Everywhere else this is inert. */
const canServe = () => Platform.OS === 'android' && vibeServerSupported;

async function poll() {
  const s = await getVibeServerStatus();
  const next: Serving = s?.running
    ? { running: true, listeners: s.listeners || 0, ip: s.ip || '', port: s.port || 0,
        maxUsers: s.maxUsers || 1 }
    : IDLE;
  if (next.running === cache.running && next.listeners === cache.listeners
      && next.ip === cache.ip && next.port === cache.port
      && next.maxUsers === cache.maxUsers) return;   // no churn, no re-render
  cache = next;
  subs.forEach(fn => fn(next));
}

function subscribe(fn: (s: Serving) => void): () => void {
  subs.add(fn);
  if (canServe() && !timer) {
    void poll();                       // answer the first subscriber now, not in 3 s
    timer = setInterval(() => { void poll(); }, 3000);
  }
  return () => {
    subs.delete(fn);
    if (!subs.size && timer) { clearInterval(timer); timer = null; }
  };
}

/** Current answer without subscribing — for code paths that run once, like a connect. */
export function servingNow(): Serving { return cache; }

/** Live serving state for a screen. Returns the idle shape on every non-serving platform. */
export function useServing(): Serving {
  const [s, setS] = useState<Serving>(cache);
  useEffect(() => {
    if (!canServe()) return;
    setS(cache);
    return subscribe(setS);
  }, []);
  return s;
}

/* ★★★ LOOPBACK, LIKE THE MAC. Listening to your own server must not leave the phone.
 *
 *  Stuart: "loopback like the mac". The address mDNS advertises is the LAN one, so tapping your
 *  own server in the discovered list sends audio and waterfall out to the router and back — a
 *  round trip that costs latency for nothing and, worse, DIES WITH THE WI-FI. Serving and
 *  listening on one phone must keep working when the network does not.
 *
 *  ★ Matched on ip:port, not on a name: mDNS resolution hands us a numeric host
 *    (VibeMdnsModule.onServiceResolved uses info.host.hostAddress), so there is no ".local" form
 *    to miss. A same-port different-ip match is a DIFFERENT server and must be left alone.
 */
export function loopbackIfSelf(host: string, port: number): string {
  const s = cache;
  if (!s.running || !s.ip || port !== s.port) return host;
  return host === s.ip ? '127.0.0.1' : host;
}
