/**
 * mapglPack.ts — puts the GPU map's bundled files (basic + relief tile packs, label fonts, icons) onto
 * disk at <documents>/mapgl/, once, in the folder layout MapLibre reads them from.
 *
 * ★★ WHY A COPY AT ALL: the fonts are fetched by MapLibre from a URL TEMPLATE —
 *    file:///…/fonts/{fontstack}/{range}.pbf — which needs a real directory per font. Native assets
 *    arrive under hashed names with no folders (see src/generated/mapglAssets.ts), so they are copied
 *    into place. ★★★ beta96: the tile packs are NOT copied any more — the page reads them by range over the
 *    bridge (readPackRange) straight from the app bundle, so the 30 MB exist once, not twice.
 *
 * ★★ The marker file is the whole correctness story, exactly as in mapPack.ts: written LAST, after
 *    every file has landed, so a copy killed halfway leaves no marker and is simply redone next open.
 *    A marker written first would latch a half-copied pack as complete and nothing would retry.
 * ★★ VERSION is the manifest's CONTENT HASH (MAPGL_VERSION — scripts/lib/mapgl-manifest.mjs), never hand-
 *    maintained, so a regenerated pack replaces the stale one on its own.
 * ★★★ IT WAS BYTES + COUNT, AND THE MANIFEST WENT STALE (2026-10-27): vibemapgl-compat.js gained the spots
 *    map's mode-letter layer, its asset copy was re-synced, the manifest was not — so the version did not
 *    move and every device that already had the pack kept the old compat. Letters on a fresh install
 *    ("i saw them once"), never again after. check-map-overlay.mjs now fails on a stale manifest.
 * ★ ensureMapglPack() runs on every GPU-map open; the already-copied path is one marker read.
 */
import { Asset } from 'expo-asset';
import { Directory, File, FileMode, Paths, type FileHandle } from 'expo-file-system';
import { NativeModules, Platform } from 'react-native';
import { MAPGL_FILES, MAPGL_VERSION } from '../generated/mapglAssets';
import { toBase64 } from './mapglDetail';

const DIR = 'mapgl';
const MARKER = '.copied';

/** ★★★ beta96: the three big packs are read BY RANGE straight from where the OS already keeps them — the iOS app
 *  bundle, the Android APK — and are never copied (basic 18 MB + relief + runways ≈ 30.5 MB, which every install held
 *  twice). Only fonts, scripts, icons and the style are copied (the WebView loads those by URL). */
const PACKS = new Set(['vibemap-basic.pmtiles', 'vibemap-relief.pmtiles', 'vibemap-runways.pmtiles']);

/** ★★ The marker names the LAYOUT as well as the content: a beta95/96 copy (marker = MAPGL_VERSION alone) holds the
 *  packs in <documents>/mapgl/, so it no longer matches, is redone once without them, and the old copies are deleted
 *  — never ~30 MB orphaned. Content changes still move MAPGL_VERSION as before. */
function packVersion(): string {
  return MAPGL_VERSION + '+bundlepacks';
}

/** iOS: each pack's file in the app bundle (expo-asset's localUri — the bundle path itself in a release build; the
 *  Metro download cache in development). Filled by ensureMapglPack; readPackRange reads only what is here. */
const bundlePacks = new Map<string, string>();

