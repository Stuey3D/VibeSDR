import Foundation
import Combine

/// CloudSyncEngine.swift — Jr's favourites + bookmarks, merged with iCloud.
///
/// ★★ MERGE, NEVER LAST-WRITER-WINS. Both are lists the user builds up over
/// time on whichever device is to hand; writing a whole blob would silently
/// discard the other device's work. Union by key, with an explicit rule for a
/// collision, and tombstones so a deletion is not resurrected by the next
/// device to sync.
///
/// The wire format is the phone's — see `src/services/cloudSync.ts`.
@MainActor
final class CloudSyncEngine: ObservableObject {
  static let shared = CloudSyncEngine()

  /// Last error, surfaced in Jr's menu. ★ A sync that has silently stopped is
  /// worse than one that never started: the user believes they are covered.
  @Published private(set) var lastError: String?
  @Published private(set) var lastSyncAt: Date?

  private weak var favStore: FavStore?
  private weak var bmStore: BookmarkStore?
  private var pending: Task<Void, Never>?
  private var running = false

  private init() {
    NotificationCenter.default.addObserver(
      forName: NSUbiquitousKeyValueStore.didChangeExternallyNotification,
      object: NSUbiquitousKeyValueStore.default, queue: .main) { [weak self] note in
        let reason = note.userInfo?[NSUbiquitousKeyValueStoreChangeReasonKey] as? Int
        Task { @MainActor in
          if reason == NSUbiquitousKeyValueStoreQuotaViolationChange {
            self?.lastError = "iCloud storage is full — sync has stopped."
            return
          }
          self?.request()
        }
      }
    NSUbiquitousKeyValueStore.default.synchronize()
  }

  func attach(fav: FavStore, bookmarks: BookmarkStore) {
    favStore = fav
    bmStore = bookmarks
    request()
  }

  /// Coalescing, debounced — safe to call after every local write.
  func request(after seconds: Double = 1.5) {
    pending?.cancel()
    pending = Task { [weak self] in
      try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
      guard !Task.isCancelled else { return }
      await self?.syncAll()
    }
  }

  /// Serialised: two overlapping passes would each read the pre-merge document
  /// and the second would undo the first.
  func syncAll() async {
    guard !running else { return }
    running = true
    defer { running = false }
    // ★ Deliberately NOT gated on `CloudSync.available`. watchOS can report a
    // nil ubiquityIdentityToken on a device that is signed in and whose
    // key-value store works perfectly — gating on it made every sync return
    // instantly and in total silence, which is indistinguishable from "there
    // was nothing to do". Attempt the sync and let the STORE say no.
    var err: String?
    if !syncFavourites() { err = "Favourites did not sync." }
    if !syncBookmarks() { err = err ?? "Bookmarks did not sync." }
    NSUbiquitousKeyValueStore.default.synchronize()
    lastError = err
    lastSyncAt = Date()
  }

  // ── Favourites ────────────────────────────────────────────────────────────

