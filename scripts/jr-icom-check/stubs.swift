// Stand-ins for the two Jr helpers IcomRadio.swift uses (Vitals.swift imports WatchKit, so it cannot build on macOS).
import Foundation
enum SafeText { static func clean(_ v: Any?, max: Int) -> String { String((v as? String ?? "").prefix(max)) } }
enum Wire {
  static func inRange(_ v: Any?, _ r: ClosedRange<Double>) -> Double? {
    guard let d = (v as? NSNumber)?.doubleValue, d.isFinite, r.contains(d) else { return nil }; return d
  }
}
