import Foundation

/// ★★★ AN ICOM TRANSCEIVER, AS THE SERVER DESCRIBES IT — Jr's port of `src/services/icomRadio.ts`, the file the
/// app and the browser both read (ONE RULE, THREE READERS). Only the parts a watch draws are ported: the parser,
/// the merge of the `{"type":"icom"}` status line, the owner lock, the labels, the span ladder, the S-meter and
/// the banner words. Keep the wording and the rules letter for letter with the TypeScript; when one changes, the
/// other must.
///
/// ★★★ THE CONTRACT IS THE SERVER'S: docs/ICOM-SUPPORT-PLAN.md "hwinfo + wire contract for clients (built)".
///     Every list (modes, FIL widths, spans) is the RADIO's, read off hwinfo.radio — never a constant here.
/// ★★★ AN IC-7300 IS NOT AN IQ RADIO: a 475-point scope, the radio's own demodulated audio and CI-V control. So
///     everything the IQ path powers (gain, rate, VibeAGC, IF ladder, RDS, DAB, our squelch, FM DSP, digital zoom)
///     does not exist and must not be DRAWN (AGENTS.md: "a control that only works on one radio should not be
///     there").
/// ★★ RECEIVE ONLY. Nothing in this file builds a message that could key the radio: `controlMessage` emits only
///    the receive-side fields the server's `icom_control` accepts (mode, FIL, span), and only values the radio
///    itself listed.
/// ★ arm64_32: Swift `Int` is 32-bit below watchOS 27, so every frequency and span is a Double.
struct IcomRadio: Equatable {
  var model: String
  var modes: [String]
  var modeNow: String
  var filters: [Int]
  var filNow: Int
  /// FIL1/2/3 FACTORY widths (Hz) per mode family (ssb / cw / rtty / am / fm). Approximate: the owner can
  /// change them in the radio and CI-V does not report it.
  var filterWidths: [String: [Double]]
  /// The scope's span ladder, TOTAL widths in Hz (5 kHz … 1 MHz on a 7300).
  var spans: [Double]
  var span: Double
  /// The radio's receive range, Hz — the outer edges of hwinfo `ranges` (30 kHz – 74.8 MHz on a 7300). 0 = unknown.
  var rangeLo: Double
  var rangeHi: Double
  var tx: Bool
  /// ★ ABSENT is not DOWN — only an explicit false says the radio stopped answering.
  var linkUp: Bool
  /// ★ The server's default is ON; an absent field must not unlock the radio in the UI.
  var ownerLock: Bool

  /// hwinfo.radio → IcomRadio, defensively (a field of the wrong type is dropped, never trusted). nil = not an Icom.
  static func parse(_ r: [String: Any]) -> IcomRadio? {
    guard (r["driver"] as? String)?.lowercased() == "icom" else { return nil }
    let sc = r["scope"] as? [String: Any] ?? [:]
    var fw: [String: [Double]] = [:]
    if let m = r["filterWidths"] as? [String: Any] {
      for (k, v) in m { let w = nums(v); if !w.isEmpty { fw[k] = w } }
    }
    var lo = Double.infinity, hi = 0.0
    for pair in r["ranges"] as? [Any] ?? [] {
      let p: [Double] = nums(pair)
      guard p.count == 2, p[0] >= 0, p[1] > p[0] else { continue }
      lo = min(lo, p[0]); hi = max(hi, p[1])
    }
    if hi <= 0 { lo = 0 }
    return IcomRadio(
      model: (r["model"] as? String).flatMap { $0.isEmpty ? nil : SafeText.clean($0, max: 32) } ?? "Icom",
      modes: strs(r["modes"]),
      modeNow: (r["modeNow"] as? String ?? "").lowercased(),
      filters: nums(r["filters"]).map { Int($0) }.filter { $0 > 0 && $0 < 10 },
      filNow: Int(num(r["filNow"]).clamped(0, 9)),
      filterWidths: fw,
      spans: nums(sc["spans"]).filter { $0 > 0 && $0 <= 1.0e8 },
      span: num(sc["span"]),
      rangeLo: lo,
      rangeHi: hi,
      tx: flag(r["tx"]),
      linkUp: !isFalse(r["linkUp"]),
      ownerLock: !isFalse(r["ownerLock"]))
  }

  /// ★ The status line is NEWER than the last hwinfo for the fields it carries — the server sends hwinfo only on
  /// RF/scope/TX/link changes, the line on every one. (mergeIcomStatus in the TypeScript.)
  func merged(_ st: IcomStatus) -> IcomRadio {
    var r = self
    if !st.mode.isEmpty { r.modeNow = st.mode }
    if st.fil > 0 { r.filNow = st.fil }
    r.tx = st.tx
    r.linkUp = st.link
    if st.span > 0 { r.span = st.span }
    return r
  }

