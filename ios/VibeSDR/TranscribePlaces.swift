import Foundation

/* Speech language ids, reduced to the LANGUAGE: BCP-47 ("en-GB", "yue-CN") or Apple's "en_GB" → "en".
 *
 * ★ Until 2026-10-17 this also decided which language gave up one of Apple's `maximumReservedLocales` places when a
 *   download needed room (V12 467: never the phone's language, the translation target or the one in use). That rule is
 *   gone: since 2026-10-17 night the places are the phone's language + up to 4 ★ languages, put right by
 *   VibeTranscriber `SpeechEngines.reconcileKept`, and nothing is ever removed to make room — a GET with every place
 *   kept says so instead. */
enum TranscribePlaces {
  /// "en-GB" / "en_GB" / "EN" → "en".
  static func base(_ id: String) -> String {
    let s = id.split(whereSeparator: { $0 == "-" || $0 == "_" }).first.map(String.init) ?? id
    return s.lowercased()
  }
}
