import Foundation
import Network

/// NATIVE spectrum → watch forwarder. Exists for ONE reason: when the iPhone is
/// LOCKED IN A POCKET, iOS throttles the React-Native JS thread, and the phone's
/// spectrum WebSocket lives in JS (see UberSDRClient — "JS only manages the spectrum
/// WS for display"). So the waterfall to the watch goes ragged the instant you lock,
/// while the native audio sails through. This class reads the spectrum natively — off
/// the JS thread, at native priority — so the wrist waterfall keeps flowing locked.
///
/// BATTERY IS KING. This runs ONLY in the one broken state: phone locked AND a watch
/// attached. Foregrounded, the JS spectrum WS feeds both the phone screen and the
/// watch as before and this stays dead. On lock (with a watch), JS hands OFF: it closes
/// its spectrum WS (the phone screen is off anyway) and starts us — so there is only
/// ever ONE spectrum subscription, never two. We also ask the server to throttle frames
/// to what the wrist needs (~10fps) via `set_rate`, and do the lightest processing that
/// still looks good (a rolling auto-range + linear map, NOT the phone's full 355-line
/// display DSP — the watch applies its own brightness/contrast on top).
///
/// A straight port of the phone's own NWConnection pattern (VibePowerModule audio WS /
/// the spike's AudioSocket): one socket, gen-guarded receive loop, autoReplyPing.
final class WatchSpectrumForwarder {

  // The row width the watch expects — MUST match WaterfallBuffer.width (watch) and
  // watchProvider WATCH_BINS (JS). 128 halves the WCSession row traffic + watch render CPU.
  static let watchBins = 128

  private var conn: NWConnection?
  private let queue = DispatchQueue(label: "com.vibesdr.watchspectrum")
  private var gen = 0

  // ── Geometry, handed over by JS at lock (it barely changes in a pocket) ──────
  private var binBandwidth: Double = 0     // Hz per raw bin
  private var tuneHz: Double = 0           // VFO centre — the row is cropped around this
  private var centerHz: Double = 0         // spectrum centre; updated from each SPEC header
  private var filterLow: Double = 0        // passband edges (Hz offsets), for the watch VFO lines
  private var filterHigh: Double = 0
  private var brightness: Double = 0       // dB offset (watch mirrors the phone's wf brightness)
  private var contrast: Double = 0         // −10…+10, light S-curve

  // ── SPEC decode state ────────────────────────────────────────────────────────
  private var bins = [Double]()            // current full frame, dBFS, maintained across deltas
  private var floorDb = -120.0             // rolling auto-range, EMA'd
  private var ceilDb  = -20.0

  private var pingTimer: DispatchSourceTimer?
  private var watchdog: DispatchSourceTimer?
  private var lastFrameAt: Double = 0     // systemUptime of the last SPEC frame
  private var currentUrl = ""

  private(set) var running = false

  /// A finished 256-byte watch row + its metadata. VibeWatchModule wires this to its
  /// own WCSession row sender (same batching/format the JS `sendRow` path uses).
  var onRow: ((_ row: Data, _ freq: Double, _ span: Double, _ snr: Double,
               _ level: Double, _ lo: Double, _ hi: Double, _ meter: String) -> Void)?

  // ── Lifecycle ────────────────────────────────────────────────────────────────

  /// Start forwarding. `url` is the FULL spectrum WS URL JS would have used (incl. the
  /// PIN `authSuffix`/password), so we never re-implement the handshake. The geometry is
  /// whatever JS last had — good enough while locked.
  func start(url: String, binBandwidth: Double, tuneHz: Double,
             filterLow: Double, filterHigh: Double,
             brightness: Double, contrast: Double) {
    stop()
    self.binBandwidth = binBandwidth
    self.tuneHz = tuneHz
    self.centerHz = tuneHz
    self.filterLow = filterLow
    self.filterHigh = filterHigh
    self.brightness = brightness
    self.contrast = contrast
    self.bins = []
    self.currentUrl = url
    guard let u = URL(string: url) else { NSLog("[WatchSpec] bad url"); return }

    let secure = (u.scheme == "wss")
    let params: NWParameters = secure ? .tls : .tcp
    let ws = NWProtocolWebSocket.Options()
    ws.autoReplyPing = true
    params.defaultProtocolStack.applicationProtocols.insert(ws, at: 0)

    gen &+= 1
    let g = gen
    let c = NWConnection(to: .url(u), using: params)
    conn = c
    running = true
    c.stateUpdateHandler = { [weak self] state in
      guard let self, self.gen == g else { return }
      switch state {
      case .ready:
        NSLog("[WatchSpec] connected")
        self.lastFrameAt = ProcessInfo.processInfo.systemUptime   // grace before the first frame
        self.subscribe(c)
        self.receive(c, g)
        self.startPing(c, g)
        self.startWatchdog(g)
      case .failed(let e):
        NSLog("[WatchSpec] failed: \(e)")
        self.scheduleReopen(g)
      case .cancelled:
        break
      default:
        break
      }
    }
    c.start(queue: queue)
  }

