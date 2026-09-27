/**
 * mapglDetail.ts — the OPTIONAL high-detail map pack for the GPU map: one ~169 MB PMTiles file,
 * downloaded once to <documents>/mapgl/vibemap-detail.pmtiles, removable, and read by BYTE RANGE.
 *
 * ★★★ STUART'S SPEC (2026-09-27): prompt on first launch (and on the first launch after updating from an
 *     older build — those users have never been asked); if declined, the coarse maps still do the job;
 *     the directory's footer says either "High Detail Maps installed, current space taken N MB — Remove?"
 *     or "High Detail Maps not installed … approx 200 MB … one time download — Download now?".
 *
 * ★★ ONE FILE, RANGED READS OVER THE BRIDGE — NOT 1.3 MILLION TILE FILES. The pack addresses 1,342,238
 *    tiles; the map WebView cannot read part of a file:// file, but React Native can (FileHandle). So the
 *    page asks for {offset, length} and this answers with exactly those bytes. Tiles are a few KB and
 *    only wanted at close zoom (a handful per view) — the HFDL flyover never reaches detail zoom at all.
 *    (What made the bridge slow before was MEGABYTE JSON files crossing it, not KB-sized tiles.)
 * ★★ A HALF-DOWNLOAD IS NEVER "INSTALLED": it lands as .part and is renamed only after its size and the
 *    PMTiles magic have been checked — the marker-last rule of mapPack.ts, in file form.
 */
import AsyncStorage from '@react-native-async-storage/async-storage';
import { Directory, File, FileHandle, Paths } from 'expo-file-system';

/** Where the pack is published — a GitHub release asset, beside the Lite APK. */
export const DETAIL_URL = 'https://github.com/Stuey3D/VibeSDR/releases/download/mapgl-detail-v1/vibemap-detail.pmtiles';
/** The published size in bytes, for the "approx" wording and the integrity check.
 *  ★ TIED TO THE PUBLISHED ASSET: rebuild the pack and this, the release asset and DETAIL_URL's tag move
 *    together (a new tag, mapgl-detail-v2, so an installed v1 is never mistaken for the new one). */
export const DETAIL_BYTES = 177024426;
/** "Asked once" — set when the first-launch prompt has been answered either way. */
const ASKED_KEY = 'mapgl_detail_asked_v1';

function dir(): Directory { return new Directory(Paths.document, 'mapgl'); }
function packFile(): File { return new File(dir(), 'vibemap-detail.pmtiles'); }
function partFile(): File { return new File(dir(), 'vibemap-detail.pmtiles.part'); }

export function detailInstalled(): boolean {
  try { return packFile().exists; } catch { return false; }
}
export function detailSizeBytes(): number {
  try { const f = packFile(); return f.exists ? (f.size ?? 0) : 0; } catch { return 0; }
}
export async function detailAsked(): Promise<boolean> {
  try { return (await AsyncStorage.getItem(ASKED_KEY)) === '1'; } catch { return false; }
}
export async function markDetailAsked(): Promise<void> {
  try { await AsyncStorage.setItem(ASKED_KEY, '1'); } catch { /* asked again next launch — harmless */ }
}

/* ── Download state, shared by the prompt and the directory footer ─────────────────────────────── */
export type DetailState =
  | { kind: 'idle' }
  | { kind: 'downloading'; written: number; total: number }
  | { kind: 'error'; message: string };
let state: DetailState = { kind: 'idle' };
const listeners = new Set<(s: DetailState) => void>();
function set(s: DetailState) { state = s; for (const l of listeners) l(s); }
export function detailState(): DetailState { return state; }
export function onDetailState(fn: (s: DetailState) => void): () => void {
  listeners.add(fn); return () => { listeners.delete(fn); };
}

let running: Promise<void> | null = null;
/** Starts (or joins) the one download. Resolves when the pack is installed; never throws — failure
 *  lands in the state as { kind: 'error' } for the footer to show. */
export function downloadDetail(): Promise<void> {
  if (running) return running;
  running = (async () => {
    try {
      dir().create({ intermediates: true, idempotent: true });
      try { if (partFile().exists) partFile().delete(); } catch { /* overwritten below */ }
      set({ kind: 'downloading', written: 0, total: DETAIL_BYTES });
      const task = File.createDownloadTask(DETAIL_URL, partFile(), {
        onProgress: ({ bytesWritten, totalBytes }) =>
          set({ kind: 'downloading', written: bytesWritten, total: totalBytes > 0 ? totalBytes : DETAIL_BYTES }),
      });
      await task.downloadAsync();
      // ★ Checked BEFORE it is allowed to be called installed: the right size, and a PMTiles header.
      const part = partFile();
      if ((part.size ?? 0) !== DETAIL_BYTES) throw new Error(`download incomplete (${part.size} of ${DETAIL_BYTES} bytes)`);
      const h = part.open();
      try { h.offset = 0; const magic = String.fromCharCode(...h.readBytes(7)); if (magic !== 'PMTiles') throw new Error('not a map pack'); }
      finally { h.close(); }
      try { if (packFile().exists) packFile().delete(); } catch { /* replaced by the move */ }
      await part.move(packFile());   // ★ async — awaited, or "installed" could be said before the file is there
      closeHandle();
      set({ kind: 'idle' });
    } catch (e) {
      try { if (partFile().exists) partFile().delete(); } catch { /* a stale .part is never read */ }
      set({ kind: 'error', message: String((e as Error)?.message || e) });
    } finally {
      running = null;
    }
  })();
  return running;
}

export function removeDetail(): void {
  closeHandle();
  try { if (packFile().exists) packFile().delete(); } catch { /* the footer re-reads the truth */ }
  set({ kind: 'idle' });
}

/* ── Ranged reads for the map page ─────────────────────────────────────────────────────────────── */
let handle: FileHandle | null = null;
function closeHandle() { try { handle?.close(); } catch { /* already closed */ } handle = null; }

/** Exactly `length` bytes from `offset`, base64 — what the page's pmtiles source asked for. null if the
 *  pack is not installed or the read fails (the page then draws the coarse map there). */
export function readDetailRange(offset: number, length: number): string | null {
  try {
    if (!handle) { if (!detailInstalled()) return null; handle = packFile().open(); }
    handle.offset = offset;
    return toBase64(handle.readBytes(length));
  } catch {
    closeHandle();
    return null;
  }
}

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
function toBase64(u: Uint8Array): string {
  let out = '', i = 0;
  for (; i + 2 < u.length; i += 3) {
    const n = (u[i] << 16) | (u[i + 1] << 8) | u[i + 2];
    out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + B64[n & 63];
  }
  if (i < u.length) {
    const n = (u[i] << 16) | ((i + 1 < u.length ? u[i + 1] : 0) << 8);
    out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + (i + 1 < u.length ? B64[(n >> 6) & 63] : '=') + '=';
  }
  return out;
}
