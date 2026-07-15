import Foundation
import CoreGraphics
import SwiftUI

/// Scrolling RGBA pixel buffer for the wrist waterfall.
///
/// Three things conspire to make a naive implementation judder, and each needs
/// its own fix:
///
///  1. **WCSession is not a stream.** It delivers rows in BURSTS — several land
///     together, then nothing for a few hundred ms. Blitting on arrival lurches
///     no matter how you interpolate, because during a gap there is nothing to
///     interpolate towards. Fix: a JITTER BUFFER — queue arrivals, drain on a
///     steady clock (`tick`). Same medicine as the rtl_tcp client buffer.
///  2. **The feed is ~10fps**, so one row per frame is a slow march of fat bands.
///     Fix: INTERPOLATE UP — synthesise `subRows` blended rows between each pair
///     of received rows, so the waterfall scrolls at ~20 rows/sec.
///     But know what this buys: interpolation hides a low FRAME RATE, it does NOT
///     recover MISSING DATA. We tried 5fps and it read as mush — on SSB speech the
///     energy changes on a ~100ms timescale, so half the syllables were simply
///     never sampled, and no blend brings them back. Smooth is not the same as
///     legible.
///  3. **Scaling.** 256 bins is ABOVE the watch's ~205pt width, so the image is
///     DOWNscaled — sharp. (It was 128, which meant upscaling every column 1.6x:
///     a self-inflicted blur no sharpening can undo.) Bilinear still smooths the
///     row banding vertically, and the fat bins from a zoomed-out phone.
final class WaterfallBuffer {
  /// MUST MATCH WATCH_BINS in watchProvider.ts — rows of any other length are
  /// dropped, so a mismatch shows as a blank waterfall.
  static let width  = 256
  /// One row of headroom beyond what's shown: the newest row lives just ABOVE the
  /// visible edge and slides down into view, which is what makes the scroll a
  /// glide rather than a step.
  ///
  /// NEWEST AT THE TOP, ageing downward — the opposite of the phone. On a watch
  /// the chrome (ticker + frequency bar) lives at the BOTTOM, so a bottom-up
  /// waterfall would give birth to every new row underneath it: you'd only see a
  /// new signal once it had already scrolled clear of the furniture. Tuning is
  /// done off the newest row, so the newest row must be the one in clear air.
  static let height  = 89
  static let visible = 88

  /// Synthesised rows per received row. 2 x 10fps = ~20 rows/sec of scroll.
  ///
  /// Fewer than before, deliberately: the feed went back to 10fps of REAL data,
  /// so there is less to invent. Synthesised rows smooth the scroll but carry no
  /// information — the fewer of them between real samples, the more of what you
  /// see is something the receiver actually heard.
  private let subRows = 2

  /// 0..1 extra temporal blend from the phone's settings, on top of the
  /// interpolation. 0 = rely on interpolation alone.
  var smoothing: Double = 0.0

  /// 0-10, mirrors the phone's waterfall sharpness.
  ///
  /// The watch NEEDS its own: the phone applies sharpness in its SkSL shader, not
  /// in SignalProcessor, so the row we borrow arrives unsharpened — and we then
  /// bilinear-upscale it, which softens it further. An SSB signal is only ~12 of
  /// the 256 bins wide, so it loses the most and goes mushy.
  var sharpness: Double = 0.0

  // ── WATCH-LOCAL BRIGHTNESS / CONTRAST ──────────────────────────────────────
  //
  // The phone's render settings are MIRRORED and stay the base — the wrist should
  // look like the phone. But the same settings do not serve both screens: the phone
  // is big, bright and looked at directly; the watch is small, often outdoors, often
  // at an angle, and a waterfall that reads fine on a phone can be near-black on a
  // wrist. Forcing one set of numbers to serve both means blowing out the phone just
  // to see the watch — so the watch gets its OWN offsets, applied ON TOP.
  //
  // Applied at INGEST, through a 256-entry tone table, so the cost is one lookup per
  // pixel and the waterfall, the spectrum trace and the peak line all agree (they're
  // all derived from the same row).
  var brightness: Double = 0 { didSet { buildTone() } }   // -1…+1
  var contrast:   Double = 0 { didSet { buildTone() } }   // -1…+1
  private var tone = [UInt8](repeating: 0, count: 256)