  private func syncFavourites() -> Bool {
    guard let store = favStore else { return true }
    let now = CloudSync.nowMs()
    let local = store.favourites
    var doc = CloudSync.readDoc(CloudSync.favKey)

    var tombs = CloudSync.pruneTombs(doc.tombs, now: now)
    let localKeys = Set(local.map { CloudSync.favouriteKey($0.url) })
    let snap = CloudSync.snapshot("favourites")
    // ★★ A FAILED LOAD SKIPS THE PASS ENTIRELY, touching neither the cloud nor
    // the local list. It used to read as an empty list — indistinguishable from
    // "the user deleted everything" — so this guessed instead, refusing to
    // tombstone a wholesale disappearance. The hidden cost of that guess: the
    // LAST favourite could never be deleted. Removing it emptied the list, the
    // refusal fired, and the merge below then restored it from the cloud AND
    // SAVED IT BACK. FavStore.loadOK now reports the truth, so an empty list can
    // only mean the user emptied it, and its deletions tombstone normally.
    guard store.loadOK else { return true }
    // ★★★ ALWAYS RE-STAMP — `tombs[k] == nil` made an item PERMANENTLY UNDELETABLE
    // once its key had been tombstoned and then outranked by a re-add. See the
    // long note in src/services/cloudSync.ts; both engines must agree or one
    // device keeps reviving what the other buries.
    for k in snap where !localKeys.contains(k) { tombs[k] = now }

    // Remote first, then fold local over it, so the rule is applied per key
    // rather than one side winning wholesale.
    var byKey: [String: [String: Any]] = [:]
    for item in doc.items {
      guard let url = item["url"] as? String else { continue }
      var it = item                                   // heal a poisoned stamp on the way in
      it["updatedAt"] = CloudSync.saneStamp(it["updatedAt"], now: now)
      byKey[CloudSync.favouriteKey(url)] = it
    }
    for f in local {
      let mine = favDict(f, now: now)
      let k = CloudSync.favouriteKey(f.url)
      byKey[k] = byKey[k].map { mergeFav($0, mine) } ?? mine
    }
    for (k, at) in tombs {
      if let it = byKey[k], (it["updatedAt"] as? Double ?? 0) <= at { byKey.removeValue(forKey: k) }
    }

    // Keep this device's order, then append what only the cloud had.
    var merged: [[String: Any]] = []
    var seen = Set<String>()
    for f in local {
      let k = CloudSync.favouriteKey(f.url)
      if let it = byKey[k], !seen.contains(k) { merged.append(it); seen.insert(k) }
    }
    for item in doc.items {
      guard let url = item["url"] as? String else { continue }
      let k = CloudSync.favouriteKey(url)
      guard !seen.contains(k), let it = byKey[k] else { continue }
      merged.append(it); seen.insert(k)
    }

    let byUrl = Dictionary(local.map { (CloudSync.favouriteKey($0.url), $0) },
                           uniquingKeysWith: { a, _ in a })
    store.replaceAll(merged.compactMap { d in
      favFrom(d, existing: (d["url"] as? String).flatMap { byUrl[CloudSync.favouriteKey($0)] })
    })
    doc.items = merged
    doc.tombs = tombs
    let ok = CloudSync.writeDoc(CloudSync.favKey, doc)
    CloudSync.setSnapshot("favourites", seen)
    return ok
  }

  /// ★ The higher `visits` wins: it is a tally of real connections, so the
  /// larger number is the truer one. The newer edit wins for the descriptive
  /// fields. ★ `pin` is deliberately NOT in the document — a credential does not
  /// belong in the key-value store, so it stays on the device that saved it.
  private func favDict(_ f: Favourite, now: Double) -> [String: Any] {
    var d: [String: Any] = [
      "name": f.name,
      "url": f.url,
      "serverType": f.serverType.rawValue,
      "visits": f.visits,
      // ★★ 1, NOT `now`. See FavStore.stamp(): stamping an untimed entry with the
      // current time makes it beat every tombstone, so a favourite deleted on the
      // phone is resurrected by the watch on the very next sync — forever.
      "updatedAt": CloudSync.saneStamp(f.updatedAt, now: now),
    ]
    if let v = f.latitude  { d["latitude"] = v }
    if let v = f.longitude { d["longitude"] = v }
    if let v = f.bestSnr   { d["bestSnr"] = v }
    if let v = f.isCustom  { d["custom"] = v }
    return d
  }

  private func mergeFav(_ a: [String: Any], _ b: [String: Any]) -> [String: Any] {
    let aAt = a["updatedAt"] as? Double ?? 0
    let bAt = b["updatedAt"] as? Double ?? 0
    var out = bAt >= aAt ? a.merging(b) { _, new in new } : b.merging(a) { _, new in new }
    out["visits"] = max(a["visits"] as? Int ?? 0, b["visits"] as? Int ?? 0)
    return out
  }

