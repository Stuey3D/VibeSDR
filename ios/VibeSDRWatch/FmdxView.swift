import SwiftUI
import WatchKit

/// FM-DX: a SECOND SCREEN, not a variant of the waterfall — ported from the companion (`ios/VibeSDRWatch/
/// FmdxView.swift`) and rebound from the phone-fed `WatchLink` to the standalone `SpikeLink` + `FmDxClient`.
///
/// FM-DX has no spectrum, so the STATION is the content. The dial is drawn from the spike's OWN learned
/// station memory (`link.learnedStations`) — the phone built that as it tuned and the companion piggybacked over
/// WCSession; standalone, the spike learns it itself (see SpikeLink.learnStation).
///
/// ── THE CROWN IS DISARMED BY DEFAULT. FM-DX ONLY. ────────────────────────────
/// An FM-DX server has ONE receiver, shared: retuning it changes the frequency for EVERY listener. So the
/// crown must be armed deliberately, and it disarms itself again. (This is why the chat here is about
/// TUNING etiquette, not OWRX's profile etiquette.)
struct FmdxView: View {
  @EnvironmentObject var link: WatchLink

  private let driver = Timer.publish(every: 1.0 / 20.0, on: .main, in: .common).autoconnect()
  @State private var tick = 0

  @State private var showChat = false
  @State private var showSettings = false
  @State private var armed = false
  @State private var volumeMode = false
  @AppStorage("seenFmdxTutorial") private var seenTut = false
  @State private var showTut = false
  @State private var showNumpad = false
  @State private var disarmAt: Date? = nil
  @State private var volTimer: DispatchWorkItem?
  @State private var crown = 0.0
  @State private var lastDetent = 0
  @FocusState private var crownFocused: Bool

  private static let detents = 1000.0
  private static let armSeconds: TimeInterval = 10
  private static let volSeconds: TimeInterval = 15

  private var st: FmdxInfo { link.fmdxInfo ?? FmdxInfo() }

