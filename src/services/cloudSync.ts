/**
 * cloudSync.ts — the one place that knows how to merge VibeSDR's state across
 * devices, sitting on top of the dumb key/value pipe in cloudKvs.ts.
 *
 * ★★ MERGE, NEVER LAST-WRITER-WINS for anything list-shaped. Favourites,
 * bookmarks and dials are all things a user builds up over time on whichever
 * device is to hand — pushing a whole blob would silently discard the other
 * device's work. Only genuine preferences (colours and the last tune) are
 * last-writer-wins, and those live in perServerSync.ts.
 *
 * ★ Deletions need tombstones or the next device to sync resurrects them. We
 * derive them from a SNAPSHOT DIFF rather than asking call sites to report a
 * delete: comparing the keys present at the last sync against the keys present
 * now catches every removal path, including ones written later that nobody
 * remembered to instrument. Deletion is the case that always breaks naive
 * merges, so it must not depend on being called.
 *
 * See BRIEF-icloud-sync.md.
 */
import AsyncStorage from '@react-native-async-storage/async-storage';
import { AppState } from 'react-native';
import {
  kvsAvailable, kvsGet, kvsSet, kvsSupported, kvsSynchronize, onKvsChanged,
  kvsAllKeys, kvsRemove,
} from './cloudKvs';

// ── Keys ────────────────────────────────────────────────────────────────────
// ★ iCloud KVS keys are capped at 64 bytes, so a per-server key can never be
//   the baseUrl itself — it uses serverSlug() below. Keep these prefixes short.
export const CK = {
  favourites: 'vs.fav',
  bookmarks:  'vs.bm',
  prefs:      'vs.prefs',            // global display prefs (colours)
} as const;

/** Prefixes for the per-server key families. Defined here, with the rest of the
 *  key map, so nothing can drift: `vs.p.` living in one file and `vs.d.` in
 *  another is how two of them end up sharing a prefix by accident. */
export const CKP = {
  prefs: 'vs.p.',
  tune:  'vs.t.',
  dial:  'vs.d.',
} as const;

/** Local mirror of "what we had at the last successful sync", per collection.
 *
 *  ★★ VERSIONED, and the version MUST be bumped whenever a collection's
 *  `keyOf` changes. The snapshot is a set of item KEYS, and deletions are
 *  inferred from keys that were there last time and are not there now — so if
 *  the key FORMAT changes, every single item looks deleted and gets tombstoned
 *  across every device. Changing this string makes the first pass after an
 *  upgrade read an empty snapshot, which infers no deletions at all: the safe
 *  direction. v2 = favourites moved from the raw url to favouriteKey(). */
const SNAP_VERSION = 'v2';
const snapKey = (name: string) => `lsv_sync_snap:${name}:${SNAP_VERSION}`;

/** Tombstones live 90 days. Long enough that a device left in a drawer over a
 *  holiday still learns about a deletion; short enough that the store does not
 *  fill up with the ghosts of every bookmark ever removed. */
const TOMB_TTL_MS = 90 * 24 * 3600_000;

// ── Server keys ─────────────────────────────────────────────────────────────

/** Normalise a baseUrl so two devices that typed it slightly differently still
 *  land on the same KVS key. */
export function normaliseServerUrl(baseUrl: string): string {
  return baseUrl.trim().replace(/\/+$/, '').toLowerCase();
}

/** Short, stable, collision-resistant key fragment for a server baseUrl.
 *  Two independent FNV-1a passes (forward and reversed) give 64 bits in 16 hex
 *  chars, which keeps `vs.d.<slug>` at 21 bytes — well inside the 64-byte key
 *  limit that rules out using the URL itself. The value always carries the full
 *  url as well, so a collision is detectable rather than silently merging two
 *  receivers' dials. */
