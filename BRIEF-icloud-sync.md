# BRIEF: iCloud sync — scope and rules

**Project:** VibeSDR (phone / iPad / Mac), Jr, Buddy
**Author:** Stuart Carr (Stuey3D) with Claude, 2026-07-26
**Status:** SCOPED, not built. The last V10 deliverable.

---

## 1. What syncs

Stuart's list, with the store each already uses:

| What | Where it lives today | Scope |
|---|---|---|
| **Favourite servers + visit counts** | `vsdr_favourites` (visits are a field on each) | account-wide |
| **Bookmarks** | `lsv_user_bookmarks` | ★ SELECTIVE — see §2 |
| **Waterfall colour + VFO colour** | `lsv_display_prefs` and `lsv_display_prefs:<server>` | account-wide, and per server |
| **Last tuned station + demodulator** | `lsv_last_tune:<server>` → `{ frequency, mode }` | per server |
| **The FM-DX dial** | `lsv_fmdx_dial:<server>` | per server — see `BRIEF-dial-and-station-sync.md` |

★ Note the two shapes already present: **account-wide** things (favourites, colours) and
**per-server** things (last tune, dial, per-server display prefs). Per-server keys must stay
keyed by server — a last-tune or a dial pooled across receivers would be meaningless, because
they describe *that aerial*.

★★ **LAST TUNE IS FOR SERVERS ONLY — Stuart, confirmed.** `lsv_last_tune` also uses a
**per-device** key for local hardware (`usb` / `tcp:host:port`), because local baseUrls carry a
per-session port. Those are deliberately excluded: what a dongle plugged into one phone was last
tuned to has no meaning on another device, and syncing it would restore a frequency the user
never chose there.

★ Practical consequence for whoever builds this: filter by KEY SHAPE. Sync only
`lsv_last_tune:<baseUrl>` where the suffix is a real server address, never `usb` or
`tcp:host:port` — the two live in the same key namespace, so a naive prefix match would carry
both.

## 2. ★★ Bookmarks sync SELECTIVELY, and the asymmetry is the point

Stuart: *"A new button will need to be added to save to iCloud on the phone. Jr always saves to
iCloud — it means you can be selective as to which bookmarks get sync'd, to avoid too many being
on Jr that will become too hard to manage."*

- **Phone / iPad / Mac** — bookmarks are local by default, with an explicit **save to iCloud**
  action per bookmark.
- **Jr** — everything it saves goes to iCloud automatically.

★★ **The constraint belongs to the CONSUMER, not the producer.** A watch cannot present a long
bookmark list usefully, so the filtering has to happen where the list is *created* on the big
screens rather than where it is *read* on the small one. Making the phone opt in is what keeps
Jr's list short; asking Jr to filter would mean the user curating on the worst possible screen.

★ And the asymmetry is right rather than inconsistent: on a watch you will only ever save a
handful, deliberately, so "always sync" costs nothing there. On a phone you accumulate dozens
without thinking, which is exactly the list you do not want arriving on a 1-inch screen.

★ IMPLIES a per-bookmark flag on `UserBookmark` — e.g. `synced?: boolean` — plus a migration
that leaves existing bookmarks LOCAL. An update that silently pushed everything to iCloud would
produce precisely the unmanageable Jr list this design exists to prevent.

## 3. Merge rules

★★ **Merge, never last-writer-wins.** Every one of these is a list a user builds up over time on
whichever device is to hand, so replacing a blob would silently discard the other device's work.

- **Favourites** — union by `url`. On a conflict keep the higher `visits` (it is a tally of real
  connections, so the larger number is the truer one) and the newer edit for name/type.
- **Bookmarks** — union by `name|frequency`; a deletion needs a tombstone or it will be
  resurrected by the next device to sync. ★ Deletion is the case that always breaks naive merges.
- **Colours** — genuinely a preference rather than a collection, so last-writer-wins is correct
  here. Per-server prefs override the global one, as they already do locally.
- **Last tune** — newest wins by timestamp; it is a single value per server.

## 4. Limits worth respecting

iCloud KVS allows **1 MB total** and **1024 keys**. Favourites and bookmarks are small; the
FM-DX dial is the heavy one at roughly 12 KB per server (300 entries), so the 300-entry cap
should stay and the number of synced servers is the practical ceiling. ★ Worth failing visibly if
KVS rejects a write rather than losing data quietly — a sync that silently stops is worse than
one that never started, because the user believes they are covered.

## 5. Order

1. The KVS layer itself, with the merge helpers — one place that knows how to union and resolve.
2. Favourites + visits (smallest, and proves the merge).
3. Colours (last-writer-wins, no merge needed — a good early win).
4. Last tune per server.
5. Bookmarks with the selective flag and its migration.
6. The FM-DX dial (`BRIEF-dial-and-station-sync.md`), which needs its own schema change first.

★ Jr and Buddy differ: **Jr reads KVS directly** (it is standalone), **Buddy takes everything
from the phone** over WatchConnectivity, as it already does for the rest of its state.
