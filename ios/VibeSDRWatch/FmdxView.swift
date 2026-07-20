import SwiftUI
import WatchKit

/// FM-DX: a SECOND SCREEN, not a variant of the waterfall.
///
/// FM-DX has no spectrum at all, so the STATION is the content. The layout is
/// therefore the inverse of the spectrum screen: the logo fills the background,
/// frosted, and everything you read sits over it.
///
/// ── THE CROWN IS DISARMED BY DEFAULT. This is FM-DX ONLY. ────────────────────
/// An FM-DX server has ONE receiver, shared: retuning it changes the frequency for
/// EVERY listener on that server. So an accidental crown nudge from a wrist
/// movement is not a private mistake — it yanks the radio out from under everyone
/// else. You must arm the crown deliberately, and it disarms itself again.
///
/// Do NOT generalise this to the other backends. Kiwi and OWRX give every user
/// their OWN VFO, so tuning there disturbs nobody and an arming step would be pure
/// friction. (OWRX's only communal surface is the profile switcher, which is
/// deliberately not on the watch.)
///
/// The LISTENER COUNT is the other half of the same idea, and the sharper half: it
/// turns "am I allowed to tune?" from a guess into a fact you can read at a glance.
/// Alone — tune freely. Others listening — think, or go and ask on the phone.
struct FmdxView: View {
  @EnvironmentObject var link: WatchLink

  /// Explicit clock, like the spectrum screen. NOT TimelineView(.animation), whose
  /// cadence watchOS is free to ignore. Only drives the RadioText marquee.
  private let driver = Timer.publish(every: 1.0 / 20.0, on: .main, in: .common).autoconnect()
  @State private var tick = 0

  @State private var showFavs = false
  @State private var armed = false
  /// The crown drives the iPhone's SYSTEM volume instead of the tuner.
  ///
  /// This screen has no long-press control menu (the waterfall's crown-mode picker lives
  /// there), so volume needs a control of its own up here — otherwise it would simply be
  /// unreachable on FM-DX. Unlike the tune latch it does NOT time out: it is not
  /// dangerous, it is just a mode, and a mode that expires under you is worse than one
  /// you have to switch back.
  @State private var volumeMode = false

  /// First-run coach. Its OWN flag — this screen's gestures are nothing like the
  /// waterfall's, and someone who only ever opens FM-DX must still be taught it.
  @AppStorage("coachSeenFmdx") private var coachSeen = false
  @State private var disarmAt: Date? = nil
  @State private var crown = 0.0
  @State private var lastDetent = 0
  @FocusState private var crownFocused: Bool

  private static let detents = 1000.0
  /// Arming times out. A crown left live on a shared receiver is a hazard you have
  /// forgotten about — the whole point is that tuning is a deliberate act.
  private static let armSeconds: TimeInterval = 10

  private var st: WatchLink.FmdxState { link.fmdx ?? .init() }

