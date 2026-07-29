# BRIEF: Syncing the FM-DX dial (and the learned-station map generally)

**Project:** VibeSDR (`src/screens/TunerScreen.tsx`, `src/components/FmdxDial.tsx`, Buddy)
**Author:** Stuart Carr (Stuey3D) with Claude, 2026-07-26
**Status:** DESIGNED, not built. Rides on the iCloud KVS work — build that first, then this.

---

## 1. The problem, in Stuart's words

> *"The FM-DX analogue tuner that populates with stations and is remembered per server — they
> need to sync, and Buddy should already have the dial built from the data on the phone. If you
> are on a shared server with little access to the tuner it can take a long time to get that
> analogue tuner display to build. Now imagine having to do it 4 or 5 times — Jr, Buddy, iPhone,
> iPad, Mac — and it becomes very tedious."*

★★ **The cost is not storage, it is TUNER TIME on someone else's radio.** The dial is built by
decoding RDS as you tune across the band, so filling it means parking on every station in
88–108. On a **shared** FM-DX server that is time you are taking from other listeners, and the
etiquette the rest of the app is careful about (see the SHARED TUNER notice, the settle-commit
debounce) makes it something you cannot simply do quickly. Even on your own server it is a slow
scan. Doing it **five times over** — once per device — is the same courtesy cost multiplied by
five for no gain, because every device is learning the identical thing about the identical
transmitters.

★ So this is not a convenience feature. It is the difference between a dial that fills once and
one that has to be earned again on every surface the user owns.

## 2. ★★ Buddy is a GAP, not a feature

Buddy is the phone's **remote** — it has no receiver of its own and already takes everything it
shows over WatchConnectivity. It should therefore have been building its dial from the phone's
data all along; that it does not is an omission rather than something new to design.

★ This is the same shape as the FM-DX gaps recorded in `BRIEF-fmdx-backend-adapter.md`: a
surface that misses a capability because nobody carried it there, and the failure mode is
silence rather than breakage.

Jr is different — it is standalone, so it needs the synced copy rather than the phone's.

## 3. What has to change in the data

Today (`TunerScreen.tsx`, `learnStation`):

```ts
const DIAL_KEY = `lsv_fmdx_dial:${baseUrl}`;   // per server
dialStations: { freqHz: number; name: string }[]   // capped at 300
```

★ That record cannot express either of the rules Stuart wants, because it has **no timestamp and
no PI code**. It needs:

```ts
{ freqHz: number; name: string; lastHeard: number; pi?: string }
```

- `lastHeard` — for expiry.
- `pi` — RDS Programme Identification, so a *different broadcaster* on a frequency is recognised
  as a replacement rather than a rename.

## 4. The rules — already designed once, in VibeServer

Stuart: *"Same applies with VibeServer RDS, though the entries expire after say a month… also
that dial continues to update and evolve like the RDS bookmarks in VibeServer, so if a new
station is occupying the frequency then it takes the slot."*

★★ **These are not new rules. VibeServer already learns station names this way** (see the v8.0.0
notes in `AboutOverlay.tsx`): a station unheard for 30 days expires rather than sitting on top of
static, and the PI code spots a different broadcaster on a frequency immediately. The FM-DX dial
should adopt the **same** rules rather than inventing a parallel set — one behaviour for "what
this aerial can hear", however it was learned.

1. **EXPIRE after ~30 days unheard.** Stuart's reasoning is specific and correct: *"some stations
   may come in with sporadic E and may never be on that dial again, so clear the space up for new
   stations."* A dial cluttered with one-off Sporadic-E catches is worse than a sparse one,
   because it claims the band is busier than it is.
2. **A NEW STATION TAKES THE SLOT.** Same frequency, different PI → replace, do not merge. Without
   PI, a changed name that persists across several decodes should still win.
3. **Refresh `lastHeard` on every confirmed decode**, so a station you hear often never expires.

## 5. Sync design

- **Per server**, as today — the dial describes what *that aerial* hears, so it cannot be pooled.
  One KVS key per server, keyed as now by `baseUrl`.
- **Merge, never replace.** Two devices will each have heard things the other has not, so a sync
  is a union: for a frequency present in both, keep the entry with the newer `lastHeard`.
  ★ Last-writer-wins on the whole blob would silently discard a device's hard-won scanning, which
  is exactly the cost this brief exists to avoid.
- **Size**: 300 entries × ~40 bytes ≈ 12 KB per server. iCloud KVS allows 1 MB total and 1024
  keys, so this is comfortable for a realistic number of servers — but the cap matters, and the
  300-entry limit should stay.
- **Buddy** takes the merged map from the phone over WatchConnectivity, as it does everything
  else. **Jr**, being standalone, needs its own KVS read.

## 6. Order of work

1. iCloud KVS sync (the V10 deliverable this depends on).
2. Add `lastHeard` + `pi` to the record, with a migration that stamps existing entries as heard
   now — an upgrade must not expire a dial someone spent hours filling.
3. Expiry and PI-displacement in `learnStation`.
4. Sync the per-server map, merging by `lastHeard`.
5. Forward to Buddy; read from KVS on Jr.

★ Step 2's migration is the one with a sharp edge: without it, the first launch after the update
sees entries with no `lastHeard`, and a naive expiry pass would wipe the lot.