  private func buildTone() {
    for i in 0..<256 {
      var v = Double(i) / 255
      // Contrast: an S-curve about the midpoint, same shape as the phone's.
      if contrast != 0 {
        let k = contrast * 0.9                     // keep it short of a hard step
        v = v < 0.5 ? pow(v * 2, 1 - k) / 2
                    : 1 - pow((1 - v) * 2, 1 - k) / 2
      }
      // Brightness: a straight lift. Applied AFTER contrast so raising brightness
      // lifts the floor rather than crushing the top.
      v += brightness * 0.5
      tone[i] = UInt8(max(0, min(255, (v * 255).rounded())))
    }
    cached = nil
  }

  private var pixels: [UInt8]
  private var lut: [UInt8]
  private var cached: CGImage?

  // Jitter buffer + interpolation state
  private var queue: [[UInt8]] = []
  private var prevRow: [UInt8]          // last fully-applied row (blend source)
  private var target: [UInt8]?          // row we're interpolating towards
  private var subStep = 0

  /// Scroll clock, in SUB-rows.
  private var accum: Double = 0
  private var lastTick: CFTimeInterval = 0
  private var lastArrivalAt: CFTimeInterval = 0
  /// Measured arrival cadence of RECEIVED rows (not sub-rows). Seeded at the
  /// expected 10fps so the first few frames don't glide against a wrong guess.
  private var interval: CFTimeInterval = 0.1
  private var arrivals = 0

  /// How many rows to bank before drawing — i.e. how much LATENCY we deliberately
  /// accept to insure against a late row.
  ///
  /// This was 2, and 2 rows at ~10fps is ~200ms of delay ON TOP of the WCSession hop
  /// (~240ms). That total is invisible while you're just watching a signal, but it is
  /// very visible while you TUNE — most of all on WFM at 100kHz steps, where each
  /// detent moves the whole picture and you can see it arrive late.
  ///
  /// 1 row halves our half of it. The insurance is worth less than it was: the row
  /// feed is far healthier now than when 2 was chosen (it was picked back when rows
  /// were being dropped by a too-tight send gate). If a row does arrive late we drop
  /// back to prefilling, which is the same recovery as before — just entered slightly
  /// more often.
  private let targetDepth = 1.0

  /// PREFILL. A jitter buffer that starts draining while empty alternates between
  /// running dry and catching up — which is a stutter, and it's why the first
  /// second after launch (and after a screen wake) looked rough. Hold still until
  /// the buffer is banked, then drain; drop back to holding if we ever run dry.
  private var prefilling = true

  /// Sub-row offset for the renderer, 0..1.
  var progress: Double { min(1, max(0, accum)) }

  /// The most recently drawn row — i.e. exactly what the top of the waterfall is
  /// showing. The waterfall wants this raw.
  private(set) var liveRow: [UInt8] = []

  /// The spectrum trace's own copy, TIME-SMOOTHED.
  ///
  /// The raw row is what the waterfall should show — every twitch of it is real
  /// data, and the waterfall's job is to record it. But a trace redrawn from raw
  /// bins 20x a second is unreadable noise: the eye can't integrate a jittering
  /// line the way it integrates a scrolling texture. The phone solves this with a
  /// 5-frame EMA on its spectrum (SignalProcessor.smoothingFrames) while feeding
  /// the waterfall unsmoothed — so we do the same, and for the same reason.
  private(set) var specRow: [Double] = []

