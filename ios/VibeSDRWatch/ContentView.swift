import SwiftUI
import Combine
import WatchKit

// ── Link-hint tuning ─────────────────────────────────────────────────────────
/// Rows quiet for longer than this and the local hop is suspect. A watch on the
/// end of Bluetooth drops the odd frame; that is not news.
private let hintRowGap   = 1.2
/// Hold a condition for this long before showing a pill. A single late frame must
/// not strobe it.
private let hintDebounce = 0.7
/// …and keep it up for at least this long once shown, so a marginal link doesn't
/// flicker the pill on and off.
private let hintHold     = 2.0
/// A state echo older than this means the WCSession hop itself is suspect, and we
/// can no longer trust anything the phone last told us about the FAR hop. The
/// phone answers a heartbeat every 4s, so a stale state message is itself a
/// finding.
private let hintStateFresh = 8.0

/// WHICH HOP is rough. There are two radio links in series — server↔iPhone and
/// iPhone↔watch — and they fail independently, so "LINK ROUGH" was never an
/// actionable thing to say.
///
/// Rendered as a miniature DIAGRAM of the chain with the troubled link marked,
/// not as text: it reads at a glance, fits the smallest watch, and needs no
/// localisation. Direction convention: the FURTHER device is always on the left,
/// the wrist end on the right, so the two two-device pills are visually parallel
/// and the user learns the grammar once.
///
/// TUNING SURVIVES ALL OF THESE except `.indeterminate`. Crown commands travel
/// watch → WCSession → phone → audio WS, and the audio WS has its own native
/// watchdog, so in the first three cases the crown (and the phone's audio) still
/// work while the spectrum is degraded. Half the app still working must not read
/// as the whole app broken — hence a small pill over a live waterfall, never an
/// overlay, and never whole-app wording like "connection lost".
enum LinkHint: Equatable {
  /// "Reconnecting to the server." The phone is rebuilding its spectrum socket
  /// right now (UberSDRClient's starvation watchdog doing its job). Shown even
  /// though rows have stopped — a recovery in progress is not a failure.
  case reconnecting
  /// "The server hop is rough." Shown EVEN WHILE ROWS STILL ARRIVE: this is the
  /// erratic-but-working case, and a gap-only trigger misses it entirely.
  case serverHop
  /// "The wrist hop is weak." The phone says its own link is fine, so the rows
  /// are being lost between here and the pocket.
  case wristHop
  /// The whole WCSession pipe is suspect, so we cannot honestly blame either hop.
  case indeterminate
}

/// Spectrum screen: full-bleed waterfall with chrome FLOATING over it.
///
/// Solid bars would cut the waterfall in two and steal height at both ends, and
/// waterfall height is the scarcest thing on a watch. Legibility comes from a
/// dark scrim, not glass/frosting: frosting blurs but does not darken, so green
/// text over a sonar-green waterfall would still be green-on-green — and the
/// waterfall scrolls under the chrome at ~10fps, which would re-blur every frame
/// on the watch GPU.
///
/// ALL chrome lives at the BOTTOM. The system clock owns the top-right corner of
/// every standard watch app and cannot be moved or hidden, so a frequency ticker
/// up there would collide with it.
struct ContentView: View {
  @EnvironmentObject var link: WatchLink

  // Developer CPU comparison badge (companion vs standalone JR). Gated by
  // CpuMeter.enabled — turn OFF before any public release.
  @StateObject private var cpuMeter = CpuMeter()
  @Environment(\.scenePhase) private var scenePhase

  /// Digital Crown position, in step-detents. We only ever read the DELTA out of
  /// this and hand it to the phone — the phone owns the frequency, multiplies by
  /// its own step size, and echoes back what it actually landed on.
  ///
  /// The range is deliberately SMALL and wrapping. A huge from/through span with
  /// `by: 1` makes watchOS materialise a detent map that size and tick haptics
  /// across it — rotating then hangs the main thread and the watchdog SIGKILLs
  /// the app (which reads as "the app bounced back to the app list").
  @State private var crown = 0.0

  // ── Link hint (hop-diagnostic pill) ──────────────────────────────────────
  /// What is actually on screen, after debounce/hold. See syncHint().
  @State private var hint: LinkHint? = nil
  @State private var shownSince: Date? = nil
  /// Opacity of the marked-link glyph, animated to read as a live problem.
  @State private var pulse = 1.0

  /// First-run coach. Persisted, so it is shown ONCE — ever, not once per session.
  @AppStorage("coachSeenSDR") private var coachSeen = false
  @State private var lastDetent = 0
  @FocusState private var crownFocused: Bool

  /// Explicit 20fps redraw clock — the same default the phone app runs at (30 is
  /// its high-performance toggle). The glide is interpolating between rows that
  /// arrive at ~10/sec, so 30fps bought very little for a third more redraws, each
  /// of which rebuilds a 256x89 image. On a watch that's battery for nothing.
  ///
  /// We do NOT use `TimelineView(.animation)`: on watchOS its `minimumInterval` is
  /// a floor on the GAP, not a promise of cadence, and it proved free to update
  /// lazily — in practice it fired sometimes and not others, so the waterfall
  /// scrolled smoothly for a few frames, stalled, then lurched. (It only looked
  /// right at all while a second TimelineView — a debug overlay — happened to be
  /// forcing repaints.) The scroll glide and the jitter buffer both advance on
  /// this clock, so a cadence we don't control is a cadence we can't render on.
  ///
  /// Splitting the trace onto its own faster clock was tried and REVERTED: both
  /// Canvases sat in the same view body observing the same @EnvironmentObject, so
  /// ANY published change repainted BOTH. The clocks simply summed (12 + 25 + rows
  /// ≈ 45 redraws/sec of everything) and CPU rose. The decoupling was imaginary;
  /// only the cost was real. Doing it properly means giving each Canvas its own
  /// View struct observing only what it needs — not worth it while 20fps looks fine.
  @State private var frame = 0
  @State private var showNumpad = false
  @State private var showMenu = false
  /// What the crown does. Explicit and persistent — never a timed-out HUD, because
  /// on a wrist you must always know what a turn is about to do.
  @State private var crownMode: CrownMode = .tune

  /// How far you must turn for one step. watchOS's OWN sensitivity, not a divisor of
  /// our own: it changes how many detents a rotation produces, so the HAPTIC CLICKS
  /// STAY IN STEP WITH THE TUNING. (A divisor was tried — it kept 1 click = 1 detent
  /// while silently swallowing 2 of every 3, so the crown clicked without doing
  /// anything, which feels broken rather than fine.) `.low` = turn further per step,
  /// which is what a 9kHz step needs: a flick used to cross half a band.
  @AppStorage("crownSens") private var crownSens = CrownSens.medium.rawValue
  private var crownSensitivity: DigitalCrownRotationalSensitivity {
    CrownSens(rawValue: crownSens)?.sensitivity ?? .medium
  }

  /// WATCH-LOCAL waterfall brightness/contrast, persisted.
  ///
  /// The phone's render settings are MIRRORED and stay the base — the wrist should look
  /// like the phone. But the same numbers cannot serve both screens: a waterfall tuned
  /// for a big phone held in front of you is often near-black on a wrist, glanced at
  /// outdoors and at an angle. The alternative was blowing out the PHONE just to see the
  /// WATCH, which is the wrong trade — so the wrist gets its own offsets on top.
  ///
  /// -1…+1, 0 = exactly what the phone shows. Saved, so you set them once.
  @AppStorage("wfBright")   private var wfBright   = 0.0
  @AppStorage("wfContrast") private var wfContrast = 0.0

