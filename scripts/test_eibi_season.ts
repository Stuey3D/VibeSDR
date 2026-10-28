/**
 * The EiBi season pick and its fallback in the app (src/services/eibiSeason.ts).
 *
 * ★★★ NickB, 2026-10-27: "curl: (22) The requested URL returned error: 404" — we switched to the
 *     winter file on 25 October and EiBi had not published sked-b26.csv yet. The app must use
 *     sked-a26.csv, say so in words, ask for B26 again every few hours (not on every call), and
 *     report a real network failure as one. Same scenarios as vibeserver/test-eibi.cpp.
 *
 * Run: node --no-warnings scripts/test_eibi_season.ts   (run-tests.sh does)
 */
import {
  seasonFor, previousSeason, seasonLabel, shouldFetch, refreshWithFallback, noteFor, utcDate,
  RETRY_UNPUBLISHED_MS, type EibiState, type FetchOutcome,
} from '../src/services/eibiSeason.ts';

let fails = 0, passes = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { passes++; return; }
  fails++; console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`);
};
const at = (y: number, m: number, d: number, h = 12) => new Date(Date.UTC(y, m - 1, d, h));

// ── Season arithmetic ──
eq('27 Oct 2026 is B26', seasonFor(at(2026, 10, 27)), 'b26');
eq('24 Oct 2026 is A26', seasonFor(at(2026, 10, 24)), 'a26');
eq('15 Jan 2027 is still B26', seasonFor(at(2027, 1, 15)), 'b26');
eq('24 Mar 2027 is still B26 (the old "m <= 2" asked for b27)', seasonFor(at(2027, 3, 24)), 'b26');
eq('25 Mar 2027 is A27', seasonFor(at(2027, 3, 25)), 'a27');
eq('1 Jan 2100 is B99', seasonFor(at(2100, 1, 1)), 'b99');
eq('before b26 is a26', previousSeason('b26'), 'a26');
eq('before a27 is b26', previousSeason('a27'), 'b26');
eq('before a00 is b99', previousSeason('a00'), 'b99');
eq('label b26', seasonLabel('b26'), 'B26 (winter)');

// ── A fake eibispace.de ──
let site: Record<string, number> = {};
let down = false;
let asked: string[] = [];
const fetchSeason = async (code: string): Promise<FetchOutcome> => {
  asked.push(code);
  if (down) return { kind: 'failed', message: 'Network request failed' };
  const n = site[code];
  return n ? { kind: 'ok', csv: `csv-${code}`, count: n } : { kind: 'unpublished' };
};
const fresh = (): EibiState => ({ season: '', date: '', triedSeason: '', triedAt: 0 });

const NOTE = "EiBi hasn't published the B26 (winter) schedule yet — using A26 until it appears.";

async function main() {
  // B26 not published, nothing cached: falls back to A26 and says so.
  let now = at(2026, 10, 27, 21);
  let s = fresh();
  site = { a26: 3 }; asked = [];
  eq('no cache: fetch', shouldFetch(s, now), true);
  let r = await refreshWithFallback(s, now, fetchSeason);
  eq('fell back to A26', r, { kind: 'ok', season: 'a26', csv: 'csv-a26', count: 3, note: NOTE });
  eq('asked B26 then A26', asked, ['b26', 'a26']);
  s.season = 'a26'; s.date = utcDate(now);
  eq('note in words', noteFor(s, now), NOTE);

  // Not on every call — every few hours.
  eq('1 h later: no fetch', shouldFetch(s, new Date(now.getTime() + 3600e3)), false);
  eq('just under the retry: no fetch', shouldFetch(s, new Date(now.getTime() + RETRY_UNPUBLISHED_MS - 1)), false);
  eq('after the retry: fetch', shouldFetch(s, new Date(now.getTime() + RETRY_UNPUBLISHED_MS)), true);

  // Still not there, same day: keeps the A26 it holds, only asks for B26.
  asked = [];
  now = new Date(now.getTime() + 1000);  // same UTC day
  r = await refreshWithFallback(s, now, fetchSeason);
  eq('keeps A26', r, { kind: 'keep', note: NOTE });
  eq('only B26 asked', asked, ['b26']);

  // B26 appears: switches, note gone.
  site.b26 = 5; asked = [];
  now = at(2026, 10, 29);
  r = await refreshWithFallback(s, now, fetchSeason);
  eq('switched to B26', r, { kind: 'ok', season: 'b26', csv: 'csv-b26', count: 5, note: '' });
  s.season = 'b26'; s.date = utcDate(now);
  eq('tried marker cleared', [s.triedSeason, s.triedAt], ['', 0]);
  eq('no note once current', noteFor(s, now), '');
  eq('current, same day: no fetch', shouldFetch(s, now), false);
  eq('current, next day: fetch', shouldFetch(s, at(2026, 10, 30)), true);

  // A real network failure is reported as one.
  down = true;
  r = await refreshWithFallback(s, at(2026, 10, 30), fetchSeason);
  eq('network failure', r, { kind: 'failed', error: "couldn't reach eibispace.de — Network request failed" });
  down = false;

  // Unpublished, and the fallback fails, nothing cached: an honest error naming both.
  s = fresh(); asked = [];
  let n = 0;
  r = await refreshWithFallback(s, at(2026, 10, 27), async (c) => (++n === 1 ? { kind: 'unpublished' } : { kind: 'failed', message: 'timeout' }));
  eq('both fail, no cache', r.kind === 'failed' && r.error.includes('B26 (winter)') && r.error.includes("couldn't reach eibispace.de — timeout"), true);

  // …the same with a cache in hand: keep it.
  s = { season: 'a26', date: '2026-10-20', triedSeason: '', triedAt: 0 }; n = 0;
  r = await refreshWithFallback(s, at(2026, 10, 27), async () => (++n === 1 ? { kind: 'unpublished' } : { kind: 'failed', message: 'timeout' }));
  eq('both fail, stale A26 cached: keep it', r, { kind: 'keep', note: NOTE });

  // March: A27 unpublished → B26.
  s = fresh(); site = { b26: 6 }; asked = [];
  r = await refreshWithFallback(s, at(2027, 3, 26), fetchSeason);
  eq('A27 → B26', r.kind === 'ok' && r.season === 'b26' && r.note
     === "EiBi hasn't published the A27 (summer) schedule yet — using B26 until it appears.", true);

  // January asks for B26 (the year it started), not B27.
  s = fresh(); asked = [];
  await refreshWithFallback(s, at(2027, 1, 10), fetchSeason);
  eq('January asks for B26', asked, ['b26']);

  console.log(fails ? `${fails} FAILED, ${passes} passed` : `eibi season: all ${passes} passed`);
  process.exit(fails ? 1 : 0);
}
main();
