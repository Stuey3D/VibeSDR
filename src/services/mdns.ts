import { NativeModules, NativeEventEmitter, Platform } from 'react-native';

// mDNS/Bonjour auto-discovery of networked RTL-TCP servers. Platform-agnostic
// wrapper over the native discovery:
//   • iOS     — folded into VibePowerModule (NWBrowser), NativeModules.VibePowerModule
//   • Android — VibeMDNS module (NsdManager)
// Both emit the same events (VibeMdnsFound / VibeMdnsLost) via the device event
// emitter, so the only per-platform difference is which module hosts them.
//
// This is the App-Store-clean path: we only ever see servers that *advertise*
// `_rtl_tcp._tcp` — no subnet scanning. The advertiser (e.g. an mDNS record next
// to rtl_tcp on the UberSDR host) publishes host, port, and an optional `name`
// TXT record for a friendly label.

export type ServerProto = 'rtltcp' | 'vibeserver';
export type DiscoveredServer = {
  name: string; host: string; port: number;
  proto: ServerProto;   // which protocol the advertised server speaks
  pin: boolean;         // VibeServer only: whether it requires a PIN
};

const nativeModule: any =
  Platform.OS === 'ios'
    ? (NativeModules as any).VibePowerModule
    : (NativeModules as any).VibeMDNS;

const key = (host: string, port: number) => `${host}:${port}`;

/**
 * Start browsing for RTL-TCP servers. `onChange` is called with the full current
 * list whenever it changes. Returns a stop function — call it on unmount/blur.
 * No-ops safely (returns a stop fn that does nothing) if the native module is
 * unavailable on this build.
 */
export function startMdnsDiscovery(
  onChange: (servers: DiscoveredServer[]) => void,
): () => void {
  if (!nativeModule?.startDiscovery) return () => {};

  const emitter = new NativeEventEmitter(nativeModule);
  const byKey = new Map<string, DiscoveredServer>();
  const nameToKey = new Map<string, string>();

  const push = () => onChange(Array.from(byKey.values()));

  const foundSub = emitter.addListener('VibeMdnsFound', (e: any) => {
    const host = String(e?.host ?? '').trim();
    const port = Number(e?.port);
    if (!host || !Number.isFinite(port) || port <= 0) return;
    const name = String(e?.name ?? '').trim() || `${host}:${port}`;
    const proto: ServerProto = e?.proto === 'vibeserver' ? 'vibeserver' : 'rtltcp';
    const pin = !!e?.pin;
    const k = key(host, port);
    byKey.set(k, { name, host, port, proto, pin });
    if (e?.name) nameToKey.set(String(e.name), k);
    push();
  });

  // Removal events carry only the service name (host/port aren't re-resolved on
  // teardown), so map it back to the key we stored on discovery.
  const lostSub = emitter.addListener('VibeMdnsLost', (e: any) => {
    const svcName = String(e?.name ?? '');
    const k = nameToKey.get(svcName);
    if (k && byKey.delete(k)) {
      nameToKey.delete(svcName);
      push();
    }
  });

  try { nativeModule.startDiscovery(); } catch {}

  // ★★★ THE PHONE'S OWN HOTSPOT (Stuart, 2026-10-10: a VibeServer Portable on his iPhone's hotspot — "I also have no way
  //     of knowing the ip address of the connected pi from the phone", and the app did not list it). An iPhone's
  //     Personal Hotspot always hands out 172.20.10.2–14 — thirteen addresses — and shows nobody their numbers. So,
  //     alongside Bonjour (which may not cross from a hotspot's clients to the phone hosting it), ask those thirteen
  //     "are you a VibeServer?" — /vibeserver.json, 1.5 s each, all at once, every 20 s while the list is open.
  //     Nothing there costs thirteen refused connections. iOS only: that range is Apple's.
  const swept = new Map<string, DiscoveredServer>();
  let sweepDead = false;
  const sweep = async () => {
    if (Platform.OS !== 'ios') return;
    const found = await Promise.all(Array.from({ length: 13 }, (_, i) => `172.20.10.${i + 2}`).map(hotspotProbe));
    if (sweepDead) return;
    let changed = false;
    for (let i = 0; i < 13; i++) {
      const host = `172.20.10.${i + 2}`, k = key(host, 48000), s = found[i];
      if (s && !byKey.has(k)) { byKey.set(k, s); swept.set(k, s); changed = true; }
      if (!s && swept.has(k)) { swept.delete(k); byKey.delete(k); changed = true; }
    }
    if (changed) push();
  };
  void sweep();
  const sweepTimer = setInterval(() => { void sweep(); }, 20000);

  return () => {
    sweepDead = true;
    clearInterval(sweepTimer);
    try { nativeModule.stopDiscovery?.(); } catch {}
    foundSub.remove();
    lostSub.remove();
  };
}

/** One address on the phone's hotspot: a VibeServer there, or null. Named from the pocket box's own hello if it is one. */
async function hotspotProbe(host: string): Promise<DiscoveredServer | null> {
  const get = async (path: string) => {
    const ac = new AbortController();
    const t = setTimeout(() => ac.abort(), 1500);
    try { const r = await fetch(`http://${host}:48000${path}`, { signal: ac.signal }); return r.ok ? await r.json() : null; }
    catch { return null; } finally { clearTimeout(t); }
  };
  const j: any = await get('/vibeserver.json');
  if (!j || j.server !== 'vibeserver') return null;
  const hello: any = await get('/vibeserver/pocket/hello');
  const name = hello?.pocket ? `VibeServer Portable (${host})` : `VibeServer at ${host}`;
  return { name, host, port: 48000, proto: 'vibeserver', pin: !!j.pin };
}

// ── Advertise (Android only): publish this device's RTL-TCP server ────────────
// Uses the Android VibeMDNS module (NsdManager). No-op on iOS (the server
// feature is Android-only). Call again with a new name to re-advertise.
export async function advertiseRtlTcp(name: string, port: number): Promise<void> {
  return advertiseServer(name, port, 'rtltcp', false);
}

// Advertise a server of either protocol. `pinRequired` publishes a `pin` TXT so
// clients know a VibeServer needs auth before they connect. Android-only.
export async function advertiseServer(
  name: string, port: number, proto: ServerProto, pinRequired: boolean,
): Promise<void> {
  const m: any = (NativeModules as any).VibeMDNS;
  if (Platform.OS !== 'android' || !m?.advertise) return;
  try { await m.advertise(name, port, proto, pinRequired); } catch {}
}

export function stopAdvertiseRtlTcp(): void {
  const m: any = (NativeModules as any).VibeMDNS;
  try { m?.stopAdvertise?.(); } catch {}
}