  /// The ONE knob that matters. Smoothness vs battery, nothing else — the Canvas
  /// repaints everything (waterfall, trace, VFO) on every tick regardless.
  private let driver = Timer.publish(every: 1.0 / 20.0, on: .main, in: .common).autoconnect()

  /// The crown can be on either side (it flips when the watch is worn on the other
  /// wrist), so the meter goes BESIDE it and the X goes OPPOSITE it. Ask the
  /// device rather than assuming.
  private var crownOnRight: Bool {
    WKInterfaceDevice.current().crownOrientation == .right
  }

  private static let detents = 1000.0

  private func clamp(_ v: Double) -> Double { min(1, max(-1, v)) }

  /// Push the SAVED brightness/contrast at the buffer.
  ///
  /// @AppStorage remembers the VALUE across launches, but the WaterfallBuffer is built
  /// fresh and starts at neutral — so the setting was saved and simply never applied:
  /// it only appeared once you nudged the crown, because that's the one thing that
  /// wrote it through. Persisting a setting and applying it are two different jobs.
  private func applyTone() {
    link.waterfall.brightness = wfBright
    link.waterfall.contrast = wfContrast
  }

  /// When the crown was last actually turned. A non-tune mode ENDS when you leave —
  /// not while you're using it.
  @State private var crownUsedAt = Date()

  /// Idle timeout before the crown falls back to TUNE.
  ///
  /// The rule was "the mode is EXPLICIT and PERSISTENT, never a timed-out HUD" — and
  /// that rule still holds for the case it was written about: a mode must not vanish
  /// while you are USING it. But it also has to end when the SESSION does. Zoom, then
  /// drop your wrist, then come back minutes later, and the crown was still on zoom:
  /// the mode had outlived the interaction, and the next turn does something you
  /// didn't ask for — which is the very thing "explicit" was supposed to prevent.
  ///
  /// So: any turn resets the clock, and the mode only lapses once you have genuinely
  /// left it alone. Generous, because a short timeout WOULD be the HUD we rejected.
  private static let crownIdleTimeout: TimeInterval = 30



  var body: some View {
    ZStack {
      Color.black.ignoresSafeArea()

      if link.everGotRow { waterfall } else { placeholder }

      // The watch's own battery, tucked in beside the clock. A live waterfall costs the
      // watch ~34% of a core, and this is the one screen you'd leave running on a hilltop
      // — so the number you'd reach for is on the screen you're already looking at.
      // pointer-events off: it must never eat a tap meant for the waterfall.
      topStrip

      // Developer CPU badge, top-left (clear of the clock/battery on the right).
      // Same measurement as the JR spike so companion vs standalone is directly
      // comparable. Gated by CpuMeter.enabled — a comparison overlay, not a feature.
      if CpuMeter.enabled {
        VStack {
          HStack {
            Text(String(format: "%.0f%%", cpuMeter.cpu))
              .font(.system(size: 11, weight: .semibold, design: .monospaced))
              .foregroundStyle(.white.opacity(0.75))
              .padding(.leading, 8)
              .padding(.top, 21)
            Spacer()
          }
          Spacer()
        }
        .ignoresSafeArea()
        .allowsHitTesting(false)
      }

      // DEGRADE, DON'T BLOCK.
      //
      // A frozen picture with a black box over it is the worst of both worlds: you
      // lose the data AND you learn nothing. The phone<->watch link is a RADIO link —
      // it hops between Bluetooth and Wi-Fi, it drops out when you walk away from the
      // router — so an interruption is a NORMAL condition, not a fault, and the app
      // should behave like a radio: keep showing what it has and TELL YOU the signal
      // is rough.
      //
      // So: a brief gap gets a small WARNING PILL and the waterfall keeps rendering
      // whatever it's got. Only a genuinely dead link (or a phone that has told us
      // it's doing something else) gets the full overlay.
      if let h = hint {
        VStack {
          hintPill(h)
            .padding(.top, 46)      // clear of the clock
          Spacer()
        }
      }

      if link.everGotRow, let msg = stalledMessage {
        VStack(spacing: 4) {
          Image(systemName: msg.icon).font(.title3)
          Text(msg.text)
            .font(.caption2)
            .multilineTextAlignment(.center)
        }
        .foregroundStyle(.white)
        .padding(10)
        .background(.black.opacity(0.78))
        .clipShape(RoundedRectangle(cornerRadius: 10))
      }

      VStack(spacing: 2) {
        Spacer()
        // THE BAND, with the ticker — not at the top of the screen.
        //
        // The top row of the waterfall is the line that arrived a moment ago: it is the
        // NEWEST data and the only part you are actually tuning by. Putting a label there
        // covers the very thing you are looking at. Down here the waterfall is already
        // seconds old and nobody is reading it, so the label costs nothing — and it now
        // sits directly above the band-boundary marks it explains, which is where it
        // belonged all along.
        bandLabel
        ticker
        Button { showNumpad = true } label: { readout }
          .buttonStyle(.plain)
      }
      .padding(.horizontal, 6)
      .padding(.bottom, 4)

      if crownMode != .tune { crownOverlay }

      // THE COACH, once. Gated on everGotRow so it lands on a WORKING waterfall — a
      // tutorial over a black boot screen teaches you the app is broken. It also sits
      // ABOVE the crown overlay in the stack, so nothing can draw over it.
      if link.everGotRow && !coachSeen {
        CoachOverlay(
          title: "VibeSDR",
          items: [
            .init(glyph: "digitalcrown.horizontal.arrow.clockwise",
                  text: "Turn the Crown to tune"),
            .init(glyph: "hand.tap",
                  text: "Tap the frequency to type one"),
            .init(glyph: "hand.point.up.left.fill",
                  text: "Press and hold the waterfall for the menu"),
            // Called out on its OWN line, deliberately. These are the two controls a
            // listener reaches for first and the two they will never find by accident —
            // "there's a menu" does not tell you that the DEMOD is in it.
            .init(glyph: "slider.horizontal.3",
                  text: "Demodulator and tuning step live in that menu"),
          ],
          onDismiss: {
            WKInterfaceDevice.current().play(.click)
            coachSeen = true
          }
        )
      }
    }
    // PUSHED, not presented as a sheet. A watchOS sheet comes with a big header —
    // the X, the clock and a grab handle — which ate ~100pt off the top before the
    // pad's own content began, pushing the bottom row clean off the screen (and
    // hiding the readout behind the X). A navigation push gets a compact back
    // chevron instead, which leaves the pad the room it needs.
    .navigationDestination(isPresented: $showNumpad) {
      NumpadView().environmentObject(link)
    }
    .navigationDestination(isPresented: $showMenu) {
      ControlMenu { mode in crownMode = mode; crownUsedAt = Date() }
        .environmentObject(link)
    }
    .ignoresSafeArea()
    .focusable(true)
    .focused($crownFocused)
    .digitalCrownRotation(
      $crown,
      from: 0, through: Self.detents, by: 1,
      sensitivity: crownSensitivity,
      isContinuous: true,               // wraps at the ends
      isHapticFeedbackEnabled: true     // detents: the "fidget-spinner" feel
    )
    .onChange(of: crown) { _, new in
      let detent = Int(new.rounded())
      guard detent != lastDetent else { return }

      // Unwrap: the crown is continuous, so crossing 0 <-> 999 is a step of one,
      // not a leap of 999. Without this a single detent at the wrap point would
      // fling the VFO across the band.
      var delta = detent - lastDetent
      let range = Int(Self.detents)
      if delta >  range / 2 { delta -= range }
      if delta < -range / 2 { delta += range }

      lastDetent = detent
      crownUsedAt = Date()          // in use — the idle timeout must not fire

      // THE CROWN KEEPS TUNING UNDERNEATH A PRESENTED SCREEN. Swallow it.
      //
      // This view holds the crown focus, and pushing the numpad or the control menu
      // does NOT take it away — so scrolling one of those lists with the crown was
      // ALSO turning the VFO, invisibly, behind the screen you were looking at. It
      // reads as the radio tuning itself: you pick a demod and the frequency has
      // moved, or you turn the crown in the menu and come back somewhere else
      // entirely. (It also made zoom and tune look like they were crossing over.)
      //
      // Swallow the delta but KEEP tracking lastDetent, or the accumulated rotation
      // would be applied in one lump the moment the screen closes.
      guard !showMenu && !showNumpad else { return }

      switch crownMode {
      case .tune: link.tune(delta: delta)
      case .zoom: link.zoom(delta: delta)
      case .volume: link.volume(delta: delta)
      case .brightness:
        wfBright = clamp(wfBright + Double(delta) * 0.04)
        link.waterfall.brightness = wfBright
      case .contrast:
        wfContrast = clamp(wfContrast + Double(delta) * 0.04)
        link.waterfall.contrast = wfContrast
      }
    }
    // Long-press anywhere on the waterfall for the control grid.
    .onLongPressGesture(minimumDuration: 0.45) {
      WKInterfaceDevice.current().play(.click)
      showMenu = true
    }
    .onAppear {
      crownFocused = true
      link.ping()          // tell the phone we're here — see below
      applyTone()
      cpuMeter.start()
    }
    // The pill's clock. Rows tick this ~16/sec while all is well; when the rows are
    // the very thing that stopped, the 4/sec state echo keeps it running — which is
    // exactly the moment it has to work. No timer of its own.
    .onChange(of: link.lastRowAt)   { _, _ in syncHint() }
    .onChange(of: link.lastStateAt) { _, _ in syncHint() }
    .onChange(of: scenePhase) { _, phase in
      // YOU LEFT — the mode ends. This is the honest signal, and it's the case that
      // actually bit: zoom, lower your wrist, come back later, and the crown was
      // still zooming. A timeout is the backstop; this is the real trigger.
      if phase != .active { crownMode = .tune; return }
      crownUsedAt = Date()
      applyTone()          // the buffer is reset on wake — re-assert our settings
      // Screen woke: the queued rows and the scroll clock are both stale. Draining
      // them as usual fast-forwards through old data and then runs dry — the
      // stutter you get for the first second after a wake. Start clean.
      link.waterfall.reset()
      // ANNOUNCE OURSELVES. The phone's WCSession.isReachable goes stale and it
      // then refuses to send anything, while the crown still tunes — the downlink
      // dies silently. A message from us is proof we're here, so say so rather
      // than waiting for the user to turn the crown before rows start flowing.
      link.ping()
    }
  }

