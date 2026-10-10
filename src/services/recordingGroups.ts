/**
 * ★★ RECORDINGS, BY SERVER THEN BY TIME (Stuart, 2026-10-10: "lists it by server name and then chronologically").
 * Pure, so it is tested (scripts/test_recording_groups.ts). The index that says which server a file came from is
 * services/recordingsIndex.ts; recordings made before it existed have no server and go last, under their own heading.
 *
 * Order: the receiver you are on first (inside a receiver's audio menu), then every other server A–Z, then the
 * recordings with no server recorded. Within a server, newest first — the one you just made is at the top.
 */
export interface RecLike { name: string; mtime: number }
export interface RecGroup<T extends RecLike> { server: string; recs: T[] }

export const NO_SERVER = '';

export function groupRecordings<T extends RecLike>(recs: T[], serverOf: (name: string) => string | undefined,
                                                   current?: string): RecGroup<T>[] {
  const by = new Map<string, T[]>();
  for (const r of recs) {
    const s = (serverOf(r.name) ?? '').trim() || NO_SERVER;
    const list = by.get(s);
    if (list) list.push(r); else by.set(s, [r]);
  }
  const cur = (current ?? '').trim();
  const groups: RecGroup<T>[] = [...by.entries()].map(([server, list]) => ({
    server, recs: list.slice().sort((a, b) => b.mtime - a.mtime || (a.name < b.name ? 1 : -1)),
  }));
  const rank = (g: RecGroup<T>) => (g.server === NO_SERVER ? 2 : cur && g.server === cur ? 0 : 1);
  groups.sort((a, b) => rank(a) - rank(b)
    || a.server.localeCompare(b.server, undefined, { sensitivity: 'base' }));
  return groups;
}
