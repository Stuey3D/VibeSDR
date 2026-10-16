import Foundation

/* ★★★ WHICH SPEECH LANGUAGE GIVES UP ITS PLACE — pure, so it is tested (scripts/test_transcribePlaces_swift.sh).
 *
 * Apple lets an app keep `AssetInventory.maximumReservedLocales` speech languages (5 so far). A download the user
 * asked for makes room by giving one back. V12 467 (2026-10-16, Stuart, an English iPhone on a Vietnamese broadcast)
 * gave back ENGLISH: the old rule ranked places only by how recently each was TRANSCRIBED, and the phone's own
 * language is rarely heard on shortwave — but it is the language every translation goes INTO and the one the user
 * speaks. Least heard is not least used.
 *
 * So:
 *  1. PROTECTED languages are never chosen: the phone's own language(s), the translation target, the language being
 *     transcribed right now, and the one the place is being made for.
 *  2. Use is counted per LANGUAGE (en-GB and en-US are one English), and being the translation target counts as use.
 *  3. Every place protected → nothing is removed; the user is asked (REMOVE in the LANGUAGE list), never surprised.
 *
 * Ids are BCP-47 ("en-GB", "yue-CN") or Apple's "en_GB" — both reduce to the base code here. */
enum TranscribePlaces {
  enum Decision: Equatable {
    /// A place is free (or the language already holds one): reserve, remove nothing.
    case room
    /// Give back this held place (an id from `held`) first.
    case remove(String)
    /// Every held place is protected: remove nothing, ask the user which to give back.
    case ask
  }

  /// "en-GB" / "en_GB" / "EN" → "en".
  static func base(_ id: String) -> String {
    let s = id.split(whereSeparator: { $0 == "-" || $0 == "_" }).first.map(String.init) ?? id
    return s.lowercased()
  }

  /// held: the app's reserved ids. lastUsed: id OR base code → seconds since 1970 (absent = never).
  /// protectedBases: base codes that must keep their place (the incoming language is always added).
  static func decide(held: [String], max: Int, incoming: String,
                     protectedBases: Set<String>, lastUsed: [String: Double]) -> Decision {
    let inc = base(incoming)
    if held.contains(where: { base($0) == inc }) { return .room }    // already holds a place
    if held.count < max { return .room }
    let keep = Set(protectedBases.map { base($0) }).union([inc])
    // A language's last use is the latest of any of its variants, or of its bare code (the translation target).
    var used: [String: Double] = [:]
    for (k, t) in lastUsed { let b = base(k); used[b] = Swift.max(used[b] ?? 0, t) }
    let free = held.filter { !keep.contains(base($0)) }
    guard let victim = free.min(by: { a, b in
      let ua = used[base(a)] ?? 0, ub = used[base(b)] ?? 0
      return ua != ub ? ua < ub : a < b                              // ties: stable, by id
    }) else { return .ask }
    return .remove(victim)
  }
}