  // ── Who may do what ─────────────────────────────────────────────────────────────────────────────
  /// ★ Tune, mode, FIL and span: anyone unless the owner lock is on, then a SIGNED-IN admin (this socket's
  ///   `admin_unlock`, which the server reports as `adminOk`). Exactly as the server judges it.
  func mayTune(admin: Bool) -> Bool { admin || !ownerLock }
  /// The words a listener sees under the owner lock — the app's and the browser's (icomFollowNote).
  static let followNote = "Everyone hears what the radio is tuned to — the owner tunes it."

  /// What the banner says, or "" for none. TX outranks link-down (a transmitting radio is answering).
  /// ★ The server has ALREADY muted the audio and frozen the waterfall during TX — the silence is not a fault,
  ///   and the banner is what says so.
  var banner: String {
    if tx { return "The radio is transmitting — receive paused" }
    if !linkUp { return "The radio is not answering — VibeServer is retrying" }
    return ""
  }
  /// The radio sends no sweeps (and perhaps no audio) ON PURPOSE — not a dead link, so no reconnect, no
  /// "reconnecting" pill.
  var quiet: Bool { tx || !linkUp }

  // ── Labels ──────────────────────────────────────────────────────────────────────────────────────
  /// The radio's mode as its own panel writes it: usbd → USB-D, cwr → CW-R, rttyr → RTTY-R.
  static func modeLabel(_ m: String) -> String {
    let s = m.lowercased()
    if s.isEmpty { return "" }
    if ["usbd", "lsbd", "amd", "fmd"].contains(s) { return "\(s.dropLast().uppercased())-D" }
    if s == "cwr" { return "CW-R" }
    if s == "rttyr" { return "RTTY-R" }
    return s.uppercased()
  }
  /// The radio mode → the client vocabulary (the server's own icomToClientMode, for the passband).
  static func clientMode(_ m: String) -> String {
    switch m.lowercased() {
    case "usb", "usbd", "rttyr": return "usb"
    case "lsb", "lsbd", "rtty": return "lsb"
    case "am": return "am"
    case "cw": return "cwu"
    case "cwr": return "cwl"
    case "fm": return "nfm"
    default: return "usb"
    }
  }
  /// The FIL width family a mode belongs to — the server's icom::widthFamily(), letter for letter.
  static func widthFamily(_ mode: String) -> String {
    switch (mode.isEmpty ? "usb" : mode.lowercased()).first {
    case "l", "u": return "ssb"
    case "c": return "cw"
    case "r": return "rtty"
    case "a": return "am"
    case "f": return "fm"
    default: return "ssb"
    }
  }
  /// FIL1/2/3 widths (Hz) for the current mode. [] = unknown.
  var widthsNow: [Double] { filterWidths[Self.widthFamily(modeNow)] ?? [] }
  /// "FIL2 2.4k", or "FIL2" when the radio published no width for it.
  func filLabel(_ f: Int) -> String {
    let w = widthsNow
    return f >= 1 && f <= w.count ? "FIL\(f) \(Self.fmtWidth(w[f - 1]))" : "FIL\(f)"
  }
  /// 2400 → "2.4k", 500 → "500", 15000 → "15k".
  static func fmtWidth(_ hz: Double) -> String {
    guard hz > 0 else { return "" }
    if hz < 1000 { return String(Int(hz.rounded())) }
    let k = hz / 1000
    return k == k.rounded() ? "\(Int(k))k" : String(format: "%.1fk", k).replacingOccurrences(of: ".0k", with: "k")
  }
  /// The passband DRAWN for the radio's FIL (factory widths, so approximate) — written, never sent. The
  /// browser's icomPassband(). nil = no width known.
  func passband(defaultLow: Double, defaultHigh: Double) -> (low: Double, high: Double)? {
    let w = widthsNow
    guard filNow >= 1, filNow <= w.count, w[filNow - 1] > 0 else { return nil }
    let width = w[filNow - 1]
    switch Self.clientMode(modeNow) {
    case "usb": return (50, 50 + width)
    case "lsb": return (-(50 + width), -50)
    case "cwu", "cwl":
      let c = (defaultLow + defaultHigh) / 2
      return ((c - width / 2).rounded(), (c + width / 2).rounded())
    default: return (-(width / 2).rounded(), (width / 2).rounded())
    }
  }

