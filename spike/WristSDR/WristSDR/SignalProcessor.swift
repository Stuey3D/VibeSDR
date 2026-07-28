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

  /// ★★ MANUAL RANGE. Auto-contrast guesses wrong exactly when a strong signal dominates the
  /// span — which is when you most want to fix it, and until now Jr had no way to
  /// (BRIEF-jr-display-manual-range.md §2: "the one real gap is RANGE").
  /// ★ The histogram still runs while manual: the noise floor it derives feeds the SNR readout,
  ///   which must stay honest regardless of how the waterfall is being coloured.
  var manualRange = false
  var manualFloorDb: Double = -110
  var manualCeilDb:  Double = -30

  /// 0–255 units. Bins below this are floored to 0, then the rest is re-stretched — this
  /// is what keeps the background black instead of a wash of grey.
  private let clipThreshold: Double = 14.97
  private let rangeMargin: Double = 5
  private let noisePercentile: Double = 0.10
  /// ★★★ CEILING = the top fraction of bins DISCARDED, so a one-bin spike cannot set the scale.
  /// Jr used the raw `absoluteMax`, which is why an Airspy at full zoom-out went enormously tall:
  /// the SDR++-style side lobes and the DC spike are a handful of very strong bins, and one of
  /// them owning the ceiling squashes every real signal into the bottom of the range.
  /// ★ 0.4% of 1024 bins ≈ 4 bins — narrower than any real signal at any zoom (an FM carrier is
  /// dozens of bins, SSB several), so nothing legitimate is excluded, and a genuinely clipping
  /// band simply saturates the top few bins to white, which is what it should look like.
  /// ★ Ported verbatim from the phone's signalProcessor.ts (PEAK_EXCLUDE_FRAC) rather than
  /// re-derived — the brief was explicit about that, and the two must agree.
  private let peakExcludeFrac: Double = 0.004
  private let minHistoryMs: Double = 2000   // noise-floor smoothing window
  private let maxHistoryMs: Double = 5000   // ceiling window — recovers faster
  // Below this visible span the 10th-percentile floor is untrustworthy: zoomed into a busy
  // band every bin is signal, the "floor" climbs into it, and the waterfall rides up to
  // white. Past this we FREEZE the floor at its last wide-view value. Mirrors the JS
  // signalProcessor.ts fix (FLOOR_FREEZE_SPAN_HZ) so Jr matches Buddy. Tunable.
  private let floorFreezeSpanHz: Double = 25_000
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
  /// Last trustworthy (wide-view) noise-floor dB, held while zoomed in past
  /// floorFreezeSpanHz. Cleared on every range flush so a band change re-learns.
  private var frozenFloorDb: Double? = nil
  /// Signal readout, near-free from the auto-range pass: passband (centre-window) peak minus
  /// the noise floor. snrDb is the SNR in dB; level is that mapped to a 0…1 bar fill.
  private(set) var snrDb: Double = 0
  private(set) var level: Double = 0
  private var prevCenterHz: Double = 0

  /// One raw dBFS frame in, one 0–255 intensity row out.
  /// ★★★ DROP EVERY DERIVED VALUE. Nothing ever called this because it did not exist: the
  /// processor is created once per client and its state — the noise-floor and ceiling
  /// histories, the frozen floor, the smoothed frame — survived every disconnect and
  /// reconnect. So a run of corrupt frames on a marginal link could latch a range that a
  /// RECONNECT COULD NOT CLEAR, and only force-quitting the app would (Stuart, 2026-07-28:
  /// "i did try reconnecting … a full force quit and restart fixed it").
  ///
  /// ★★ Same shape as the server-side bug fixed the same morning — resumeCaptureIdle
  /// restarted the capture and never reset the DSP, so stale RDS hypotheses survived a
  /// stream discontinuity. A discontinuity in the sample stream is PRECISELY when derived
  /// state should be dropped. See [[vibeserver_idle_resume_breaks_rds]].
  func reset() {
    dbAvg.removeAll(); tmp.removeAll(); outRow.removeAll()
    minHistory.removeAll(); maxHistory.removeAll()
    frozenFloorDb = nil
    prevCenterHz = 0
    snrDb = 0; level = 0
  }

  func process(_ bins: [Float], centerHz: Double, bwHz: Double) -> [UInt8] {
    let n = bins.count
    guard n > 0 else { return [] }
    // ★ systemUptime over Date(): this runs on EVERY frame, and Date() builds a calendar-
    //   capable value from wall-clock time when all we need is a monotonic stopwatch for the two
    //   history windows. It is also immune to the clock being stepped, which Date() is not — an
    //   NTP correction could otherwise expire or freeze both histories at once.
    let now = ProcessInfo.processInfo.systemUptime * 1000

    if dbAvg.count != n {
      dbAvg  = bins                      // prime from real data: no settling delay
      tmp    = [Float](repeating: 0, count: n)
      outRow = [UInt8](repeating: 0, count: n)
      minHistory.removeAll(); maxHistory.removeAll()
      frozenFloorDb = nil
    }

    // A big tune is a different band, and the old noise floor is a lie about it.
    if centerHz != 0, prevCenterHz != 0, bwHz > 0,
       abs(centerHz - prevCenterHz) > bwHz * bandFlushFrac {
      dbAvg = bins
      minHistory.removeAll(); maxHistory.removeAll()
      frozenFloorDb = nil   // new band → re-derive the floor (broadcast caveat)
    }
    if centerHz != 0 { prevCenterHz = centerHz }

    // ── Auto-range. The noise floor is the 10th percentile, not the minimum: the
    //    minimum is a single unlucky bin, the percentile is the FLOOR.
    for i in 0..<300 { hist[i] = 0 }
    var absoluteMax = -Double.infinity
    var count = 0
    var floorDb = -120.0
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
      for b in 0..<300 {
        acc += Int(hist[b])
        if acc > target { floorDb = Double(b - 280); break }
      }
      let targetMin = (floorDb - rangeMargin).rounded(.down)
      // Falls back to the true maximum when the view is too narrow for the fraction to mean
      // anything (fewer bins than it would exclude).
      var peakDb = absoluteMax
      let drop = Int(Double(count) * peakExcludeFrac)
      if drop >= 1 {
        var accTop = 0
        for b in stride(from: 299, through: 0, by: -1) {
          accTop += Int(hist[b])
          if accTop > drop { peakDb = Double(b - 280); break }
        }
      }
      let targetMax = (peakDb + rangeMargin).rounded(.up)

      // Ceiling ALWAYS tracks the strongest bin — a signal appearing as you zoom in
      // must still set the top of the scale.
      // ★ A non-finite target would poison a history that persists for seconds; and since the
      //   histories are averaged, one such entry makes every frame in the window wrong.
      guard targetMin.isFinite, targetMax.isFinite else { return outRow }
      maxHistory.append((targetMax, now))
      while let f = maxHistory.first, now - f.t > maxHistoryMs { maxHistory.removeFirst() }
      let sumMax = maxHistory.reduce(0.0) { $0 + $1.v }
      actualMaxDb = sumMax / Double(maxHistory.count) - autoContrast

      // FLOOR FREEZE. The 10th-percentile floor is only real NOISE when the view is wide
      // enough to contain noise. Zoomed into a busy band it climbs into the signal and the
      // waterfall washes to white. Past floorFreezeSpanHz hold the last wide-view floor;
      // stop pushing signal-polluted mins so zooming back out recovers cleanly. Band change
      // clears frozenFloorDb so tuning onto a broadcast re-derives. Mirrors signalProcessor.ts.
      let floorTrust = !(bwHz > 0 && bwHz < floorFreezeSpanHz)
      if floorTrust || frozenFloorDb == nil {
        minHistory.append((targetMin, now))
        while let f = minHistory.first, now - f.t > minHistoryMs { minHistory.removeFirst() }
        let avgMin = minHistory.reduce(0.0) { $0 + $1.v } / Double(minHistory.count)
        actualMinDb = avgMin + autoContrast
        if floorTrust { frozenFloorDb = avgMin }
      } else {
        actualMinDb = frozenFloorDb! + autoContrast
      }
    }
    // ★ Manual overrides the auto window AFTER it is computed, not instead of it — see the
    //   note on manualRange: the derived floor is still needed for SNR below.
    if manualRange {
      actualMinDb = min(manualFloorDb, manualCeilDb - 10)
      actualMaxDb = max(manualCeilDb, manualFloorDb + 10)
    }
    // Never let the window collapse — a 2dB range makes noise look like signal.
    if actualMaxDb - actualMinDb < 10 {
      let mid = (actualMaxDb + actualMinDb) / 2
      actualMinDb = mid - 5
      actualMaxDb = mid + 5
    }
    let dbRange = actualMaxDb - actualMinDb

    // ── SNR readout (near-free): peak of the PASSBAND — a small window around the centre,
    //    since we're VFO-locked so the tuned signal sits dead centre — minus the noise floor
    //    just found. A handful of bin reads on top of the histogram we already ran.
    let cmid = n / 2
    let cwin = max(3, n / 24)
    var pbPeak = -Double.infinity
    for i in max(0, cmid - cwin)..<min(n, cmid + cwin + 1) {
      let d = Double(bins[i]); if d.isFinite, d > pbPeak { pbPeak = d }
    }
    let snr = (pbPeak.isFinite ? pbPeak : absoluteMax) - floorDb
    // Light EMA so the bar doesn't jitter frame to frame.
    snrDb = snrDb * 0.7 + snr * 0.3
    level = min(1, max(0, snrDb / 40))          // 0–40 dB SNR → 0…1 fill

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
