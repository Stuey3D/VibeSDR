/**
 * cloudKvs.ts — thin typed wrapper over the iCloud key-value store.
 *
 * Deliberately dumb: strings in, strings out. Everything that knows what a
 * favourite or a dial IS lives in cloudSync.ts. On Android (and on any iOS
 * build where the pod is missing) `kvsSupported` is false and every call
 * resolves to an empty/no-op result, so callers never need a Platform check.
 *
 * ★ KVS keys are limited to 64 bytes and the whole store to 1 MB / 1024 keys.
 *   Per-server keys therefore use a short hash of the baseUrl (serverSlug),
 *   never the URL itself — see cloudSync.ts.
 */
import { NativeModules, NativeEventEmitter, Platform } from 'react-native';

type KvsNative = {
  isAvailable(): Promise<boolean>;
  getItem(key: string): Promise<string | null>;
  multiGet(keys: string[]): Promise<Record<string, string>>;
  setItem(key: string, value: string): Promise<boolean>;
  removeItem(key: string): Promise<boolean>;
  getAllKeys(): Promise<string[]>;
  usage(): Promise<{ bytes: number; keys: number; maxBytes: number; maxKeys: number }>;
  synchronize(): Promise<boolean>;
};

const M: KvsNative | undefined =
  Platform.OS === 'ios' ? (NativeModules as any).VibeICloudKVS : undefined;

/** Is the native bridge present at all? (Compile-time capability, not sign-in state.) */
export const kvsSupported = !!M;

let availableCache: boolean | null = null;

/** Is iCloud usable right now — bridge present AND the user signed in?
 *  ★ A signed-out device still ACCEPTS KVS writes; they just never leave it.
 *  That failure is invisible, so we check the identity token rather than
 *  trusting a successful write. Cached, with a `refresh` escape hatch for the
 *  sign-in-while-running case. */
export async function kvsAvailable(refresh = false): Promise<boolean> {
  if (!M) return false;
  if (availableCache !== null && !refresh) return availableCache;
  try { availableCache = await M.isAvailable(); } catch { availableCache = false; }
  return availableCache;
}

export async function kvsGet(key: string): Promise<string | null> {
  if (!M) return null;
  try { return await M.getItem(key); } catch { return null; }
}

export async function kvsMultiGet(keys: string[]): Promise<Record<string, string>> {
  if (!M || !keys.length) return {};
  try { return await M.multiGet(keys); } catch { return {}; }
}

/** Write. ★ Rejects rather than swallowing: over-quota is the one failure the
 *  user must be told about, because a sync that has silently stopped is worse
 *  than one that never started. Callers surface it via cloudSync's lastError. */
export async function kvsSet(key: string, value: string): Promise<void> {
  if (!M) return;
  await M.setItem(key, value);
}

export async function kvsRemove(key: string): Promise<void> {
  if (!M) return;
  try { await M.removeItem(key); } catch { /* a stale key costs a few bytes */ }
}

export async function kvsAllKeys(): Promise<string[]> {
  if (!M) return [];
  try { return await M.getAllKeys(); } catch { return []; }
}

export async function kvsUsage() {
  if (!M) return { bytes: 0, keys: 0, maxBytes: 0, maxKeys: 0 };
  try { return await M.usage(); } catch { return { bytes: 0, keys: 0, maxBytes: 0, maxKeys: 0 }; }
}

export async function kvsSynchronize(): Promise<boolean> {
  if (!M) return false;
  try { return await M.synchronize(); } catch { return false; }
}

export type KvsChange = { keys: string[]; reason: number; quotaExceeded: boolean };

/** Another device (or Jr) changed something. Returns an unsubscribe function. */
export function onKvsChanged(cb: (c: KvsChange) => void): () => void {
  if (!M) return () => {};
  const emitter = new NativeEventEmitter(M as any);
  const sub = emitter.addListener('kvsChanged', cb);
  return () => sub.remove();
}