  var body: some View {
    ZStack {
      background

      VStack(spacing: 0) {
        topBar
        controlRow
        identity.frame(maxWidth: .infinity, maxHeight: .infinity)   // the ONLY flexible row
        dial
        readouts
      }
      .padding(.horizontal, 12).padding(.top, 12).padding(.bottom, 8)

      // Battery — same corner + numbers as the waterfall screen.
      // Battery + connection glyphs, in ONE row BELOW the clock (top-right). The system clock owns the
      // very top-right corner and can't be covered, so this sits a row DOWN (top 40) to clear it —
      // the connection method + link-quality pair the DAB/ADS-B screens carry.
      ZStack(alignment: .topTrailing) {
        // ★★ SIDE BY SIDE, BUT SMALLER — matching Jr. The fault was only a SLIGHT clip of the
        //   battery against the display's corner arc, and stacking the pair to fix it cost more
        //   than it saved: the capsule got taller and reached down into the station name on a
        //   44mm, worse on a 41mm. A few points of trim beats a change of layout when the fault
        //   is a few points wide.
        HStack(spacing: WatchScale.s(6)) {
          ConnGlyph(transport: link.transport).font(.system(size: WatchScale.s(11)))
          QualityGlyph(link: link)
          BatteryPill(level: link.battery, scrim: false)
        }
        .padding(.horizontal, WatchScale.s(8)).padding(.vertical, WatchScale.s(3))
        .background(Capsule().fill(.black.opacity(0.55)))
        .padding(.top, 40).padding(.trailing, 10)
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topTrailing)
      .ignoresSafeArea().allowsHitTesting(false)

      // Native volume HUD — the real output/speaker volume (like DAB), not engine gain, so AirPods and
      // the built-in speaker reach full loudness. Times out like the other screens.
      if volumeMode {
        HStack {
          Spacer()
          // ★ No native VolumeControl in Buddy: WKInterfaceVolumeControl drives the WATCH's own

          //   output, and Buddy's crown drives the PHONE's system volume via cmd:vol. Keeping the crown

          //   ours is what makes the phone's real level readable and adjustable from the wrist.

          EmptyView()
        }
      }
    }
    .ignoresSafeArea()
    /* ★★★ THE CROWN STAYS OURS, ALWAYS — this released it to a VolumeControl that DOES NOT EXIST
     *   in Buddy. The comment fifteen lines up says so itself ("No native VolumeControl in Buddy")
     *   and renders EmptyView() in its place, but the focus rule was carried over from Jr, where a
     *   real WKInterfaceVolumeControl takes the crown. So in volume mode Buddy handed the crown to
     *   nothing: the wrist could not change the volume at all, and the phone had to be dug out to
     *   do it. Stuart, 2026-08-28: "the volume control in FM-DX mode isnt working ... turning up
     *   the volume on the phone i hear it."
     * ★★ Buddy's volume is the PHONE's system volume, relayed as cmd:vol — there is nothing local
     *   for the crown to be given to, which is exactly why keeping it is the whole mechanism. */
    .focusable()
    .focused($crownFocused)
    .digitalCrownRotation($crown, from: 0, through: Self.detents, by: 1,
                          sensitivity: .low, isContinuous: true, isHapticFeedbackEnabled: true)
    .onChange(of: crown) { _, new in
      let detent = Int(new.rounded())
      guard detent != lastDetent else { return }
      var delta = detent - lastDetent
      let range = Int(Self.detents)
      if delta >  range / 2 { delta -= range }
      if delta < -range / 2 { delta += range }
      lastDetent = detent
      // ★★ IN VOLUME MODE THE CROWN IS THE VOLUME. It used to return here and leave the delta on
      //    the floor, because Jr's VolumeControl would have consumed it; Buddy has none, so this is
      //    the only thing that can act on it. Not gated on `armed`: the arm switch protects a
      //    SHARED TUNER from being moved, and the loudness in your own ear is nobody else's
      //    business — the same rule the phone's own handler states.
      if volumeMode { link.volume(delta: delta); return }
      guard armed else { return }        // DISARMED = the crown does nothing on a shared receiver
      disarmAt = Date().addingTimeInterval(Self.armSeconds)
      link.tune(delta: delta)
    }
    .onChange(of: volumeMode) { _, v in
      // ★ Keep the focus in BOTH directions — the crown is ours either way now, it just means
      //   something different. Dropping focus here is what left volume mode with a dead crown.
      if v { crownFocused = true; armed = false; disarmAt = nil; armVolTimeout() }
      else { volTimer?.cancel(); crownFocused = true }
    }
    .onReceive(driver) { _ in
      tick &+= 1
      // DRIVE THE LINK. This is what mirrors the client's state (socket status, freq, RDS, chat,
      // listener count) into the published SpikeLink the UI reads — every other screen calls it and
      // the port dropped it, which is why FM-DX looked dead (blank + "connecting…") even when the
      // sockets were up. Audio decodes independently of this; the SCREEN does not.
      link.driverTick(now: ProcessInfo.processInfo.systemUptime)
      if let d = disarmAt, Date() >= d { armed = false; disarmAt = nil }
    }
    .sheet(isPresented: $showChat) { NavigationStack { ChatSheet().environmentObject(link) } }
    .sheet(isPresented: $showSettings) {
      // THIN-REMOTE server controls: the watch shows the phone-advertised state and relays taps; the
      // phone runs the FM-DX Webserver command. Only shown when the server actually exposes them.
      NavigationStack {
        ScrollView {
          VStack(spacing: 14) {
            let eq = link.fmdx?.eq ?? false
            let ims = link.fmdx?.ims ?? false
            HStack(spacing: 10) {
              fmdxOpt("cEQ", on: eq) { link.setFmdxEq(!eq) }
              fmdxOpt("iMS", on: ims) { link.setFmdxIms(!ims) }
            }
            if let ants = link.fmdx?.antennas, ants.count > 1 {
              Text("ANTENNA").font(.system(size: 10, weight: .semibold))
                .foregroundStyle(.white.opacity(0.5)).frame(maxWidth: .infinity, alignment: .leading)
              ForEach(ants) { a in
                let sel = (link.fmdx?.ant ?? 0) == a.id
                Button { link.setFmdxAntenna(a.id) } label: {
                  HStack {
                    Text(a.name).font(.system(size: 14)).lineLimit(1)
                    Spacer()
                    if sel { Image(systemName: "checkmark").font(.system(size: 13, weight: .bold)) }
                  }.frame(maxWidth: .infinity)
                }.buttonStyle(.plain).foregroundStyle(sel ? .green : .white.opacity(0.85))
                  .padding(.vertical, 8).padding(.horizontal, 12)
                  .background(RoundedRectangle(cornerRadius: 9).fill(.white.opacity(sel ? 0.12 : 0.05)))
              }
            }
            Divider().padding(.vertical, 2)
            // Servers is the way OUT (and to switch receiver) — must be here or FM-DX is a dead end.
            Button { showSettings = false; link.backToPicker() } label: {
              Label("Servers", systemImage: "antenna.radiowaves.left.and.right")
                .font(.system(size: 15, weight: .semibold)).frame(maxWidth: .infinity)
            }.tint(.orange)
          }.padding()
        }
      }
    }
    .onAppear {
      crownFocused = true
      if !seenTut { DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { showTut = true } }
    }
    // A PUSH, not a sheet — a sheet's grabber and inset eat the top of the pad and push its
    // bottom row off a small screen.
    .navigationDestination(isPresented: $showNumpad) {
      NumpadView().environmentObject(link)
    }
    .sheet(isPresented: $showTut) {
      TutorialSheet(title: "FM-DX Tuner", tips: fmdxTutorialTips()) { seenTut = true; showTut = false }
    }
  }