  /// PEAK HOLD, mirrored from the phone.
  ///
  /// Same behaviour as the phone's SignalProcessor: rise instantly to the current
  /// value, then decay. The phone decays in dB (10 dB/s) — we can't, because the rows
  /// arrive ALREADY NORMALISED to 0-255 with the dB range baked in, so there is no dB
  /// scale here to decay along. The equivalent rate over a typical 80 dB window is
  /// ~32 units/sec, which is what this is: the same fall, expressed in the only units
  /// the wrist has.
  ///
  /// It tracks the SMOOTHED trace (specRow), not the raw row — the peak of a line
  /// that jitters is just noise held up on a stick.
  private(set) var peakRow: [Double] = []
  var peakHold = true { didSet { if !peakHold { peakRow = [] } } }
  private let peakDecayPerSec = 32.0
  /// Trace smoothing.
  ///
  /// Raised from 0.22. The rows now arrive at 10/sec (batched two to a message, to stop
  /// us over-driving WCSession) instead of 16 — so the trace sees 60% of the real
  /// samples it used to, and every one of them is being smoothed just as hard. The
  /// result reads as SLUGGISH: the waterfall still looks right, because it's a
  /// scrolling texture and the eye integrates it happily, but a LINE redrawn from
  /// fewer samples with the same lag is the thing you notice — most of all while
  /// ZOOMING, where the whole trace has to re-settle.
  ///
  /// The smoothing exists because a trace drawn from raw bins 20x/sec is unreadable
  /// noise. It does not need to be this heavy at a lower sample rate. Track faster.
  private let specAlpha = 0.4

  // Diagnostics — is the render loop actually ticking, and is the queue healthy?
  private(set) var renderFps: Double = 0
  var queueDepth: Int { queue.count }
  var arrivalMs: Int { Int(interval * 1000) }

  init() {
    pixels  = [UInt8](repeating: 0, count: Self.width * Self.height * 4)
    prevRow = [UInt8](repeating: 0, count: Self.width)
    // Greyscale until the phone sends its palette, so a dropped settings message
    // shows a working waterfall rather than a black rectangle.
    var l = [UInt8](repeating: 255, count: 256 * 4)
    for i in 0..<256 {
      l[i * 4 + 0] = UInt8(i); l[i * 4 + 1] = UInt8(i); l[i * 4 + 2] = UInt8(i)
    }
    lut = l
    buildTone()
  }

  /// Have we ever been given the phone's real palette? Until then we draw in the
  /// greyscale fallback — which is a working waterfall, but not the one the user chose.
  private(set) var hasLUT = false

  func setLUT(_ newLUT: [UInt8]) {
    hasLUT = true
    lut = newLUT
    cached = nil
  }

  /// A colour from the phone's own palette. The spectrum trace is drawn in these
  /// rather than in white: white fights the system clock for attention, and a
  /// trace coloured out of the same LUT as the waterfall reads as part of it.
  func lutColor(_ v: Int, opacity: Double = 1) -> Color {
    let i = min(255, max(0, v)) * 4
    return Color(
      red:   Double(lut[i    ]) / 255,
      green: Double(lut[i + 1]) / 255,
      blue:  Double(lut[i + 2]) / 255
    ).opacity(opacity)
  }

  /// The trace's hue: the palette's colour, but never bright enough to fight the
  /// system clock.
  ///
  /// watchOS draws the time itself and gives us no way to recolour or hide it, so
  /// the palette has to yield rather than the clock. Most palettes are well under
  /// the cap and come through untouched — but Greyscale and Black Hot top out at
  /// pure WHITE, which is exactly the clock's colour. Those get held down to a
  /// mid-tone: still perfectly legible as a trace, no longer competing.
  func traceColor(_ v: Int = 205, opacity: Double = 1) -> Color {
    let i = min(255, max(0, v)) * 4
    var r = Double(lut[i]) / 255, g = Double(lut[i + 1]) / 255, b = Double(lut[i + 2]) / 255

    // Rec. 709 relative luminance — how bright it actually LOOKS, not its max channel.
    let lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
    let cap = 0.62
    if lum > cap {
      let k = cap / lum
      r *= k; g *= k; b *= k
    }
    return Color(red: r, green: g, blue: b).opacity(opacity)
  }