  // ── The span ladder (ZOOM IS THE SPAN LADDER) ─────────────────────────────────────────────────
  /// The index of the rung nearest `totalHz` (log distance, like the server's nearestSpan). -1 = no ladder.
  static func nearestSpanIndex(_ spans: [Double], _ totalHz: Double) -> Int {
    var best = -1, bd = Double.infinity
    for (i, s) in spans.enumerated() {
      let d = abs(log(s / max(1, totalHz)))
      if d < bd { bd = d; best = i }
    }
    return best
  }
  /// One rung narrower (dir > 0 = zoom in) or wider from `current`; the same rung back when there is none
  /// further. 0 = no ladder.
  static func spanStep(_ spans: [Double], current: Double, dir: Int) -> Double {
    guard !spans.isEmpty else { return 0 }
    let sorted = spans.sorted()
    let i = nearestSpanIndex(sorted, current)
    let j = max(0, min(sorted.count - 1, i + (dir > 0 ? -1 : dir < 0 ? 1 : 0)))
    return sorted[j]
  }

  // ── The S-meter (from the status line — NEVER from SPEC bins, which are relative to REF) ─────────
  /// dB-over-S9 → ("S7" | "S9+20", 0..1 along a bar S0 … S9+60). 6 dB per S-unit below S9 (the IARU
  /// convention the server's calibration uses). nil = the meter has not been read.
  static func sMeter(_ dbOverS9: Double?) -> (text: String, frac: Double)? {
    guard let raw = dbOverS9, raw.isFinite else { return nil }
    let db = max(-54, min(60, raw))
    // ★ A real meter's bar: S0-S9 fills the first 60 % (54 dB), S9+60 the rest (60 dB).
    let frac = db <= 0 ? ((db + 54) / 54) * 0.6 : 0.6 + (db / 60) * 0.4
    if db >= 1 { return ("S9+\(Int(db.rounded()))", frac) }
    let s = max(0, min(9, Int((9 + db / 6).rounded())))
    return ("S\(s)", frac)
  }

  // ── What a client SENDS ─────────────────────────────────────────────────────────────────────────
  /// ★ `{"type":"icom_control", …}` with ONLY values the radio's own lists contain — a stale UI can never send a
  ///   value the server would have to refuse. nil = nothing left. ★★ Receive-side fields only, by design: Jr
  ///   offers mode and FIL; the span travels as an ordinary `zoom` (the server maps it to the nearest rung).
  func controlMessage(mode: String? = nil, fil: Int? = nil) -> [String: Any]? {
    var out: [String: Any] = [:]
    if let m = mode, modes.contains(m) { out["mode"] = m }
    if let f = fil, filters.contains(f) { out["fil"] = f }
    guard !out.isEmpty else { return nil }
    out["type"] = "icom_control"
    return out
  }

  // ── Defensive readers (the TypeScript's num/str/bool/nums/strs) ─────────────────────────────────
  private static func num(_ v: Any?) -> Double {
    guard let d = (v as? NSNumber)?.doubleValue, d.isFinite else { return 0 }
    return d
  }
  private static func flag(_ v: Any?) -> Bool { (v as? Bool) ?? false }
  private static func isFalse(_ v: Any?) -> Bool { (v as? Bool) == false }
  private static func nums(_ v: Any?) -> [Double] {
    (v as? [Any] ?? []).compactMap { ($0 as? NSNumber)?.doubleValue }.filter { $0.isFinite }
  }
  private static func strs(_ v: Any?) -> [String] {
    (v as? [Any] ?? []).compactMap { $0 as? String }.map { $0.lowercased() }.filter { !$0.isEmpty && $0.count <= 12 }
  }
}

/// The `{"type":"icom", …}` status line — on join, on every change, the S-meter at most 4 times a second.
struct IcomStatus {
  var freq: Double
  var mode: String
  var fil: Int
  /// dB relative to S9 (0 = S9, −54 = S0, +60 = S9+60). nil when the meter has not been read.
  var sDbOverS9: Double?
  var tx: Bool
  var link: Bool
  var span: Double

  static func parse(_ m: [String: Any]) -> IcomStatus? {
    guard m["type"] as? String == "icom" else { return nil }
    let sRaw = Wire.inRange(m["sRaw"], -1.0e6...1.0e6) ?? -1
    let sDb = Wire.inRange(m["sDbOverS9"], -998...998)
    return IcomStatus(
      freq: Wire.inRange(m["freq"], 0...1.0e11) ?? 0,
      mode: (m["mode"] as? String ?? "").lowercased(),
      fil: Int((Wire.inRange(m["fil"], 0...9) ?? 0).rounded()),
      sDbOverS9: sRaw >= 0 ? sDb : nil,
      tx: (m["tx"] as? Bool) ?? false,
      link: (m["link"] as? Bool) != false,
      span: Wire.inRange(m["span"], 0...1.0e8) ?? 0)
  }
}

private extension Double {
  func clamped(_ lo: Double, _ hi: Double) -> Double { Swift.max(lo, Swift.min(hi, self)) }
}