export function serverSlug(baseUrl: string): string {
  const s = normaliseServerUrl(baseUrl);
  const fnv = (str: string) => {
    let h = 0x811c9dc5;
    for (let i = 0; i < str.length; i++) {
      h ^= str.charCodeAt(i);
      h = Math.imul(h, 0x01000193) >>> 0;
    }
    return h.toString(16).padStart(8, '0');
  };
  return fnv(s) + fnv([...s].reverse().join(''));
}

/**
 * Canonical identity for a favourite, for merging only — the stored `url` is
 * left exactly as it is, because that is what the app connects with.
 *
 * ★★ Two apps spelled the same receiver two ways and the union could not tell:
 * Jr saves a VibeServer as `ws://<host>`, the phone as `http://<host>:<port>`,
 * so the SAME radio arrived on the other device as a second favourite. The
 * phone's own parseHostPort already treats ws:// and http:// as equivalent when
 * CONNECTING — the merge key simply had not been taught the same thing.
 *
 * ★ Jr's Swift copy in CloudSync.swift must apply the identical rules. A
 * canonicaliser that disagrees across devices is worse than none, because it
 * silently splits entries that used to match.
 */
export function favouriteKey(url: string): string {
  let s = url.trim().toLowerCase()
    .replace(/^ws:\/\//, 'http://')
    .replace(/^wss:\/\//, 'https://')
    .replace(/\/+$/, '');
  // Drop the default port so `https://x` and `https://x:443` are one server.
  s = s.replace(/^http:\/\/([^/]+):80(\/|$)/, 'http://$1$2')
       .replace(/^https:\/\/([^/]+):443(\/|$)/, 'https://$1$2');
  return s;
}

/** ★ Per-server state syncs only for real SERVERS. `lsv_last_tune` and friends
 *  share their key namespace with LOCAL hardware (`usb`, `tcp:host:port`), and
 *  what a dongle plugged into one phone was last tuned to is meaningless on
 *  another device — it would restore a frequency the user never chose there.
 *  A naive prefix match over the namespace would carry both, so filter by shape. */
export function isSyncableServerKey(suffix: string): boolean {
  return /^https?:\/\//i.test(suffix);
}

// ── Documents ───────────────────────────────────────────────────────────────

type CloudDoc<T> = {
  v: 1;
  items: T[];
  /** itemKey → deletion time (ms). */
  tombs?: Record<string, number>;
};

export type Syncable = { updatedAt?: number };

export interface Collection<T extends Syncable> {
  /** Internal id; also names the local snapshot. */
  name: string;
  /** KVS key. Must be ≤ 64 bytes. */
  cloudKey: string;
  load(): Promise<T[]>;
  save(items: T[]): Promise<void>;
  keyOf(item: T): string;
  /** Resolve two versions of the same key. Must be order-independent. */
  merge(a: T, b: T): T;
  /** Which items may leave this device. Ineligible items stay local and are
   *  never tombstoned by the snapshot diff. Bookmarks use this for the
   *  per-bookmark opt-in; omit it and everything syncs. */
  eligible?(item: T): boolean;
  /** Applied to items arriving FROM the cloud, before merging. Use it to set
   *  whatever `eligible` tests — an item pulled down that does not satisfy it
   *  would be treated as opted-out on the next pass and tombstoned away. */
  adopt?(item: T): T;
}

// ── Status ──────────────────────────────────────────────────────────────────

export type SyncStatus = {
  supported: boolean;
  available: boolean;
  enabled: boolean;
  lastSyncAt: number | null;
  /** ★ Surfaced in the UI. A quota rejection must be visible: the user believes
   *  they are covered, and a sync that has stopped looks exactly like one that
   *  is up to date. */
  lastError: string | null;
};

let status: SyncStatus = {
  supported: kvsSupported, available: false, enabled: true,
  lastSyncAt: null, lastError: null,
};
const statusListeners = new Set<(s: SyncStatus) => void>();

export function getSyncStatus(): SyncStatus { return status; }
export function onSyncStatus(cb: (s: SyncStatus) => void): () => void {
  statusListeners.add(cb);
  return () => { statusListeners.delete(cb); };
}
/** Fired when a sync CHANGED a collection's local copy.
 *
 *  ★ Screens load their lists once, at mount. Without this a bookmark arriving
 *  from Jr lands correctly in storage and stays invisible until the next
 *  remount — which reads as "sync is broken" when in fact it worked. Anything
 *  holding synced state in React state must subscribe. */
const changeListeners = new Set<(name: string) => void>();
export function onCollectionChanged(cb: (name: string) => void): () => void {
  changeListeners.add(cb);
  return () => { changeListeners.delete(cb); };
}
function emitChanged(name: string) {
  for (const cb of changeListeners) { try { cb(name); } catch {} }
}

function setStatus(patch: Partial<SyncStatus>) {
  status = { ...status, ...patch };
  for (const cb of statusListeners) { try { cb(status); } catch {} }
}

const ENABLED_KEY = 'vibe.icloud.enabled';

export async function loadSyncEnabled(): Promise<boolean> {
  try {
    const v = await AsyncStorage.getItem(ENABLED_KEY);
    const enabled = v == null ? true : v === '1';   // on by default where iCloud works
    setStatus({ enabled });
    return enabled;
  } catch { return status.enabled; }
}

export async function setSyncEnabled(on: boolean): Promise<void> {
  setStatus({ enabled: on, lastError: null });
  try { await AsyncStorage.setItem(ENABLED_KEY, on ? '1' : '0'); } catch {}
  if (on) requestSync();
}

// ── The merge ───────────────────────────────────────────────────────────────

function parseDoc<T>(raw: string | null): CloudDoc<T> {
  if (!raw) return { v: 1, items: [] };
  try {
    const d = JSON.parse(raw);
    if (!d || !Array.isArray(d.items)) return { v: 1, items: [] };
    return { v: 1, items: d.items, tombs: d.tombs && typeof d.tombs === 'object' ? d.tombs : {} };
  } catch {
    return { v: 1, items: [] };
  }
}

function pruneTombs(tombs: Record<string, number>, now: number): Record<string, number> {
  const out: Record<string, number> = {};
  for (const k of Object.keys(tombs)) {
    if (now - tombs[k] < TOMB_TTL_MS) out[k] = tombs[k];
  }
  return out;
}

/**
 * Merge one collection with iCloud, both directions, and write both sides.
 * Returns the merged eligible list (or null if nothing was done).
 */
export async function syncCollection<T extends Syncable>(spec: Collection<T>): Promise<T[] | null> {
  if (!status.enabled || !(await kvsAvailable())) return null;
  const now = Date.now();

  // ★★ A FAILED READ ABORTS THIS COLLECTION, TOUCHING NOTHING. Storage faults
  // used to surface as an empty list, indistinguishable from "the user deleted
  // everything" — so the engine guessed, refusing to tombstone a wholesale
  // disappearance. That guess had a cost nobody spotted: emptying a list (or
  // deleting its LAST item) then fell through to the merge below, which refilled
  // it from the cloud and SAVED IT BACK LOCALLY. The item was immortal.
  //
  // The loaders now throw on a real failure and return [] only when the list is
  // genuinely empty, so there is nothing left to guess about: an empty list is
  // an empty list, and its deletions tombstone normally.
  let localAll: T[];
  try {
    localAll = await spec.load();
  } catch (e) {
    setStatus({ lastError: `${spec.name}: could not read local list — sync skipped.` });
    return null;
  }
  const isEligible = spec.eligible ?? (() => true);

  // ★★★ A TIMESTAMP FROM THE FUTURE MAKES AN ITEM IMMORTAL, so it is not
  // believed. A tombstone only wins over an item OLDER than the delete
  // (see below), which is right — a genuine re-add on another device must
  // survive a stale deletion. But it means an item stamped ahead of now can
  // never be tombstoned by anything, ever: delete it, and the next sync a
  // second later restores it from the cloud. No amount of deleting helps,
  // on any device, and nothing in the app could clear it. (Stuart's zombie
  // favourite + bookmark, left by an early test build, 2026-07-28.)
  //
  // A future stamp is not data, it is damage — a bad clock, a unit mix-up
  // (seconds vs ms), or a build that wrote something silly. So it is read as
  // 1, the same "exists but never knowingly edited" value untimed items get,
  // which lets a tombstone reach it. The healed value is written back to both
  // the device and the cloud, so this repairs the document rather than
  // working around it forever.
  //
  // The allowance covers a device whose clock is legitimately a little ahead;
  // beyond that, no honest edit can claim to have happened yet.
  const SKEW_MS = 5 * 60_000;
  const sane = (at: unknown): number =>
    typeof at === 'number' && Number.isFinite(at) && at > 0 && at <= now + SKEW_MS ? at : 1;

  // Stamp anything that predates sync so it has an ordering at all.
  //
  // ★★ STAMPED WITH 1, NOT `now`, AND THAT IS THE WHOLE POINT. Stamping an
  // untimed item with the current time declares it "just edited", so it beats
  // every tombstone — including one another device wrote a moment ago. The item
  // then becomes IMMORTAL: delete it on the phone, the watch re-stamps and
  // re-uploads it, forever. (Hit for real, 2026-07-26.)
  //
  // 1 means "exists, but has never been knowingly edited", which is the truth:
  // every real mutation sets updatedAt (see favourites.ts / userBookmarks.ts).
  // So a genuine edit still outranks an older tombstone, while an untouched
  // legacy item correctly yields to another device's deletion — which is
  // precisely what a tombstone is for.
  let stampedLocally = false;
  const localEligible = localAll.filter(isEligible).map(it => {
    const at = sane(it.updatedAt);
    if (it.updatedAt === at) return it;
    stampedLocally = true;                 // untimed, or an implausible stamp healed
    return { ...it, updatedAt: at };
  });

  const remoteDoc = parseDoc<T>(await kvsGet(spec.cloudKey));
  // Heal the cloud's copies on the way in, for the same reason.
  const remote = {
    ...remoteDoc,
    items: remoteDoc.items.map(it =>
      it && typeof it === 'object' ? { ...it, updatedAt: sane((it as T).updatedAt) } : it),
  };

  // Deletions: anything that was here at the last sync and is not here now.
  let snapshot: string[] = [];
  try {
    const raw = await AsyncStorage.getItem(snapKey(spec.name));
    if (raw) { const a = JSON.parse(raw); if (Array.isArray(a)) snapshot = a; }
  } catch {}
  const localKeys = new Set(localEligible.map(spec.keyOf));
  const tombs = pruneTombs({ ...(remote.tombs ?? {}) }, now);

  // Anything present at the last sync and absent now was deleted here. Safe to
  // do unconditionally: a read that FAILED never reaches this line (it returned
  // above), so an empty list can only mean the user emptied it.
  // ★★★ ALWAYS RE-STAMP. `&& !tombs[k]` used to guard this, and it made an item
  // PERMANENTLY UNDELETABLE once its key had ever been tombstoned and lost.
  //
  // A tombstone only beats an item OLDER than the delete, which is right: a
  // genuine re-add on another device must survive a stale deletion. But pair
  // that with "don't overwrite an existing tombstone" and there is no way back.
  // Delete it (tombstone at T1) → anything re-adds or re-stamps it at T2 > T1 →
  // the item wins, correctly → and now every future delete writes NOTHING,
  // because a tombstone for that key already exists. The stale T1 stays, forever
  // older than the item, so the item returns on every sync no matter how many
  // times it is deleted, on any device. (Stuart's zombie favourite: item at
  // …554553.241, tombstone at …515455.04 — 39 seconds too early, and stuck.)
  //
  // The delete happened NOW, so the tombstone says now. It is refreshed once per
  // actual deletion, not repeatedly: a successful pass rewrites the snapshot
  // without the key, so the diff stops firing for it.
  for (const k of snapshot) {
    if (!localKeys.has(k)) tombs[k] = now;
  }

  // Union. Remote first, then fold local over it — so the merge is applied per
  // key rather than one side winning wholesale.
  const byKey = new Map<string, T>();
  const adopt = spec.adopt ?? ((it: T) => it);
  for (const raw of remote.items) {
    if (raw && typeof raw === 'object') { const it = adopt(raw); byKey.set(spec.keyOf(it), it); }
  }
  for (const it of localEligible) {
    const k = spec.keyOf(it);
    const other = byKey.get(k);
    byKey.set(k, other ? spec.merge(other, it) : it);
  }

  // Apply tombstones — a delete only wins over an item older than the delete,
  // so a genuine re-add on another device survives.
  for (const [k, at] of Object.entries(tombs)) {
    const it = byKey.get(k);
    if (it && (it.updatedAt ?? 0) <= at) byKey.delete(k);
  }

  // Order: this device's own order first (favourites' manual drag order IS the
  // array position), then whatever only the cloud had.
  const merged: T[] = [];
  const seen = new Set<string>();
  for (const it of localEligible) {
    const k = spec.keyOf(it);
    if (seen.has(k)) continue;
    const m = byKey.get(k);
    if (m) { merged.push(m); seen.add(k); }
  }
  for (const it of remote.items) {
    const k = it && typeof it === 'object' ? spec.keyOf(it) : '';
    if (!k || seen.has(k)) continue;
    const m = byKey.get(k);
    if (m) { merged.push(m); seen.add(k); }
  }

  // Write back locally IN THE ORDER THE LIST ALREADY HAS. Appending the merged
  // set and then the opted-out remainder would shuffle every synced bookmark to
  // the top of the user's list on every pass — and, because the order changed,
  // make it look dirty and rewrite it again next time.
  const mergedByKey = new Map(merged.map(it => [spec.keyOf(it), it]));
  const nextLocal: T[] = [];
  const placed = new Set<string>();
  for (const it of localAll) {
    if (!isEligible(it)) { nextLocal.push(it); continue; }
    const k = spec.keyOf(it);
    const m = mergedByKey.get(k);
    if (m && !placed.has(k)) { nextLocal.push(m); placed.add(k); }   // absent ⇒ tombstoned away
  }
  for (const it of merged) {                                          // arrived from another device
    const k = spec.keyOf(it);
    if (!placed.has(k)) { nextLocal.push(it); placed.add(k); }
  }
  if (stampedLocally || !sameList(localAll, nextLocal, spec.keyOf)) {
    await spec.save(nextLocal);
    emitChanged(spec.name);
  }

  // Write back to iCloud only when it would actually change.
  const nextDoc: CloudDoc<T> = { v: 1, items: merged, tombs };
  const nextRaw = JSON.stringify(nextDoc);
  // ★ Compared against what was ACTUALLY IN THE STORE (remoteDoc), not the
  // healed copy — otherwise a document whose only fault is a poisoned timestamp
  // compares equal to its own repair and is never written back, leaving the
  // damage in iCloud to be re-read forever.
  if (nextRaw !== JSON.stringify({ v: 1, items: remoteDoc.items, tombs: remoteDoc.tombs ?? {} })) {
    await kvsSet(spec.cloudKey, nextRaw);
  }

  try { await AsyncStorage.setItem(snapKey(spec.name), JSON.stringify([...seen])); } catch {}
  return merged;
}

function sameList<T>(a: T[], b: T[], keyOf: (t: T) => string): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (keyOf(a[i]) !== keyOf(b[i])) return false;
    if (JSON.stringify(a[i]) !== JSON.stringify(b[i])) return false;
  }
  return true;
}