  /// A row arrived. Queue it — do NOT draw here.
  func push(row: [UInt8]) {
    guard row.count == Self.width else { return }

    let now = ProcessInfo.processInfo.systemUptime
    if lastArrivalAt > 0 {
      let dt = now - lastArrivalAt
      // Only learn from plausible gaps: a burst (dt~0) and a stall (watch app was
      // backgrounded) would both poison the cadence estimate.
      if dt > 0.05, dt < 1.0 {
        // Converge FAST for the first few rows, then settle. Starting from a
        // guessed cadence and creeping towards the real one is itself a stutter.
        arrivals += 1
        let alpha = arrivals < 5 ? 0.5 : 0.1
        interval += alpha * (dt - interval)
      }
    }
    lastArrivalAt = now

    // Tone (watch-local brightness/contrast) applied ONCE, here — so the waterfall,
    // the trace and the peak line all see the same numbers.
    queue.append(sharpen(row).map { tone[Int($0)] })
    // If we somehow can't keep up, drop the OLDEST: the newest row is the one that
    // matters, and a growing queue is just latency. The cap was 8 — i.e. we were
    // willing to sit ~800ms behind the radio before throwing anything away, which is
    // most of the lag we're trying to remove. 4 is still ample slack for a hiccup.
    if queue.count > 4 { queue.removeFirst(queue.count - 4) }
  }

  /// Unsharp mask across the bins: subtract a blurred copy of the row from itself,
  /// which pulls a narrow carrier back up out of the smear that bilinear upscaling
  /// puts it into. Horizontal only — blurring along TIME is what makes the scroll
  /// look continuous, so we must not undo that.
  private func sharpen(_ row: [UInt8]) -> [UInt8] {
    guard sharpness > 0 else { return row }
    let amt = sharpness / 10 * 1.6
    var out = row
    let n = row.count
    for i in 0..<n {
      let l = Double(row[max(0, i - 1)])
      let m = Double(row[i])
      let r = Double(row[min(n - 1, i + 1)])
      let blur = (l + 2 * m + r) / 4          // 3-tap Gaussian
      out[i] = UInt8(clamping: Int((m + amt * (m - blur)).rounded()))
    }
    return out
  }

  /// Peak hold off (or a fresh view) — drop what's held. A stale peak line would sit
  /// there implying a signal that isn't there any more.
  func clearPeaks() { peakRow = [] }

  /// Screen woke / link came back. The queue holds stale rows and the clock is
  /// stale, so drain-as-usual would fast-forward through old data and then run
  /// dry. Start clean and re-prefill instead.
  func reset() {
    queue.removeAll()
    target = nil
    subStep = 0
    accum = 0
    lastTick = 0
    lastArrivalAt = 0
    arrivals = 0
    prefilling = true
  }

  /// Advance the scroll clock. Called every redraw (~30fps), NOT on arrival.
  /// Advance the trace and the peak line ON THE RENDER CLOCK, every frame.
  ///
  /// The trace's EMA used to run inside blit(), i.e. only when a ROW ARRIVED. So at 10
  /// rows/sec it stepped ten times a second no matter that we were redrawing at 20fps:
  /// the DRAWING was smooth and the VALUE was stepping, which reads as a low frame rate.
  /// (It got worse when we batched rows to protect the link — fewer, lumpier arrivals.)
  ///
  /// Advancing it here instead means the line eases toward the newest row EVERY FRAME,
  /// between arrivals — the same averaging, twice the motion, and completely decoupled
  /// from the message rate. Interpolation is exactly what the waterfall already does
  /// with its sub-rows; the trace simply never got the same treatment.
  private func advanceTrace(dt: CFTimeInterval) {
    guard !liveRow.isEmpty, specRow.count == liveRow.count else { return }

    // Time-based, so the feel doesn't change if the render rate does.
    //
    // 0.10, not 0.18. The trace's own smoothing was adding as much delay as the batching
    // does — and it is the one part of the chain that costs nothing to shorten. You can
    // SEE the total: a voice starts and the trace lifts a moment later. The rest of that
    // delay is structural (the WCSession hop, the batch, the jitter buffer); the AUDIO
    // never crosses the watch link at all, so it will always arrive first.
    //
    // The averaging still has to exist — a line redrawn from raw bins is unreadable
    // noise — but it is now advanced every FRAME rather than every row, so a short time
    // constant no longer makes it jittery.
    let tc = 0.10                                    // seconds to close ~63% of the gap
    let a = min(1, max(0, dt / tc))
    for i in 0..<specRow.count {
      specRow[i] += (Double(liveRow[i]) - specRow[i]) * a
    }

    guard peakHold else { return }
    if peakRow.count != specRow.count { peakRow = specRow; return }
    let fall = peakDecayPerSec * dt
    for i in 0..<peakRow.count {
      peakRow[i] = specRow[i] > peakRow[i] ? specRow[i] : max(0, peakRow[i] - fall)
    }
  }