  // ── Waterfall ──────────────────────────────────────────────────────────────

  /// WCSession delivers rows in BURSTS, not on a clock. So the renderer owns the
  /// scroll clock: it ticks the jitter buffer on a steady timeline, which drains
  /// queued rows at an even cadence and hands back a sub-row offset to glide by.
  /// Drawing on arrival — however you interpolate it — always lurches, because
  /// during a gap there is nothing to interpolate towards.
  /// ONE Canvas, ONE clock.
  ///
  /// Splitting the trace onto its own faster clock was tried and REVERTED. It could
  /// not work: both Canvases sat in the same view body observing the same
  /// @EnvironmentObject, so ANY published change — a row landing, either clock —
  /// invalidated the whole body and repainted BOTH. The clocks simply summed: a
  /// 12fps waterfall clock plus a 25fps trace clock plus 10 rows/sec measured as 45
  /// redraws/sec of everything, and CPU went 30% -> 42%. The decoupling was
  /// imaginary; only the cost was real.
  ///
  /// (Doing it properly would mean giving each Canvas its own View struct with only
  /// the state it needs, so SwiftUI can invalidate them independently. Worth it
  /// only if the trace's smoothness at 20fps proves inadequate — and it doesn't.)
  private var waterfall: some View {
    Canvas { ctx, size in
      _ = frame        // read so SwiftUI must redraw; see `driver`

      let wf = link.waterfall
      wf.tick(at: ProcessInfo.processInfo.systemUptime)

      // The spectrum gets a BAND of its own — the top third — and the waterfall
      // takes the rest. A floating overlay was cheaper in pixels, but the trace has
      // to be readable as a HEIGHT: squashed into a strip it is just another
      // texture. The system clock sits in this band and reads as a label there.
      let specH = (size.height / 3).rounded()

      if let img = wf.makeImage() {
        let rowPx = (size.height - specH) / Double(WaterfallBuffer.visible)
        let p = wf.progress

        // Newest row is index 0 (top) with one row of headroom above the visible
        // edge. As p goes 0->1 the window walks from "newest not yet in" to "newest
        // fully in at the top", exactly as the next row lands and resets p.
        var wctx = ctx
        wctx.clip(to: Path(CGRect(x: 0, y: specH,
                                  width: size.width, height: size.height - specH)))
        wctx.draw(
          Image(decorative: img, scale: 1),
          in: CGRect(x: 0, y: specH - (1 - p) * rowPx,
                     width: size.width,
                     height: rowPx * Double(WaterfallBuffer.height))
        )
      }

      drawSpectrum(ctx, size, wf.specRow, peaks: wf.peakRow, height: specH)
      drawVFO(ctx, size)   // through BOTH: the trace and its history stay aligned
    }
    .ignoresSafeArea()
    .onReceive(driver) { _ in
      frame &+= 1
      // Lapse back to TUNE once the crown has been left alone. Checked on the render
      // clock we already run, rather than adding a timer of its own.
      if crownMode != .tune,
         Date().timeIntervalSince(crownUsedAt) > Self.crownIdleTimeout {
        crownMode = .tune
      }
    }
  }