  /// A cEQ/iMS toggle chip — green when the server has it on (state comes from the phone).
  @ViewBuilder
  private func fmdxOpt(_ label: String, on: Bool, _ action: @escaping () -> Void) -> some View {
    Button(action: action) {
      Text(label).font(.system(size: 14, weight: .semibold)).frame(maxWidth: .infinity)
    }
    .buttonStyle(.plain)
    .foregroundStyle(on ? .black : .white.opacity(0.85))
    .padding(.vertical, 10)
    .background(RoundedRectangle(cornerRadius: 10).fill(on ? Color.green : Color.white.opacity(0.08)))
  }

  private func armVolTimeout() {
    volTimer?.cancel()
    let work = DispatchWorkItem { volumeMode = false }
    volTimer = work
    DispatchQueue.main.asyncAfter(deadline: .now() + Self.volSeconds, execute: work)
  }

  // MARK: - Background (no logo pipeline on the spike — the frosted fallback, always)

  private var background: some View {
    let screen = WKInterfaceDevice.current().screenBounds
    return ZStack {
      Color.black
      ZStack {
        LinearGradient(colors: [.blue.opacity(0.55), .purple.opacity(0.45)],
                       startPoint: .topLeading, endPoint: .bottomTrailing)
        Image(systemName: "antenna.radiowaves.left.and.right")
          .font(.system(size: 90, weight: .light)).foregroundStyle(.white.opacity(0.30))
      }
      .frame(width: screen.width, height: screen.height).blur(radius: 18).opacity(0.55).clipped()
      LinearGradient(colors: [.black.opacity(0.25), .black.opacity(0.85)], startPoint: .top, endPoint: .bottom)
    }
    .frame(width: screen.width, height: screen.height).clipped().ignoresSafeArea()
  }

  // MARK: - Top bar (clock's band) — the ChatGlyph carries the listener count AND opens chat

  private var topBar: some View {
    HStack(spacing: 8) {
      // Person+count glyph, breathing on inbound chat — same component + position as the waterfall screen.
      ChatGlyph(clients: st.users, activity: link.chatActivity) { showChat = true }
      // SETTINGS. Was a bare "back to servers" button — but an FM-DX server DOES have things worth
      // changing (antenna, cEQ, iMS), so it opens a menu with Servers at the bottom.
      Button { showSettings = true } label: {
        Image(systemName: "slider.horizontal.3").font(.system(size: 13, weight: .semibold))
          .foregroundStyle(.white).padding(4).contentShape(Rectangle())
      }.buttonStyle(.plain)
      Spacer()
      Color.clear.frame(width: 62, height: 1)   // the clock's territory
    }
    .padding(.leading, 6).frame(height: 22)
  }

  // MARK: - Control row — the two claims on the crown (arm-tune, volume)