  func tick(at now: CFTimeInterval) {
    defer { lastTick = now }
    guard lastTick > 0, interval > 0 else { return }
    let dt = min(0.25, now - lastTick)   // a long gap (app resumed) must not fling
    if dt > 0 { renderFps += 0.1 * (1.0 / dt - renderFps) }
    advanceTrace(dt: dt)

    // Hold still until the buffer has banked enough to drain smoothly.
    if prefilling {
      guard Double(queue.count) >= targetDepth else { return }
      prefilling = false
    }

    // Drain rate tracks queue pressure: run slightly fast when backing up, slow
    // when running dry. Keeps the scroll smooth across a feed whose true rate
    // drifts (foreground vs locked) without ever stalling or visibly fast-forwarding.
    let depth = Double(queue.count) + (target != nil ? 1 : 0)
    let rate: Double
    if depth == 0                   { rate = 0; prefilling = true }  // dry — re-prefill
    else if depth > targetDepth + 2 { rate = 1.5 }                   // behind — catch up
    else                            { rate = 0.85 + 0.15 * (depth / targetDepth) }

    let subInterval = interval / Double(subRows)
    accum += (dt / subInterval) * rate

    while accum >= 1 {
      guard emitSubRow() else { break }
      accum -= 1
    }
    // Ran dry mid-row: park at the boundary rather than drifting past it.
    if target == nil && queue.isEmpty { accum = min(accum, 1) }
  }

  /// Blit ONE synthesised row, interpolating from the last applied row towards
  /// the row we're heading for. Returns false when there's nothing to draw.
  private func emitSubRow() -> Bool {
    if target == nil {
      guard !queue.isEmpty else { return false }
      target = queue.removeFirst()
      subStep = 0
    }
    guard let dst = target else { return false }

    subStep += 1
    let t = Double(subStep) / Double(subRows)

    var row = [UInt8](repeating: 0, count: Self.width)
    for i in 0..<Self.width {
      let v = Double(prevRow[i]) + (Double(dst[i]) - Double(prevRow[i])) * t
      row[i] = UInt8(clamping: Int(v.rounded()))
    }
    blit(row)

    if subStep >= subRows {
      prevRow = dst
      target = nil
    }
    return true
  }

  private func blit(_ row: [UInt8]) {
    liveRow = row

    // Prime the trace from the first real row (no settle-in from zero). It is then
    // advanced on the RENDER clock — see advanceTrace() — NOT here.
    if specRow.count != row.count { specRow = row.map(Double.init) }


    let stride = Self.width * 4
    // Scroll DOWN one row, then paint the newest along the TOP edge.
    pixels.withUnsafeMutableBytes { raw in
      guard let base = raw.baseAddress else { return }
      memmove(base.advanced(by: stride), base, stride * (Self.height - 1))
    }

    for x in 0..<Self.width {
      let l = Int(row[x]) * 4
      let p = x * 4
      pixels[p + 0] = lut[l + 0]
      pixels[p + 1] = lut[l + 1]
      pixels[p + 2] = lut[l + 2]
      pixels[p + 3] = 255
    }
    cached = nil
  }

  /// Cached: rebuilt only when pixels change, not on every one of the ~30
  /// redraws/sec the glide needs. On a watch, waste is battery.
  func makeImage() -> CGImage? {
    if let c = cached { return c }
    let stride = Self.width * 4
    guard let provider = CGDataProvider(data: Data(pixels) as CFData) else { return nil }
    let img = CGImage(
      width: Self.width,
      height: Self.height,
      bitsPerComponent: 8,
      bitsPerPixel: 32,
      bytesPerRow: stride,
      space: CGColorSpaceCreateDeviceRGB(),
      bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
      provider: provider,
      decode: nil,
      // Bilinear on the way up to screen size: smooths row banding AND the fat
      // bins you get when the phone is zoomed out. The GPU is scaling this
      // image anyway, so it costs nothing.
      shouldInterpolate: true,
      intent: .defaultIntent
    )
    cached = img
    return img
  }
}