  /// A thin spectrum trace across the top.
  ///
  /// The waterfall is a TIME view: judging how strong a signal is *right now* means
  /// eyeballing brightness, which is hard work on a small screen. A trace turns
  /// that into a height you can read at a glance.
  ///
  /// It renders the row the waterfall's top edge is currently showing, so the two
  /// are the same instant — spectrum on top, its own history flowing down beneath
  /// it. That only works because we scroll top-down.
  ///
  /// Occupies the top third. The clock lives up here too and reads as a label.
  private func drawSpectrum(_ ctx: GraphicsContext, _ size: CGSize, _ row: [Double],
                            peaks: [Double], height h: CGFloat) {
    let n = row.count

    // Solid black ground — the trace's own baseline, and what makes a thin line
    // read at a glance.
    ctx.fill(Path(CGRect(x: 0, y: 0, width: size.width, height: h)),
             with: .color(.black))

    guard n > 1 else { return }

    // Peak-preserving downsample to pixels: a narrow carrier must not fall
    // between two samples and vanish — the whole point is to SEE it spike.
    let cols = max(2, Int(size.width))
    var pts: [CGPoint] = []
    pts.reserveCapacity(cols)
    for c in 0..<cols {
      let a = n * c / cols
      let b = max(a + 1, n * (c + 1) / cols)
      var peak: Double = 0
      for i in a..<min(b, n) where row[i] > peak { peak = row[i] }
      let y = h - (CGFloat(peak) / 255) * (h - 2) - 1
      pts.append(CGPoint(x: CGFloat(c) * size.width / CGFloat(cols), y: y))
    }

    // ONE hue, taken from the palette, so the trace belongs to the same instrument
    // as the waterfall. Not white: white fights the system clock, which sits in
    // this band. The fill fades DOWNWARD, so it is at its most transparent where
    // the clock is — the clock stays legible over it and the trace stays readable
    // underneath.
    // The APP's spectrum colouring, ported: a 9-stop gradient sampled from the LUT
    // at index 90->235, hot at the top. It starts at 90, not 0, because black-based
    // palettes (Sonar) are near-invisible below that — so the fill's baseline
    // begins where the palette has actually picked up colour, and weak signals stay
    // visible while the trace still inherits the waterfall's hue.
    //
    // Uncapped brightness: the clock has its own scrim now, so even a near-white
    // palette can't be mistaken for it, and no palette has to be dimmed.
    let wf = link.waterfall
    let stops = (0...8).map { gi -> Gradient.Stop in
      let idx = Int((90 + (Double(gi) / 8) * 145).rounded())
      return .init(color: wf.lutColor(idx), location: 1 - Double(gi) / 8)
    }.reversed()

    var fill = Path()
    fill.move(to: CGPoint(x: 0, y: h))
    pts.forEach { fill.addLine(to: $0) }
    fill.addLine(to: CGPoint(x: size.width, y: h))
    fill.closeSubpath()
    ctx.fill(fill, with: .linearGradient(
      Gradient(stops: Array(stops)),
      startPoint: CGPoint(x: 0, y: 0), endPoint: CGPoint(x: 0, y: h)))

    // The outline is what you actually read a peak off — the palette's hot end.
    var line = Path()
    line.addLines(pts)
    ctx.stroke(line, with: .color(wf.lutColor(235)), lineWidth: 1.2)

    // PEAK HOLD — mirrored from the phone, in the VFO's colour (same as the phone).
    //
    // Drawn ON TOP of the trace, and in a DIFFERENT hue to it: the trace takes its
    // colour from the palette, so a peak line in the same family would read as part
    // of the same curve. The VFO colour is already the app's "this is a marker, not
    // the signal" colour, and the user picked it — so the peaks belong to the same
    // language as the needle rather than inventing another one.
    //
    // Peak-preserving downsample, same as the trace: a peak that falls between two
    // sample columns and vanishes is precisely the thing peak-hold exists to stop.
    if link.peakHold, peaks.count == n {
      var pk: [CGPoint] = []
      pk.reserveCapacity(cols)
      for c in 0..<cols {
        let a = n * c / cols
        let b = max(a + 1, n * (c + 1) / cols)
        var hi: Double = 0
        for i in a..<min(b, n) where peaks[i] > hi { hi = peaks[i] }
        let y = h - (CGFloat(hi) / 255) * (h - 2) - 1
        pk.append(CGPoint(x: CGFloat(c) * size.width / CGFloat(cols), y: y))
      }
      var peakLine = Path()
      peakLine.addLines(pk)
      // Thinner than the trace: it's a reference mark, not the signal itself.
      ctx.stroke(peakLine, with: .color(link.needle.opacity(0.9)), lineWidth: 0.9)
    }

    // Scrim behind the system CLOCK, same as the ticker and the frequency pill.
    //
    // watchOS draws the time itself and gives us no way to recolour or hide it — so
    // rather than dimming the trace to avoid clashing with white text (which would
    // punish every palette for the sake of Greyscale and Black Hot), give the clock
    // a dark backing of its own. It then stays legible over ANY trace colour, and
    // the trace keeps the palette's full brightness. Same scrim-not-glass logic as
    // the rest of the chrome.
    // Sits BELOW the top edge: the clock is not flush to the bezel, and a scrim
    // starting at y=2 cut through the digits about halfway down.
    //
    // IT CARRIES THE BATTERY TOO. This scrim already existed for the clock; when the
    // battery badge arrived it got a background of its own, and the two stacked — one
    // scrim visibly overlaid on another, which is the "two pills" you could see. There is
    // only ever ONE scrim up here, and this is it. Measured off the device: the clock's
    // digits start 58.8pt in from the right edge, so 0.47 of the width reaches past the
    // battery sitting to their left, and nothing wider is spectrum worth spending.
    //
    // 0.55 → 0.75. It was 0.55 when it only had to carry WHITE clock digits. It now also
    // carries the battery's LOW-BATTERY RED, which is a dark colour on a bright green
    // waterfall and needs a darker ground under it than white ever did. (It read fine back
    // when two scrims were accidentally stacked — 0.55 over 0.62 is an effective 0.83 —
    // which is the clue that told us the number, not the shape, was what was carrying it.)
    let cw = size.width * 0.47
    let ch: CGFloat = 30
    ctx.fill(
      Path(roundedRect: CGRect(x: size.width - cw - 4, y: 11, width: cw, height: ch),
           cornerRadius: 9),
      with: .color(.black.opacity(0.75))
    )

    // Hairline under the band, so the trace's baseline and the waterfall's top
    // edge don't bleed into one another.
    ctx.stroke(
      Path { $0.move(to: CGPoint(x: 0, y: h)); $0.addLine(to: CGPoint(x: size.width, y: h)) },
      with: .color(.white.opacity(0.18)),
      lineWidth: 1
    )
  }

  /// The VFO. Always dead-centre — the phone crops the bin window around it — so
  /// this is a fixed mark and the signal slides under it as you tune.
  ///
  /// Deliberately NOT a port of the phone's acrylic pane. At ~184px wide a diffuse
  /// tinted panel just reads as a smudge and buries the signal underneath it. On a
  /// watch, crisp geometry beats soft geometry: a bright solid carrier, and the
  /// passband edges as 1px DASHED lines, so you can see the filter width without
  /// it competing with the waterfall for the same pixels.
  ///
  /// Colour and intensity come from the phone's own VFO settings, so the two
  /// screens agree — but intensity drives BRIGHTNESS here, not glow spread.
  /// Nothing is blurred: it would only smear the signal under the line, and it
  /// would re-blur 30x/sec on the watch GPU for the privilege.
  private func drawVFO(_ ctx: GraphicsContext, _ size: CGSize) {
    let x = size.width / 2
    let h = size.height
    let c = link.needle
    let k = max(0.2, link.needleI / 5)   // 1-10, 5 = the phone's stock look

    // ── Passband edges: 1px dashed, drawn at their TRUE offsets from the carrier.
    //    Not mirrored: on LSB both edges fall to the LEFT of the carrier, on USB
    //    both to the right, and CW is offset — mirroring a single width would draw
    //    every mode as AM.
    if link.span > 0, link.filtHi != link.filtLo {
      let hzToPx = size.width / link.span
      let dash = StrokeStyle(lineWidth: 1, dash: [3, 3])
      for edge in [link.filtLo, link.filtHi] {
        let ex = x + edge * hzToPx
        guard ex > 1, ex < size.width - 1 else { continue }   // off-span: skip
        ctx.stroke(
          Path { $0.move(to: CGPoint(x: ex, y: 0)); $0.addLine(to: CGPoint(x: ex, y: h)) },
          with: .color(c.opacity(min(1, 0.75 * k))),
          style: dash
        )
      }
    }

    // ── The carrier: bright, solid, crisp. NO glow, NO blur — on a watch those
    //    only smear the signal sitting underneath the line. Intensity drives
    //    brightness, not spread.
    ctx.stroke(
      Path { $0.move(to: CGPoint(x: x, y: 0)); $0.addLine(to: CGPoint(x: x, y: h)) },
      with: .color(c.opacity(min(1, 0.55 + 0.09 * link.needleI))),
      lineWidth: 2.5
    )
  }