  private var controlRow: some View {
    HStack(spacing: 10) { armButton; volumeButton; Spacer() }
      .padding(.leading, 6).padding(.top, 6)
      // 30 was the BUTTON height — but the ✓/✗ badges hang ~5pt BELOW their button
      // (offset y:4 from bottomTrailing), so pinning the row to 30 let them spill into the
      // station name underneath. Invisible at 49mm where there is slack; visible fouling at
      // 41mm. Reserve the overhang instead of clipping the badge, which is the bit that
      // tells you whether the crown is armed. (Found on a 41mm simulator, 2026-07-19.)
      .frame(height: WatchScale.s(36))
  }

  private var volumeButton: some View {
    Button {
      volumeMode.toggle()
      WKInterfaceDevice.current().play(volumeMode ? .start : .stop)
    } label: {
      Image(systemName: link.muted ? "speaker.slash.fill" : "speaker.wave.2.fill")
        .font(.system(size: 12, weight: .semibold)).foregroundStyle(.white)
        .overlay(alignment: .bottomTrailing) {
          Image(systemName: volumeMode ? "checkmark.circle.fill" : "xmark.circle.fill")
            .font(.system(size: WatchScale.s(8), weight: .bold)).foregroundStyle(volumeMode ? .green : .red)
            .background(Circle().fill(.black)).offset(x: WatchScale.s(5), y: WatchScale.s(4))
        }
        .frame(width: WatchScale.s(36), height: WatchScale.s(30))
        .background(RoundedRectangle(cornerRadius: 8).fill(volumeMode ? .green.opacity(0.22) : .white.opacity(0.14)))
        .contentShape(Rectangle())
    }.buttonStyle(.plain)
  }

  private var armButton: some View {
    Button {
      armed.toggle()
      disarmAt = armed ? Date().addingTimeInterval(Self.armSeconds) : nil
      if armed { volumeMode = false }
      WKInterfaceDevice.current().play(armed ? .start : .stop)
    } label: {
      TuneScaleGlyph().stroke(.white, style: StrokeStyle(lineWidth: 1.1, lineCap: .round))
        .frame(width: WatchScale.s(18), height: WatchScale.s(11))
        .overlay(alignment: .bottomTrailing) {
          Image(systemName: armed ? "checkmark.circle.fill" : "xmark.circle.fill")
            .font(.system(size: WatchScale.s(8), weight: .bold)).foregroundStyle(armed ? .green : .red)
            .background(Circle().fill(.black)).offset(x: WatchScale.s(5), y: WatchScale.s(4))
        }
        .frame(width: WatchScale.s(36), height: WatchScale.s(30))
        .background(RoundedRectangle(cornerRadius: 8).fill(armed ? .green.opacity(0.22) : .white.opacity(0.14)))
        .contentShape(Rectangle())
    }.buttonStyle(.plain)
  }

  // MARK: - Identity (the middle — the station)

  private var identity: some View {
    VStack(spacing: 3) {
      // Until the first state frame arrives (freq==0) show a connecting indicator instead of a blank.
      if st.freq == 0 {
        Text("Connecting…").font(.system(size: 12, weight: .medium)).foregroundStyle(.white.opacity(0.6))
      }
      if !st.tx.isEmpty {
        Text(st.tx).font(.system(size: 15, weight: .bold, design: .rounded)).foregroundStyle(.white)
          .lineLimit(2).multilineTextAlignment(.center).minimumScaleFactor(0.7)
      }
      HStack(spacing: 4) {
        if !st.flag.isEmpty { Text(st.flag).font(.system(size: 12)) }
        if !st.city.isEmpty {
          Text(st.city).font(.system(size: 11, weight: .medium)).foregroundStyle(.white.opacity(0.8)).lineLimit(1)
        }
        if st.dist > 0 {
          Text("\(Int(st.dist)) km").font(.system(size: 11, weight: .semibold, design: .rounded))
            .monospacedDigit().foregroundStyle(.orange)
        }
      }
      // The receiver's own name — origin of the distance above. (PTY moved down to the PI-code line.)
      if st.dist > 0 && !st.rx.isEmpty {
        Text("to \(st.rx)").font(.system(size: 10, weight: .medium)).foregroundStyle(.white.opacity(0.55))
          .lineLimit(1).truncationMode(.tail)
      }
    }.padding(.horizontal, 4)
  }

