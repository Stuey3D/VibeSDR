import Foundation

/// ★★★ THE ZOOM YOU LEFT IT AT, PER RECEIVER.
///
/// Jr remembered a VibeServer's FREQUENCY and MODE per host and nothing else, so every fresh entry
/// adopted whatever span the server's first config happened to carry — which is how a watch lands
/// on a scale nobody chose. Reported from the field (GitHub #21, ff-mish, 2026-08-21: *"Zoom level
/// is too high when newly enter jr app on watch"*), and Stuart's answer was to widen it rather than
/// patch it: *"you could do the zoom memory for all services if its possible so that UberSDR
/// KiwiSDR etc remember the rate I last used."*
///
/// ★★ THE UNIT IS SPAN IN HERTZ, AND THAT IS THE WHOLE TRICK. The three backends do not agree on
///    how to say "zoom": Kiwi counts power-of-two ZOOM LEVELS from a 30 MHz full scale, OWRX carries
///    a view bandwidth bounded by the sample rate, and UberSDR/VibeServer speak BIN BANDWIDTH (span
///    ÷ bin count). None of those travel. **How wide is the window, in Hz** is a fact about the
///    picture rather than about the protocol, so it survives moving between backends, changing bin
///    counts, and a server that re-rates itself.
///
/// ★★ EACH CLIENT CLAMPS ON THE WAY OUT. This store deliberately validates nothing beyond "is it a
///    positive number": a span that is legal on a KiwiSDR is nonsense on a 192 kHz OWRX profile, and
///    only the client knows its own limits. Restoring is a REQUEST, never an assertion — every
///    caller already has bounded assert-then-adopt machinery for a server that says no, and that is
///    what must decide, not this file.
///
/// ★ Keyed by BACKEND AND HOST. The same address can serve different things over time, and two
///   backends on one host (a VibeServer and an OWRX behind the same name) have no business sharing
///   a span.
enum ViewMemory {

  /// `kind` is the backend family ("uber", "vibe", "kiwi", "owrx"); `host` its address.
  private static func key(_ kind: String, _ host: String) -> String {
    // ★ Lowercased so "Radio.local" and "radio.local" are one receiver, not two.
    "view.span.\(kind).\(host.lowercased())"
  }

  /// Remember the span currently on screen. Silently ignores nonsense so callers can fire it from
  /// hot paths without guarding.
  static func save(kind: String, host: String, spanHz: Double) {
    guard !host.isEmpty, spanHz.isFinite, spanHz > 0 else { return }
    UserDefaults.standard.set(spanHz, forKey: key(kind, host))
  }

  /// The remembered span, or nil if this receiver has never been zoomed.
  /// ★ nil is meaningfully different from a default: it means "adopt whatever the server offers",
  ///   which is the correct behaviour on a receiver you have never opened.
  static func span(kind: String, host: String) -> Double? {
    guard !host.isEmpty else { return nil }
    let v = UserDefaults.standard.double(forKey: key(kind, host))
    return v > 0 ? v : nil
  }
}