// ── Registry + scheduling ───────────────────────────────────────────────────

const collections = new Map<string, Collection<any>>();
/** Extra passes contributed by screens (per-server prefs, last tune, dials). */
const hooks = new Map<string, () => Promise<void>>();

export function registerCollection<T extends Syncable>(spec: Collection<T>) {
  collections.set(spec.name, spec);
}

export function registerSyncHook(name: string, fn: () => Promise<void>) {
  hooks.set(name, fn);
}
export function unregisterSyncHook(name: string) {
  hooks.delete(name);
}

let running: Promise<void> | null = null;
let pending: ReturnType<typeof setTimeout> | null = null;

/** Run every registered pass. Serialised — two overlapping syncs would each
 *  read the pre-merge document and the second would undo the first. */
export async function syncAll(): Promise<void> {
  if (running) return running;
  running = (async () => {
    if (!(await kvsAvailable(true))) { setStatus({ available: false }); return; }
    setStatus({ available: true });
    let err: string | null = null;
    for (const spec of collections.values()) {
      try { await syncCollection(spec); }
      catch (e: any) { err = err ?? `${spec.name}: ${e?.message ?? e}`; }
    }
    for (const [name, fn] of hooks) {
      try { await fn(); }
      catch (e: any) { err = err ?? `${name}: ${e?.message ?? e}`; }
    }
    await kvsSynchronize();
    setStatus({ lastSyncAt: Date.now(), lastError: err });
  })().finally(() => { running = null; });
  return running;
}

