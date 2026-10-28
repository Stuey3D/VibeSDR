/**
 * eibiSeason.ts — which EiBi season file to ask for, and what to do when EiBi has not published it.
 *
 * ★★★ NickB, 2026-10-27: "fetching the EiBi schedule seems to be broken this evening — curl: (22)
 *     The requested URL returned error: 404". Our season switch is a DATE GUESS (25 March, 25
 *     October); EiBi publishes the new file when it is ready, which can be days or weeks later.
 *     sked-b26.csv was still a 404 two days after we switched to it, and the app — which only
 *     trusted a cache from the CURRENT season — had no schedule at all and re-fetched on every call.
 *     Now: ask for this season; if it is not there, use the PREVIOUS season's file, say so in
 *     words, and ask for the new one again every few hours until it appears.
 *
 * ★ The same rule is in vibeserver/eibi.cpp (server) and vibeserver/mac/EibiStations.swift (Mac
 *   app) — keep all three in step. Pure (no React Native imports) so it runs under plain node:
 *   scripts/test_eibi_season.ts.
 */

/** "a26" / "b26" for `d`. A = summer (~last Sun Mar → ~last Sun Oct), B = winter. EiBi labels a
 *  season by the year it STARTED, so January to 24 March belong to the previous year's B. */
export function seasonFor(d: Date): string {
  const m = d.getUTCMonth() + 1, day = d.getUTCDate(), y = d.getUTCFullYear();
  let summer: boolean;
  if (m > 3 && m < 10) summer = true;
  else if (m < 3 || m > 10) summer = false;
  else if (m === 3) summer = day >= 25;   // ~last Sunday of March
  else /* m === 10 */ summer = day < 25;  // ~last Sunday of October
  // ★★ m <= 3, NOT m <= 2: 1–24 March is still the winter that started the previous October
  //    ("<= 2" asked for sked-b27.csv on 1 March 2027 — a file that will never exist).
  const yy = !summer && m <= 3 ? y - 1 : y;
  return (summer ? 'a' : 'b') + String(((yy % 100) + 100) % 100).padStart(2, '0');
}

/** The season before `code`: b26 → a26, a27 → b26. */
export function previousSeason(code: string): string {
  const yy = parseInt(code.slice(1), 10);
  if (code[0] === 'b') return 'a' + String(yy).padStart(2, '0');
  return 'b' + String((yy + 99) % 100).padStart(2, '0');
}

export const seasonFile  = (code: string) => `sked-${code}.csv`;
export const seasonUrl   = (code: string) => `http://www.eibispace.de/dx/${seasonFile(code)}`;
/** "B26 (winter)" / "A26 (summer)". */
export const seasonLabel = (code: string) =>
  code.toUpperCase() + (code[0] === 'a' ? ' (summer)' : ' (winter)');

/** How long after EiBi said "not yet" before asking again. */
export const RETRY_UNPUBLISHED_MS = 6 * 3600 * 1000;

/** The plain sentence for the user when the schedule in use is not this season's. */
export function notPublishedNote(current: string, using: string): string {
  return `EiBi hasn't published the ${seasonLabel(current)} schedule yet — using ${using.toUpperCase()} until it appears.`;
}

/** What we hold: the season of the cached schedule, the UTC day it was fetched, and the season EiBi
 *  last said it has not published (with when). */
export interface EibiState {
  season: string;        // '' = nothing cached
  date: string;          // YYYY-MM-DD it was fetched
  triedSeason: string;   // '' = none outstanding
  triedAt: number;       // ms since epoch
}

export const utcDate = (d: Date) => d.toISOString().slice(0, 10);

/** Should we go to the network now? */
export function shouldFetch(s: EibiState, now: Date): boolean {
  if (!s.season) return true;
  const cur = seasonFor(now);
  if (s.season === cur) return s.date !== utcDate(now);              // daily
  // ★★ Not this season's file. Ask — unless EiBi said "not yet" for this very season recently.
  const t = now.getTime();
  if (s.triedSeason === cur && t >= s.triedAt && t - s.triedAt < RETRY_UNPUBLISHED_MS) return false;
  return true;
}

/** Ok = a file with entries (`csv`, `count`); unpublished = 404/410 or nothing parseable; failed =
 *  the network or the server is broken (`message`). */
export type FetchOutcome =
  | { kind: 'ok'; csv: string; count: number }
  | { kind: 'unpublished' }
  | { kind: 'failed'; message: string };

export type RefreshResult =
  | { kind: 'ok'; season: string; csv: string; count: number; note: string }
  | { kind: 'keep'; note: string }        // keep the cache we hold (fresh fallback, or nothing better)
  | { kind: 'failed'; error: string };

/**
 * Fetch this season, falling back to the previous one. `state` is updated in place (tried marker).
 * ★ A real network failure is reported as one; an unpublished season is not an error at all.
 */
export async function refreshWithFallback(
  state: EibiState, now: Date, fetchSeason: (code: string) => Promise<FetchOutcome>,
): Promise<RefreshResult> {
  const cur = seasonFor(now), prev = previousSeason(cur);
  const r = await fetchSeason(cur);
  if (r.kind === 'ok') {
    state.triedSeason = ''; state.triedAt = 0;
    return { kind: 'ok', season: cur, csv: r.csv, count: r.count, note: '' };
  }
  if (r.kind === 'failed') return { kind: 'failed', error: `couldn't reach eibispace.de — ${r.message}` };

  state.triedSeason = cur; state.triedAt = now.getTime();
  // Already holding today's copy of the previous season: nothing more to fetch.
  if (state.season === prev && state.date === utcDate(now)) return { kind: 'keep', note: notPublishedNote(cur, prev) };
  const p = await fetchSeason(prev);
  if (p.kind === 'ok') return { kind: 'ok', season: prev, csv: p.csv, count: p.count, note: notPublishedNote(cur, prev) };
  if (state.season) return { kind: 'keep', note: notPublishedNote(cur, state.season) };
  return {
    kind: 'failed',
    error: `EiBi hasn't published the ${seasonLabel(cur)} schedule yet, and the ${seasonLabel(prev)} one `
         + (p.kind === 'failed' ? `could not be fetched — couldn't reach eibispace.de — ${p.message}`
                                : 'has no entries either'),
  };
}

/** The note for what is held now ('' when it is this season's, or nobody has asked yet). */
export function noteFor(s: EibiState, now: Date): string {
  const cur = seasonFor(now);
  if (!s.season || s.season === cur || s.triedSeason !== cur) return '';
  return notPublishedNote(cur, s.season);
}
