import { NativeModules, Platform } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';

// Thin JS wrapper over the Android RTL-TCP SERVER native methods (VibeLocalSDR).
// The server shares this device's USB RTL-SDR over the LAN so another VibeSDR /
// SDR# can use it. Android-only (iOS has no USB host SDR).

const Local: any = (NativeModules as any).VibeLocalSDR;

export const rtlTcpServerSupported =
  Platform.OS === 'android' && !!Local?.startRtlTcpServer;

export type ServerInfo = { ip: string; port: number; name: string };
export type ServerStatus = {
  running: boolean;
  client: boolean;
  clientAddr: string;
  sampleRate: number;
  overrideRate: number;   // 0 = client-controlled
  droppedBytes: number;   // IQ dropped since the current client connected
  port: number;           // bound port (0 when not running)
  ip: string;             // this device's LAN address ('' when not running)
};

// Human-readable byte count for the drop counter.
export function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  if (n < 1024 * 1024 * 1024) return `${(n / (1024 * 1024)).toFixed(1)} MB`;
  return `${(n / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

// Bandwidth override options. value 0 = client-controlled (default).
export const BANDWIDTH_OPTIONS: { label: string; value: number }[] = [
  { label: 'Client-controlled', value: 0 },
  { label: '3.2 MHz',           value: 3_200_000 },
  { label: '2.4 MHz',           value: 2_400_000 },
  { label: '2.048 MHz',         value: 2_048_000 },
  { label: '1.024 MHz',         value: 1_024_000 },
  { label: '0.25 MHz',          value: 250_000 },
];

const NAME_KEY = 'vsdr_rtltcp_server_name';
const PERSIST_KEY = 'vsdr_rtltcp_server_persist';

/**
 * Persist ("keep sharing") mode.
 *
 * OFF (default) — an ad-hoc, one-shot share: the server is owned by the server
 * screen and dies when you leave it, freeing the dongle for on-device use. This
 * is what most people want and it can't be left running by accident.
 *
 * ON — a set-and-forget node (e.g. an old phone + dongle at the base of an
 * antenna). The server outlives the screen, so leaving the screen no longer
 * stops it; only the explicit STOP button does. The foreground service keeps it
 * alive. Callers must re-adopt a running server rather than starting a second
 * one on the same dongle.
 */
export async function getServerPersist(): Promise<boolean> {
  try { return (await AsyncStorage.getItem(PERSIST_KEY)) === '1'; } catch { return false; }
}
export async function saveServerPersist(on: boolean): Promise<void> {
  try { await AsyncStorage.setItem(PERSIST_KEY, on ? '1' : '0'); } catch {}
}

/** ★ The PUBLIC name's key, defined once. It is read by the location publisher and written by the
 *  server screen, and two spellings of the same string is how one of them quietly stops working. */
export const PUBLIC_NAME_KEY = 'vs_publicname';

export async function getServerName(fallback = 'VibeSDR RTL-SDR'): Promise<string> {
  try { return (await AsyncStorage.getItem(NAME_KEY)) || fallback; } catch { return fallback; }
}
export async function saveServerName(name: string): Promise<void> {
  try { await AsyncStorage.setItem(NAME_KEY, name); } catch {}
}

export async function startRtlTcpServer(opts: {
  name: string; port?: number; overrideRate?: number;
}): Promise<ServerInfo> {
  return Local.startRtlTcpServer({
    name: opts.name,
    port: opts.port ?? 1234,
    overrideRate: opts.overrideRate ?? 0,
  });
}

export async function stopRtlTcpServer(): Promise<void> {
  try { await Local?.stopRtlTcpServer?.(); } catch {}
}

export function setServerSampleRate(rate: number): void {
  try { Local?.setServerSampleRate?.(rate); } catch {}
}

export async function getServerStatus(): Promise<ServerStatus | null> {
  try { return await Local?.getServerStatus?.(); } catch { return null; }
}