/** Coalescing, debounced sync — safe to call after every local write. */
/**
 * REPLACE WHAT IS IN iCLOUD WITH WHAT IS ON THIS DEVICE.
 *
 * ★ The one operation the inferred path cannot safely perform. Everything else
 * here MERGES, because a device that has been offline for a week must not lose
 * what the others added — which also means anything already in the cloud comes
 * back, including entries seeded by a build or a device that no longer exists.
 * There was no way to say "that is rubbish, throw it away", so it accumulated.
 *
 * ★★ Pressing a button IS unambiguous intent, which is exactly what the engine
 * cannot infer from an empty list. So this is allowed to do what no automatic
 * pass may: drop the remote document outright.
 *
 * It does NOT delete anything local. It DOES tombstone every entry the cloud
 * holds that this device does not, because otherwise it cannot do its job:
 * emptying the store simply invites the next device to sync to refill it from
 * its own copy, which is exactly what happened — delete a favourite, reset,
 * and Jr uploaded it straight back (2026-07-28). A tombstone is the only thing
 * that tells another device an entry is DEAD rather than merely missing here.
 *
 * ★ So this is genuinely destructive across devices, and that is the point:
 * "replace iCloud with this device" means anything not on this device dies,
 * including something added on the watch and never synced here. The wording and
 * the confirmation say so.
 */
