/**
 * dialSync.ts — the FM-DX dial: its storage shape, its ageing rules, and its
 * iCloud sync.
 *
 * ★★ The cost this exists to remove is TUNER TIME ON SOMEONE ELSE'S RADIO, not
 * storage. The dial fills by decoding RDS as you tune across 88–108, so filling
 * it means parking on every station in the band. On a shared FM-DX server that
 * is time taken from other listeners; doing it five times over — Jr, Buddy,
 * iPhone, iPad, Mac — is that same courtesy cost multiplied for no gain,
 * because every device is learning the identical thing about the identical
 * transmitters.
 *
 * See briefs/BRIEF-dial-and-station-sync.md.
 */
import AsyncStorage from '@react-native-async-storage/async-storage';
import { DIAL_EXPIRY_MS, DIAL_MAX, type DialStation } from '../components/FmdxDial';
import { kvsAllKeys, kvsMultiGet, kvsSet } from './cloudKvs';
import { CKP, isSyncableServerKey, normaliseServerUrl, serverSlug } from './cloudSync';

const LOCAL_PREFIX = 'lsv_fmdx_dial:';
const CLOUD_PREFIX = CKP.dial;

export const dialKeyFor = (baseUrl: string) => `${LOCAL_PREFIX}${baseUrl}`;

/** ★ Stamp entries that predate `lastHeard`, BEFORE anything applies expiry.
 *  Without this the first launch after the update sees undated entries and
 *  throws the whole dial away. */
export function stampUndatedDial(raw: unknown, now = Date.now()): DialStation[] {
  if (!Array.isArray(raw)) return [];
  return raw
    .filter((s): s is DialStation => !!s && typeof s.freqHz === 'number' && typeof s.name === 'string')
    .map((s) => (typeof s.lastHeard === 'number' ? s : { ...s, lastHeard: now }));
}

/** Drop what has expired, then cap. The cap keeps the FRESHEST entries rather
 *  than the last 300 inserted — an old slice would throw away this evening's
 *  scanning to keep last month's. */
export function pruneDial(list: DialStation[], now = Date.now()): DialStation[] {
  return list
    .filter((s) => now - (s.lastHeard ?? now) < DIAL_EXPIRY_MS)
    .sort((a, b) => (b.lastHeard ?? 0) - (a.lastHeard ?? 0))
    .slice(0, DIAL_MAX)
    .sort((a, b) => a.freqHz - b.freqHz);
}

/** ★ MERGE, never replace. Two devices have each heard things the other has
 *  not, so a sync is a union; for a frequency present in both, the newer
 *  `lastHeard` wins — which also settles PI displacement, because the device
 *  that heard the new broadcaster heard it more recently. */
export function mergeDials(a: DialStation[], b: DialStation[], now = Date.now()): DialStation[] {
  const byFreq = new Map<number, DialStation>();
  for (const s of [...a, ...b]) {
    const prev = byFreq.get(s.freqHz);
    if (!prev || (s.lastHeard ?? 0) > (prev.lastHeard ?? 0)) byFreq.set(s.freqHz, s);
  }
  return pruneDial([...byFreq.values()], now);
}

type DialDoc = { v: 1; url: string; stations: DialStation[] };

/** Sync every per-server dial, both directions. Keyed by server as today: a
 *  dial describes what THAT aerial hears, so it cannot be pooled. */
export async function syncFmdxDials(): Promise<void> {
  const now = Date.now();

  const localKeys = (await AsyncStorage.getAllKeys())
    .filter((k) => k.startsWith(LOCAL_PREFIX))
    .filter((k) => isSyncableServerKey(k.slice(LOCAL_PREFIX.length)));
  const urls = new Map<string, string>();
  for (const k of localKeys) urls.set(normaliseServerUrl(k.slice(LOCAL_PREFIX.length)), k);

  const cloudRaw = await kvsMultiGet((await kvsAllKeys()).filter((k) => k.startsWith(CLOUD_PREFIX)));
  const cloudDocs = new Map<string, DialDoc>();
  for (const raw of Object.values(cloudRaw)) {
    try {
      const d = JSON.parse(raw) as DialDoc;
      if (d && typeof d.url === 'string' && Array.isArray(d.stations)) {
        cloudDocs.set(normaliseServerUrl(d.url), d);
      }
    } catch { /* a corrupt doc is replaced by the next upload */ }
  }

  for (const url of new Set([...urls.keys(), ...cloudDocs.keys()])) {
    const localKey = urls.get(url) ?? `${LOCAL_PREFIX}${url}`;
    let local: DialStation[] = [];
    try {
      const raw = await AsyncStorage.getItem(localKey);
      if (raw) local = stampUndatedDial(JSON.parse(raw), now);
    } catch {}

    const remote = stampUndatedDial(cloudDocs.get(url)?.stations ?? [], now);
    const merged = mergeDials(local, remote, now);

    if (JSON.stringify(merged) !== JSON.stringify(pruneDial(local, now))) {
      try { await AsyncStorage.setItem(localKey, JSON.stringify(merged)); } catch {}
    }
    if (merged.length && JSON.stringify(merged) !== JSON.stringify(pruneDial(remote, now))) {
      const doc: DialDoc = { v: 1, url, stations: merged };
      await kvsSet(`${CLOUD_PREFIX}${serverSlug(url)}`, JSON.stringify(doc));
    }
  }
}
