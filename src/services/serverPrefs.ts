/**
 * ★★★ THE SERVER SCREEN'S SETTINGS, READ IN ONE GO (2026-10-06).
 *
 * Stuart, 2026-10-06: "even slow EMMC shouldn't take an eternity to load a small config". It was never
 * the storage. VibeServer Lite runs React Native 0.73 on the OLD architecture, whose renderer has no
 * automatic batching: every setState that lands after an `await` re-renders the whole server screen
 * (3,500 lines of it) synchronously, there and then. The load read its ~70 settings one `getItem` at a
 * time and set each as it arrived — MEASURED on the emulator harness: 77 full renders to show one
 * screen of settings. On the Sony (whose FIRST render alone took ~4 s) that is a minute or more, and
 * every timer on the screen (the DAB scan's seconds counter among them) queued behind it.
 *
 * So the screen asks for EVERY key in one `multiGet` — one bridge round trip, one SQLite query — and
 * applies the answers in one batched update. This file is the part that can be tested without React
 * Native: it is handed the store, so a test can count the calls.
 *
 * ★ Nothing here decides what a value MEANS. Absent stays `null` — "never set" — and the screen keeps
 *   its own rules for which defaults an absent value reads as (several of them are deliberately ON).
 */

/** The one AsyncStorage method this needs, as a structural type so a test can stand in for it. */
export interface PrefsStore {
  multiGet(keys: readonly string[]): Promise<readonly (readonly [string, string | null])[]>;
}

/** What multiGet answered, keyed by name. Every key asked for is present; absent ones are `null`. */
export type PrefsSnapshot = Record<string, string | null>;

/** ★ Pure: the pairs as a lookup. A key the store did not return reads as `null`, never `undefined`. */
export function prefsFromPairs(keys: readonly string[],
                               pairs: readonly (readonly [string, string | null])[] | null | undefined): PrefsSnapshot {
  const out: PrefsSnapshot = {};
  for (const k of keys) out[k] = null;
  for (const p of pairs ?? []) {
    if (!p || typeof p[0] !== 'string' || !(p[0] in out)) continue;
    out[p[0]] = p[1] == null ? null : String(p[1]);
  }
  return out;
}

/**
 * Every key in ONE store call. Duplicates are asked for once.
 * ★ A rejection is passed straight through — the screen retries a failed read and must be able to
 *   tell it from a read that returned nothing.
 */
export async function readPrefs(store: PrefsStore, keys: readonly string[]): Promise<PrefsSnapshot> {
  const unique = Array.from(new Set(keys));
  const pairs = await store.multiGet(unique);
  return prefsFromPairs(unique, pairs);
}
