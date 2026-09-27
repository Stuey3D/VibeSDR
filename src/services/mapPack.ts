/**
 * mapPack.ts — unpacks the bundled vector map pack (tier0 + tier1 + index.json) out of the JS
 * bundle and onto the device's document directory, once, as real files.
 *
 * ★★★ WHY THIS EXISTS AT ALL: the pack is ~15 MB of JSON text living in `src/generated/
 *   mapdataBundle.ts`. Serving it to the map WebView from memory means every layer crosses the
 *   React Native bridge as a string — and that is what made the HFDL animation judder: the bridge
 *   was busy shipping megabytes of coastline while the animation wanted frames. Written to disk
 *   ONCE, the page loads from a local origin and reads its layers off the filesystem instead, so
 *   the bridge carries nothing but control messages.
 *
 * ★★ The marker file is the whole correctness story. It is written LAST, after every entry has
 *   landed, so an unpack killed halfway (backgrounded, out of storage, process death) leaves NO
 *   marker and is simply redone on the next map open. A marker written first, or alongside, would
 *   latch a half-written pack as complete and the map would come up with holes in it — and nothing
 *   would ever retry (see "never defer a write nothing will retry").
 *
 * ★★ VERSION is derived from the bundle itself — total bytes plus the file count — not
 *   hand-maintained. A hand-maintained counter is a gate that gets forgotten: regenerate the
 *   bundle with one extra layer and the version moves on its own, so the stale pack on disk is
 *   replaced without anyone remembering to bump anything.
 *
 * ★ ensureMapPack() runs on EVERY map open, so the already-unpacked path must be near-free: one
 *   file read of a short marker and nothing else. No directory listing, no per-file stat.
 *
 * ★ The getters in MAPDATA_FILES are lazy by design. We call them one at a time and let each
 *   string go out of scope before the next — building an array of all 27 would hold the whole
 *   15 MB resident at once, which on a low-memory Android device is a kill.
 */
import { Directory, File, Paths } from 'expo-file-system';

import { MAPDATA_BYTES, MAPDATA_FILES } from '../generated/mapdataBundle';

/** Bumping this invalidates every unpacked pack on every device — only for layout changes. */
const PACK_DIR = 'mapdata';
const PACK_VER = 'v1';
const MARKER    = '.unpacked';

/** Derived from the bundle, never hand-written: bytes + entry count. */
function packVersion(): string {
  return `${MAPDATA_BYTES}-${Object.keys(MAPDATA_FILES).length}`;
}

/** The pack's directory URI. Does NOT touch the filesystem. */
export function mapPackDir(): string {
  return new Directory(Paths.document, PACK_DIR, PACK_VER).uri;
}

/** Everything under <documents>/mapdata/, all versions — the root a storage readout reports on. */
function mapRoot(): Directory {
  return new Directory(Paths.document, PACK_DIR);
}

/**
 * Unpacks the bundled pack if it is not already on disk at this version, and returns the
 * directory URI. Cheap on the already-unpacked path; throws if the unpack could not complete
 * (leaving no marker, so the next call retries rather than trusting a partial pack).
 */
export async function ensureMapPack(): Promise<string> {
  const dir     = new Directory(Paths.document, PACK_DIR, PACK_VER);
  const marker  = new File(dir, MARKER);
  const version = packVersion();

  // ★ The fast path. A read that throws (no file, no directory, no permission) is simply "not
  //   unpacked" — never a reason to fail the map.
  try {
    if (marker.exists && (await marker.text()).trim() === version) return dir.uri;
  } catch {
    /* fall through and unpack */
  }

  try {
    dir.create({ intermediates: true, idempotent: true });
  } catch (e) {
    throw new Error(`mapPack: cannot create ${dir.uri}: ${String(e)}`);
  }

  // ★ A stale marker must go BEFORE we start overwriting files: if we die mid-rewrite, a marker
  //   left from the previous version would claim a pack that is now a mixture of two.
  try {
    if (marker.exists) marker.delete();
  } catch {
    /* if it will not delete we still overwrite it at the end, which is the same outcome */
  }

  for (const name of Object.keys(MAPDATA_FILES)) {
    if (name === MARKER) continue; // a data file must never collide with the marker
    try {
      const getter = MAPDATA_FILES[name];
      const target = new File(dir, name);
      // write() creates the file when absent and truncates it when present, so no create() first.
      target.write(getter());
    } catch (e) {
      throw new Error(`mapPack: failed writing ${name}: ${String(e)}`);
    }
  }

  try {
    marker.write(version);
  } catch (e) {
    // No marker = the next open redoes the unpack. Worse than ideal, never wrong.
    throw new Error(`mapPack: unpacked but could not mark complete: ${String(e)}`);
  }

  return dir.uri;
}