export async function resetCloudToThisDevice(): Promise<void> {
  if (!(await kvsAvailable(true))) {
    setStatus({ available: false, lastError: 'iCloud unavailable — nothing was reset.' });
    return;
  }
  const now = Date.now();
  try {
    // ── Collections: keep this device's items, BURY everything else ──
    for (const spec of collections.values()) {
      let localKeys = new Set<string>();
      try {
        const isEligible = spec.eligible ?? (() => true);
        localKeys = new Set((await spec.load()).filter(isEligible).map(spec.keyOf));
      } catch {
        // Can't read this list ⇒ can't say what belongs here. Leave it alone
        // rather than burying the user's entire collection on a bad read.
        continue;
      }
      const doc = parseDoc<any>(await kvsGet(spec.cloudKey));
      const tombs: Record<string, number> = pruneTombs({ ...(doc.tombs ?? {}) }, now);
      for (const it of doc.items) {
        if (!it || typeof it !== 'object') continue;
        const k = spec.keyOf(it);
        if (!localKeys.has(k)) tombs[k] = now;      // in the cloud, not here ⇒ dead
      }
      await kvsSet(spec.cloudKey, JSON.stringify({ v: 1, items: [], tombs }));
    }
    // ── Per-server families (colours, last tune, dials): last-writer-wins, no
    // tombstones exist for them, so dropping the keys IS the reset. ──
    const perServer = (k: string) => Object.values(CKP).some(p => k.startsWith(p));
    for (const k of (await kvsAllKeys()).filter(perServer)) await kvsRemove(k);
    // Drop the local snapshots too. A snapshot lists what was in the cloud at the
    // last sync, so leaving them would make the very next pass read every item as
    // "deleted elsewhere" and tombstone this device's own lists — the reset would
    // eat the data it exists to preserve.
    for (const name of collections.keys()) {
      try { await AsyncStorage.removeItem(snapKey(name)); } catch {}
    }
    await kvsSynchronize();
    setStatus({ lastError: null });
  } catch (e: any) {
    setStatus({ lastError: `Reset failed: ${e?.message ?? e}` });
    return;
  }
  await syncAll();          // re-upload this device's lists into the empty store
}