  /// Crown is in Volume or Zoom: a meter up the edge BESIDE the crown, its glyph
  /// beside it, and an X on the OPPOSITE edge to return to tuning.
  ///
  /// The meter sits next to the crown because that's the thing you're turning —
  /// your eye shouldn't have to cross the screen to see the effect of your finger.
  /// And the X goes opposite it so your hand isn't covering the way out.
  private var crownOverlay: some View {
    ZStack {
      // X on the edge OPPOSITE the crown, so your hand isn't over the way out.
      VStack {
        HStack {
          if crownOnRight { exitButton; Spacer() } else { Spacer(); exitButton }
        }
        Spacer()
      }

      // The meter hugs the crown's edge, vertically CENTRED and SHORT — the shape
      // Apple uses for its own volume indicator. A full-height bar with the glyph
      // stacked in a circle above it reads as furniture; this reads as an
      // indicator. The glyph sits inline to its left, unadorned.
      HStack {
        if crownOnRight { Spacer() }
        HStack(spacing: 5) {
          if crownOnRight { glyph; bar } else { bar; glyph }
        }
        if !crownOnRight { Spacer() }
      }
    }
    .padding(.horizontal, 8)
    .padding(.vertical, 10)
  }

  /// The way out — wrapped in a COUNTDOWN RING showing the mode's remaining life.
  ///
  /// The timeout has to be visible, or it's just the crown changing its mind behind
  /// your back — which is exactly the "you must always know what a turn will do"
  /// problem the explicit mode was introduced to solve. A ring that drains says
  /// "this is about to end" without a word, and any turn of the crown refills it, so
  /// the thing that keeps the mode alive is the thing you can see keeping it alive.
  private var exitButton: some View {
    Button { crownMode = .tune } label: {
      ZStack {
        Circle()
          .stroke(.white.opacity(0.18), lineWidth: 2)
        Circle()
          .trim(from: 0, to: crownRemaining)
          .stroke(meterTint, style: StrokeStyle(lineWidth: 2, lineCap: .round))
          .rotationEffect(.degrees(-90))          // drain from 12 o'clock
        Image(systemName: "xmark")
          .font(.system(size: 13, weight: .bold))
          .foregroundStyle(.white)
      }
      .frame(width: 30, height: 30)
      .contentShape(Circle())
    }
    .buttonStyle(.plain)
  }

  /// 1 → full life, 0 → about to lapse. Recomputed on the render clock we already run
  /// (`frame` ticks at 20fps), so the ring drains smoothly without its own timer.
  private var crownRemaining: CGFloat {
    let left = Self.crownIdleTimeout - Date().timeIntervalSince(crownUsedAt)
    return CGFloat(min(1, max(0, left / Self.crownIdleTimeout)))
  }

  private var glyph: some View {
    Image(systemName: crownMode == .volume && link.muted ? "speaker.slash.fill"
                                                         : crownMode.glyph)
      .font(.system(size: 15, weight: .semibold))
      .foregroundStyle(meterTint)
  }

  /// Short, edge-hugging, filling upward — the crown's own direction of travel.
  private var bar: some View {
    ZStack(alignment: .bottom) {
      Capsule().fill(.white.opacity(0.22))
      Capsule()
        .fill(meterTint)
        .frame(height: max(3, 74 * meterValue))
    }
    .frame(width: 5, height: 74)
  }

  private var meterTint: Color {
    switch crownMode {
    case .brightness: return .yellow
    case .contrast:   return .white
    // Muted must be unmistakable at a glance: the bar still shows the level the phone
    // will return to, but nothing is coming out of it.
    case .volume:     return link.muted ? .red : .green
    default:          return .cyan
    }
  }

  /// Zoom has no natural 0..1, so we place the current span on a LOG scale between
  /// "as tight as it gets" and "wide" — which is how zoom actually feels, and it's
  /// what the phone's own zoom drum does in octaves.
  private var meterValue: Double {
    switch crownMode {
    // -1…+1 mapped onto the bar, so centre = "no change from the phone".
    case .brightness: return (wfBright + 1) / 2
    case .contrast:   return (wfContrast + 1) / 2
    // The iPhone's REAL system volume — so a phone sitting at 50% reads half a bar, not
    // a full one. That lie is the entire reason this feature was rebuilt.
    case .volume:     return link.volume
    case .zoom:
      guard link.span > 0 else { return 0 }
      let lo = log2(2_000.0), hi = log2(4_000_000.0)
      let t = (log2(link.span) - lo) / (hi - lo)
      return min(1, max(0, 1 - t))     // full bar = zoomed right in
    case .tune:
      return 0
    }
  }


  /// nil = healthy. Otherwise, WHICH HOP is rough.
  ///
  /// Driven by the frame clock, so it appears without needing its own timer — and
  /// when the ROWS stop, the 4/sec state echo keeps the redraws coming, which is
  /// exactly when this matters most.
  ///
  /// A ROUGH LINK, not a dead one — say so and KEEP DRAWING. Rows going quiet is a
  /// normal thing for a watch on the end of Bluetooth; throwing a black box over
  /// perfectly good data because of it is not.
  ///
  /// This is the raw reading. `hint` is the debounced one that reaches the screen.
  private var rawHint: LinkHint? {
    guard stalledMessage == nil else { return nil }   // the hard overlay owns it
    guard link.everGotRow else { return nil }         // a cold boot is not a fault

    let now = Date()
    let stateFresh = link.lastStateAt.map { now.timeIntervalSince($0) < hintStateFresh } ?? false
    let gap = link.lastRowAt.map { now.timeIntervalSince($0) } ?? 0

    // 1. The phone TOLD us it is rebuilding the link. Outranks everything below:
    //    of course the rows have stopped — that is what a reconnect IS.
    if stateFresh, link.why == "reconnecting" { return .reconnecting }

    // 2. The phone's own link to the server is poor. Shown even while rows are
    //    STILL ARRIVING — jerky-but-working is precisely the case a row-gap
    //    trigger cannot see, and the case the user most wants explained.
    if stateFresh, link.serverLink <= 1 { return .serverHop }

    // Below here, something must actually have stopped.
    guard gap > hintRowGap else { return nil }

    // 3. The phone says its own hop is fine, and it is still answering us — so the
    //    rows are dying between the pocket and the wrist.
    if stateFresh { return .wristHop }

    // 4. The phone has gone quiet on the state channel too, so the whole WCSession
    //    pipe is suspect and we know nothing about the far hop. Say only that.
    return .indeterminate
  }

  /// The pill: a miniature diagram of the two-hop chain with the troubled link
  /// marked. The previous text strings are kept beside each case as the canonical
  /// meaning — the diagram asserts exactly that sentence and nothing more.
  @ViewBuilder
  private func hintPill(_ h: LinkHint) -> some View {
    let glyphs: [String] = {
      switch h {
      // "Reconnecting to server" — the circular-arrows glyph IS the universal
      // reconnecting sign, so it needs no marked link.
      case .reconnecting:   return ["arrow.triangle.2.circlepath", "server.rack"]
      // "Server link rough — spectrum erratic"
      case .serverHop:      return ["server.rack", "wifi.exclamationmark", "iphone"]
      // "Watch link weak — spectrum erratic"
      case .wristHop:       return ["iphone", "wifi.exclamationmark", "applewatch"]
      // "Link rough" — nothing more is honestly known.
      case .indeterminate:  return ["wifi.exclamationmark"]
      }
    }()
    HStack(spacing: 3) {
      ForEach(Array(glyphs.enumerated()), id: \.offset) { _, g in
        Image(systemName: g)
          .font(.system(size: 11, weight: .semibold))
          .foregroundStyle(.white)
          // The marked link pulses gently so it reads as a LIVE problem rather
          // than a static badge. Opacity only — nothing per-frame or laid out.
          .opacity(g == "wifi.exclamationmark" ? pulse : 1)
      }
    }
    .padding(.horizontal, 7)
    .padding(.vertical, 3)
    .background(.orange.opacity(0.85), in: Capsule())
    // VoiceOver gets the words the sighted user no longer needs.
    .accessibilityElement(children: .ignore)
    .accessibilityLabel({
      switch h {
      case .reconnecting:  return "Reconnecting to server. Tuning still works."
      case .serverHop:     return "iPhone's link to the server is rough. Spectrum erratic. Tuning still works."
      case .wristHop:      return "Watch link to iPhone is weak. Spectrum erratic. Tuning still works."
      case .indeterminate: return "Link rough. Spectrum erratic."
      }
    }())
    .onAppear  { withAnimation(.easeInOut(duration: 1).repeatForever(autoreverses: true)) { pulse = 0.5 } }
    .onDisappear { pulse = 1 }
  }

