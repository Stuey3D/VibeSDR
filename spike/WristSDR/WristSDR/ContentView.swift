import SwiftUI
import WatchKit
import AVKit

/// The VibeSDR waterfall screen, recreated — but fed by a socket the WATCH owns.
///
/// Deliberately plain. This is a measurement rig, not a product: it shows the picture, it
/// lets you tune and zoom, and it puts the numbers that matter ON THE SCREEN so you can
/// see the cost while you are causing it.
struct ContentView: View {
  @StateObject private var client = UberClient()
  @StateObject private var vitals = Vitals()
  @Environment(\.scenePhase) private var scenePhase

  @State private var crown = 0.0
  @State private var lastDetent = 0
  @State private var zoomMode = false
  @State private var frame = 0
  @State private var showVolume = false
  @FocusState private var crownFocused: Bool

  /// 20fps render clock. The waterfall interpolates between the 10fps of real rows, so the
  /// scroll is smooth without the server having to send twice as much.
  private let driver = Timer.publish(every: 1.0 / 20.0, on: .main, in: .common).autoconnect()

  /// Small and wrapping — a huge detent span makes watchOS materialise a detent map that
  /// size and tick haptics across it, which hangs the main thread and gets the app killed.
  private static let detents = 1000.0

  /// 9 kHz: Radio Caroline is on 648 kHz medium wave, and MW in ITU Region 1 is a 9 kHz
  /// raster. A 1 kHz step here would just put you between channels.
  private static let step: Double = 9_000