  // MARK: - Dial (drawn from the spike's learned station memory)

  private var dial: some View {
    Canvas { ctx, size in
      let midX = size.width / 2
      let span = Self.dialSpanHz
      let hzToX = { (hz: Double) in midX + (hz - st.freq) / span * size.width }

      let start = ((st.freq - span / 2) / 100_000).rounded(.down) * 100_000
      var hz = start
      while hz <= st.freq + span / 2 {
        let x = hzToX(hz)
        let isMHz = (hz / 1_000_000).truncatingRemainder(dividingBy: 1) == 0
        var p = Path()
        p.move(to: CGPoint(x: x, y: size.height - 1))
        p.addLine(to: CGPoint(x: x, y: size.height - (isMHz ? 13 : 6)))
        ctx.stroke(p, with: .color(.green.opacity(isMHz ? 0.8 : 0.4)), lineWidth: isMHz ? 1.4 : 0.9)
        if isMHz {
          let label = Text("\(Int(hz / 1_000_000))").font(.system(size: 10, weight: .semibold, design: .rounded))
            .foregroundStyle(.green.opacity(0.85))
          // ★ CLAMP INSIDE THE CANVAS. `draw(at:)` centres on x, so an edge label lost half
          //   its digits off-screen — on a 44 mm Series 6 the dial read "7 … 88 … 8" instead
          //   of "87 … 88 … 89" (Stuart, 2026-07-29). The tick still marks the true
          //   frequency; only the text is nudged in, which is far better than half a number.
          let resolved = ctx.resolve(label)
          let half = resolved.measure(in: size).width / 2 + 1
          let lx = min(size.width - half, max(half, x))
          ctx.draw(resolved, at: CGPoint(x: lx, y: size.height - 21))
        }
        hz += 100_000
      }

      let inRange = link.learnedStations.filter { abs($0.freqHz - st.freq) < span / 2 && !$0.name.isEmpty }
      for stn in inRange {
        let x = hzToX(stn.freqHz)
        var p = Path()
        p.move(to: CGPoint(x: x, y: size.height - 1))
        p.addLine(to: CGPoint(x: x, y: size.height - 17))
        ctx.stroke(p, with: .color(.green), lineWidth: 1.6)
      }

      let rowY: [CGFloat] = [2, 12]
      let minGap: CGFloat = 34
      var used: [[CGFloat]] = [[], []]
      for stn in inRange.sorted(by: { abs($0.freqHz - st.freq) < abs($1.freqHz - st.freq) }) {
        guard abs(stn.freqHz - st.freq) > span * 0.04 else { continue }
        let x = hzToX(stn.freqHz)
        guard let row = (0..<rowY.count).first(where: { r in used[r].allSatisfy { abs($0 - x) >= minGap } })
        else { continue }
        used[row].append(x)
        let t = Text(stn.name).font(.system(size: 9, weight: .semibold, design: .rounded))
          .foregroundStyle(.green.opacity(0.85))
        ctx.draw(t, at: CGPoint(x: x, y: rowY[row]), anchor: .top)
      }

      var n = Path()
      n.move(to: CGPoint(x: midX, y: 0))
      n.addLine(to: CGPoint(x: midX, y: size.height))
      ctx.stroke(n, with: .color(Color(hue: 4.0 / 360, saturation: 0.85, brightness: 1.0)), lineWidth: 1.6)
    }
    .frame(height: 50)   // full size — the PTY moved to the PI line, so the identity block has room again
    .background(.black.opacity(0.35), in: RoundedRectangle(cornerRadius: 8))
    .padding(.bottom, 4)
  }

  private static let dialSpanHz: Double = 2_000_000

  // MARK: - Readouts (bottom)