  var body: some View {
    ZStack {
      background

      // A FIXED SKELETON. Every row that can change height is pinned, and only ONE
      // region is allowed to flex.
      //
      // It was `Spacer / identity / Spacer`, and the identity block changes height
      // constantly — RDS arrives, a PTY capsule appears, the transmitter resolves.
      // Two springs either side of a growing box means every one of those turns into
      // a reflow of the whole screen, so the layout visibly jumped and glitched as
      // the station data trickled in.
      VStack(spacing: 0) {
        topBar
        controlRow
        identity
          .frame(maxWidth: .infinity, maxHeight: .infinity)   // the ONLY flexible row
        dial
        readouts
      }
      // THE DISPLAY IS A ROUNDED RECTANGLE AND EVERY MEASUREMENT YOU GET IS A PLAIN
      // ONE. The top bar sat at x=0,y=0 and the corner curve ate the listener count.
      // Inset generously, and drop into the CLOCK'S BAND rather than above it — the
      // clock is NOT flush to the bezel (it sits ~11pt down), so anything level with
      // the very top is in the curve by definition.
      .padding(.horizontal, 12)
      .padding(.top, 12)
      .padding(.bottom, 8)

      // THE BATTERY, placed EXACTLY as the waterfall screen places it.
      //
      // It lived in this screen's top-bar HStack, which meant two screens were siting the
      // same badge with two different sets of paddings — and it visibly JUMPED as you
      // moved between them. A thing that appears on both screens must be positioned by
      // one rule, not by two that happen to agree. (Same numbers as ContentView: the
      // clock sits ~11pt down and owns the right corner.)
      // Same badge, same corner, same numbers as the waterfall screen — see the note on its
      // `topStrip`. Positioned by ONE rule shared by both screens; it was sited twice once,
      // and it visibly shifted as you moved between them.
      // Same capsule, same numbers, as the waterfall screen — see the note on its `topStrip`.
      ZStack(alignment: .topTrailing) {
        // This screen has NO waterfall Canvas, so it has no clock scrim of its own to
        // inherit — this badge IS the only one, which is why it draws a background here and
        // none on the waterfall screen.
        //
        // 0.62, not the waterfall's 0.75: it sits over a blurred station logo, not over a
        // bright green spectrum, so the low-battery red already reads. Darkening past what
        // legibility needs is just a hole in the picture.
        RoundedRectangle(cornerRadius: 9, style: .continuous)
          .fill(.black.opacity(0.62))
          .frame(width: 92, height: 22)
          .padding(.top, 16)
          .padding(.trailing, 10)

        BatteryPill(level: link.battery, scrim: false)
          .padding(.trailing, 66)
          .padding(.top, 21)
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
      .ignoresSafeArea()
      .allowsHitTesting(false)

      // VOLUME HUD. Without it the crown was moving a value the user could not see — you
      // armed volume, turned, and nothing on screen changed, which reads as "broken"
      // whether or not the phone heard you. A control with no feedback is not a control.
      //
      // Overlaid, NOT in the layout: it must not reflow the station identity as it comes
      // and goes. Same visual language as the waterfall screen's crown overlay.
      if volumeMode {
        VStack {
          Spacer()
          HStack(spacing: 8) {
            Image(systemName: link.muted ? "speaker.slash.fill" : "speaker.wave.2.fill")
              .font(.system(size: 14, weight: .semibold))
              .foregroundStyle(link.muted ? .red : .green)
            ZStack(alignment: .leading) {
              Capsule().fill(.white.opacity(0.22))
              Capsule()
                .fill(link.muted ? Color.red : Color.green)
                .frame(width: max(4, 90 * link.volume))
            }
            .frame(width: 90, height: 5)
            Text("\(Int((link.volume * 100).rounded()))")
              .font(.system(size: 13, weight: .semibold, design: .rounded))
              .monospacedDigit()
              .foregroundStyle(.white)
          }
          .padding(.horizontal, 12)
          .padding(.vertical, 8)
          .background(.black.opacity(0.78), in: Capsule())
          .padding(.bottom, 26)
        }
        .allowsHitTesting(false)   // never steal a tap from the controls beneath
        .transition(.opacity)
      }

      // THE COACH, once. This screen NEEDS one more than the waterfall does: its crown is
      // DEAD by default, so a user who doesn't know about the latch turns it, nothing
      // happens, and they conclude the app is broken. And the reason it's dead is the one
      // thing they genuinely must be told — there is a real person at the other end of
      // that receiver, and tuning it moves the dial for them too.
      if link.fmdx != nil && !coachSeen {
        CoachOverlay(
          title: "FM-DX Tuner",
          items: [
            .init(glyph: "dial.medium",
                  text: "Tap the dial button to arm the Crown, then turn it to tune"),
            .init(glyph: "speaker.wave.2.fill",
                  text: "Tap the speaker to give the Crown your iPhone's volume"),
            .init(glyph: "timer",
                  text: "The Crown disarms itself after a few seconds"),
            .init(glyph: "server.rack",
                  text: "Tap the servers button to switch receiver"),
          ],
          caution: "This receiver is SHARED. Tuning it retunes it for everyone listening — which is why the Crown is off until you arm it.",
          onDismiss: {
            WKInterfaceDevice.current().play(.click)
            coachSeen = true
          }
        )
      }
    }
    .ignoresSafeArea()
    .focusable(true)
    .focused($crownFocused)
    .digitalCrownRotation(
      $crown,
      from: 0, through: Self.detents, by: 1,
      sensitivity: .low,               // FM channels are 100kHz apart: coarse is fine
      isContinuous: true,
      isHapticFeedbackEnabled: true
    )
    .onChange(of: crown) { _, new in
      let detent = Int(new.rounded())
      guard detent != lastDetent else { return }
      var delta = detent - lastDetent
      let range = Int(Self.detents)
      if delta >  range / 2 { delta -= range }
      if delta < -range / 2 { delta += range }
      lastDetent = detent

      // VOLUME MODE takes the crown outright. It needs no arming and no timeout: the
      // loudness in YOUR ear is yours, and turning it disturbs nobody on the shared
      // receiver. (Tuning is the dangerous one, and it keeps its latch below.)
      if volumeMode { link.volume(delta: delta); return }

      // DISARMED = the crown does nothing at all. Not "does something smaller".
      guard armed else { return }
      disarmAt = Date().addingTimeInterval(Self.armSeconds)   // keep it alive while used
      // The command carries the assertion — the PHONE enforces it (see tuneArmed).
      link.tuneArmed(delta: delta)
    }
    .onReceive(driver) { _ in
      tick &+= 1
      if let d = disarmAt, Date() >= d { armed = false; disarmAt = nil }
    }
    .sheet(isPresented: $showFavs) {
      FavouritesList(favs: link.favourites) { url in
        link.selectInstance(url)
        showFavs = false
      }
    }
    .onAppear { crownFocused = true; link.ping() }
  }

  // MARK: - Background

  /// The logo, frosted, filling the screen — with the app icon behind it when there
  /// is no logo, so the glass NEVER sits on nothing (which reads as a broken box).
  ///
  /// The blur is STATIC: it recomputes only when the station changes, never per
  /// frame. Nothing animates behind it — the RadioText scrolls in the layer ABOVE.
  /// ── `.clipped()` CLIPS DRAWING. IT DOES NOT CONSTRAIN LAYOUT. ────────────────
  ///
  /// This bit the layout hard: `Image.resizable().scaledToFill()` reports the
  /// SCALED-UP image as its ideal size, a ZStack sizes to its largest child, and so
  /// the instant a real logo arrived the background claimed a rectangle far bigger
  /// than the screen. Everything else was then laid out against THAT — the listener
  /// count drifted up into the corner curve and the signal bar fell off the bottom.
  /// With no logo it looked perfect, because the drawn fallback has no intrinsic
  /// size. So the bug appeared only once a station's logo actually loaded.
  ///
  /// The cure is to PIN the background to the hardware's rectangle, so it can never
  /// have an opinion about how big this view is. Size off `screenBounds`, not
  /// GeometryReader — the same rule the numpad already learned.
  private var background: some View {
    let screen = WKInterfaceDevice.current().screenBounds

    return ZStack {
      Color.black
      Group {
        if let d = link.logo, let img = UIImage(data: d) {
          Image(uiImage: img).resizable().scaledToFill()
        } else {
          // NOT the app icon asset: an .appiconset can't be loaded by name at
          // runtime, so that fallback would silently render NOTHING — the exact
          // "glass over a broken grey box" this is here to prevent. Draw something
          // that cannot be missing.
          ZStack {
            LinearGradient(colors: [.blue.opacity(0.55), .purple.opacity(0.45)],
                           startPoint: .topLeading, endPoint: .bottomTrailing)
            Image(systemName: "antenna.radiowaves.left.and.right")
              .font(.system(size: 90, weight: .light))
              .foregroundStyle(.white.opacity(0.30))
          }
        }
      }
      .frame(width: screen.width, height: screen.height)   // <- the load-bearing line
      .blur(radius: 18)
      .opacity(0.55)
      .clipped()
      // A scrim, NOT more glass. Frosting blurs but does not DARKEN, so white text
      // over a frosted white logo is still white-on-white. Same rule as the
      // spectrum screen's chrome.
      LinearGradient(colors: [.black.opacity(0.25), .black.opacity(0.85)],
                     startPoint: .top, endPoint: .bottom)
    }
    .frame(width: screen.width, height: screen.height)
    .clipped()
    .ignoresSafeArea()
  }

  // MARK: - Top bar (the clock's band — free height)

  /// watchOS reserves this strip whether we use it or not, and the clock only sits
  /// at its RIGHT end. The listener count and the arm button cost ZERO height here.
  /// TWO ROWS, because three controls plus the listener count cannot share one.
  ///
  /// The clock's band is free height, but it is NOT free width — the clock reserves ~62pt
  /// of it, and on a 41mm watch that leaves ~90pt for everything else. Three buttons and
  /// a count do not fit in 90pt at a legal tap size (they'd be ~15pt each; the minimum is
  /// 24). A third button was added for volume, and the count is what got pushed off.
  ///
  /// So the band keeps what it can genuinely hold — the listener count, which is the one
  /// thing here that is INFORMATION rather than an affordance — and the controls drop to
  /// their own row underneath, where they have the full width and can be a comfortable
  /// size. The cost is one row of height, and it comes out of `identity`, which is the
  /// designated flexible row and absorbs it without reflowing anything else.
  /// The clock's band: STATUS only. Who's listening, how much battery is left, and the
  /// clock itself. Nothing here changes what the crown does.
  ///
  /// EXTRA LEADING INSET. The display is a rounded rectangle and every measurement you
  /// get is a plain one, so the top-left is the worst place on the screen to put anything
  /// — the corner curve ate the listener glyph at the shared 12pt inset. The top row
  /// needs more than the rows below it, because only the top row is in the curve.
  private var topBar: some View {
    HStack(spacing: 6) {
      Label("\(st.users)", systemImage: "person.fill")
        .font(.system(size: 12, weight: .semibold, design: .rounded))
        .labelStyle(.titleAndIcon)
        .foregroundStyle(.white.opacity(0.9))
        .fixedSize()          // never truncate the count — a truncated number is a lie
      ServersButton(show: $showFavs)
      Spacer()
      // NO BatteryPill here. It is placed by the SAME overlay both screens use (see the
      // ZStack above) — two screens positioning it with two sets of paddings is exactly
      // how it ended up visibly jumping when you moved between them.
      Color.clear.frame(width: 62, height: 1)   // the clock's territory
    }
    .padding(.leading, 6)     // clear of the corner curve — see above
    .frame(height: 22)
  }

  /// WHAT THE CROWN DOES — and nothing else. `armButton` and `volumeButton` are the two
  /// claims on the crown, they are mutually exclusive, and they are the only two controls
  /// on this screen that change its behaviour. So they get a row to themselves, where you
  /// can see at a glance which one won.
  private var controlRow: some View {
    HStack(spacing: 10) {
      armButton
      volumeButton
      Spacer()
    }
    // Inset to match the top row, off the corner curve — and dropped clear of the row
    // above, because Servers sat DIRECTLY over Volume and reaching for one hit the other.
    // Adjacent rows of small targets need real space between them, not just a boundary.
    .padding(.leading, 6)
    .padding(.top, 6)
    .frame(height: 30)
  }

  /// Hand the crown to the iPhone's SYSTEM volume.
  ///
  /// This screen has no long-press menu, so without a button of its own volume would be
  /// simply unreachable on FM-DX. It shares the arm button's badge grammar — same shape,
  /// same green ✓ — so the two read as one family: "what is the crown doing?"
  ///
  /// Taking the crown for volume DISARMS tuning, because the crown can only do one thing
  /// and leaving a shared receiver armed behind a mode you have switched away from is
  /// exactly the hazard the latch exists to prevent.
  private var volumeButton: some View {
    Button {
      volumeMode.toggle()
      if volumeMode { armed = false; disarmAt = nil }
      WKInterfaceDevice.current().play(volumeMode ? .start : .stop)
    } label: {
      Image(systemName: link.muted ? "speaker.slash.fill" : "speaker.wave.2.fill")
        .font(.system(size: 12, weight: .semibold))
        .foregroundStyle(.white)
        .overlay(alignment: .bottomTrailing) {
          Image(systemName: volumeMode ? "checkmark.circle.fill" : "xmark.circle.fill")
            .font(.system(size: 8, weight: .bold))
            .foregroundStyle(volumeMode ? .green : .red)
            .background(Circle().fill(.black))
            .offset(x: 5, y: 4)
        }
        // 32x26 → 36x30. Big enough to hit (reaching for Volume was landing on Servers),
        // small enough not to crowd the station name underneath. The frame is the target,
        // not the glyph — the extra area is free, it just must not eat the layout.
        .frame(width: 36, height: 30)
        // The same chip the arm button wears. They are the two claims on the crown; they
        // must read as one family, and the chip is what says "this is a latch".
        .background(RoundedRectangle(cornerRadius: 8)
          .fill(volumeMode ? .green.opacity(0.22) : .white.opacity(0.14)))
        .contentShape(Rectangle())             // the whole frame is tappable, not the ink
    }
    .buttonStyle(.plain)
    .accessibilityLabel(volumeMode ? "Crown controls iPhone volume. Tap to release."
                                   : "Give the crown to iPhone volume")
  }

  /// A RADIO TUNING SCALE, badged: red ✗ when the crown is dead, green ✓ when live.
  ///
  /// Drawn, not an SF Symbol. `slider.horizontal.3` was there first and read as
  /// "settings" or "equaliser" — it told you nothing about what the button DOES. A
  /// tick scale with a needle is the universal picture of tuning a radio (it's the
  /// dial off the front of every receiver ever made), so the button explains itself
  /// without a label, which is the only kind of explanation that fits up here.
  ///
  /// Deliberately small: it's a latch, not a control you use constantly.
  private var armButton: some View {
    Button {
      armed.toggle()
      disarmAt = armed ? Date().addingTimeInterval(Self.armSeconds) : nil
      // The crown does ONE thing. Arming tuning takes it back off volume.
      if armed { volumeMode = false }
      WKInterfaceDevice.current().play(armed ? .start : .stop)
    } label: {
      TuneScaleGlyph()
        .stroke(.white, style: StrokeStyle(lineWidth: 1.1, lineCap: .round))
        .frame(width: 18, height: 11)
        .overlay(alignment: .bottomTrailing) {
          Image(systemName: armed ? "checkmark.circle.fill" : "xmark.circle.fill")
            .font(.system(size: 8, weight: .bold))
            .foregroundStyle(armed ? .green : .red)
            .background(Circle().fill(.black))
            .offset(x: 5, y: 4)
        }
        .frame(width: 36, height: 30)          // the TAP TARGET is the frame, not the glyph
        .background(RoundedRectangle(cornerRadius: 8)
          .fill(armed ? .green.opacity(0.22) : .white.opacity(0.14)))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }

  // MARK: - Identity (the middle — the station, not the numbers)

  /// The logo sits behind everything, so the middle of the screen was dead space.
  /// This is what FM-DX is actually FOR: who is this, what are they playing, and
  /// how far away are they.
  private var identity: some View {
    VStack(spacing: 3) {
      if !st.tx.isEmpty {
        Text(st.tx)
          .font(.system(size: 15, weight: .bold, design: .rounded))
          .foregroundStyle(.white)
          .lineLimit(2)
          .multilineTextAlignment(.center)
          .minimumScaleFactor(0.7)
      }

      HStack(spacing: 4) {
        if !st.flag.isEmpty { Text(st.flag).font(.system(size: 12)) }
        if !st.city.isEmpty {
          Text(st.city)
            .font(.system(size: 11, weight: .medium))
            .foregroundStyle(.white.opacity(0.8))
            .lineLimit(1)
        }
        // Distance is from the SERVER's location, not yours — it's how far the
        // signal travelled to the receiver, which is the DX part of FM-DX.
        if st.dist > 0 {
          Text("\(Int(st.dist)) km")
            .font(.system(size: 11, weight: .semibold, design: .rounded))
            .monospacedDigit()
            .foregroundStyle(.orange)
        }
      }

      // "46 km" IS MEANINGLESS WITHOUT AN ORIGIN — and the origin is not you, it's
      // the receiver you're borrowing, which may be in another country. Name it.
      if st.dist > 0 && !st.rx.isEmpty {
        Text("to \(st.rx)")
          .font(.system(size: 9, weight: .medium))
          .foregroundStyle(.white.opacity(0.55))
          .lineLimit(1)
          .truncationMode(.tail)
      }

      if !st.pty.isEmpty {
        Text(st.pty.uppercased())
          .font(.system(size: 9, weight: .semibold, design: .rounded))
          .foregroundStyle(.white.opacity(0.75))
          .padding(.horizontal, 6)
          .padding(.vertical, 2)
          .background(Capsule().fill(.white.opacity(0.16)))
          .lineLimit(1)
      }
    }
    .padding(.horizontal, 4)
  }

  // MARK: - Dial (the phone's, mirrored)

  /// The BAND, with the station memory pinned to it — the same dial the phone draws,
  /// from the same learned list, so the wrist is a mirror and not a second idea of
  /// what the band looks like.
  ///
  /// The VFO is FIXED AT THE CENTRE and the band slides under it, exactly as the
  /// waterfall does: you tune the dial to the station, not the marker to the dial.
  /// Same LED palette as the phone's drum — green band, warm red needle.
  private var dial: some View {
    Canvas { ctx, size in
      let midX = size.width / 2
      let span = Self.dialSpanHz                 // total width of the visible band
      let hzToX = { (hz: Double) in midX + (hz - st.freq) / span * size.width }

      // Ticks every 100 kHz (the FM channel raster), taller every 1 MHz.
      let start = ((st.freq - span / 2) / 100_000).rounded(.down) * 100_000
      var hz = start
      while hz <= st.freq + span / 2 {
        let x = hzToX(hz)
        let isMHz = (hz / 1_000_000).truncatingRemainder(dividingBy: 1) == 0
        var p = Path()
        p.move(to: CGPoint(x: x, y: size.height - 1))
        p.addLine(to: CGPoint(x: x, y: size.height - (isMHz ? 13 : 6)))
        ctx.stroke(p, with: .color(.green.opacity(isMHz ? 0.8 : 0.4)),
                   lineWidth: isMHz ? 1.4 : 0.9)
        if isMHz {
          let label = Text("\(Int(hz / 1_000_000))")
            .font(.system(size: 10, weight: .semibold, design: .rounded))
            .foregroundStyle(.green.opacity(0.85))
          ctx.draw(label, at: CGPoint(x: x, y: size.height - 21))
        }
        hz += 100_000
      }

      // Learned stations. EVERY one in range gets a TICK — that's the proof it was
      // saved — but the NAMES are laid out with collision avoidance, exactly as the
      // phone's dial does it (FmdxDial: two rows, a minimum gap, nearest-to-tuned
      // placed first). Without it, a dense city band writes every name at the same
      // height and they overlap into mush.
      let inRange = link.stations
        .filter { abs($0.freqHz - st.freq) < span / 2 && !$0.name.isEmpty }

      for stn in inRange {
        let x = hzToX(stn.freqHz)
        var p = Path()
        p.move(to: CGPoint(x: x, y: size.height - 1))
        p.addLine(to: CGPoint(x: x, y: size.height - 17))
        ctx.stroke(p, with: .color(.green), lineWidth: 1.6)
      }

      // Two rows, and the ones NEAREST what you're tuned to get first refusal — a
      // label that has to be dropped should be a distant one, never the neighbour
      // you're about to tune into.
      // Rows [3,14] → [2,12]. The two name rows sat a full text-height apart, which is
      // more air than they need — tightening them buys the MHz numbers underneath the
      // clearance they were missing, at no cost to legibility.
      let rowY: [CGFloat] = [2, 12]
      let minGap: CGFloat = 34
      var used: [[CGFloat]] = [[], []]

      for stn in inRange.sorted(by: {
        abs($0.freqHz - st.freq) < abs($1.freqHz - st.freq)
      }) {
        // Skip the tuned station: it's already the headline, and its label would sit
        // under the needle.
        guard abs(stn.freqHz - st.freq) > span * 0.04 else { continue }
        let x = hzToX(stn.freqHz)
        guard let row = (0..<rowY.count).first(where: { r in
          used[r].allSatisfy { abs($0 - x) >= minGap }
        }) else { continue }                       // no room on either row — drop it
        used[row].append(x)

        let t = Text(stn.name)
          .font(.system(size: 9, weight: .semibold, design: .rounded))
          .foregroundStyle(.green.opacity(0.85))
        ctx.draw(t, at: CGPoint(x: x, y: rowY[row]), anchor: .top)
      }

      // The needle: fixed at the centre, warm red, like the phone's drum.
      var n = Path()
      n.move(to: CGPoint(x: midX, y: 0))
      n.addLine(to: CGPoint(x: midX, y: size.height))
      ctx.stroke(n, with: .color(Color(hue: 4.0 / 360, saturation: 0.85,
                                       brightness: 1.0)), lineWidth: 1.6)
    }
    // 42 → 50. Shrinking this to 42 broke it: the MHz numbers are drawn from the BOTTOM
    // (size.height − 21) while the station names are laid out from the TOP, so taking
    // height out marched the two straight into each other and the names clipped the
    // numbers. Two rows of names + the numbers + the ticks need ~50pt, and that is not
    // negotiable — so the buttons gave the height back instead.
    .frame(height: 50)
    .background(.black.opacity(0.35), in: RoundedRectangle(cornerRadius: 8))
    .padding(.bottom, 4)
  }

  /// How much band the dial shows. Wide enough that a neighbouring station is
  /// visible before you reach it — the point of a dial is seeing what's coming.
  private static let dialSpanHz: Double = 2_000_000

  // MARK: - Readouts (bottom)

  private var readouts: some View {
    VStack(spacing: 2) {
      // Station name + RadioText, scrolling. The RDS text IS the content on FM-DX.
      marquee(rdsLine)

      // 26 → 22. The controls moved to a row of their own (three of them could not share
      // the clock's band), and that row has to be paid for out of somewhere. This is the
      // tallest thing on the screen and the most legible per point — it can give up 4pt
      // and still be readable across a room, which the station name and RDS cannot.
      Text(freqText)
        .font(.system(size: 22, weight: .semibold, design: .rounded))
        .monospacedDigit()
        .foregroundStyle(.white)
        .lineLimit(1)
        .minimumScaleFactor(0.8)
        .minimumScaleFactor(0.6)

      // PI, then the transmitter site IF it fits — it's the first thing to go.
      HStack(spacing: 5) {
        if !st.pi.isEmpty {
          Text(st.pi.uppercased())
            .font(.system(size: 10, weight: .semibold, design: .rounded))
            .monospacedDigit()
            .foregroundStyle(.cyan)
        }
        Spacer(minLength: 0)
        if st.stereo {
          Image(systemName: "dot.radiowaves.left.and.right")
            .font(.system(size: 9, weight: .semibold))
            .foregroundStyle(.green)
        }
        Text(st.meter.isEmpty ? "—" : st.meter)   // the phone's own meter, mirrored
          .font(.system(size: 10, weight: .semibold, design: .rounded))
          .monospacedDigit()
          .foregroundStyle(.white.opacity(0.9))
      }

      // dBf is a LEVEL, so draw it as one. Same red->green gradient as the phone's
      // signal bar — the number tells you the value, the bar tells you at a glance
      // whether it's any good, which is what you actually want on a wrist.
      signalBar
    }
    .padding(.horizontal, 7)
    .padding(.vertical, 5)
    .background(.black.opacity(0.45), in: RoundedRectangle(cornerRadius: 11))
  }

  private var signalBar: some View {
    GeometryReader { geo in
      ZStack(alignment: .leading) {
        Capsule().fill(.white.opacity(0.18))
        Capsule()
          .fill(LinearGradient(colors: [.red, .yellow, .green],
                               startPoint: .leading, endPoint: .trailing))
          .frame(width: max(2, geo.size.width * min(1, max(0, st.level))))
      }
    }
    .frame(height: 3)
  }

  private var rdsLine: String {
    let name = st.ps.trimmingCharacters(in: .whitespaces)
    let text = st.rt.trimmingCharacters(in: .whitespaces)
    if name.isEmpty && text.isEmpty { return link.reachable ? "No RDS" : "iPhone not reachable" }
    if text.isEmpty { return name }
    if name.isEmpty { return text }
    return "\(name)  ·  \(text)"
  }

  private var freqText: String {
    guard st.freq > 0 else { return "—" }
    return String(format: "%.2f MHz", st.freq / 1_000_000)
  }

  /// A marquee that only scrolls when it NEEDS to.
  ///
  /// RadioText is up to 64 characters and the screen holds maybe 20, so it has to
  /// move — but a station name that already fits must sit still: text that slides
  /// for no reason is just noise on a screen this size. Driven by the same explicit
  /// clock as everything else; the background behind it never animates.
  private func marquee(_ s: String) -> some View {
    let charW: CGFloat = 6.2                       // caption-ish, rounded
    let width = WKInterfaceDevice.current().screenBounds.width - 30
    let textW = CGFloat(s.count) * charW
    let overflow = max(0, textW - width)

    // Ease across, pause at each end — a constantly-cycling loop is unreadable
    // because you can never catch the start of the sentence.
    let period = 4.0 + Double(overflow) / 18.0     // longer text, slower crawl
    let t = Double(tick) / 20.0
    let phase = period > 0 ? (t.truncatingRemainder(dividingBy: period * 2)) / period : 0
    let eased = phase <= 1 ? phase : 2 - phase     // 0->1->0, with a dwell at the turns
    let offset = -overflow * min(1, max(0, eased * 1.4 - 0.2))

    return Text(s)
      .font(.system(size: 12, weight: .semibold, design: .rounded))
      .foregroundStyle(.white)
      .lineLimit(1)
      .fixedSize(horizontal: true, vertical: false)
      .offset(x: overflow > 0 ? offset : 0)
      .frame(width: width, alignment: overflow > 0 ? .leading : .center)
      .clipped()
  }
}

/// The face of a radio: a scale of ticks with a tuning needle standing over it.
///
/// Drawn rather than borrowed from SF Symbols, which has no dial — and the nearest
/// stand-ins (`slider.horizontal.3`, `dial.medium`) read as "settings" or "volume".
/// This shape is what a tuning scale looks like on every receiver ever built, so the
/// arm button needs no label to say what it does.
struct TuneScaleGlyph: Shape {
  func path(in r: CGRect) -> Path {
    var p = Path()
    let baseY = r.maxY - 1

    // The scale.
    p.move(to: CGPoint(x: r.minX, y: baseY))
    p.addLine(to: CGPoint(x: r.maxX, y: baseY))

    // Ticks, alternating long/short like a real dial's raster.
    let n = 7
    for i in 0..<n {
      let x = r.minX + r.width * CGFloat(i) / CGFloat(n - 1)
      let h: CGFloat = i.isMultiple(of: 2) ? 4 : 2.5
      p.move(to: CGPoint(x: x, y: baseY))
      p.addLine(to: CGPoint(x: x, y: baseY - h))
    }

    // The needle — off-centre, because a needle parked dead centre reads as just
    // another tick.
    let nx = r.minX + r.width * 0.63
    p.move(to: CGPoint(x: nx, y: r.minY))
    p.addLine(to: CGPoint(x: nx, y: baseY + 1))
    return p
  }
}