  func stop() {
    gen &+= 1
    pingTimer?.cancel(); pingTimer = nil
    watchdog?.cancel(); watchdog = nil
    recenterWork?.cancel(); recenterWork = nil
    conn?.cancel(); conn = nil
    running = false
  }

  /// STARVATION WATCHDOG. A spectrum WS can go HALF-OPEN — it stops delivering frames but
  /// never fires .failed or a receive error (the "zombie socket" the JS client also guards
  /// against). Without this the waterfall just freezes and the wrist shows "iPhone not
  /// responding" while tuning still works. So: if no SPEC frame has landed for a few seconds
  /// while we believe we're connected, tear the socket down and reopen. Runs on our own queue
  /// (kept alive with the app by the audio assertion), so it fires even while locked.
  private func startWatchdog(_ g: Int) {
    let t = DispatchSource.makeTimerSource(queue: queue)
    t.schedule(deadline: .now() + 2, repeating: 2)
    t.setEventHandler { [weak self] in
      guard let self, self.gen == g, self.running, self.conn != nil else { return }
      let idle = ProcessInfo.processInfo.systemUptime - self.lastFrameAt
      if idle > 4 {
        NSLog("[WatchSpec] STALLED (%.1fs no frames) — reopening", idle)
        self.scheduleReopen(g)     // cancels the dead socket + reopens after a beat
      }
    }
    watchdog?.cancel()
    watchdog = t
    t.resume()
  }

  /// The watch crown retuned while locked.
  ///
  /// The crop follows `tuneHz` LOCALLY and immediately — no server command per tune. That's
  /// the fix for the saturation: re-centering the server on every tune (a round trip + config
  /// echo + re-crop, on top of the per-tune audio retune) piled up and choked the phone on a
  /// fast spin. Instead we re-centre the server ONCE, after tuning SETTLES — the waterfall
  /// still follows (the crop is always centred on tuneHz, so no snap), but the mid-spin churn
  /// is gone.
  func retune(tuneHz: Double) {
    queue.async { [weak self] in
      guard let self else { return }
      self.tuneHz = tuneHz        // local crop follows now; server view re-centres on settle
      self.lastRetuneAt = ProcessInfo.processInfo.systemUptime
      self.armRecenter()
    }
  }

  private var lastRetuneAt: Double = 0
  private var lastRowSentAt: Double = 0

  private var recenterWork: DispatchWorkItem?
  private func armRecenter() {
    recenterWork?.cancel()
    let work = DispatchWorkItem { [weak self] in
      guard let self, let c = self.conn else { return }
      let viewSpan = Double(self.bins.count) * self.binBandwidth
      // Only worth a round trip if the VFO has drifted meaningfully off the view centre.
      if viewSpan <= 0 || abs(self.tuneHz - self.centerHz) > viewSpan * 0.15 {
        self.subscribe(c)
      }
    }
    recenterWork = work
    queue.asyncAfter(deadline: .now() + 0.35, execute: work)   // ~settle after the spin stops
  }

  // ── Server protocol ───────────────────────────────────────────────────────────

  /// Subscribe / recentre. Same `zoom` message the JS client sends on open, plus a
  /// `set_rate` so the server only polls fast enough for the wrist — battery, not frames.
  private func subscribe(_ c: NWConnection) {
    sendText(c, ["type": "zoom",
                 "frequency": Int(tuneHz.rounded()),
                 "binBandwidth": Int(binBandwidth.rounded())])
    // ~10fps is all the watch draws; ask the server to poll at a third rate.
    sendText(c, ["type": "set_rate", "divisor": 3])
  }

