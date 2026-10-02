/**
 * spriteCache — ONE bounded cache for every glow sprite in the app, shared by every component that draws
 * the same glyph at the same size and colour. Pure (no React Native, no Skia) so scripts/test_sprite_cache.ts
 * can hold the rules down with fake images.
 *
 * ★★★ WHY (audit, 2026-10-02, Stuart's Mac at ~1 GB): every component instance built its OWN sprite set in a
 *   useMemo, per colour × size, and nothing was ever disposed — the Skia handles were freed only when Hermes
 *   happened to collect them, and Hermes feels no pressure from native memory. `footprint` showed 370–460 MB of
 *   graphics in 700–970 regions. Sprites are immutable images, so one copy per (glyph, size, colour) is enough
 *   for every instance on screen, and an evicted copy can be disposed at once — if nobody is drawing it.
 *
 * ★★ AN IMAGE IN USE IS NEVER DISPOSED. A mounted component RETAINS its key (useSharedSprite's effect) and
 *   releases it on unmount or key change; eviction takes only entries with no retainers. The gap between a
 *   render that built an entry and the effect that retains it is covered by `graceMs`: nothing touched within
 *   it is evicted. Over budget with nothing evictable, the cache simply runs over until something is released.
 */

/** What a builder hands the cache: the value to share, its size, and how to free it. */
export interface Built<T> { value: T; bytes: number; dispose: () => void }

interface Entry<T> extends Built<T> { refs: number; used: number }

export class SpriteCache<T> {
  private map = new Map<string, Entry<T>>();
  private total = 0;
  private readonly maxBytes: number;
  private readonly graceMs: number;
  private readonly now: () => number;

  // ★ Plain fields, not constructor parameter properties: node's type stripping (the test runner) refuses those.
  constructor(maxBytes: number, graceMs: number, now: () => number) {
    this.maxBytes = maxBytes; this.graceMs = graceMs; this.now = now;
  }

  /** The shared value for `key`, built once by `make`. null (and nothing cached) when make fails. */
  get(key: string, make: () => Built<T> | null): T | null {
    const hit = this.map.get(key);
    if (hit) {
      // LRU: move to the end (Map keeps insertion order).
      this.map.delete(key);
      hit.used = this.now();
      this.map.set(key, hit);
      return hit.value;
    }
    const b = make();
    if (!b) return null;   // ★ never cache a failed surface — the next render tries again
    this.map.set(key, { ...b, refs: 0, used: this.now() });
    this.total += b.bytes;
    this.trim();
    return b.value;
  }

  /** A mounted user of `key`. Unknown keys are ignored (the entry failed to build). */
  retain(key: string): void {
    const e = this.map.get(key);
    if (e) { e.refs++; e.used = this.now(); }
  }

  /** That user has gone. The entry stays cached (LRU) until the budget needs its room. */
  release(key: string): void {
    const e = this.map.get(key);
    if (e && e.refs > 0) { e.refs--; e.used = this.now(); }
    this.trim();
  }

  /** Evict least-recently-used, unretained, out-of-grace entries until under budget; dispose each. */
  trim(): void {
    if (this.total <= this.maxBytes) return;
    const t = this.now();
    for (const [k, e] of this.map) {
      if (this.total <= this.maxBytes) break;
      if (e.refs > 0 || t - e.used < this.graceMs) continue;
      this.map.delete(k);
      this.total -= e.bytes;
      try { e.dispose(); } catch { /* ★ a dispose that throws must not stop the trim */ }
    }
  }

  /** For tests and diagnostics. */
  stats(): { entries: number; bytes: number; retained: number } {
    let retained = 0;
    for (const e of this.map.values()) if (e.refs > 0) retained++;
    return { entries: this.map.size, bytes: this.total, retained };
  }
}
