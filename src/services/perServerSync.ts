/**
 * perServerSync.ts — iCloud sync for the state that is keyed BY SERVER:
 * display prefs (waterfall + VFO colours) and the last tuned frequency/mode.
 *
 * ★ These stay keyed by server rather than being pooled. A last tune or a set
 *   of waterfall levels describes *that aerial* — pooled across receivers it
 *   would be meaningless, and would restore a frequency the user never chose.
 *
 * ★★ `lsv_last_tune` shares its key namespace with LOCAL hardware
 *   (`lsv_last_tune:usb`, `lsv_last_tune:tcp:host:port`), and those are
 *   deliberately EXCLUDED: what a dongle plugged into one phone was last tuned
 *   to has no meaning on another device. A naive prefix match would carry both,
 *   so we filter by key shape (isSyncableServerKey).
 *
 * Both of these are single values rather than collections, so last-writer-wins
 * by timestamp is correct — hence the `at` field now stamped into each blob.
 * Unknown fields are ignored by both readers, so no migration is needed: a blob
 * written before this feature simply has at === 0 and loses to the cloud, which
 * is the safe direction.
 */
import AsyncStorage from '@react-native-async-storage/async-storage';
import { kvsAllKeys, kvsGet, kvsMultiGet, kvsSet } from './cloudKvs';
import { CK, CKP, isSyncableServerKey, normaliseServerUrl, serverSlug } from './cloudSync';

export const LOCAL_TUNE_PREFIX  = 'lsv_last_tune:';
export const LOCAL_PREFS_PREFIX = 'lsv_display_prefs:';
const CLOUD_TUNE_PREFIX  = CKP.tune;
const CLOUD_PREFS_PREFIX = CKP.prefs;

type ServerDoc = { v: 1; at: number; url: string; data: unknown };

/** The server the user is looking at right now, if any. Set by SDRScreen.
 *  ★ We do not push a downloaded blob into the store for the ACTIVE server: the
 *  screen holds its prefs in memory and its own debounced write would overwrite
 *  ours a moment later, so the change would appear to work and then vanish.
 *  It lands on the next connect instead. */
let activeServer: string | null = null;
export function setActiveSyncServer(baseUrl: string | null): void {
  activeServer = baseUrl ? normaliseServerUrl(baseUrl) : null;
}

function parseAt(raw: string | null): number {
  if (!raw) return 0;
  try {
    const d = JSON.parse(raw);
    return typeof d?.at === 'number' ? d.at : 0;
  } catch { return 0; }
}

/**
 * Sync one per-server namespace in both directions.
 * Servers are discovered from BOTH sides — the set differs on every device, and
 * a manifest listing them would be one more thing to merge.
 */
async function syncNamespace(localPrefix: string, cloudPrefix: string, skipActive: boolean) {
  // ── Local side ──
  const allLocal = await AsyncStorage.getAllKeys();
  const urls = new Map<string, string>();   // normalised url → local key
  for (const k of allLocal) {
    if (!k.startsWith(localPrefix)) continue;
    const suffix = k.slice(localPrefix.length);
    if (!isSyncableServerKey(suffix)) continue;   // usb / tcp:… — never synced
    urls.set(normaliseServerUrl(suffix), k);
  }

  // ── Cloud side ──
  const cloudKeys = (await kvsAllKeys()).filter(k => k.startsWith(cloudPrefix));
  const cloudRaw = await kvsMultiGet(cloudKeys);
  const cloudDocs = new Map<string, ServerDoc>();
  for (const raw of Object.values(cloudRaw)) {
    try {
      const d = JSON.parse(raw) as ServerDoc;
      if (d && typeof d.at === 'number' && typeof d.url === 'string') {
        cloudDocs.set(normaliseServerUrl(d.url), d);
      }
    } catch { /* a corrupt doc is replaced by the next upload */ }
  }

  for (const url of new Set([...urls.keys(), ...cloudDocs.keys()])) {
    const localKey = urls.get(url) ?? `${localPrefix}${url}`;
    const localRaw = await AsyncStorage.getItem(localKey).catch(() => null);
    const localAt  = parseAt(localRaw);
    const remote   = cloudDocs.get(url);

    if (remote && remote.at > localAt) {
      if (skipActive && url === activeServer) continue;
      try {
        // data === null is a TOMBSTONE (a per-server override that was reset).
        // Without it a reset is invisible to last-writer-wins — the other
        // device's untouched copy simply uploads again and the override
        // reappears, which reads as the reset button not working.
        if (remote.data === null) await AsyncStorage.removeItem(localKey);
        else await AsyncStorage.setItem(localKey, JSON.stringify({ ...(remote.data as object), at: remote.at }));
      } catch {}
    } else if (localRaw && localAt > (remote?.at ?? 0)) {
      let data: unknown;
      try { data = JSON.parse(localRaw); } catch { continue; }
      const doc: ServerDoc = { v: 1, at: localAt, url, data };
      await kvsSet(`${cloudPrefix}${serverSlug(url)}`, JSON.stringify(doc));
    }
  }
}

/** Record that a server's display override was RESET, so the reset propagates
 *  instead of being undone by the next device to sync. */
export async function markServerPrefsReset(baseUrl: string): Promise<void> {
  const url = normaliseServerUrl(baseUrl);
  const doc: ServerDoc = { v: 1, at: Date.now(), url, data: null };
  try { await kvsSet(`${CLOUD_PREFS_PREFIX}${serverSlug(url)}`, JSON.stringify(doc)); } catch {}
}

/** Last tuned frequency + demodulator, per server. */
export async function syncLastTune(): Promise<void> {
  await syncNamespace(LOCAL_TUNE_PREFIX, CLOUD_TUNE_PREFIX, false);
}

/** Waterfall + VFO colours and the rest of the display blob, per server. */
export async function syncServerDisplayPrefs(): Promise<void> {
  await syncNamespace(LOCAL_PREFS_PREFIX, CLOUD_PREFS_PREFIX, true);
}

/** The GLOBAL display blob — one value, account-wide.
 *  ★ Colours are a preference rather than a collection, so last-writer-wins is
 *  right here; there is nothing to union. */
export async function syncGlobalDisplayPrefs(): Promise<void> {
  const localRaw = await AsyncStorage.getItem('lsv_display_prefs').catch(() => null);
  const localAt = parseAt(localRaw);
  const remoteRaw = await kvsGet(CK.prefs);
  let remote: { at: number; data: unknown } | null = null;
  try {
    const d = remoteRaw ? JSON.parse(remoteRaw) : null;
    if (d && typeof d.at === 'number') remote = d;
  } catch {}

  if (remote && remote.at > localAt) {
    try {
      await AsyncStorage.setItem('lsv_display_prefs',
        JSON.stringify({ ...(remote.data as object), at: remote.at }));
    } catch {}
  } else if (localRaw && localAt > (remote?.at ?? 0)) {
    let data: unknown;
    try { data = JSON.parse(localRaw); } catch { return; }
    await kvsSet(CK.prefs, JSON.stringify({ v: 1, at: localAt, data }));
  }
}
