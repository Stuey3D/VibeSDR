/**
 * chunkVerify.ts — the PURE half of the shared chunk cache (chunkCache.ts): what a stored copy must
 * prove before a byte of it may run. No imports, no DOM, so a node test can drive every rule here
 * exactly as the page does (scripts/test_chunk_cache.ts).
 *
 * ★★★ THE RULE (Stuart, 2026-10-01: "security checks to make sure everything is correct and that
 *     nobody has hosted a virus-ridden server to send malware via our maps"). The shared store is
 *     writable by EVERY *.vibeserver.vibesdr.net page — including one a stranger runs. So nothing in
 *     it is trusted, ever. A page runs a stored chunk only if the SHA-256 of its exact bytes equals
 *     the hash THIS SERVER's own page carries (build-web.mjs computes it from the file it serves).
 *     Same bytes as this server would have sent, or not used at all.
 *  ★ The hash is asked FOR, never offered: the page looks up its own hash; the store's answer cannot
 *    choose which version gets run. That also makes the hash the version check — a release that
 *    changes the map carries a new hash, so the old copy simply is not the one asked for.
 */

/** One cacheable chunk, as the page carries it (the `vs-chunks` JSON in the HTML). */
export interface ChunkEntry {
  /** The chunk's file under /vs/ — `c-<HASH>.js`. */
  file: string;
  /** Lower-case hex SHA-256 of the file's exact bytes, as served. */
  sha256: string;
  /** Its length in bytes — checked first, so an obviously wrong copy costs no hashing. */
  size: number;
  /** The relative specifiers inside it (`./c-<HASH>.js`) that must become absolute before it can run
   *  from a blob: URL, which has no directory to resolve them against. */
  rewrite: string[];
}
export type ChunkManifest = Record<string, ChunkEntry>;

const FILE_RE = /^c-[A-Z0-9]+\.js$/;
const SPEC_RE = /^\.\/c-[A-Z0-9]+\.js$/;
const SHA_RE = /^[0-9a-f]{64}$/;
const NAME_RE = /^[a-z0-9-]{1,32}$/;

/** The page's manifest, or null if it is missing or malformed in ANY way (then nothing is cached). */
export function parseManifest(text: string): ChunkManifest | null {
  let raw: unknown;
  try { raw = JSON.parse(text); } catch { return null; }
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  const out: ChunkManifest = {};
  for (const [name, v] of Object.entries(raw as Record<string, any>)) {
    if (!NAME_RE.test(name) || !v || typeof v !== 'object') return null;
    const { file, sha256, size, rewrite } = v;
    if (typeof file !== 'string' || !FILE_RE.test(file)) return null;
    if (typeof sha256 !== 'string' || !SHA_RE.test(sha256)) return null;
    if (!Number.isInteger(size) || size <= 0) return null;
    if (!Array.isArray(rewrite) || !rewrite.every((s) => typeof s === 'string' && SPEC_RE.test(s))) return null;
    out[name] = { file, sha256, size, rewrite: [...rewrite] };
  }
  return out;
}

export function toHex(buf: ArrayBuffer): string {
  const b = new Uint8Array(buf);
  let s = '';
  for (let i = 0; i < b.length; i++) s += b[i].toString(16).padStart(2, '0');
  return s;
}

/** Minimal shape of SubtleCrypto this needs — node's webcrypto and the browser's both fit. */
export interface Digester { digest(alg: 'SHA-256', data: BufferSource): Promise<ArrayBuffer> }

/** ★★★ THE GATE. True only if `bytes` are exactly the chunk this page expects. */
export async function verifyChunk(entry: Pick<ChunkEntry, 'sha256' | 'size'>, bytes: Uint8Array | null | undefined,
                                  subtle: Digester): Promise<boolean> {
  if (!bytes || !(bytes instanceof Uint8Array)) return false;
  if (!SHA_RE.test(entry.sha256)) return false;
  if (bytes.length !== entry.size) return false;
  const got = toHex(await subtle.digest('SHA-256', bytes as BufferSource));
  return got === entry.sha256;
}

/**
 * The verified bytes with each relative chunk specifier `"./c-X.js"` replaced by the absolute URL the
 * page's own modules were loaded from — so the chunk's imports reach the SAME module instances the
 * page already has (shared state such as the admin ticket stays one copy).
 * ★ Byte-level, on purpose: the bytes are not decoded and re-encoded (a chunk may carry raw binary in
 *   a string literal). Only the exact quoted specifiers the PAGE lists are touched, and each must be
 *   present — a mismatch means this is not the chunk the build described, so it throws.
 * ★ `resolve` is the page's own (import.meta.url based), never anything from the store.
 */
export function rewriteImports(bytes: Uint8Array, specs: string[], resolve: (spec: string) => string): Uint8Array {
  let cur = bytes;
  for (const spec of specs) {
    if (!SPEC_RE.test(spec)) throw new Error(`bad specifier ${spec}`);
    const needle = ascii(JSON.stringify(spec));
    const repl = ascii(JSON.stringify(resolve(spec)));
    const parts: Uint8Array[] = [];
    let from = 0, hits = 0;
    for (let i = indexOf(cur, needle, 0); i >= 0; i = indexOf(cur, needle, i + needle.length)) {
      parts.push(cur.subarray(from, i), repl);
      from = i + needle.length;
      hits++;
    }
    if (!hits) throw new Error(`specifier ${spec} not found in the chunk`);
    parts.push(cur.subarray(from));
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    cur = out;
  }
  return cur;
}

function ascii(s: string): Uint8Array {
  const u = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c > 0x7e) throw new Error('non-ASCII URL');
    u[i] = c;
  }
  return u;
}

function indexOf(hay: Uint8Array, needle: Uint8Array, from: number): number {
  const n = needle.length, first = needle[0];
  outer: for (let i = hay.indexOf(first, from); i >= 0 && i + n <= hay.length; i = hay.indexOf(first, i + 1)) {
    for (let j = 1; j < n; j++) if (hay[i + j] !== needle[j]) continue outer;
    return i;
  }
  return -1;
}
