import Foundation

/// THE WORK THAT MOVES TO THE WATCH.
///
/// In the shipped companion app the PHONE does all of this and hands the watch a finished
/// row: 0–255 intensities with the noise floor found, the range auto-scaled and the
/// spatial smoothing already applied. The watch just maps it through a palette.
///
/// In JR there is no phone. The watch receives raw dBFS bins and has to do this itself,
/// every frame, forever. So this is a FAITHFUL port of `src/assets/signalProcessor.ts` —
/// not a simplified stand-in. A spike that cheats here measures nothing: the whole point
/// is to find out what this costs on a wrist.
///
/// (Peak hold and the spectrum-trace EMA are omitted — the spike draws no trace, and
/// including work we would not do would inflate the number in the other direction.)
final class SignalProcessor {

  /// Symmetric dB squeeze on the auto-ranged window. UberSDR's web client uses 10, which
  /// crushes the noise floor to black; 5 is what VibeSDR ships.
  var autoContrast: Double = 5

  /// 0–255 units. Bins below this are floored to 0, then the rest is re-stretched — this
  /// is what keeps the background black instead of a wash of grey.
  private let clipThreshold: Double = 14.97
  private let rangeMargin: Double = 5
  private let noisePercentile: Double = 0.10
  private let minHistoryMs: Double = 2000   // noise-floor smoothing window
  private let maxHistoryMs: Double = 5000   // ceiling window — recovers faster
  private let bandFlushFrac: Double = 0.4

  private var dbAvg: [Float] = []
  private var tmp: [Float] = []
  private var outRow: [UInt8] = []

  /// 1 dB histogram, reused every frame. The JS original sorted all the bins with a
  /// comparator per frame and it was the single biggest cost in the profile — sub-dB
  /// precision is irrelevant here because the answer is floored, margined and then
  /// averaged over seconds anyway.
  private var hist = [UInt32](repeating: 0, count: 300)

  private var minHistory: [(v: Double, t: Double)] = []
  private var maxHistory: [(v: Double, t: Double)] = []
  private(set) var actualMinDb: Double = -120
  private(set) var actualMaxDb: Double = -20
  private var prevCenterHz: Double = 0

  /// One raw dBFS frame in, one 0–255 intensity row out.
  func process(_ bins: [Float], centerHz: Double, bwHz: Double) -> [UInt8] {
    let n = bins.count
    guard n > 0 else { return [] }
    let now = Date().timeIntervalSince1970 * 1000

    if dbAvg.count != n {
      dbAvg  = bins                      // prime from real data: no settling delay
      tmp    = [Float](repeating: 0, count: n)
      outRow = [UInt8](repeating: 0, count: n)
      minHistory.removeAll(); maxHistory.removeAll()
    }

    // A big tune is a different band, and the old noise floor is a lie about it.
    if centerHz != 0, prevCenterHz != 0, bwHz > 0,
       abs(centerHz - prevCenterHz) > bwHz * bandFlushFrac {
      dbAvg = bins
      minHistory.removeAll(); maxHistory.removeAll()
    }
    if centerHz != 0 { prevCenterHz = centerHz }

    // ── Auto-range. The noise floor is the 10th percentile, not the minimum: the
    //    minimum is a single unlucky bin, the percentile is the FLOOR.
    for i in 0..<300 { hist[i] = 0 }
    var absoluteMax = -Double.infinity
    var count = 0
    for i in 0..<n {
      let db = Double(bins[i])
      guard db.isFinite else { continue }
      count += 1
      if db > absoluteMax { absoluteMax = db }
      var b = Int(db + 280)
      if b < 0 { b = 0 } else if b > 299 { b = 299 }
      hist[b] &+= 1
    }
    if count > 0 {
      let target = Int(Double(count) * noisePercentile)
      var acc = 0
      var floorDb: Double = -120
      for b in 0..<300 {
        acc += Int(hist[b])
        if acc > target { floorDb = Double(b - 280); break }
      }
      let targetMin = (floorDb - rangeMargin).rounded(.down)
      let targetMax = (absoluteMax + rangeMargin).rounded(.up)

      minHistory.append((targetMin, now))
      while let f = minHistory.first, now - f.t > minHistoryMs { minHistory.removeFirst() }
      maxHistory.append((targetMax, now))
      while let f = maxHistory.first, now - f.t > maxHistoryMs { maxHistory.removeFirst() }

      let sumMin = minHistory.reduce(0.0) { $0 + $1.v }
      let sumMax = maxHistory.reduce(0.0) { $0 + $1.v }
      actualMinDb = sumMin / Double(minHistory.count) + autoContrast
      actualMaxDb = sumMax / Double(maxHistory.count) - autoContrast
    }
    // Never let the window collapse — a 2dB range makes noise look like signal.
    if actualMaxDb - actualMinDb < 10 {
      let mid = (actualMaxDb + actualMinDb) / 2
      actualMinDb = mid - 5
      actualMaxDb = mid + 5
    }
    let dbRange = actualMaxDb - actualMinDb

    dbAvg = bins

    // ── Spatial 5-tap smooth [1,2,3,2,1]/9. This is the expensive one: O(n) with five
    //    reads per bin, every frame.
    if n >= 5 {
      dbAvg.withUnsafeBufferPointer { a in
        tmp.withUnsafeMutableBufferPointer { t in
          t[0]     = (a[0] * 3 + a[1] * 2) / 5
          t[1]     = (a[0] + a[1] * 2 + a[2] * 2) / 5
          t[n - 1] = (a[n - 2] * 2 + a[n - 1] * 3) / 5
          t[n - 2] = (a[n - 3] + a[n - 2] * 2 + a[n - 1] * 2) / 5
          for k in 2..<(n - 2) {
            t[k] = (a[k - 2] + a[k - 1] * 2 + a[k] * 3 + a[k + 1] * 2 + a[k + 2]) / 9
          }
        }
      }
    } else {
      tmp = dbAvg
    }

    // ── Normalise → clip the floor → re-stretch. The clip is what makes black black.
    let inv = dbRange > 0 ? 1.0 / dbRange : 0
    tmp.withUnsafeBufferPointer { t in
      outRow.withUnsafeMutableBufferPointer { o in
        for j in 0..<n {
          let nrm = max(0, min(1, (Double(t[j]) - actualMinDb) * inv))
          var mag = nrm * 255
          mag = mag < clipThreshold ? 0 : ((mag - clipThreshold) / (255 - clipThreshold)) * 255
          o[j] = UInt8(max(0, min(255, mag.rounded())))
        }
      }
    }
    return outRow
  }
}
