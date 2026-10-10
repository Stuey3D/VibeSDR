/**
 * ★★ WHICH SERVER EACH RECORDING CAME FROM (2026-10-10). The native recorders (iOS and Android) name a file
 * VibeSDR_<time>_<freq>_<mode>.m4a and know nothing of servers; the screen that stopped the recording does. So the
 * screen notes it here, beside the recordings, and the Recordings list groups by it (services/recordingGroups.ts).
 * ★ A side file rather than a new filename, so neither recorder changes and the files people already shared keep
 *   their names. A file deleted outside the app just leaves a stale line, pruned the next time the list loads.
 */
import * as FileSystem from 'expo-file-system/legacy';

type Index = Record<string, { server: string; at?: number }>;
const FILE = () => (FileSystem.documentDirectory ?? '') + 'recordings-index.json';

export async function loadRecordingIndex(): Promise<Index> {
  try {
    const info = await FileSystem.getInfoAsync(FILE());
    if (!info.exists) return {};
    const j = JSON.parse(await FileSystem.readAsStringAsync(FILE()));
    return j && typeof j === 'object' ? (j as Index) : {};
  } catch { return {}; }
}

async function save(ix: Index): Promise<void> {
  try { await FileSystem.writeAsStringAsync(FILE(), JSON.stringify(ix)); } catch {}
}

const baseName = (path: string) => String(path).replace(/^file:\/\//, '').split('/').pop() ?? '';

/** Called when a recording stops: `path` is what the native recorder returned. */
export async function noteRecordingServer(path: string, server: string): Promise<void> {
  const name = baseName(path), s = String(server ?? '').trim();
  if (!name || !s) return;
  const ix = await loadRecordingIndex();
  ix[name] = { server: s, at: Date.now() };
  await save(ix);
}

/** Drop lines for files that are gone (deleted here or elsewhere). */
export async function pruneRecordingIndex(present: string[]): Promise<void> {
  const ix = await loadRecordingIndex();
  const keep = new Set(present);
  let changed = false;
  for (const k of Object.keys(ix)) if (!keep.has(k)) { delete ix[k]; changed = true; }
  if (changed) await save(ix);
}