async function resolveBundlePacks(): Promise<void> {
  for (const [rel, mod] of MAPGL_FILES) {
    if (!PACKS.has(rel) || bundlePacks.has(rel)) continue;
    const asset = Asset.fromModule(mod);
    await asset.downloadAsync();
    if (!asset.localUri) throw new Error(`mapglPack: no bundle file for ${rel}`);
    bundlePacks.set(rel, asset.localUri);
  }
  for (const p of PACKS) if (!bundlePacks.has(p)) throw new Error(`mapglPack: ${p} is not in the app`);
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
  await resolveBundlePacks();   // ★ every open: the packs are read from the bundle, wherever this launch put it
  try {
    if (marker.exists && (await marker.text()).trim() === version) return DIR + '/';
  } catch {
    /* fall through and copy */
  }
  dir.create({ intermediates: true, idempotent: true });
  closePackHandles();   // ★ a handle on the OLD copy must never answer for the new one
  // ★ A stale marker goes BEFORE any overwrite, so a copy that dies part-way cannot be mistaken for
  //   a complete one of either version.
  try { if (marker.exists) marker.delete(); } catch { /* overwritten at the end regardless */ }
  // ★★ UPGRADE CLEAN-UP: an older copy kept the packs here (~30 MB). Gone before anything else is written.
  for (const p of PACKS) {
    try { const f = new File(dir, p); if (f.exists) f.delete(); } catch { /* best effort — retried on the next copy */ }
  }
  for (const [rel, mod] of MAPGL_FILES) {
    if (PACKS.has(rel)) continue;   // read from the bundle by range (resolveBundlePacks), never copied
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

/* ── Ranged reads of the BUNDLED packs (beta96) ───────────────────────────────────────────────────────────────────
 * ★★★ THE MAP READS THE BASIC PACK BY RANGE, NOT WHOLE (Stuart, 11-26). The map page used to XHR each bundled pack whole
 *     into the WebView and keep it there as an ArrayBuffer for as long as a map was open: basic 18.2 MB (since the
 *     rivers), relief 8.4, runways 3.9 — ~30 MB, on phones that include 32-bit 1 GB ones. Now the page asks for
 *     {name, offset, length} (MapOverlay's packRange → vibemapgl.js) and this answers with exactly those bytes, base64,
 *     from the one file on disk — the path the High Detail pack has always used (mapglDetail.ts readDetailRange).
 * ★ Trade-off: a bridge round trip per tile (a few KB; a world tile ~100–200 KB) instead of a memcpy — some JS-thread
 *   time on first draw, for ~30 MB less RAM the whole time a map is open. MapLibre keeps decoded tiles either way.
 * ★ ONLY the three bundled packs, from where the OS keeps them (beta96: the iOS app bundle, the Android APK — never a
 *   copy), and reads are capped: the page is ours, but the bridge is not a file server. `base` is still checked (the
 *   page's own fixed folder name) though neither platform reads under it any more. */
const MAX_READ = 4 * 1024 * 1024;   // ★ pmtiles asks 16 KB for the header, then directories and tiles — never MBs
const handles = new Map<string, FileHandle>();

export function closePackHandles(): void {
  for (const h of handles.values()) { try { h.close(); } catch { /* already closed */ } }
  handles.clear();
}

/** Exactly `length` bytes from `offset` of bundled pack `name` under `base` (the page's glBase, e.g. 'mapgl-bundled/'),
 *  base64; fewer at the end of the file. null on any failure (the page then leaves that tile out). */
export function readPackRange(base: string, name: string, offset: number, length: number): string | null {
  if (!PACKS.has(name) || !/^[a-z-]+\/$/.test(base)) return null;
  if (!(offset >= 0) || !(length > 0) || length > MAX_READ) return null;
  if (Platform.OS === 'android') {
    /* ★★★ beta96 ANDROID: straight out of the APK (VibeMapGL.readPackRange — the packs are stored uncompressed and
     *  no longer unpacked; an entry that turned out compressed is read from its unpacked copy there instead). */
    const mod = NativeModules.VibePowerModule as
      { mapglPackRange?: (n: string, o: number, l: number) => string | null } | undefined;
    try { return mod?.mapglPackRange?.(name, Math.floor(offset), Math.floor(length)) ?? null; } catch { return null; }
  }
  /* iOS: the app bundle's own file (resolveBundlePacks), opened READ-ONLY — the bundle cannot be opened for update. */
  const src = bundlePacks.get(name);
  if (!src) return null;
  const key = src;
  try {
    let h = handles.get(key);
    if (!h) { h = new File(src).open(FileMode.ReadOnly); handles.set(key, h); }
    const size = h.size ?? 0;
    if (offset >= size) return null;
    h.offset = offset;
    return toBase64(h.readBytes(Math.min(length, size - offset)));
  } catch {
    try { handles.get(key)?.close(); } catch { /* gone */ }
    handles.delete(key);
    return null;
  }
}