  /// Promote a raw reading to the screen only once it has HELD, and retire it only
  /// after it has been up long enough to read. Without this, a marginal link strobes
  /// the pill on every late frame.
  ///
  /// Driven by the row and state clocks, so it needs no timer of its own.
  private func syncHint() {
    let now = Date()
    guard let c = rawHint else {
      // Healthy again — but a pill that flashed up for 200ms is worse than none, so
      // hold it for its minimum read time before retiring it.
      if let shown = shownSince, now.timeIntervalSince(shown) < hintHold { return }
      hint = nil
      shownSince = nil
      return
    }
    guard heldLongEnough(c, now: now) else { return }
    if hint != c { hint = c; shownSince = now }
  }

  /// Facts the PHONE ASSERTS — it is reconnecting; its own link to the server is
  /// poor — are shown at once. They arrive already debounced (the phone's link meter
  /// only emits on a CHANGE, off jitter/RTT moving averages) and the phone pushes a
  /// state echo the moment either flips. There is nothing to wait for and nothing
  /// that can strobe.
  ///
  /// Facts the WATCH INFERS FROM SILENCE must wait, because one late frame is not a
  /// fault. Measure from the moment the condition BECAME true (lastRowAt + the gap
  /// threshold), not from the moment we noticed it: when the rows are the very thing
  /// that died, this view is only redrawing on the phone's 4s heartbeat, and dating
  /// the debounce from the observation would cost a whole heartbeat — the pill would
  /// arrive eight seconds into the problem it exists to explain.
  private func heldLongEnough(_ h: LinkHint, now: Date) -> Bool {
    switch h {
    case .reconnecting, .serverHop:
      return true
    case .wristHop, .indeterminate:
      guard let t = link.lastRowAt else { return false }
      return now.timeIntervalSince(t) >= hintRowGap + hintDebounce
    }
  }

  /// nil = healthy. Otherwise, WHY there's nothing moving — and the phone TELLS us
  /// which, rather than leaving us to guess from silence.
  ///
  /// "No spectrum from iPhone" was a symptom, not a diagnosis: a paused socket, a
  /// stalled renderer, a wedged link and a stale build all produce exactly the same
  /// blank screen, and that ambiguity cost hours of chasing. The state channel keeps
  /// working in every one of those cases except the last (it's why the frequency kept
  /// updating), so it carries the reason with it.
  private var stalledMessage: (icon: String, text: String)? {
    if !link.reachable {
      return ("iphone.slash", "Reconnecting to iPhone")
    }

    // ROWS BEAT ANY CLAIM. A waterfall that is drawing is PROOF the phone is on a
    // server, and no status message may contradict a fact we can see.
    //
    // This guard is the fix for a real bug: phoneStatus is only ever pushed when it
    // CHANGES, and the phone only announced 'ready' on the WATCH-DRIVEN launch path. So
    // a watch cold-boot that landed on 'pick' (favourites, but no default), followed by
    // the user simply connecting ON THE PHONE, left the status stuck at 'pick' forever —
    // and the wrist sat there telling the user to "Choose a server" over a live, tuning,
    // perfectly healthy waterfall. The phone now reports 'ready' on every connect, but a
    // stale claim must never be able to do this again, whatever the phone forgets to say.
    let rowsFlowing = link.lastRowAt.map { Date().timeIntervalSince($0) < 2.0 } ?? false

    // WHAT THE PHONE IS DOING. A cold launch is a BOOT, not a fault — reporting it as
    // a missing waterfall was both wrong and useless. The watch WAKES the phone (iOS
    // launches it straight into the background), so this state is normal, not an error.
    if !rowsFlowing {
      switch link.phoneStatus {
      case "starting":
        return ("hourglass", "Starting VibeSDR…")
      case "pick":
        // No default instance — but there ARE favourites. The wrist can choose.
        return ("server.rack", "Choose a server\nLong-press → Servers")
      case "setup":
        // Nothing to connect to. Say so plainly rather than showing a dead screen.
        // ♥ is FAVOURITE. ★ is DEFAULT. Getting that backwards in the one message a
        // stranded user reads would send them to press the wrong button.
        return ("iphone", "Open VibeSDR on iPhone\nand ♥ a server")
      default:
        break
      }
    }
    // Rows may never have arrived at all (a cold start) — the phone's own status
    // above still applies, and is checked BEFORE this.
    guard let t = link.lastRowAt, Date().timeIntervalSince(t) > 2.0 else { return nil }

    // SPLIT THE FALLBACK. "No spectrum from iPhone" covered two completely different
    // faults and told us nothing:
    //   - the phone isn't answering AT ALL (state messages aren't arriving either), or
    //   - the phone says it IS sending rows, and they aren't reaching us.
    // The phone answers every ping (we heartbeat at 4s), so a stale state message is
    // itself a finding.
    let stateFresh = link.lastStateAt.map { Date().timeIntervalSince($0) < 8 } ?? false

    // The phone TOLD us it isn't sending — that's a fact, not a guess, so say it now.
    //
    // These `why`-driven overlays all imply a FRESH state message, i.e. the
    // WCSession command path is alive — so the "tuning still works" line is safe
    // here, and it matters: a user staring at a black overlay has no reason to try
    // the crown unless told. The "Watch link lost" / "iPhone not responding"
    // overlays below must NOT gain that line — there, the command path is itself
    // the casualty.
    if stateFresh {
      switch link.why {
      case "paused":
        return ("pause.circle", "iPhone paused the spectrum")
      case "reconnecting":
        // A recovery IN PROGRESS is not a failure, and must not be drawn as one.
        // The phone's watchdog budget is ~15s, so hold the pill (which leaves the
        // last frames on screen) and only escalate to a hard overlay past that.
        // Beyond ~45s, stop making excuses and fall through to the "idle" message.
        let recovering = Date().timeIntervalSince(t)
        if recovering < 20 { return nil }
        if recovering < 45 {
          return ("arrow.triangle.2.circlepath", "Reconnecting to server…\nTuning still works")
        }
        return ("dot.radiowaves.left.and.right", "iPhone lost the server\nTuning may still work")
      case "idle":
        // "iPhone isn't receiving" read as a WATCH-side fault, which is exactly
        // backwards: the wrist is fine, the phone's link to the server is the
        // casualty. Name the hop, and preserve the half of the app that still works.
        return ("dot.radiowaves.left.and.right", "iPhone lost the server\nTuning may still work")
      default:
        break
      }
    }

    // Otherwise it's the LINK. Be slow to call it dead: a bumpy Bluetooth link is
    // normal, and until it has been quiet for a good while the warning pill (which
    // leaves the waterfall on screen) is the better answer.
    guard Date().timeIntervalSince(t) > 10 else { return nil }
    return stateFresh
      ? ("exclamationmark.triangle", "Watch link lost\nSpectrum stopped")
      : ("iphone.slash", "iPhone not responding")
  }