  var body: some View {
    ZStack {
      Color.black.ignoresSafeArea()

      // The waterfall itself — the same buffer the shipped watch app draws with, so the
      // picture is a fair comparison and not a different renderer flattering itself.
      //
      // The trace advances on the RENDER clock (20fps), not on the data clock (10fps):
      // rows are interpolated between real samples, which is what makes 10fps of honest
      // data look like a smooth scroll. Take that away and JR would need twice the frames
      // for the same feel — and twice the CPU we are here to measure.
      Canvas { ctx, size in
        _ = frame                                   // read it, so SwiftUI must redraw
        client.waterfall.tick(at: ProcessInfo.processInfo.systemUptime)
        if let img = client.waterfall.makeImage() {
          ctx.draw(Image(decorative: img, scale: 1), in: CGRect(origin: .zero, size: size))
        }
      }
      .ignoresSafeArea()

      // The VFO — fixed at centre, the band slides under it. Same idea as the phone.
      Rectangle()
        .fill(.red.opacity(0.9))
        .frame(width: 1.5)
        .ignoresSafeArea()

      VStack(spacing: 0) {
        Spacer()

        // THE NUMBERS. On screen, while you cause them.
        VStack(spacing: 1) {
          Text(freqText)
            .font(.system(size: 20, weight: .semibold, design: .rounded))
            .monospacedDigit()
            .foregroundStyle(.white)

          HStack(spacing: 6) {
            Text(String(format: "CPU %.0f%%", vitals.cpu))
              .foregroundStyle(vitals.cpu > 80 ? .red : .green)
            Text(String(format: "%.0ffps", client.framesPerSec))
              .foregroundStyle(.white.opacity(0.75))
            Text(client.audioLive ? String(format: "AUD %.0f/s", client.audioPerSec) : "NO AUDIO")
              .foregroundStyle(client.audioLive ? .white.opacity(0.75) : .red)
            Text(String(format: "%.0f%%", vitals.battery * 100))
              .foregroundStyle(.white.opacity(0.55))
          }
          .font(.system(size: 9, weight: .medium, design: .rounded))
          .monospacedDigit()

          HStack(spacing: 5) {
            Text(zoomMode ? "CROWN: ZOOM" : "CROWN: TUNE")
              .foregroundStyle(zoomMode ? .yellow : .cyan)
            Text(client.rateDivisor == 1 ? "10fps" : "5fps")
              .foregroundStyle(client.rateDivisor == 1 ? .white.opacity(0.6) : .orange)
          }
          .font(.system(size: 8, weight: .bold, design: .rounded))

          // SAY WHAT IS WRONG. This used to show the audio route once status hit "live",
          // which meant a spectrum socket that was open and silent was completely invisible
          // — the screen looked healthy and the waterfall was empty. The thing that is
          // BROKEN always wins the line.
          Text(socketLine)
            .font(.system(size: 8))
            .foregroundStyle(.white.opacity(0.7))
            .lineLimit(4)
            .multilineTextAlignment(.center)
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 3)
        .background(.black.opacity(0.72), in: RoundedRectangle(cornerRadius: 8))
        .padding(.bottom, 4)
      }

      // The way IN to volume + output. Top-left, out of the clock's way.
      VStack {
        HStack {
          Button { showVolume = true } label: {
            Image(systemName: client.audioLive ? "speaker.wave.2.fill" : "speaker.slash.fill")
              .font(.system(size: 12, weight: .semibold))
              .foregroundStyle(client.audioLive ? .white : .red)
              .frame(width: 34, height: 28)
              .background(.black.opacity(0.6), in: Capsule())
          }
          .buttonStyle(.plain)
          Spacer()
        }
        .padding(.leading, 6)
        .padding(.top, 8)
        Spacer()
      }
    }
    // WHERE THE AUDIO WENT, and how loud.
    //
    // watchOS has NO SwiftUI volume control. `VolumeView` does not exist here (that is
    // iOS/tvOS); the watch has only `AVRoutePickerView` and the storyboard-era
    // `WKInterfaceVolumeControl`, neither of which drops into a SwiftUI view.
    //
    // For JR that is a real design constraint, not an oversight to fix later: the Digital
    // Crown is the volume control on a watch, and it is only that inside a media context.
    // JR will have to CHOOSE — crown tunes, or crown sets volume — and say which, exactly
    // as the companion app does with its crown modes. Worth knowing now.
    .sheet(isPresented: $showVolume) {
      VStack(spacing: 8) {
        Text("AUDIO")
          .font(.system(size: 11, weight: .bold, design: .rounded))
          .foregroundStyle(.secondary)
        Text(client.audioRoute)
          .font(.system(size: 11, design: .rounded))
          .foregroundStyle(.white)
          .multilineTextAlignment(.center)
        Text("Volume: side-swipe → Control Centre.\nwatchOS has no in-app volume slider for SwiftUI.")
          .font(.system(size: 9))
          .foregroundStyle(.secondary)
          .multilineTextAlignment(.center)
      }
      .padding(.horizontal, 8)
    }
    // Tap anywhere to swap the crown between tuning and zooming. One control, two jobs,
    // and the label above says which — no menu to build.
    .onTapGesture {
      zoomMode.toggle()
      WKInterfaceDevice.current().play(.click)
    }
    // LONG-PRESS: 10fps ↔ 5fps, live. The whole question this spike asks is "what does it
    // cost", and the cheapest lever on that cost is the frame rate — so being able to halve
    // it WHILE WATCHING THE CPU NUMBER, on the wrist, without a rebuild, is worth more than
    // any amount of reasoning about it.
    .onLongPressGesture(minimumDuration: 0.45) {
      client.rateDivisor = client.rateDivisor == 1 ? 2 : 1
      WKInterfaceDevice.current().play(.start)
    }
    .focusable(true)
    .focused($crownFocused)
    .digitalCrownRotation($crown, from: 0, through: Self.detents, by: 1,
                          sensitivity: .medium, isContinuous: true,
                          isHapticFeedbackEnabled: true)
    .onChange(of: crown) { _, new in
      let detent = Int(new.rounded())
      guard detent != lastDetent else { return }
      // Unwrap: the crown is continuous, so 999 → 0 is one step, not a leap of 999.
      var delta = detent - lastDetent
      let range = Int(Self.detents)
      if delta >  range / 2 { delta -= range }
      if delta < -range / 2 { delta += range }
      lastDetent = detent

      if zoomMode { client.zoom(delta: delta) }
      else        { client.tune(delta: delta, step: Self.step) }
    }
    .onReceive(driver) { _ in frame &+= 1 }
    // WRIST UP → check the sockets are still alive. watchOS suspended us while the screen
    // was off and the WebSockets died with it; without this the waterfall never comes back.
    .onChange(of: scenePhase) { _, phase in
      switch phase {
      case .active:
        Vitals.crumb("SCENE → active")
        client.resumeSpectrum()
        client.reconnectIfNeeded()
      // Wrist down. Drop the WATERFALL only — the audio keeps playing, which is the entire
      // reason this app declares WKBackgroundModes=[audio] and a .longFormAudio session.
      case .background:
        Vitals.crumb("SCENE → background (wrist down)")
        client.suspend()
      default: break
      }
    }
    .onAppear {
      crownFocused = true
      vitals.framesPerSec = { client.framesPerSec }
      vitals.audioPerSec  = { client.audioPerSec }
      vitals.audioLive    = { client.audioLive }
      vitals.start()
      client.start()
    }
  }

  /// Whatever is most WRONG, in priority order. A screen that reports the healthy half
  /// while the broken half is silent is worse than a screen that reports nothing.
  /// BOTH sockets, side by side, always. Every round of this bug has been lost to a screen
  /// that showed one socket's story and hid the other's.
  private var socketLine: String {
    "S: \(client.specWsState.isEmpty ? "—" : client.specWsState)\nA: \(client.audioWsState.isEmpty ? "—" : client.audioWsState)"
  }

  private var diagLine: String {
    if client.unknownFlags != 0 {
      return String(format: "spec: unknown frame flags 0x%02X", client.unknownFlags)
    }
    if client.framesPerSec == 0 {
      if !client.wsDiag.isEmpty { return client.wsDiag }
      if client.status != "live" { return client.status }
      return "spec: connected, 0 frames"
    }
    if client.rowsPushed == 0 {
      // Frames arriving, no rows drawn — the data is being thrown away somewhere between
      // the socket and the screen, which is a different bug entirely.
      return "spec: \(Int(client.framesPerSec))fps but 0 rows drawn · bins=\(client.binCount)"
    }
    return client.audioRoute
  }

  private var freqText: String {
    let f = client.frequency
    if f >= 1_000_000 {
      return String(format: "%.3f MHz", f / 1_000_000)
    }
    return String(format: "%.0f kHz", f / 1_000)
  }
}