  private func favFrom(_ d: [String: Any], existing: Favourite?) -> Favourite? {
    guard let name = d["name"] as? String, let url = d["url"] as? String else { return nil }
    let type = ServerType(rawValue: d["serverType"] as? String ?? "") ?? .ubersdr
    return Favourite(
      name: name, url: url, serverType: type,
      visits: d["visits"] as? Int ?? 0,
      latitude: d["latitude"] as? Double,
      longitude: d["longitude"] as? Double,
      bestSnr: d["bestSnr"] as? Double,
      // Not synced — kept from whatever this device already had.
      host: existing?.host ?? "",
      pin: existing?.pin ?? "",
      isCustom: d["custom"] as? Bool,
      updatedAt: d["updatedAt"] as? Double)
  }

  // ── Bookmarks ─────────────────────────────────────────────────────────────

  /// ★★ Everything Jr saves goes to iCloud (`synced: true`), while the phone
  /// opts in per bookmark. The asymmetry is the design: on a watch you save a
  /// handful deliberately, on a phone you accumulate dozens without thinking —
  /// and the second list is exactly what must not arrive on a 1-inch screen.
  private func syncBookmarks() -> Bool {
    guard let store = bmStore else { return true }
    let now = CloudSync.nowMs()
    let local = store.bookmarks
    var doc = CloudSync.readDoc(CloudSync.bmKey)

    let key: ([String: Any]) -> String? = { d in
      guard let n = d["name"] as? String, let f = d["frequency"] as? Double else { return nil }
      // The phone writes integral Hz; format identically or the two devices key
      // the same bookmark differently and it duplicates rather than merges.
      return "\(n)|\(Int(f.rounded()))"
    }

    var tombs = CloudSync.pruneTombs(doc.tombs, now: now)
    let localKeys = Set(local.map { "\($0.name)|\(Int($0.frequency.rounded()))" })
    let snap = CloudSync.snapshot("bookmarks")
    guard store.loadOK else { return true }                // see syncFavourites
    for k in snap where !localKeys.contains(k) { tombs[k] = now }   // always re-stamp, see syncFavourites

    var byKey: [String: [String: Any]] = [:]
    for item in doc.items {
      guard let k = key(item) else { continue }
      var it = item                                   // heal a poisoned stamp on the way in
      it["updatedAt"] = CloudSync.saneStamp(it["updatedAt"], now: now)
      byKey[k] = it
    }
    for b in local {
      let mine = bmDict(b, now: now)
      guard let k = key(mine) else { continue }
      if let theirs = byKey[k] {
        let aAt = theirs["updatedAt"] as? Double ?? 0
        let bAt = mine["updatedAt"] as? Double ?? 0
        byKey[k] = bAt >= aAt ? theirs.merging(mine) { _, new in new }
                              : mine.merging(theirs) { _, new in new }
      } else {
        byKey[k] = mine
      }
    }
    for (k, at) in tombs {
      if let it = byKey[k], (it["updatedAt"] as? Double ?? 0) <= at { byKey.removeValue(forKey: k) }
    }

    let merged = byKey.values.sorted { ($0["frequency"] as? Double ?? 0) < ($1["frequency"] as? Double ?? 0) }
    store.replaceAll(merged.compactMap(bmFrom))
    doc.items = merged
    doc.tombs = tombs
    let ok = CloudSync.writeDoc(CloudSync.bmKey, doc)
    CloudSync.setSnapshot("bookmarks", Set(merged.compactMap(key)))
    return ok
  }

  private func bmDict(_ b: Bookmark, now: Double) -> [String: Any] {
    [
      "name": b.name,
      "frequency": b.frequency.rounded(),
      "mode": b.mode.lowercased(),
      // Jr has no per-server scope — a bookmark is just a frequency you can
      // recall on whatever server reaches it — so it lands on the phone as an
      // ALL SERVERS bookmark, which is the same meaning.
      "scope": "",
      "synced": true,
      "updatedAt": CloudSync.saneStamp(b.updatedAt, now: now),
    ]
  }

  private func bmFrom(_ d: [String: Any]) -> Bookmark? {
    guard let name = d["name"] as? String, let f = d["frequency"] as? Double else { return nil }
    return Bookmark(name: name, frequency: f.rounded(),
                    mode: (d["mode"] as? String ?? "wfm").lowercased(),
                    updatedAt: d["updatedAt"] as? Double)
  }
}