/**
 * What is ACTUALLY stored, on both sides, for every synced collection.
 *
 * ★ Because reasoning about this from the source has now been wrong twice. A
 * resurrecting item has several possible causes that look identical from the
 * outside — a stamp that outranks every tombstone, a key that never enters the
 * snapshot, two spellings of one URL colliding on `keyOf` — and they need
 * different fixes. This prints the evidence: the local list, the remote
 * document, the tombstones, and the snapshot the deletions are inferred from,
 * with the KEY each item resolves to alongside its raw url.
 */
export async function syncDiagnostic(): Promise<string> {
  const now = Date.now();
  const when = (v: unknown) =>
    typeof v === 'number' ? `${v}${v > now ? ' ★FUTURE' : ''}` : String(v);
  const out: string[] = [`now=${now}  (${new Date(now).toISOString()})`];
  for (const spec of collections.values()) {
    out.push(`\n═══ ${spec.name} (${spec.cloudKey}) ═══`);
    let local: any[] = [];
    try { local = await spec.load(); out.push(`local: ${local.length} item(s)`); }
    catch (e: any) { out.push(`local: READ FAILED — ${e?.message ?? e}`); }
    for (const it of local) {
      out.push(`  L key=${spec.keyOf(it)}  at=${when(it.updatedAt)}  ${JSON.stringify(it).slice(0, 200)}`);
    }
    const raw = await kvsGet(spec.cloudKey);
    if (raw == null) { out.push('cloud: (no document)'); }
    else {
      const doc = parseDoc<any>(raw);
      out.push(`cloud: ${doc.items.length} item(s), ${Object.keys(doc.tombs ?? {}).length} tombstone(s)`);
      for (const it of doc.items) {
        out.push(`  C key=${spec.keyOf(it)}  at=${when(it.updatedAt)}  ${JSON.stringify(it).slice(0, 200)}`);
      }
      for (const [k, at] of Object.entries(doc.tombs ?? {})) out.push(`  T ${k} = ${when(at)}`);
    }
    let snap: string[] = [];
    try {
      const s = await AsyncStorage.getItem(snapKey(spec.name));
      if (s) snap = JSON.parse(s);
    } catch {}
    out.push(`snapshot(${snapKey(spec.name)}): ${JSON.stringify(snap)}`);
  }
  return out.join('\n');
}

export function requestSync(delayMs = 1500): void {
  if (!status.supported) return;
  if (pending) clearTimeout(pending);
  pending = setTimeout(() => { pending = null; void syncAll(); }, delayMs);
}

let started = false;

/** Call once at app start. Syncs now, on foreground, and whenever another
 *  device (or Jr) changes the store. */
export function startCloudSync(): () => void {
  if (started) return () => {};
  started = true;

  void loadSyncEnabled().then(() => requestSync(400));

  const appSub = AppState.addEventListener('change', s => {
    if (s === 'active') requestSync(400);
  });

  const offKvs = onKvsChanged(c => {
    if (c.quotaExceeded) {
      setStatus({ lastError: 'iCloud storage is full — sync has stopped.' });
      return;
    }
    requestSync(300);
  });

  return () => {
    appSub.remove();
    offKvs();
    started = false;
  };
}
