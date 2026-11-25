import { NativeModules, Platform } from 'react-native';
import AsyncStorage from '@react-native-async-storage/async-storage';

// Thin JS wrapper over the Android RTL-TCP SERVER native methods (VibeLocalSDR).
// The server shares this device's USB radio — any radio the server drives — over the LAN as rtl_tcp,
// for SDR# / SDR++ / SDR Console or another VibeSDR. Local network only. Android-only.

const Local: any = (NativeModules as any).VibeLocalSDR;

export const rtlTcpServerSupported =
  Platform.OS === 'android' && !!Local?.startRtlTcpServer;

export type ServerInfo = { ip: string; port: number; name: string };
export type ServerStatus = {
  running: boolean;
  client: boolean;
  clientAddr: string;
  sampleRate: number;     // the radio's capture rate
  clientRate?: number;    // what the client asked for (the stream is resampled to it when the radio cannot run it)
  freq?: number;          // the client's centre frequency
  drops?: number;         // IQ blocks dropped behind a slow client
  port: number;           // bound port (0 when not running)
  ip: string;             // this device's LAN address ('' when not running)
  iqBytes?: number;       // IQ bytes sent to the client
  usbEnded?: boolean;     // the radio stopped answering under a running server
  agc?: boolean;          // automatic gain in force (the client asked for it, and the radio has an AGC)
  agcName?: string;       // 'VibeAGC', 'AGC' (the radio's own), '' (none — a HackRF)
  gainTenthDb?: number;   // the gain now, tenths of a dB (-1 unknown)
  driver?: string;        // rtl, sdrplay, airspyhf, airspy, hackrf, usrp
  refused?: number;       // clients turned away (not on the local network, one at a time, …)
  lastRefusal?: string;
};

/** ★ Exported so the server screen can read it in its ONE batched read (serverPrefs.ts). */
export const NAME_KEY = 'vsdr_rtltcp_server_name';
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
  name: string; port?: number;
}): Promise<ServerInfo> {
  return Local.startRtlTcpServer({
    name: opts.name,
    port: opts.port ?? 1234,
  });
}

export async function stopRtlTcpServer(): Promise<void> {
  try { await Local?.stopRtlTcpServer?.(); } catch {}
}

export async function getServerStatus(): Promise<ServerStatus | null> {
  try { return await Local?.getServerStatus?.(); } catch { return null; }
}
