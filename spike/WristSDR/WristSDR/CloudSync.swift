import Foundation

/// CloudSync.swift — Jr's half of the iCloud key-value sync.
///
/// ★★ Jr is STANDALONE, so it reads and writes the shared key-value store
/// directly rather than taking state from the phone (that is Buddy's job).
/// Both apps must therefore carry the SAME literal
/// `com.apple.developer.ubiquity-kvstore-identifier`
/// (`$(TeamIdentifierPrefix)com.vibesdr.app`) — the default derives from the
/// bundle id, and Jr's differs from the phone's, which would give them two
/// separate stores that silently never see each other's writes.
///
/// ★★ The document format is the PHONE'S, byte for byte — see
/// `src/services/cloudSync.ts`. Anything that drifts here does not fail loudly;
/// it fails as a favourite that simply never appears on the other device.
///
///     { "v": 1, "items": [ … ], "tombs": { "<key>": <deletedAtMs> } }
///
/// ★ Bookmarks are the asymmetric one. The phone opts IN per bookmark; Jr marks
/// everything it saves `synced: true`, because on a watch you only ever save a
/// handful, deliberately. The filtering belongs on the big screen where the list
/// is created, not on the 1-inch one where it is read.
enum CloudSync {

  // Keys, matching the phone's CK map.
  static let favKey  = "vs.fav"
  static let bmKey   = "vs.bm"

  /// Tombstones live 90 days, as on the phone. Both sides must agree or one of
  /// them resurrects what the other deleted.
  private static let tombTTL: TimeInterval = 90 * 24 * 3600

  private static var store: NSUbiquitousKeyValueStore { .default }

  /// iCloud usable? A signed-out device still ACCEPTS writes to the store; they
  /// just never leave it, which looks exactly like a working sync.
  static var available: Bool { FileManager.default.ubiquityIdentityToken != nil }

  static func nowMs() -> Double { Date().timeIntervalSince1970 * 1000 }

  /// Canonical identity for a favourite, for MERGING only — the stored url is
  /// left alone, because that is what the app connects with.
  ///
  /// ★★ Jr saves a VibeServer as `ws://<host>`; the phone saves the same radio
  /// as `http://<host>:<port>`. Keyed on the raw string those are two different
  /// servers, so the same receiver arrived on the other device as a DUPLICATE.
  ///
  /// ★ These rules must stay identical to `favouriteKey()` in
  /// `src/services/cloudSync.ts`. A canonicaliser that disagrees between the two
  /// apps is worse than none: it splits entries that previously matched.
  static func favouriteKey(_ url: String) -> String {
    var s = url.trimmingCharacters(in: .whitespaces).lowercased()
    if s.hasPrefix("ws://")  { s = "http://"  + s.dropFirst(5) }
    if s.hasPrefix("wss://") { s = "https://" + s.dropFirst(6) }
    while s.hasSuffix("/") { s.removeLast() }
    // Drop the default port, so `https://x` and `https://x:443` are one server.
    if s.hasPrefix("http://"), let r = s.range(of: ":80", options: .backwards),
       r.upperBound == s.endIndex { s.removeSubrange(r) }
    if s.hasPrefix("https://"), let r = s.range(of: ":443", options: .backwards),
       r.upperBound == s.endIndex { s.removeSubrange(r) }
    return s
  }

  // ── Document plumbing ─────────────────────────────────────────────────────

  struct Doc {
    var items: [[String: Any]]
    var tombs: [String: Double]
  }

  static func readDoc(_ key: String) -> Doc {
    guard let raw = store.string(forKey: key),
          let data = raw.data(using: .utf8),
          let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    else { return Doc(items: [], tombs: [:]) }
    let items = obj["items"] as? [[String: Any]] ?? []
    let tombs = obj["tombs"] as? [String: Double] ?? [:]
    return Doc(items: items, tombs: tombs)
  }

  @discardableResult
  static func writeDoc(_ key: String, _ doc: Doc) -> Bool {
    let payload: [String: Any] = ["v": 1, "items": doc.items, "tombs": doc.tombs]
    guard let data = try? JSONSerialization.data(withJSONObject: payload),
          let s = String(data: data, encoding: .utf8) else { return false }
    // ★ Fail visibly rather than silently: over-quota writes are dropped by KVS
    // with no error, and a sync that has stopped looks identical to one that is
    // up to date. 1 MB is the store-wide ceiling.
    guard s.utf8.count <= 1000 * 1024 else {
      NSLog("[CloudSync] refusing oversize write to %@ (%d bytes)", key, s.utf8.count)
      return false
    }
    store.set(s, forKey: key)
    return store.synchronize()
  }

  static func pruneTombs(_ tombs: [String: Double], now: Double) -> [String: Double] {
    tombs.filter { now - $0.value < tombTTL * 1000 }
  }

  /// ★★★ A TIMESTAMP FROM THE FUTURE MAKES AN ITEM IMMORTAL. A tombstone only
  /// wins over an item OLDER than the delete — right, so a real re-add survives
  /// a stale deletion, but it means an item stamped ahead of now can never be
  /// tombstoned by anything: delete it and the next sync restores it, on every
  /// device, with no way to clear it from inside the app. (Hit for real
  /// 2026-07-28 — a favourite and a bookmark from an early test build.)
  ///
  /// A future stamp is damage, not data — a bad clock or a seconds/ms mix-up —
  /// so it reads as 1, the same "never knowingly edited" value untimed entries
  /// get, which puts it back within a tombstone's reach. Must match sane() in
  /// src/services/cloudSync.ts or the two devices disagree about what is alive.
  static func saneStamp(_ at: Any?, now: Double) -> Double {
    guard let v = at as? Double, v.isFinite, v > 0, v <= now + 5 * 60_000 else { return 1 }
    return v
  }

  /// Deletions are derived from a SNAPSHOT DIFF, exactly as on the phone: the
  /// keys present at the last sync, minus the keys present now. Deriving it
  /// means no call site can forget to record a delete — and a resurrected
  /// bookmark is the failure everyone hits with naive merges.
  /// ★★ VERSIONED — bump whenever an item's KEY FORMAT changes. Deletions are
  /// inferred from keys missing since last time, so a changed format makes every
  /// item look deleted and tombstones the lot across every device. A new version
  /// string reads as an empty snapshot, which infers no deletions: the safe
  /// direction. v2 = favourites moved from the raw url to favouriteKey().
  /// Must move in step with SNAP_VERSION in src/services/cloudSync.ts.
  private static let snapVersion = "v2"
  static func snapshot(_ name: String) -> Set<String> {
    Set(UserDefaults.standard.stringArray(forKey: "vibe.sync.snap.\(name).\(snapVersion)") ?? [])
  }
  static func setSnapshot(_ name: String, _ keys: Set<String>) {
    UserDefaults.standard.set(Array(keys), forKey: "vibe.sync.snap.\(name).\(snapVersion)")
  }
}
