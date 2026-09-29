/**
 * Saving an mDNS-discovered server as a CUSTOM SERVER favourite, and repairing the ones an earlier
 * build saved wrongly.
 *
 * ★★★ A STARRED VIBESERVER CAME BACK AS RTL-TCP (Stuart, 2026-09-29). Discovery knew exactly what
 *   it had found — `_vibesdr._tcp` vs `_rtl_tcp._tcp` — and the ☆ threw that away: the picker's
 *   saveDiscovered() wrote `{ name, host, port }` with no `proto`, and every reader of a TcpFav
 *   says `f.proto ?? 'rtltcp'` (the field is optional because favourites from before SpyServer
 *   support have none, and those WERE all rtl_tcp). So the saved VibeServer was dialled as a raw
 *   rtl_tcp socket, which it does not speak, and never connected. ELSE MEANS DONGLE, again: the
 *   missing field was read as "the other one".
 *
 * ★ Pure — no React Native imports — so scripts/test_discoveredFavs.ts can drive it under tsx.
 */
import type { BackendType } from './sdrTypes';
import type { TcpFav } from './favourites';
import type { DiscoveredServer } from './mdns';

/** The rtl_tcp default port. A proto-less favourite on it is not probed — see needsVibeProbe. */
const RTLTCP_DEFAULT_PORT = 1234;

const hostPortKey = (host: string, port: number) => `${host.toLowerCase()}:${port}`;

/** The favourite for a discovered server — carrying the protocol discovery already established.
 *  ★ `proto` is ALWAYS written, rtl_tcp included, so nothing saved from now on is ambiguous. */
export function favFromDiscovered(s: DiscoveredServer): TcpFav {
  return { name: s.name, host: s.host, port: s.port, proto: s.proto as BackendType };
}

/**
 * Repair favourites that live discovery PROVES are VibeServers. Returns the corrected list, or null
 * when nothing needed changing (so the caller writes storage only when it must).
 *
 * ★ mDNS is authoritative here: a host advertising `_vibesdr._tcp` on a port is telling us what
 *   that port speaks. So a favourite on the same host:port with no proto — or with 'rtltcp', which
 *   is what the missing field was being read as — is corrected. A favourite on any OTHER port is
 *   untouched, so a genuine rtl_tcp next to a VibeServer on one box is left alone. SpyServer and
 *   the HTTP types were chosen or detected deliberately and are never rewritten by this.
 * ★ Only VibeServer is repaired this way. A discovered rtl_tcp needs no repair: a proto-less
 *   favourite already reads as rtl_tcp.
 */
export function repairFromDiscovery(favs: TcpFav[], discovered: DiscoveredServer[]): TcpFav[] | null {
  const vibe = new Set(discovered.filter(s => s.proto === 'vibeserver')
                                 .map(s => hostPortKey(s.host, s.port)));
  if (!vibe.size) return null;
  let changed = false;
  const next = favs.map((f) => {
    if ((f.proto == null || f.proto === 'rtltcp') && vibe.has(hostPortKey(f.host, f.port))) {
      changed = true;
      return { ...f, proto: 'vibeserver' as BackendType };
    }
    return f;
  });
  return changed ? next : null;
}

/**
 * Should tapping this favourite first ask the host whether it is a VibeServer?
 *
 * ★★ ONLY WHEN THE FAVOURITE IS AMBIGUOUS — no `proto` at all. That is exactly the set the bug
 *    produced (plus legacy pre-SpyServer rtl_tcp favourites, which look identical on disk). Every
 *    favourite saved from the add-server sheet, and every one saved by the fixed ☆, carries its
 *    proto and is never probed.
 * ★ The rtl_tcp DEFAULT port is not probed either. An HTTP request to a real rtl_tcp server makes
 *   it accept, start streaming at us, and drop — harmless, but pointless on the one port that is
 *   rtl_tcp by convention; a VibeServer takes 48000 and up.
 * ★ The probe runs on CONNECT, not at load: it touches only the one host the user just chose to
 *   reach, and only once — its answer is written back as the favourite's proto.
 */
export function needsVibeProbe(f: TcpFav): boolean {
  return f.proto == null && f.port !== RTLTCP_DEFAULT_PORT;
}

/** The address the watch/phone should hand a VibeServer TcpFav to — an http URL, never the
 *  synthetic `vibeserver://host:port` key, which nothing can fetch. */
export function tcpFavHttpBase(f: Pick<TcpFav, 'host' | 'port'>): string {
  return `http://${f.host}:${f.port}`;
}
