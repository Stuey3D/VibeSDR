/**
 * bookmarksSync.ts — the iCloud collection spec for user bookmarks.
 *
 * ★★ SEPARATE FROM userBookmarks.ts ON PURPOSE. That module is shared with the
 * WEB CLIENT (web/client/src/search.ts imports it), which is bundled by esbuild
 * for the browser — so it must stay platform-agnostic. Putting this spec there
 * pulled in cloudSync -> `react-native`, and the web build died with
 * "Unexpected typeof" inside react-native/index.js. Nothing surfaced it until
 * someone rebuilt the served page, which is exactly the kind of breakage that
 * hides for weeks.
 *
 * ★ Rule of thumb: anything under src/services that the web client imports must
 * not reach react-native, directly or transitively.
 */
import { CK, type Collection } from './cloudSync';
import { loadUserBookmarksStrict, saveUserBookmarks, type UserBookmark } from './userBookmarks';

/** ★ Bookmarks merge (union by name|frequency) rather than replace, and only
 *  the opted-in ones ever leave the device. A deletion is caught by the sync
 *  engine's snapshot diff and tombstoned — without that, the next device to
 *  sync would resurrect it. */
export const bookmarksCollection: Collection<UserBookmark> = {
  name: 'bookmarks',
  cloudKey: CK.bookmarks,
  load: loadUserBookmarksStrict,
  save: (items) => saveUserBookmarks(items),
  keyOf: (b) => `${b.name}|${b.frequency}`,
  eligible: (b) => b.synced === true,
  // Anything the cloud holds is by definition shared — mark it so, or the next
  // pass would read it as opted-out and tombstone what Jr just saved.
  adopt: (b) => (b.synced ? b : { ...b, synced: true }),
  merge: (a, b) => ((b.updatedAt ?? 0) >= (a.updatedAt ?? 0) ? { ...a, ...b } : { ...b, ...a }),
};
