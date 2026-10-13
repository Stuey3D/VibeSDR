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
import { NativeModules, Platform } from 'react-native';
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
 * Puts the GPU-map files on disk and returns WHERE, as a path RELATIVE TO <documents>/ with a trailing slash
 * — the map page (vibemap.html) lives in <documents>/ and loads them relative to itself. Throws if they
 * cannot all be there; the caller then falls back to the Leaflet map rather than draw one with holes.
 *
 * ★★★ ANDROID READS THE APK'S OWN BUNDLE (2026-10-13). In a RELEASE build an embedded asset has no file
 *  path — expo-asset's localUri is a bare resource name, and File.copy threw "URI is not absolute" — so
 *  this copy never worked on Android and every phone drew the Leaflet fallback (five full-screen canvases,
 *  ~250 MB of GPU memory, ~20 fps on the Moto G35's HFDL map). The APK already carries the same files as
 *  plain assets for the phone-as-server map; VibeMapGL.bundleDir unpacks them (once, versioned) to
 *  <documents>/mapgl-bundled/ and both use that one copy. iOS keeps the copy below — its localUri is a
 *  real bundle path.
 */
export async function ensureMapglPack(): Promise<string> {
  if (Platform.OS === 'android') {
    const mod = NativeModules.VibePowerModule as { mapglBundleDir?: () => Promise<string> } | undefined;
    if (!mod?.mapglBundleDir) throw new Error('mapglPack: native bundle unavailable');
    const abs = await mod.mapglBundleDir();
    const name = abs.replace(/\/+$/, '').split('/').pop();
    if (!name) throw new Error(`mapglPack: odd bundle path ${abs}`);
    return name + '/';
  }
  const dir = mapglDir();
  const marker = new File(dir, MARKER);
  const version = packVersion();
  try {
    if (marker.exists && (await marker.text()).trim() === version) return DIR + '/';
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
  return DIR + '/';
}
