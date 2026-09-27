/**
 * mapglPack.ts — puts the GPU map's bundled files (basic + relief tile packs, label fonts, icons) onto
 * disk at <documents>/mapgl/, once, in the folder layout MapLibre reads them from.
 *
 * ★★ WHY A COPY AT ALL: the fonts are fetched by MapLibre from a URL TEMPLATE —
 *    file:///…/fonts/{fontstack}/{range}.pbf — which needs a real directory per font. Native assets
 *    arrive under hashed names with no folders (see src/generated/mapglAssets.ts), so they are copied
 *    into place. The tile packs come too, so every path the style names is stable across launches.
 *
 * ★★ The marker file is the whole correctness story, exactly as in mapPack.ts: written LAST, after
 *    every file has landed, so a copy killed halfway leaves no marker and is simply redone next open.
 *    A marker written first would latch a half-copied pack as complete and nothing would retry.
 * ★★ VERSION is derived from the manifest (total bytes + file count), never hand-maintained, so a
 *    regenerated pack replaces the stale one on its own.
 * ★ ensureMapglPack() runs on every GPU-map open; the already-copied path is one marker read.
 */
import { Asset } from 'expo-asset';
import { Directory, File, Paths } from 'expo-file-system';
import { MAPGL_BYTES, MAPGL_FILES } from '../generated/mapglAssets';

const DIR = 'mapgl';
const MARKER = '.copied';

function packVersion(): string {
  return `${MAPGL_BYTES}-${MAPGL_FILES.length}`;
}

/** The pack directory. Does NOT touch the filesystem. */
export function mapglDir(): Directory {
  return new Directory(Paths.document, DIR);
}

/**
 * Copies the bundled GPU-map files into <documents>/mapgl/ if they are not already there at this
 * version, and returns the directory URI (with a trailing slash). Throws if the copy cannot finish —
 * the caller falls back to the WebView map rather than drawing a map with holes in it.
 */
export async function ensureMapglPack(): Promise<string> {
  const dir = mapglDir();
  const marker = new File(dir, MARKER);
  const version = packVersion();
  try {
    if (marker.exists && (await marker.text()).trim() === version) return dir.uri;
  } catch {
    /* fall through and copy */
  }
  dir.create({ intermediates: true, idempotent: true });
  // ★ A stale marker goes BEFORE any overwrite, so a copy that dies part-way cannot be mistaken for
  //   a complete one of either version.
  try { if (marker.exists) marker.delete(); } catch { /* overwritten at the end regardless */ }
  for (const [rel, mod] of MAPGL_FILES) {
    const asset = Asset.fromModule(mod);
    await asset.downloadAsync();                    // ★ Android: extracts from the APK; iOS: the bundle path
    if (!asset.localUri) throw new Error(`mapglPack: no local file for ${rel}`);
    const parts = rel.split('/');
    const name = parts.pop() as string;
    const folder = parts.length ? new Directory(dir, ...parts) : dir;
    folder.create({ intermediates: true, idempotent: true });
    await new File(asset.localUri).copy(new File(folder, name), { overwrite: true });
  }
  marker.write(version);
  return dir.uri;
}
