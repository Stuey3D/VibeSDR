/**
 * chunkCache.ts — the map renderer and the admin panel, fetched ONCE for every *.vibeserver.vibesdr.net
 * server this browser visits, not once per server.
 *
 * ★★★ WHY (Stuart, 2026-10-01): "We save settings for all .vibeserver.vibesdr.net servers. The map and
 *     admin pages are the same on all of them: fetch once, store it in the same storage the settings
 *     and bookmarks use, so it loads once for ANY server you go to. Only load again if an update
 *     changes the maps or the admin page." The page diet (B8) made both separate /vs/c-<hash>.js
 *     files, but the browser's HTTP cache is per ORIGIN — every server is a new origin, so a listener
 *     who hops between five servers paid ~100 KB of map code five times. The portable store
 *     (portable.ts, the directory's /store frame) is the one place they all share — on Chromium and
 *     Firefox. ★ Safari partitions that frame by the page around it: no sharing there, and nothing
 *     breaks either (it simply never finds a copy).
 *
 * ★★★ NOTHING IN THE STORE IS TRUSTED (chunkVerify.ts has the rule). Any *.vibeserver.vibesdr.net page
 *     can write to it, including one a stranger hosts. A stored copy runs only if its SHA-256 equals
 *     the hash in THIS server's own page; a copy fetched from this server is checked the same way
 *     before it is stored. The page asks for ITS hash — the store never gets to choose a version.
 *
 * ★★ NOTHING IS FETCHED AHEAD (Stuart: "on a janky cellular connection prefetching background pages
 *    may cause issues"). The map's code is fetched when the map is opened, exactly as before; the
 *    only change is that it is saved to the store afterwards, and looked for there first.
 * ★★ EVERY FAILURE IS TODAY'S BEHAVIOUR: off the VibeSDR domain, no manifest, no WebCrypto, the store
 *    slow (300 ms budget) or absent, a copy that fails its check, a blob import refused — each falls
 *    through to the plain import() the page always used.
 * ★ The Opus decoder is deliberately NOT here: on an https VibeSDR.net page every browser whose store
 *   is shared (Chromium, Firefox) has WebCodecs and never loads it, and it sits on the audio-start path.
 */
import { onVibeDomain, storeRequest } from './portable';
import { parseManifest, rewriteImports, verifyChunk, type ChunkEntry, type ChunkManifest } from './chunkVerify';

/** How long the store may take to answer before the chunk is fetched from this server as before. */
const STORE_BUDGET_MS = 300;

let manifest: ChunkManifest | null | undefined;
function pageManifest(): ChunkManifest | null {
  if (manifest === undefined) {
    // ★ Written into the HTML by build-web.mjs — served by THIS server with the code it describes.
    const el = typeof document !== 'undefined' ? document.getElementById('vs-chunks') : null;
    manifest = el ? parseManifest(el.textContent || '') : null;
  }
  return manifest;
}

/** WebCrypto exists only in a secure context (a LAN http page has none — and no shared store either). */
function subtle(): SubtleCrypto | null {
  return globalThis.isSecureContext && globalThis.crypto && crypto.subtle ? crypto.subtle : null;
}

/** Where this page's own chunks live — the directory its modules were loaded from (/vs/ or /r/<id>/vs/). */
const here = (rel: string) => new URL(rel, import.meta.url).href;

const loads = new Map<string, Promise<unknown>>();

/** The chunk `name`, from the shared store if a verified copy is there, else as `fallback` loads it. */
function cachedImport<T>(name: string, fallback: () => Promise<T>): Promise<T> {
  let p = loads.get(name) as Promise<T> | undefined;
  if (!p) {
    p = viaStore(name, fallback).catch((e) => { loads.delete(name); throw e; });
    loads.set(name, p);
  }
  return p;
}

async function viaStore<T>(name: string, fallback: () => Promise<T>): Promise<T> {
  const entry = pageManifest()?.[name];
  const sc = subtle();
  if (!entry || !sc || !onVibeDomain() || typeof Blob === 'undefined') return fallback();

  let bytes: Uint8Array | null = null;
  try {
    const got = await storeRequest({ op: 'chunkGet', sha: entry.sha256 }, STORE_BUDGET_MS);
    const b = got?.ok && got.bytes instanceof ArrayBuffer ? new Uint8Array(got.bytes) : null;
    if (b) {
      if (await verifyChunk(entry, b, sc)) bytes = b;
      else console.warn(`[chunk cache] the shared copy of "${name}" failed its integrity check — ignored, fetching from this server`);
    }
  } catch { /* the store is a convenience: anything wrong with it is a miss */ }

  if (!bytes) {
    try {
      const r = await fetch(here('./' + entry.file));
      const b = r.ok ? new Uint8Array(await r.arrayBuffer()) : null;
      if (!b || !(await verifyChunk(entry, b, sc))) {
        // ★ This server's file is not the one its page describes (an update mid-load?) — run nothing
        //   we checked, store nothing; the plain import is exactly what the page did before.
        if (b) console.warn(`[chunk cache] "${name}" from this server does not match its page — not cached`);
        return fallback();
      }
      bytes = b;
      // ★ Saved for every other server. Not awaited: the map must not wait on the store.
      const copy = b.slice().buffer;
      void storeRequest({ op: 'chunkPut', sha: entry.sha256, name, bytes: copy }, 5000, [copy]);
    } catch { return fallback(); }
  }

  try {
    return await runVerified<T>(entry, bytes);
  } catch (e) {
    console.warn(`[chunk cache] "${name}" would not run from the cache; loading it as before`, e);
    return fallback();
  }
}

/** Run bytes that have ALREADY passed verifyChunk, as a module from a blob: URL (CSP allows blob:). */
async function runVerified<T>(entry: ChunkEntry, bytes: Uint8Array): Promise<T> {
  const code = rewriteImports(bytes, entry.rewrite, here);
  const url = URL.createObjectURL(new Blob([code as BlobPart], { type: 'text/javascript' }));
  try {
    return await import(/* @vite-ignore */ url) as T;
  } finally {
    URL.revokeObjectURL(url);
  }
}

/** The shared map renderer's source (VIBEMAP_JS) — main.ts's map window and the admin page's map. */
export function loadVibemapSource(): Promise<typeof import('./generated/vibemapSource')> {
  return cachedImport('vibemap', () => import('./generated/vibemapSource'));
}

/** The admin panel's code (adminLazy.ts). */
export function loadAdminModule(): Promise<typeof import('./admin')> {
  return cachedImport('admin', () => import('./admin'));
}