  private var readouts: some View {
    VStack(spacing: 2) {
      marquee(rdsLine)
      // ★★ TAP THE FREQUENCY TO TYPE ONE — same as Jr and the waterfall screen. FM-DX never had
      //   it, so the only way to move was the crown: fine for a nudge, hopeless for crossing the
      //   band. NOT gated on `armed` — that gate is against an ACCIDENTAL crown turn on a shared
      //   receiver, and typing a frequency is not an accident.
      Button { showNumpad = true } label: {
        Text(freqText).font(.system(size: 22, weight: .semibold, design: .rounded)).monospacedDigit()
          .foregroundStyle(.white).lineLimit(1).minimumScaleFactor(0.6)
      }.buttonStyle(.plain)
      HStack(spacing: 5) {
        if !st.pi.isEmpty {
          Text(st.pi.uppercased()).font(.system(size: 10, weight: .semibold, design: .rounded))
            .monospacedDigit().foregroundStyle(.cyan)
        }
        // PTY category, moved here next to the PI code ("C202 · Other Music") so the identity block above
        // keeps room for the receiver-location line and the dial stays full size.
        if !st.pty.isEmpty {
          Text("· \(st.pty)").font(.system(size: 10, weight: .medium))
            .foregroundStyle(.white.opacity(0.7)).lineLimit(1)
        }
        Spacer(minLength: 0)
        if st.stereo {
          Image(systemName: "dot.radiowaves.left.and.right").font(.system(size: 9, weight: .semibold))
            .foregroundStyle(.green)
        }
        Text(st.meter.isEmpty ? "—" : st.meter).font(.system(size: 10, weight: .semibold, design: .rounded))
          .monospacedDigit().foregroundStyle(.white.opacity(0.9))
      }
      signalBar
    }
    .padding(.horizontal, 7).padding(.vertical, 5)
    .background(.black.opacity(0.45), in: RoundedRectangle(cornerRadius: 11))
  }

  private var signalBar: some View {
    GeometryReader { geo in
      ZStack(alignment: .leading) {
        Capsule().fill(.white.opacity(0.18))
        Capsule().fill(LinearGradient(colors: [.red, .yellow, .green], startPoint: .leading, endPoint: .trailing))
          .frame(width: max(2, geo.size.width * min(1, max(0, st.level))))
      }
    }.frame(height: 3)
  }

  private var rdsLine: String {
    let name = st.ps.trimmingCharacters(in: .whitespaces)
    let text = st.rt.trimmingCharacters(in: .whitespaces)
    if name.isEmpty && text.isEmpty { return "No RDS" }
    if text.isEmpty { return name }
    if name.isEmpty { return text }
    return "\(name)  ·  \(text)"
  }

  private var freqText: String {
    guard st.freq > 0 else { return "—" }
    return String(format: "%.2f MHz", st.freq / 1_000_000)
  }

  private func marquee(_ s: String) -> some View {
    let charW: CGFloat = 6.2
    let width = WKInterfaceDevice.current().screenBounds.width - 30
    let textW = CGFloat(s.count) * charW
    let overflow = max(0, textW - width)
    let period = 4.0 + Double(overflow) / 18.0
    let t = Double(tick) / 20.0
    let phase = period > 0 ? (t.truncatingRemainder(dividingBy: period * 2)) / period : 0
    let eased = phase <= 1 ? phase : 2 - phase
    let offset = -overflow * min(1, max(0, eased * 1.4 - 0.2))
    return Text(s).font(.system(size: 12, weight: .semibold, design: .rounded)).foregroundStyle(.white)
      .lineLimit(1).fixedSize(horizontal: true, vertical: false)
      .offset(x: overflow > 0 ? offset : 0)
      .frame(width: width, alignment: overflow > 0 ? .leading : .center).clipped()
  }
}

/// The face of a radio: a scale of ticks with a tuning needle — the arm button's glyph.
struct TuneScaleGlyph: Shape {
  func path(in r: CGRect) -> Path {
    var p = Path()
    let baseY = r.maxY - 1
    p.move(to: CGPoint(x: r.minX, y: baseY)); p.addLine(to: CGPoint(x: r.maxX, y: baseY))
    let n = 7
    for i in 0..<n {
      let x = r.minX + r.width * CGFloat(i) / CGFloat(n - 1)
      let h: CGFloat = i.isMultiple(of: 2) ? 4 : 2.5
      p.move(to: CGPoint(x: x, y: baseY)); p.addLine(to: CGPoint(x: x, y: baseY - h))
    }
    let nx = r.minX + r.width * 0.63
    p.move(to: CGPoint(x: nx, y: r.minY)); p.addLine(to: CGPoint(x: nx, y: baseY + 1))
    return p
  }
}
