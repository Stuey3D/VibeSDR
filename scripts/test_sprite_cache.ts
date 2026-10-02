/**
 * The shared glow-sprite cache (src/constants/spriteCache.ts) — the fix for the never-disposed sprites that
 * held 370–460 MB of graphics on Stuart's Mac (audit, 2026-10-02).
 *
 * Proves: one build per key however many users; a retained entry is never disposed; eviction is LRU, only
 * of unretained entries past the grace period, and DISPOSES what it evicts; a failed build is not cached;
 * over budget with everything retained, nothing is disposed (the cache runs over rather than break a draw).
 *
 * Run: node --no-warnings scripts/test_sprite_cache.ts   (run-tests.sh does)
 */
import { SpriteCache } from '../src/constants/spriteCache.ts';

let fails = 0, passes = 0;
const ok = (what: string, cond: boolean, detail = '') => {
  if (cond) { passes++; return; }
  fails++; console.error(`FAIL ${what}${detail ? `\n   ${detail}` : ''}`);
};

let t = 0;
const disposed: string[] = [];
const built: string[] = [];
const img = (name: string, bytes: number) => () => {
  built.push(name);
  return { value: name, bytes, dispose: () => disposed.push(name) };
};

const c = new SpriteCache<string>(100, 1000, () => t);

// One build per key.
ok('first get builds', c.get('a', img('a', 40)) === 'a' && built.length === 1);
ok('second get shares', c.get('a', img('a', 40)) === 'a' && built.length === 1);

// A failed build is not cached.
ok('failed build → null', c.get('bad', () => null) === null);
ok('…and is retried next time', c.get('bad', img('bad', 1)) === 'bad');

// Retained entries survive any pressure.
c.retain('a');
t = 5000;
c.get('b', img('b', 40));
c.get('c', img('c', 40));        // 40 + 1 + 40 + 40 = 121 > 100, but 'b','c' are in grace, 'a' retained
ok('retained "a" not disposed', !disposed.includes('a'));
ok('in-grace entries not disposed', !disposed.includes('b') && !disposed.includes('c'));
ok('runs over budget rather than break a draw', c.stats().bytes > 100, JSON.stringify(c.stats()));

// Out of grace and unretained → evicted LRU-first and disposed.
t = 10000;
c.trim();
ok('LRU unretained evicted and disposed ("bad" first)', disposed[0] === 'bad', disposed.join(','));
ok('under budget after trim', c.stats().bytes <= 100, JSON.stringify(c.stats()));
ok('retained "a" still cached', c.get('a', img('a', 40)) === 'a' && built.filter(n => n === 'a').length === 1);

// Release → becomes evictable.
c.release('a');
t = 20000;
c.get('d', img('d', 90));
t = 30000;
c.trim();
ok('released "a" evicted and disposed once out of grace', disposed.includes('a'), disposed.join(','));
ok('a re-get rebuilds it', c.get('a', img('a', 40)) === 'a' && built.filter(n => n === 'a').length === 2);

// A dispose that throws does not stop the trim.
const c2 = new SpriteCache<string>(10, 0, () => t);
c2.get('x', () => ({ value: 'x', bytes: 8, dispose: () => { throw new Error('boom'); } }));
c2.get('y', img('y', 8));
c2.trim();
ok('throwing dispose survived, still trims', c2.stats().bytes <= 10, JSON.stringify(c2.stats()));

console.log(`sprite cache: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