  private func startPing(_ c: NWConnection, _ g: Int) {
    let t = DispatchSource.makeTimerSource(queue: queue)
    t.schedule(deadline: .now() + 5, repeating: 5)
    t.setEventHandler { [weak self] in
      guard let self, self.gen == g else { return }
      self.sendText(c, ["type": "ping"])
    }
    pingTimer?.cancel()
    pingTimer = t
    t.resume()
  }

  private func sendText(_ c: NWConnection, _ dict: [String: Any]) {
    guard let d = try? JSONSerialization.data(withJSONObject: dict) else { return }
    let meta = NWProtocolWebSocket.Metadata(opcode: .text)
    let ctx = NWConnection.ContentContext(identifier: "t", metadata: [meta])
    c.send(content: d, contentContext: ctx, isComplete: true, completion: .contentProcessed { _ in })
  }

  private func scheduleReopen(_ g: Int) {
    guard gen == g, running else { return }
    conn?.cancel(); conn = nil          // cancel the (possibly half-open) socket, not just drop it
    let url = currentUrl
    let bb = binBandwidth, t = tuneHz, fl = filterLow, fh = filterHigh, br = brightness, co = contrast
    queue.asyncAfter(deadline: .now() + 1) { [weak self] in
      guard let self, self.running, self.gen == g else { return }
      self.start(url: url, binBandwidth: bb, tuneHz: t, filterLow: fl, filterHigh: fh,
                 brightness: br, contrast: co)
    }
  }

  private func receive(_ c: NWConnection, _ g: Int) {
    c.receiveMessage { [weak self] data, context, _, error in
      guard let self, self.gen == g, self.conn === c else { return }
      if error != nil { self.scheduleReopen(g); return }
      let op = (context?.protocolMetadata(definition: NWProtocolWebSocket.definition)
                as? NWProtocolWebSocket.Metadata)?.opcode
      if op == .binary, let data { self.parseSpec(data) }
      // Text = config/pong; geometry we care about (binBandwidth) is handed over by JS,
      // and centre rides the binary header, so we can ignore text here.
      self.receive(c, g)
    }
  }

  // ── SPEC frame decode (see UberSDRClient binary format) ────────────────────────
  //
  //  Header 22B: "SPEC"(4) ver(1) flags(1) ts(u64 LE) freq(u64 LE)
  //  flags: 0x01 full f32, 0x02 delta f32, 0x03 full u8, 0x04 delta u8  (we ask for u8)
  //  full:  binCount × elem ;  delta: u16 count, then count × {u16 idx, elem}
  //  u8: dBFS = value − 256
  private func parseSpec(_ d: Data) {
    guard d.count >= 22 else { return }
    let b = [UInt8](d)
    // magic "SPEC"
    guard b[0] == 0x53, b[1] == 0x50, b[2] == 0x45, b[3] == 0x43 else { return }
    lastFrameAt = ProcessInfo.processInfo.systemUptime   // fed the watchdog: we're alive
    let flags = b[5]
    // frequency (spectrum centre) is bytes 14..21, u64 LE
    var f: UInt64 = 0
    for i in 0..<8 { f |= UInt64(b[14 + i]) << (8 * i) }
    centerHz = Double(f)

    let isDelta = (flags == 0x02 || flags == 0x04)
    let isU8    = (flags == 0x03 || flags == 0x04)
    let elem    = isU8 ? 1 : 4
    let body    = 22

    func decodeElem(_ off: Int) -> Double {
      if isU8 { return Double(b[off]) - 256.0 }         // u8 → dBFS
      var u: UInt32 = 0
      for i in 0..<4 { u |= UInt32(b[off + i]) << (8 * i) }
      return Double(Float(bitPattern: u))
    }

    if isDelta {
      guard bins.count > 0, d.count >= body + 2 else { return }   // need a full frame first
      var p = body
      let count = Int(b[p]) | (Int(b[p + 1]) << 8); p += 2
      for _ in 0..<count {
        guard p + 2 + elem <= b.count else { break }
        let idx = Int(b[p]) | (Int(b[p + 1]) << 8); p += 2
        if idx < bins.count { bins[idx] = decodeElem(p) }
        p += elem
      }
    } else {
      let n = (b.count - body) / elem
      guard n > 1 else { return }
      var out = [Double](repeating: 0, count: n)
      var p = body
      for i in 0..<n { out[i] = decodeElem(p); p += elem }
      bins = out
    }

    buildAndSendRow()
  }

