/**
 * userBookmarks.ts — local user bookmarks (v1/skin parity replacement for the
 * UberSDR bookmarks page, which the native app can't embed).
 *
 * Field set matches upstream local-bookmarks.js normalizeBookmark exactly so
 * the JSON export is directly importable by desktop UberSDR (importJSON
 * accepts a plain array of these objects):
 *   name, frequency (Hz), mode, group?, comment?, extension?,
 *   bandwidth_low?, bandwidth_high?
 * The app adds a `scope` field ('' = all instances, else the instance
 * baseUrl) — stripped on export.
 */

import AsyncStorage from '@react-native-async-storage/async-storage';

const KEY = 'lsv_user_bookmarks';

export interface UserBookmark {
  name:            string;
  frequency:       number;   // Hz
  mode:            string;   // lowercase demodulator
  group?:          string | null;
  comment?:        string | null;
  extension?:      string | null;
  bandwidth_low?:  number | null;
  bandwidth_high?: number | null;
  /** '' = all instances; otherwise the owning instance baseUrl. App-only. */
  scope:           string;
  /** ★★ iCloud opt-in, per bookmark. Local unless the user says otherwise.
   *
   *  The asymmetry is the point: on the phone you accumulate dozens of
   *  bookmarks without thinking, and that is exactly the list you do not want
   *  arriving on a 1-inch screen — so the phone opts IN per bookmark, while Jr
   *  syncs everything it saves (on a watch you only ever save a handful,
   *  deliberately). The constraint belongs to the CONSUMER, so the filtering
   *  happens where the list is CREATED, not where it is read.
   *
   *  ★ undefined = local, which is also the migration: every bookmark that
   *  existed before this field stays on the device it was made on, with no
   *  migration write. An update that silently pushed them all to iCloud would
   *  produce precisely the unmanageable Jr list this design exists to prevent.
   */
  synced?:         boolean;
  /** Last edit time, for sync conflict resolution. Stamped on first sight. */
  updatedAt?:      number;
}

/** Flip a bookmark's iCloud opt-in. Matched the same way deletion is. */
export function setBookmarkSynced(
  list: UserBookmark[], bm: UserBookmark, synced: boolean,
): UserBookmark[] {
  return list.map((b) =>
    b.name === bm.name && b.frequency === bm.frequency && b.scope === bm.scope
      ? { ...b, synced, updatedAt: Date.now() }
      : b);
}

export async function loadUserBookmarks(): Promise<UserBookmark[]> {
  try {
    const raw = await AsyncStorage.getItem(KEY);
    if (!raw) return [];
    const arr = JSON.parse(raw);
    return Array.isArray(arr) ? arr.filter((b) => b && b.name && b.frequency) : [];
  } catch {
    return [];
  }
}

export async function saveUserBookmarks(list: UserBookmark[]): Promise<void> {
  await AsyncStorage.setItem(KEY, JSON.stringify(list));
}

/** Bookmarks visible on an instance: global ('') + instance-scoped. */
export function bookmarksForInstance(list: UserBookmark[], baseUrl: string): UserBookmark[] {
  return list.filter((b) => !b.scope || b.scope === baseUrl);
}

/** Remove all bookmarks scoped to one instance (instance-settings reset). */
export function withoutInstance(list: UserBookmark[], baseUrl: string): UserBookmark[] {
  return list.filter((b) => b.scope !== baseUrl);
}

/** UberSDR-importable JSON (desktop local-bookmarks importJSON): plain array,
 *  app-only scope field stripped, frequencies integral, modes lowercase. */
export function exportBookmarksJSON(list: UserBookmark[]): string {
  const out = list.map((b) => ({
    name:           b.name,
    frequency:      Math.round(b.frequency),
    mode:           (b.mode || 'usb').toLowerCase(),
    group:          b.group ?? null,
    comment:        b.comment ?? null,
    extension:      b.extension ?? null,
    bandwidth_low:  b.bandwidth_low ?? null,
    bandwidth_high: b.bandwidth_high ?? null,
  }));
  return JSON.stringify(out, null, 2);
}

/** Parse pasted JSON (UberSDR export, or anything normalizeBookmark-shaped).
 *  Accepts a plain array or {bookmarks:[...]} — same as upstream importJSON.
 *  Imported bookmarks land with the given scope. */