/**
 * Total bytes on disk under <documents>/mapdata/ (all versions), for the storage readout.
 * 0 when nothing has been unpacked — an unreadable tree reads as 0 rather than throwing, because
 * a readout is never worth failing a screen over.
 */
export async function mapStorageBytes(): Promise<number> {
  const root = mapRoot();
  try {
    if (!root.exists) return 0;
  } catch {
    return 0;
  }

  // ★ Directory.size is recursive on both platforms; we only walk by hand if it declines to
  //   answer (it is typed number | null), so the common case is one native call.
  try {
    const whole = root.size;
    if (typeof whole === 'number') return whole;
  } catch {
    /* fall through to the walk */
  }
  return sumDirectory(root);
}

function sumDirectory(dir: Directory): number {
  let total = 0;
  let entries: (Directory | File)[];
  try {
    entries = dir.list();
  } catch {
    return 0;
  }
  for (const entry of entries) {
    try {
      if (entry instanceof Directory) {
        const s = entry.size;
        total += typeof s === 'number' ? s : sumDirectory(entry);
      } else {
        total += entry.size;
      }
    } catch {
      /* one unreadable entry must not zero the whole figure */
    }
  }
  return total;
}

/**
 * Deletes the OPTIONAL half of the pack — the tier2 layers and the relief rasters a server may
 * have handed us — and leaves tier0, tier1 and index.json alone.
 *
 * ★★ Those two are what vibemap.js calls "present by definition": remove them and the map has no
 *   world under it at all. Only the detail that can be re-fetched is removable, which is why this
 *   matches on name and never just empties the directory.
 */
export async function removeDetailMaps(): Promise<void> {
  const dir = new Directory(Paths.document, PACK_DIR, PACK_VER);
  try {
    if (!dir.exists) return;
  } catch {
    return;
  }

  let entries: (Directory | File)[];
  try {
    entries = dir.list();
  } catch (e) {
    throw new Error(`mapPack: cannot list ${dir.uri}: ${String(e)}`);
  }

  for (const entry of entries) {
    const name = entry.name;
    if (!name.startsWith('tier2-') && !name.startsWith('relief')) continue;
    try {
      entry.delete();
    } catch {
      /* a file we cannot remove is not a reason to abandon the rest of the sweep */
    }
  }
}

/* ★★★ THE MAP PAGE ITSELF, WRITTEN BESIDE THE PACK SO IT CAN READ IT.
 *  The page used to be handed to the WebView as an HTML string with the INSTANCE as its base URL,
 *  which left the map data no route but the React Native bridge — several megabytes per layer, a
 *  round trip each, and an animation that juddered and sometimes drew blank. Written to disk one
 *  level above the pack, its own relative `mapdata/v1/...` resolves to real files.
 *  ★★ REWRITTEN EVERY OPEN. It is small, and it must track the code that generates it: a page
 *     cached from an earlier build is a ghost nobody thinks to look for.
 *  ✗ UNVERIFIED ON A DEVICE, AND DELIBERATELY NOT RELIED ON: WKWebView blocks fetch()/XHR to
 *    file:// from a file:// document by default, and whether react-native-webview's file-access
 *    flags lift that for our case is not something the type definitions can answer. The page keeps
 *    its bridge fallback, so if a local read is refused the layer still arrives the old way — slow
 *    but correct — instead of the map silently drawing nothing. */
export async function writeMapPage(html: string): Promise<string> {
  try {
    const f = new File(Paths.document, 'vibemap.html');
    f.write(html);
    return f.uri;
  } catch (e) {
    throw new Error('could not write the map page: ' + String((e as Error)?.message || e));
  }
}
