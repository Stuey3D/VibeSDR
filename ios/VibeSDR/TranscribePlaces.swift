import Foundation

/* Speech language ids, reduced to the LANGUAGE: BCP-47 ("en-GB", "yue-CN") or Apple's "en_GB" → "en".
 *
 * ★ Until 2026-10-17 this also decided which language gave up one of Apple's `maximumReservedLocales` places when a
 *   download needed room (V12 467: never the phone's language, the translation target or the one in use). That whole
 *   rule went when downloads stopped KEEPING places — a model is fetched under a reservation that is released straight
 *   after, and runs without one (VibeTranscriber `SpeechEngines.fetch`). Nothing is removed to make room any more. */
enum TranscribePlaces {
  /// "en-GB" / "en_GB" / "EN" → "en".
  static func base(_ id: String) -> String {
    let s = id.split(whereSeparator: { $0 == "-" || $0 == "_" }).first.map(String.init) ?? id
    return s.lowercased()
  }
}