export function parseBookmarksJSON(jsonString: string, scope: string): UserBookmark[] {
  const data = JSON.parse(jsonString);
  const arr = Array.isArray(data) ? data : (data?.bookmarks ?? []);
  if (!Array.isArray(arr)) throw new Error('not a bookmark list');
  const out: UserBookmark[] = [];
  for (const b of arr) {
    if (!b || typeof b !== 'object') continue;
    let freq = Number(b.frequency ?? b.frequencyHz ?? b.freq ?? b.f ?? 0);
    if (!freq || isNaN(freq)) continue;
    if (freq < 30_000) freq *= 1000; // kHz heuristic (skin normBm parity)
    const mode = String(b.mode ?? b.modulation ?? b.m ?? 'usb').toLowerCase();
    out.push({
      name:           String(b.name ?? b.label ?? b.title ?? 'Unnamed'),
      frequency:      Math.round(freq),
      mode,
      group:          b.group ?? b.category ?? null,
      comment:        b.comment ?? b.notes ?? null,
      extension:      b.extension ?? b.decoder ?? null,
      bandwidth_low:  typeof b.bandwidth_low === 'number' ? b.bandwidth_low
                    : typeof b.bandwidthLow === 'number' ? b.bandwidthLow : null,
      bandwidth_high: typeof b.bandwidth_high === 'number' ? b.bandwidth_high
                    : typeof b.bandwidthHigh === 'number' ? b.bandwidthHigh : null,
      scope,
    });
  }
  return out;
}

/** Parse UberSDR bookmark YAML (the server's export format):
 *    bookmarks:
 *      - name: "WWV 5 MHz"
 *        frequency: 5000000
 *        mode: "am"
 *        group: "Beacons"
 *        comment: "…"        # inline comments allowed
 *  Minimal hand-rolled parser (no YAML dependency) — only this flat shape. */
export function parseBookmarksYAML(yaml: string, scope: string): UserBookmark[] {
  const splitKV = (s: string): [string, string] | null => {
    const i = s.indexOf(':');
    if (i < 0) return null;
    const k = s.slice(0, i).trim();
    let v = s.slice(i + 1).trim();
    if (v.startsWith('"') || v.startsWith("'")) {
      const q = v[0], end = v.indexOf(q, 1);
      v = end > 0 ? v.slice(1, end) : v.slice(1);
    } else {
      const h = v.indexOf(' #');                 // strip trailing inline comment
      if (h >= 0) v = v.slice(0, h).trim();
    }
    return [k, v];
  };

  const items: Record<string, string>[] = [];
  let cur: Record<string, string> | null = null;
  for (const raw of yaml.split(/\r?\n/)) {
    const line = raw.replace(/\t/g, '  ');
    if (/^\s*#/.test(line) || !line.trim()) continue;
    const item = line.match(/^(\s*)-\s*(.*)$/);
    if (item) {
      cur = {}; items.push(cur);
      if (item[2]) { const kv = splitKV(item[2]); if (kv) cur[kv[0]] = kv[1]; }
    } else if (cur) {
      const kv = splitKV(line.trim());
      if (kv) cur[kv[0]] = kv[1];
    }
  }

  const out: UserBookmark[] = [];
  for (const b of items) {
    let freq = Number(b.frequency ?? b.freq ?? 0);
    if (!freq || isNaN(freq)) continue;
    if (freq < 30_000) freq *= 1000;             // kHz heuristic
    out.push({
      name:           String(b.name ?? 'Unnamed'),
      frequency:      Math.round(freq),
      mode:           String(b.mode ?? 'usb').toLowerCase(),
      group:          b.group ?? null,
      comment:        b.comment ?? null,
      extension:      b.extension ?? null,
      bandwidth_low:  b.bandwidth_low != null && b.bandwidth_low !== '' ? Number(b.bandwidth_low) : null,
      bandwidth_high: b.bandwidth_high != null && b.bandwidth_high !== '' ? Number(b.bandwidth_high) : null,
      scope,
    });
  }
  return out;
}

/** Parse pasted bookmark text — JSON or YAML, auto-detected. */
export function parseBookmarksAny(text: string, scope: string): UserBookmark[] {
  const t = text.trim();
  if (t.startsWith('{') || t.startsWith('[')) return parseBookmarksJSON(text, scope);
  // YAML (UberSDR export) — fall back to JSON if it doesn't look like YAML.
  try {
    const y = parseBookmarksYAML(text, scope);
    if (y.length) return y;
  } catch {}
  return parseBookmarksJSON(text, scope);
}

/** Merge imported/added bookmarks — name+frequency keyed, newest wins. */
export function mergeBookmarks(existing: UserBookmark[], incoming: UserBookmark[]): UserBookmark[] {
  const key = (b: UserBookmark) => `${b.name}|${b.frequency}`;
  const map = new Map<string, UserBookmark>();
  for (const b of existing) map.set(key(b), b);
  for (const b of incoming) map.set(key(b), b);
  return [...map.values()];
}