  /// Nothing has ever arrived — which, on a COLD start, is the normal state for the
  /// first few seconds while the phone boots. So it says what the phone is DOING
  /// (`stalledMessage` carries the phone's own status) rather than reporting a fault:
  /// "Starting VibeSDR…", "Choose a server", "Open VibeSDR on iPhone and save a
  /// favourite". A boot is not an error, and a wrist that cries wolf on every launch
  /// teaches you to ignore it.
  private var placeholder: some View {
    let msg = stalledMessage
      ?? (link.reachable ? ("dot.radiowaves.left.and.right", "Waiting for signal")
                         : ("iphone.slash", "Open VibeSDR on iPhone"))
    return VStack(spacing: 6) {
      Image(systemName: msg.0)
        .font(.title3)
        .foregroundStyle(.secondary)
      Text(msg.1)
        .font(.caption2)
        .foregroundStyle(.secondary)
        .multilineTextAlignment(.center)
    }
    .padding(.horizontal, 12)
  }

  // ── Frequency ticker ───────────────────────────────────────────────────────

  private var ticker: some View {
    Canvas { ctx, size in
      guard link.span > 0 else { return }
      let lo = link.frequency - link.span / 2
      let hi = link.frequency + link.span / 2
      let stepHz = tickStep(span: link.span)
      let x = { (hz: Double) in (hz - lo) / (hi - lo) * size.width }

      var hz = (lo / stepHz).rounded(.up) * stepHz
      while hz <= hi {
        let px = x(hz)
        ctx.stroke(
          Path { $0.move(to: CGPoint(x: px, y: size.height - 4))
                 $0.addLine(to: CGPoint(x: px, y: size.height)) },
          with: .color(.white.opacity(0.5)),
          lineWidth: 1
        )
        var label = ctx.resolve(
          Text(tickLabel(hz, span: link.span))
            .font(.system(size: 8, weight: .medium, design: .rounded))
        )
        label.shading = .color(.white.opacity(0.75))
        let w = label.measure(in: size).width
        // Don't let an edge label hang off the screen.
        let cx = min(max(px, w / 2), size.width - w / 2)
        ctx.draw(label, at: CGPoint(x: cx, y: 5), anchor: .center)
        hz += stepHz
      }

      // BAND BOUNDARIES — a MARK, not a wash.
      //
      // Tinting the whole strip was tried and it earns nothing: it is either too faint to
      // notice or too strong to read the tick labels through, and either way it only tells
      // you WHICH band you're in — which the label above already says, in words. A mark at
      // the edge tells you something the label cannot: how far you are from leaving it.
      // That is the thing you want to know while the crown is turning under your finger.
      let edges = [link.bandLo, link.bandHi].filter { $0 > 0 && $0 > lo && $0 < hi }
      let edgeCol = link.bandColor ?? .white
      for e in edges {
        let px = x(e)
        // Full-height bar in the band's own colour, with a dark keyline either side so it
        // survives a bright tick label landing on top of it.
        ctx.fill(Path(CGRect(x: px - 2, y: 0, width: 4, height: size.height)),
                 with: .color(.black.opacity(0.85)))
        ctx.fill(Path(CGRect(x: px - 1, y: 0, width: 2, height: size.height)),
                 with: .color(edgeCol))
      }
    }
    .frame(height: 14)
    // 0.45 → 0.68. It sat at 0.45 while the band label DIRECTLY ABOVE IT sits at 0.62, and
    // the difference is plainly visible on a bright waterfall: one reads, the other doesn't.
    // A touch darker still than the label, because the tick text is 8pt against the band
    // name's 10pt — the smaller the type, the more ground it needs under it.
    .background(.black.opacity(0.68))
    .clipShape(RoundedRectangle(cornerRadius: 4))
  }

  /// THE BAND, in words, in the strip beside the clock.
  ///
  /// A frequency alone tells you nothing unless you already know the band plan by heart —
  /// the phone says "20m Ham Band" under its waterfall, and the wrist had nowhere to say
  /// it at all. watchOS reserves this band whether we use it or not and the clock only
  /// occupies its right end, so this is the one piece of genuinely free space on a watch.
  ///
  /// Coloured by the band, so the label and the ticker underneath agree at a glance.
  private var bandLabel: some View {
    Group {
      if !link.bandName.isEmpty {
        Text(link.bandName)
          // WHITE. It was drawn in the BAND'S colour, and that is a mistake this app has
          // a rule against: legibility comes from darkening, never from the accent. A
          // band-blue label 11pt tall on a dark strip is simply not readable — the colour
          // belongs on the boundary MARK, which is a shape and can carry it, not on the
          // text, which cannot.
          .font(.system(size: 10, weight: .semibold, design: .rounded))
          .foregroundStyle(.white)
          .lineLimit(1)
          .minimumScaleFactor(0.7)
          .padding(.horizontal, 7)
          .padding(.vertical, 2)
          // Its own scrim now that it lives over the waterfall rather than inside the top
          // strip's gradient. Darkening, never frosting — the usual rule.
          .background(.black.opacity(0.62), in: Capsule())
      }
    }
  }

  /// A slim dark strip behind the clock's row, carrying the battery.
  ///
  /// The band NAME used to live up here too, and it was wrong twice over: the top of the
  /// screen is the NEWEST spectrum — the line that just arrived — and a label there covers
  /// exactly the data you are tuning by. It also stacked ABOVE the battery, so the battery
  /// shifted whenever the label appeared or vanished. The label has moved to the ticker,
  /// where the waterfall is already seconds old and nobody is reading it. The battery is
  /// pinned again, and cannot move.
  /// JUST THE BATTERY, in its own small dark pill, to the left of the clock.
  ///
  /// It carried a full-width dark strip for a while, and that was a mistake made for a
  /// reason that no longer exists: the strip was there to carry the BAND LABEL as well —
  /// and the label has since moved down to the ticker, where it belongs. What was left was
  /// a band of black across the newest rows of the spectrum, the ones you are actually
  /// tuning by, in order to darken one corner.
  ///
  /// A strip that also tried to wrap the clock was worse still: the clock is drawn by
  /// watchOS, outside our coordinate space and inside its own safe-area inset, and chasing
  /// it produced a badge that shifted about and grew a lump of black off its edge.
  ///
  /// So: one badge, one scrim, nothing else. It does not touch the spectrum it doesn't
  /// need to.
  /// THE CLOCK AND THE BATTERY, ON ONE SOFT DARK CORNER.
  ///
  /// It went through a full-width strip (which blacked out the newest rows of the spectrum
  /// — the ones you actually tune by — to darken one corner), and a hard pill that tried to
  /// WRAP the clock (which chased a badge watchOS draws in its own coordinate space, and so
  /// wandered and grew a lump of black off its edge). Both had EDGES, and every edge was a
  /// thing to get wrong.
  ///
  /// A radial fade has none. It is dark where the chrome is — the clock and the battery,
  /// side by side in the top-right — and simply gone before it reaches the spectrum. The
  /// two badges sit on one background, which is what makes them read as one thing rather
  /// than as a badge and a floating number.
  ///
  /// The dark is HELD at full strength out past the battery (~80pt from the corner) and
  /// only then allowed to fall away. A fade that starts falling immediately is either too
  /// weak at the badge or too deep into the waterfall — hold, then drop, and it can be both
  /// solid where it matters and gone where it doesn't.
  /// ONE CAPSULE, holding the battery AND the clock.
  ///
  /// Drawn at an EXPLICIT size rather than wrapped around its contents. The clock is drawn
  /// by watchOS, in its own coordinate space and inside its own safe-area inset — there is
  /// nothing here to lay out around it, so laying out around it was the mistake. A capsule
  /// of known width, pinned to the corner, simply passes underneath it.
  ///
  /// (A radial fade was tried instead and it reads as a smudge, not a badge: soft enough
  /// not to cost the spectrum anything is soft enough not to look deliberate.)
  private var topStrip: some View {
    ZStack(alignment: .topTrailing) {
      // MEASURED, not guessed. Off a device screenshot (Ultra 3, 205x251pt) the clock's
      // digits run 18.0 → 58.8 pt in from the right edge, centred on y = 27.3, and are
      // 10.7pt tall. Everything below is derived from those three numbers — which is why
      // there is no round number anywhere in it.
      //
      // Real estate is the whole problem here: every point this covers is a point of
      // spectrum, and every point it DOESN'T cover is chrome you can't read against a
      // bright waterfall. There is no slack to be sloppy with, so it is sized to the two
      // badges and nothing more: 7pt of air around them, and it stops.
      // NO BACKGROUND HERE. The waterfall Canvas has drawn a clock scrim since long before
      // the battery existed (see `waterfall`), and it now covers the battery as well. A
      // badge that brings its own is a second scrim stacked on the first — which is exactly
      // what you could see, and no amount of reshaping it would have helped.
      BatteryPill(level: link.battery, scrim: false)   // the canvas scrim darkens for us
        // The clock's leftmost pixel is 58.8pt in. 66 puts the battery's right edge 7pt
        // clear of it — enough to read as separate, not enough to waste.
        .padding(.trailing, 66)
        // Centred on the clock: 27.3 less half the pill's 13pt height.
        .padding(.top, 21)
    }
    // FILL THE SCREEN. Without this the ZStack shrinks to fit its own contents, and
    // `.topTrailing` then aligns them against nothing — the badge lands in the middle of
    // the waterfall, which is exactly what it did.
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
    .ignoresSafeArea()      // measure from the REAL edges, not from inside the inset
    .allowsHitTesting(false)
  }