  // ── Light auto-range + map + VFO crop → the watch row ───────────────────────────
  private func buildAndSendRow() {
    let n = bins.count
    guard n > 1, binBandwidth > 0 else { return }

    // WHILE ACTIVELY TUNING, get out of the way of the frequency echoes. Rows, crown commands
    // and state all share WCSession; at full row rate during a fast spin the link wedges and
    // the readout lags/snaps. You're not reading the waterfall mid-spin anyway — so throttle
    // rows hard until tuning settles, freeing the link for the frequency. Snaps back to full
    // rate the instant the crown stops.
    let now = ProcessInfo.processInfo.systemUptime
    // 10 rows/sec, matching the JS path (MIN_ROW_MS = 100). NO tune throttle: backing rows off
    // during a spin STARVED the WaterfallBuffer and froze the wrist waterfall while tuning. The
    // buffer scrolls at the DATA rate, so it needs a steady healthy feed. (See watchProvider note.)
    if now - lastRowSentAt < 0.1 { return }
    lastRowSentAt = now

    // Rolling auto-range: EMA the per-frame min/max so the map tracks the noise floor
    // and peaks without the phone's full 2–5s history machinery. Cheap and steady.
    var lo = bins[0], hi = bins[0]
    for v in bins { if v < lo { lo = v }; if v > hi { hi = v } }
    let a = 0.1
    floorDb = floorDb * (1 - a) + lo * a
    ceilDb  = ceilDb  * (1 - a) + hi * a
    let range = max(6.0, ceilDb - floorDb)          // never divide by ~0 on a flat band

    // dBFS → 0..255. brightness shifts the floor (dB), contrast is a mild gain about mid.
    let cGain = 1.0 + Double(contrast) / 20.0        // −10..+10 → 0.5..1.5
    @inline(__always) func mapDb(_ db: Double) -> Double {
      var t = (db - floorDb + brightness) / range     // 0..1-ish
      t = (t - 0.5) * cGain + 0.5                      // contrast about mid-grey
      return max(0, min(255, t * 255))
    }

    // VFO-centred, peak-preserving crop to 256 columns — ported from watchProvider.sendRow.
    let binHz = binBandwidth
    let bwHz = Double(n) * binHz
    let filtW = abs(filterHigh - filterLow)
    // A readable blob: ~3× the passband, but never sharper than the source bins.
    let wanted = (filtW > 0 ? filtW : 3000) * 3
    let floorSpan = Double(Self.watchBins) * binHz
    let span = min(bwHz, max(wanted, floorSpan))
    let centreBin = (tuneHz - centerHz) / binHz + Double(n) / 2
    let halfBins = span / binHz / 2
    let start = centreBin - halfBins
    let step = (halfBins * 2) / Double(Self.watchBins)

    var row = [UInt8](repeating: 0, count: Self.watchBins)
    for x in 0..<Self.watchBins {
      let s0 = start + Double(x) * step
      let s1 = s0 + step
      var i0 = Int(floor(s0))
      let i1 = max(i0 + 1, Int(ceil(s1)))
      var peak = 0.0
      while i0 < i1 {
        let idx = i0 < 0 ? 0 : (i0 >= n ? n - 1 : i0)
        let v = mapDb(bins[idx])
        if v > peak { peak = v }
        i0 += 1
      }
      row[x] = UInt8(peak)
    }

    // Signal readout for the pill/meter: peak within the passband, as a rough level.
    let peakDb = ceilDb
    let level = max(0, min(1, (peakDb - floorDb) / range))
    let meter = String(format: "%.0fdB", peakDb - floorDb)

    onRow?(Data(row), tuneHz, span, peakDb - floorDb, level, filterLow, filterHigh, meter)
  }
}
