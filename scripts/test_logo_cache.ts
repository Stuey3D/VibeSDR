// The station-logo cache's policy (src/services/logoCachePolicy.ts) — one copy for the app and the web client.
// Priority, retry backoff, replace-only-on-change, the LRU bound, and the air logo replacing (and being replaced).
import { LogoIndex, verdict, outranked, evictions, retryDelay, dabLogoKey, fmLogoKey, nameLogoKey,
         REFRESH_MS, AIR_STALE_MS, type LogoEntry } from '../src/services/logoCachePolicy.ts';
let pass = 0, fail = 0;
const eq = (what: string, got: unknown, want: unknown) => { const ok = JSON.stringify(got) === JSON.stringify(want); ok ? pass++ : fail++; if (!ok) console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`); };

const MIN = 60 * 1000, H = 60 * MIN, D = 24 * H;
const T0 = 1_800_000_000_000;
const ent = (src: LogoEntry['src'], url: string, sig = '', at = T0, bytes = 1000): LogoEntry =>
  ({ url, src, sig, show: `file:///${url.length}-${sig}`, bytes, savedAt: at, checkedAt: at, usedAt: at, ...(src === 'air' ? { airAt: at } : {}) });

// ── keys ──
eq('DAB key: hex identity', dabLogoKey(0xE1, 0xC1CE, 0xC4D2), 'dab:E1|C1CE|C4D2');
eq('DAB key: unknown ECC kept as "-"', dabLogoKey(-1, 0xC1CE, 0xC4D2), 'dab:-|C1CE|C4D2');
eq('FM key: country + PI + 10 kHz bearer', fmLogoKey('c203', 'gb', 96_600_000), 'fm:GB|C203|9660');
eq('FM key: no frequency', fmLogoKey('C203', 'GB'), 'fm:GB|C203|');
eq('name key: case-folded', nameLogoKey(' Heart London ', 'gb'), 'nm:GB|heart london');

// ── priority ──
eq('nothing cached: store anything', verdict(undefined, { src: 'name', url: 'a' }, T0), 'store');
eq('RadioDNS replaces a name match', verdict(ent('name', 'a'), { src: 'radiodns', url: 'b' }, T0), 'store');
eq('a name match never replaces RadioDNS', verdict(ent('radiodns', 'a'), { src: 'name', url: 'b' }, T0), 'keep');
eq('air replaces RadioDNS ("the ensemble takes over")', verdict(ent('radiodns', 'a', 'x'), { src: 'air', url: 'air', sig: 'y' }, T0), 'store');
eq('RadioDNS never replaces a fresh air logo', verdict(ent('air', 'air', 'y'), { src: 'radiodns', url: 'b' }, T0 + 29 * D), 'keep');
eq('…but may once the multiplex has not carried it for a month', verdict(ent('air', 'air', 'y'), { src: 'radiodns', url: 'b' }, T0 + AIR_STALE_MS + 1), 'store');
eq('a slide never replaces even a stale air logo', verdict(ent('air', 'air', 'y'), { src: 'slide', url: 's' }, T0 + AIR_STALE_MS + 1), 'keep');
eq('a slide is replaced by anything better', verdict(ent('slide', 's'), { src: 'name', url: 'n' }, T0), 'store');
eq('outranked: name under RadioDNS', outranked(ent('radiodns', 'a'), 'name', T0), true);
eq('outranked: nothing over an empty slot', outranked(undefined, 'slide', T0), false);

// ── replace only on change ──
eq('same URL, no signatures: same', verdict(ent('radiodns', 'u'), { src: 'radiodns', url: 'u' }, T0), 'same');
eq('new URL, no signatures: store', verdict(ent('radiodns', 'u'), { src: 'radiodns', url: 'v' }, T0), 'store');
eq('same URL, NEW bytes (replaced in place): store', verdict(ent('radiodns', 'u', 'aaa'), { src: 'radiodns', url: 'u', sig: 'bbb' }, T0), 'store');
eq('new URL, SAME bytes: same', verdict(ent('radiodns', 'u', 'aaa'), { src: 'radiodns', url: 'v', sig: 'aaa' }, T0), 'same');
eq('air logo, same bytes: same', verdict(ent('air', 'air', 'h1'), { src: 'air', url: 'air', sig: 'h1' }, T0), 'same');
eq('air logo, the ensemble sent a different one: store', verdict(ent('air', 'air', 'h1'), { src: 'air', url: 'air', sig: 'h2' }, T0), 'store');

// ── backoff ──
eq('retry ladder 1 min, 5 min, 30 min, then daily', [1, 2, 3, 4, 9].map(retryDelay), [MIN, 5 * MIN, 30 * MIN, D, D]);
{
  const ix = new LogoIndex();
  const k = 'dab:E1|C1CE|C4D2';
  eq('an unknown key may be asked', ix.shouldAsk(k, T0), true);
  eq('claimed once', [ix.begin(k), ix.begin(k)], [true, false]);
  eq('not asked again while asking (the list renders every second)', ix.shouldAsk(k, T0), false);
  ix.miss(k, T0); ix.end(k);
  eq('★★★ a failed fetch is NOT permanent: refused for a minute…', ix.shouldAsk(k, T0 + MIN - 1), false);
  eq('…then asked again', ix.shouldAsk(k, T0 + MIN), true);
  ix.miss(k, T0 + MIN);
  eq('second failure: 5 minutes', [ix.shouldAsk(k, T0 + MIN + 5 * MIN - 1), ix.shouldAsk(k, T0 + 6 * MIN)], [false, true]);
  ix.miss(k, T0 + 6 * MIN); ix.miss(k, T0 + 36 * MIN);
  eq('fourth failure: a day', [ix.shouldAsk(k, T0 + 36 * MIN + D - 1), ix.shouldAsk(k, T0 + 36 * MIN + D)], [false, true]);
  ix.store(k, { url: 'u', src: 'radiodns', sig: 's', show: 'f', bytes: 10 }, T0 + 2 * D);
  eq('an answer resets the backoff', ix.missOf(k), undefined);
  eq('held: not re-asked inside a day', ix.shouldAsk(k, T0 + 2 * D + REFRESH_MS - 1), false);
  eq('held: refreshed after a day', ix.shouldAsk(k, T0 + 2 * D + REFRESH_MS), true);
  ix.miss(k, T0 + 3 * D);
  eq('★ a failed refresh keeps the cached logo', ix.get(k)?.show, 'f');
  ix.same(k, T0 + 3 * D + MIN);
  eq('an unchanged answer only stamps checkedAt (savedAt unmoved)', [ix.get(k)?.savedAt, ix.get(k)?.checkedAt], [T0 + 2 * D, T0 + 3 * D + MIN]);
  const dead = ix.drop(k, T0 + 4 * D);
  eq('a dead picture is forgotten and backed off, not blanked for ever', [dead?.show, ix.get(k), ix.shouldAsk(k, T0 + 4 * D + MIN)], ['f', undefined, true]);
}

// ── air logo replacement in the index ──
{
  const ix = new LogoIndex();
  const k = dabLogoKey(0xE1, 0xC1CE, 0xC4D2);
  ix.store(k, { url: 'https://x/rdns.png', src: 'radiodns', sig: 'r', show: 'file:///r', bytes: 10 }, T0);
  eq('air vs RadioDNS → store', ix.verdict(k, { src: 'air', url: '/vibeserver/dablogoair?sid=C4D2', sig: 'a1' }, T0 + 1), 'store');
  const r1 = ix.store(k, { url: '/vibeserver/dablogoair?sid=C4D2', src: 'air', sig: 'a1', show: 'file:///a1', bytes: 10 }, T0 + 1);
  eq('the replaced file is handed back for deletion', r1.replaced?.show, 'file:///r');
  eq('air entry stamped seen', ix.get(k)?.airAt, T0 + 1);
  eq('the same air logo next session: same', ix.verdict(k, { src: 'air', url: '/vibeserver/dablogoair?sid=C4D2', sig: 'a1' }, T0 + D), 'same');
  ix.same(k, T0 + D, { src: 'air' });
  eq('…and the air sighting moves', ix.get(k)?.airAt, T0 + D);
  eq('a new air logo replaces the old', ix.verdict(k, { src: 'air', url: '/vibeserver/dablogoair?sid=C4D2', sig: 'a2' }, T0 + 2 * D), 'store');
}

// ── LRU bound ──
{
  const es: Record<string, LogoEntry> = {};
  for (let i = 0; i < 5; i++) es[`k${i}`] = { ...ent('name', `u${i}`, '', T0, 100), usedAt: T0 + i };
  eq('within limits: nothing evicted', evictions(es, { maxEntries: 5, maxBytes: 1000 }), []);
  eq('too many entries: oldest used goes first', evictions(es, { maxEntries: 3, maxBytes: 1e9 }), ['k0', 'k1']);
  eq('too many bytes: oldest go until it fits', evictions(es, { maxEntries: 99, maxBytes: 250 }), ['k0', 'k1', 'k2']);
  es.k0.usedAt = T0 + 99;
  eq('a touch protects it', evictions(es, { maxEntries: 4, maxBytes: 1e9 }), ['k1']);
  eq('the just-stored key is never evicted', evictions(es, { maxEntries: 1, maxBytes: 1e9 }, 'k1'), ['k2', 'k3', 'k4', 'k0']);
  const ix = new LogoIndex({}, { maxEntries: 2, maxBytes: 1e9 });
  ix.store('a', { url: 'a', src: 'name', sig: '', show: 'fa', bytes: 1 }, T0);
  ix.store('b', { url: 'b', src: 'name', sig: '', show: 'fb', bytes: 1 }, T0 + 1);
  ix.touch('a', T0 + 2);
  const r = ix.store('c', { url: 'c', src: 'name', sig: '', show: 'fc', bytes: 1 }, T0 + 3);
  eq('index store evicts the least recently used and hands its file back', [r.evicted.map(e => e.show), Object.keys(ix.entries).sort()], [['fb'], ['a', 'c']]);
  eq('touch reports "worth saving" only after an hour', [ix.touch('a', T0 + 4), ix.touch('a', T0 + 4 + 2 * H)], [false, true]);
}

// ── a stall at a higher source does not let a lower answer settle for a day ──
{
  const ix = new LogoIndex();
  const k = 'dab:E1|C1CE|C4D2';
  ix.begin(k);
  ix.store(k, { url: 'n', src: 'name', sig: 'n1', show: 'fn', bytes: 1 }, T0);   // the name search answered…
  ix.failedAbove(k, 'radiodns', T0);                                            // …RadioDNS could not be asked
  ix.end(k);
  eq('the name logo is drawn', ix.get(k)?.show, 'fn');
  eq('★★ RadioDNS is retried in a minute, not a day', [ix.shouldAsk(k, T0 + MIN - 1), ix.shouldAsk(k, T0 + MIN)], [false, true]);
  ix.confirm(k, T0 + MIN);                                                      // only the name answered again
  ix.failedAbove(k, 'radiodns', T0 + MIN);
  eq('…then five minutes (the ladder climbs while it keeps stalling)', ix.shouldAsk(k, T0 + 6 * MIN), true);
  ix.same(k, T0 + 6 * MIN, { src: 'name' });
  eq('a lower source answering does not clear the pending upgrade', ix.upgradeOf(k)?.src, 'radiodns');
  ix.store(k, { url: 'r', src: 'radiodns', sig: 'r1', show: 'fr', bytes: 1 }, T0 + 7 * MIN);
  eq('RadioDNS answering clears it — back to daily', [ix.upgradeOf(k), ix.shouldAsk(k, T0 + 7 * MIN + REFRESH_MS - 1)], [undefined, false]);
}

console.log(`logo cache: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