  /// A 1/2/5 x 10^n step giving ~4 ticks across the span. Tick density therefore
  /// reacts to zoom instead of crowding as you narrow the span.
  private func tickStep(span: Double) -> Double {
    let raw = span / 4
    let mag = pow(10, floor(log10(raw)))
    let n = raw / mag
    let mult: Double = n < 1.5 ? 1 : n < 3.5 ? 2 : n < 7.5 ? 5 : 10
    return mult * mag
  }

  /// Labels must switch UNITS with the span — decimal places alone overflow a
  /// ~150px bar (CW at ~5kHz wants Hz; FM broadcast at ~2MHz wants 100s of kHz).
  private func tickLabel(_ hz: Double, span: Double) -> String {
    if span >= 1_000_000 { return String(format: "%.1fM", hz / 1_000_000) }
    if span >= 10_000    { return String(format: "%.0fk", hz / 1_000) }
    if span >= 1_000     { return String(format: "%.1fk", hz / 1_000) }
    return String(format: "%.0f", hz)
  }

  // ── Frequency + signal ─────────────────────────────────────────────────────
  //
  // The pill's BACKGROUND *is* the signal meter — same trick as the phone's
  // ControlsBar, so there's no separate bar stealing height. The fill is the
  // phone's own smoothed 0..1 level, drawn with the phone's own red->green ramp.

  private var readout: some View {
    // Hugs its content and centres, rather than stretching edge-to-edge with the
    // two figures pushed into opposite corners — the watch's rounded corners were
    // clipping them there.
    HStack(spacing: 8) {
      Text(formatFreq(link.frequency, step: link.step, unit: link.displayUnit))
        .font(.system(size: 15, weight: .semibold, design: .rounded))
        .monospacedDigit()
        // Shrink to fit rather than scroll. A marquee would be an animation
        // running behind the waterfall forever, for text that is only long in the
        // CW case; scaling costs nothing and is always readable at a glance.
        .lineLimit(1)
        .minimumScaleFactor(0.55)
      // Whatever the phone's meter says — S-meter, dBFS, SNR or FM-DX's dBf. We
      // do not choose the metric here; see WatchLink.meter.
      Text(link.meter.isEmpty ? "—" : link.meter)
        .font(.system(size: 11, weight: .semibold, design: .rounded))
        .monospacedDigit()
        .foregroundStyle(.white.opacity(0.9))
        .layoutPriority(-1)   // the frequency wins the space
    }
    .padding(.horizontal, 12)
    .padding(.vertical, 4)
    .background(alignment: .leading) {
      GeometryReader { geo in
        ZStack(alignment: .leading) {
          Color.black.opacity(0.55)                       // track + scrim in one
          LinearGradient(
            stops: SignalGradient.stops(for: link.level),
            startPoint: .leading, endPoint: .trailing
          )
          .frame(width: geo.size.width * min(1, max(0, link.level)))
          .animation(.easeOut(duration: 0.12), value: link.level)
        }
      }
    }
    .clipShape(Capsule())
  }

  /// Two rules, and they're independent.
  ///
  /// UNIT IS INPUT-AWARE: whatever you last entered on the numpad is what it reads
  /// back in. Type 4582 kHz and you get "4582.000 kHz", not "4.582 MHz" —
  /// rendering everything ≥1MHz as MHz was technically right and practically
  /// wrong, because it threw away the frame of reference you were working in.
  /// (.auto keeps the old size-based behaviour until you've told us otherwise.)
  ///
  /// PRECISION FOLLOWS THE STEP: 3 decimals of MHz is 1kHz resolution, so on CW
  /// (1-10Hz steps) you literally could not see what you were tuning. The digits
  /// you get are the ones that can actually move.
  private func formatFreq(_ hz: Double, step: Double, unit: WatchLink.DisplayUnit) -> String {
    if hz <= 0 { return "—" }

    let resolved: WatchLink.DisplayUnit = {
      guard unit == .auto else { return unit }
      if hz >= 1_000_000 { return .mhz }
      if hz >= 1_000     { return .khz }
      return .hz
    }()

    switch resolved {
    case .mhz:
      let dp: Int
      switch step {
      case ..<10:    dp = 6      // 1Hz  — CW
      case ..<100:   dp = 5      // 10Hz
      case ..<1_000: dp = 4      // 100Hz
      default:       dp = 3      // 1kHz+
      }
      return String(format: "%.\(dp)f MHz", hz / 1_000_000)

    case .khz:
      let dp = step < 10 ? 3 : (step < 100 ? 2 : (step < 1_000 ? 1 : 0))
      return String(format: "%.\(dp)f kHz", hz / 1_000)

    case .hz, .auto:
      return String(format: "%.0f Hz", hz)
    }
  }
}

/// Port of the phone's sigGradient() (ControlsBar.tsx). The ramp GROWS with the
/// level — a weak signal is pure red, and green only enters once the bar is
/// actually long — so the colour at the tip means the same thing on both screens.
enum SignalGradient {
  static func stops(for sig: Double) -> [Gradient.Stop] {
    let red    = Color(red: 0xbb / 255, green: 0x11 / 255, blue: 0x00 / 255)
    let orange = Color(red: 0xff / 255, green: 0x44 / 255, blue: 0x00 / 255)
    let amber  = Color(red: 0xff / 255, green: 0xaa / 255, blue: 0x00 / 255)
    let green  = Color(red: 0x00 / 255, green: 0xdd / 255, blue: 0x44 / 255)

    if sig < 0.20 {
      return [.init(color: red, location: 0), .init(color: orange, location: 1)]
    }
    if sig < 0.58 {
      return [.init(color: red, location: 0),
              .init(color: orange, location: 0.20 / sig),
              .init(color: amber, location: 1)]
    }
    return [.init(color: red, location: 0),
            .init(color: orange, location: 0.15),
            .init(color: amber, location: 0.45),
            .init(color: green, location: 1)]
  }
}
