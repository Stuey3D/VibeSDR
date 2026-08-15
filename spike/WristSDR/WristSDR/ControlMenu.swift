import SwiftUI
import WatchKit

/// What the Digital Crown does right now.
///
/// The mode is EXPLICIT and PERSISTENT — not a HUD that times out. On a wrist you
/// must never be unsure what the crown is about to do: an accidental turn should
/// be recoverable by knowing, not by guessing.
/// FIRST-RUN COACH. Shown ONCE per screen, then never again.
///
/// Everything this app does on a wrist is a GESTURE, and gestures are invisible. The
/// crown tunes, a tap on the frequency opens the numpad, a long-press opens the control
/// grid — none of which announce themselves, and a user who doesn't find them has an app
/// that appears to do nothing but display. One quiet screen, once, fixes that; a coach
/// that reappears is worse than none, which is why it is gated on a stored flag rather
/// than on a session.
///
/// DELIBERATELY STATIC. No animation, nothing to wait for, nothing to dismiss by accident:
/// you read three lines and tap Got it. On a wrist, an interactive tutorial is a punishment.
struct CoachOverlay: View {
  struct Item: Identifiable {
    let id = UUID()
    let glyph: String
    let text: String
  }

  let title: String
  let items: [Item]
  /// A single line of warning, if this screen has a way to bite you. FM-DX does.
  var caution: String? = nil
  let onDismiss: () -> Void

  var body: some View {
    ZStack {
      // Opaque, not a scrim: it must be READ, not glanced past, and a waterfall scrolling
      // underneath is exactly the sort of thing that makes text unreadable on a wrist.
      Color.black.opacity(0.94).ignoresSafeArea()

      ScrollView {
        VStack(spacing: 10) {
          Text(title)
            .font(.system(size: 15, weight: .bold, design: .rounded))
            .foregroundStyle(.white)
            .padding(.top, 26)          // clear of the clock

          VStack(alignment: .leading, spacing: 9) {
            ForEach(items) { it in
              HStack(alignment: .center, spacing: 9) {
                Image(systemName: it.glyph)
                  .font(.system(size: 15, weight: .semibold))
                  .foregroundStyle(.green)
                  .frame(width: 22)     // a column, so the text edges line up
                Text(it.text)
                  .font(.system(size: 12, weight: .medium, design: .rounded))
                  .foregroundStyle(.white.opacity(0.92))
                  .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 0)
              }
            }
          }

          if let caution {
            HStack(alignment: .top, spacing: 7) {
              Image(systemName: "exclamationmark.triangle.fill")
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(.orange)
              Text(caution)
                .font(.system(size: 11, weight: .medium, design: .rounded))
                .foregroundStyle(.orange)
                .fixedSize(horizontal: false, vertical: true)
            }
            .padding(.top, 2)
          }

          Button(action: onDismiss) {
            Text("Got it")
              .font(.system(size: 13, weight: .semibold, design: .rounded))
              .frame(maxWidth: .infinity)
              .padding(.vertical, 7)
              .background(.green.opacity(0.25), in: Capsule())
              .foregroundStyle(.white)
          }
          .buttonStyle(.plain)
          .padding(.top, 4)
          .padding(.bottom, 12)
        }
        .padding(.horizontal, 12)
      }
    }
  }
}

/// THE WATCH'S OWN BATTERY, next to the clock — where a watch user already looks for it.
///
/// A live waterfall costs ~34% of a core (measured on-device), and this is an app you
/// might genuinely leave running on a hilltop with no charger. The system reading is two
/// swipes away; the thing you're watching it FOR is on this screen.
///
/// The number goes INSIDE the icon, like the iPhone's. On a wrist that is not a
/// stylistic choice — a separate "82%" label would cost width the clock's band does not
/// have, and an icon with no number only tells you what you could already guess from a
/// glance at the fill.
struct BatteryPill: View {
  /// 0…1, or negative when watchOS can't tell us (simulator, monitoring off).
  let level: Double
  /// Carry its OWN dark capsule. TRUE when it floats on the raw view; FALSE when it sits
  /// inside something that is already darkening the background for it.
  ///
  /// Both at once is visibly wrong: the capsule pokes out of the bottom of the strip's
  /// gradient and grows a little lump of black off it. Two scrims stacked is not twice as
  /// legible, it is one scrim and one blemish.
  var scrim = true

  /// Red at 20% — a wrist has no charger in reach and no time to negotiate.
  private var tint: Color { level <= 0.20 ? .red : .white.opacity(0.85) }

  var body: some View {
    if level < 0 {
      EmptyView()
    } else {
      let pct = Int((level * 100).rounded())
      HStack(spacing: 1) {
        ZStack {
          RoundedRectangle(cornerRadius: 2.5)
            .stroke(tint, lineWidth: 1)
          // Fill from the left, like every battery glyph ever drawn.
          GeometryReader { g in
            RoundedRectangle(cornerRadius: 1.5)
              .fill(tint.opacity(0.32))
              .frame(width: max(0, (g.size.width - 2) * level))
              .padding(1)
          }
          Text("\(pct)")
            .font(.system(size: 9, weight: .bold, design: .rounded))
            .monospacedDigit()
            .foregroundStyle(tint)
            .minimumScaleFactor(0.7)
            .lineLimit(1)
        }
        // Sized to sit level with the band-label pill on the same row — the font is clear, so
        // even three digits stay legible. Not huge; just matched.
        .frame(width: 30, height: 15)
        // The nub. Without it a rounded rectangle with a number in it is just a badge.
        RoundedRectangle(cornerRadius: 0.5)
          .fill(tint)
          .frame(width: 1.5, height: 4)
      }
      // A SCRIM, because this can float over the WATERFALL. White strokes and white digits
      // over a bright amber-and-red spectrum are simply not there. Legibility on this app
      // comes from darkening, never from frosting — frosting blurs but does not darken,
      // so the glyph would still be yellow-on-yellow. (Same rule as every other piece of
      // chrome on both watch screens.) Suppressed when a strip is already darkening for us.
      .padding(.horizontal, scrim ? 4 : 0)
      .padding(.vertical, scrim ? 2 : 0)
      .background(scrim ? AnyShapeStyle(Color.black.opacity(0.55))
                        : AnyShapeStyle(Color.clear), in: Capsule())
      .accessibilityElement(children: .ignore)
      .accessibilityLabel("Watch battery \(pct) percent")
    }
  }
}

/// The battery, drawn VERTICALLY, for the bottom-left corner of the spike screen.
///
/// The horizontal `BatteryPill` lived beside the clock; that spot fouls the watchOS system
/// glyphs (driving car, location arrow, recording dot) which have no detect-and-dodge API. The
/// ticker moving up to the axis strip freed the bottom-left corner, so the battery drops there —
/// upright, on its OWN dark scrim, because down here it floats over the raw waterfall and white
/// strokes/digits over a bright spectrum are simply not there without darkening behind them.
struct BatteryPillV: View {
  let level: Double
  private var tint: Color { level <= 0.20 ? .red : .white.opacity(0.85) }

  var body: some View {
    if level < 0 {
      EmptyView()
    } else {
      let pct = Int((level * 100).rounded())
      VStack(spacing: 1.5) {
        // The nub, on TOP now that the cell stands upright.
        RoundedRectangle(cornerRadius: 0.5).fill(tint).frame(width: 4, height: 1.5)
        ZStack {
          RoundedRectangle(cornerRadius: 2.5).stroke(tint, lineWidth: 1)
          // Fill from the BOTTOM, the way an upright cell reads.
          GeometryReader { g in
            RoundedRectangle(cornerRadius: 1.5)
              .fill(tint.opacity(0.32))
              .frame(height: max(0, (g.size.height - 2) * level))
              .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
              .padding(1)
          }
          // Percentage INSIDE the cell — below it, the digits ran into the watch's rounded
          // corner and clipped. The cell is widened to 17pt so "100" fits upright.
          Text("\(pct)")
            .font(.system(size: 8, weight: .bold, design: .rounded))
            .monospacedDigit()
            .foregroundStyle(tint)
            .minimumScaleFactor(0.6)
            .lineLimit(1)
        }
        .frame(width: 17, height: 26)
      }
      // The scrim — darkening, never frosting, same rule as every other piece of chrome.
      .padding(.horizontal, 4)
      .padding(.vertical, 3)
      .background(Color.black.opacity(0.55), in: Capsule())
      .accessibilityElement(children: .ignore)
      .accessibilityLabel("Watch battery \(pct) percent")
    }
  }
}

enum CrownMode: Equatable {
  case tune, zoom, brightness, contrast, autoContrast, volume, wfFloor, wfCeil

  var glyph: String {
    switch self {
    case .tune:         return "dial.medium"
    case .zoom:         return "magnifyingglass"
    case .brightness:   return "sun.max.fill"
    case .contrast:     return "circle.lefthalf.filled"
    case .autoContrast: return "wand.and.stars"
    case .wfFloor: return "arrow.down.to.line"
    case .wfCeil:  return "arrow.up.to.line"
    case .volume:       return "speaker.wave.2.fill"
    }
  }
  var label: String {
    switch self {
    case .tune: return "Tune"; case .zoom: return "Zoom"; case .volume: return "Volume"
    case .brightness: return "Bright"; case .contrast: return "Contrast"
    case .autoContrast: return "Auto"
    case .wfFloor: return "Floor"
    case .wfCeil:  return "Ceiling"
    }
  }
  /// Double-Tap cycles just the three primary crown modes.
  var nextPrimary: CrownMode {
    switch self { case .tune: return .zoom; case .zoom: return .volume; default: return .tune }
  }
}

/// Crown sensitivity — watchOS's own, exposed as three named levels.
///
/// This maps straight onto SwiftUI's `sensitivity:`, which sets how many detents a
/// rotation produces. Fine is the point: at a 9kHz step, High throws you across half
/// a band on one flick. Because it's the SYSTEM setting, the haptic clicks stay in
/// step with the tuning — one click, one step, whichever level you pick.
enum CrownSens: String, CaseIterable {
  case high, medium, low

  var sensitivity: DigitalCrownRotationalSensitivity {
    switch self {
    case .high:   return .high      // most detents per turn — the twitchiest
    case .medium: return .medium    // the original behaviour
    case .low:    return .low       // turn furthest per step — finest control
    }
  }

  /// Named for what the USER gets, which is the inverse of the detent count: `.low`
  /// sensitivity is the FINEST tuning. Calling it "Low" and leaving it there would
  /// read as "worse".
  var label: String {
    switch self {
    case .high:   return "Coarse"
    case .medium: return "Normal"
    case .low:    return "Fine"
    }
  }

  var detail: String {
    switch self {
    case .high:   return "Fastest — a flick crosses a band"
    case .medium: return "Default"
    case .low:    return "Turn further per step"
    }
  }
}

/// Long-press menu: four large buttons, Control-Centre style.
///
/// Step and Demod open a SCROLLABLE LIST rather than cycling on tap. That matters:
/// tap-to-cycle means walking THROUGH modes you didn't ask for — and landing on
/// wideband FM on the way past is a faceful of static. A picker never makes you
/// pass through anything.
///
/// ── NO VOLUME TILE. Don't add one back. ──────────────────────────────────────
/// There is NO supported way for an app to move the iOS system volume, and we
/// checked every door: AVAudioSession.outputVolume is read-only; MPRemoteCommandCenter
/// has no volume command; AVRCP absolute volume is classic Bluetooth (iOS exposes
/// BLE only, and we'd be trying to control the very phone we run on); AirPlay volume
/// is the receiver's, and an iPhone can't be an AirPlay sink. Every route ends at
/// MPVolumeView's private slider, which is a rejection risk.
///
/// The one thing that DOES work is Apple's own: because we publish Now Playing info,
/// the watch's built-in Now Playing app already drives the phone's volume over its
/// full range, one swipe away. A tile of ours could only ever be the weaker twin of
/// that — an app-local gain, mistakable for the real volume — and on a 40mm screen
/// it isn't worth a third of the menu.
struct ControlMenu: View {
  @EnvironmentObject var link: SpikeLink
  @EnvironmentObject var bookmarks: BookmarkStore
  @Environment(\.dismiss) private var dismiss

  /// Set by the caller when Volume or Zoom is chosen: the menu closes and the
  /// waterfall returns with the crown in that mode.
  let onPickCrown: (CrownMode) -> Void

  /// ★★ Open straight into the Display sheet. Manual range is inherently a PAIR — set the floor,
  /// look, set the ceiling, look — and every adjustment dismisses to the waterfall, so without
  /// this each tweak costs a full menu → Display → row walk, on a watch, outdoors
  /// (BRIEF-jr-display-manual-range.md §4). The caller decides when this applies and expires it.
  var openDisplay = false

  @State private var showModes = false
  @State private var showSteps = false
  @State private var showCrown = false
  @State private var showProfiles = false
  private var activeProfileName: String { link.profiles.first(where: { $0.active })?.name ?? "—" }
  @State private var showWrist = false
  @State private var showBw = false
  @State private var showSquelch = false
  @State private var showBookmarks = false
  @State private var showDisplay = false
  @AppStorage("crownSens") private var crownSens = CrownSens.medium.rawValue
  /// Wrist-down spectrum timeout (seconds; 0 = never drop, keep it running at the cost of
  /// battery). ContentView reads the SAME key to time its suspend. See wristOptions.
  @AppStorage("jrWristTimeout") private var wristTimeout = 30.0

  private let cols = Array(repeating: GridItem(.flexible(), spacing: 5), count: 2)

  /// Sits in the clock's band, so the X costs no height.
  private let closeH: CGFloat = 32

  var body: some View {
    Group {
      // The menu SCROLLS — watchOS's own Control Centre does, so it's the native
      // idiom and users already expect it. That means tiles no longer have to fight
      // each other for a fixed screen's worth of height: they can be a comfortable
      // size and the list can simply grow.
      //
      // Brightness and contrast are CROWN MODES, not sliders — same language as Zoom,
      // and you adjust them while looking at the very waterfall you're adjusting,
      // which a settings screen can't do.
      let h: CGFloat = 66

      VStack(spacing: 5) {
        // A visible way OUT. Hiding the nav bar reclaimed the space the pad needed,
        // but it also removed the back chevron — leaving swipe-back as the only
        // exit, and a hidden gesture is not an affordance. This lives in the clock's
        // band, which watchOS reserves whether we use it or not, so it costs no
        // height at all.
        HStack(spacing: 0) {
          Button { dismiss() } label: {
            Image(systemName: "xmark")
              .font(.system(size: 15, weight: .semibold))
              .foregroundStyle(.secondary)
              .frame(width: 36, height: closeH)
              .contentShape(Rectangle())
          }
          .buttonStyle(.plain)
          Spacer()
          // The clock's territory — we can't move it, so we don't go there.
          Color.clear.frame(width: 70, height: 1)
        }
        .frame(height: closeH)
        .padding(.leading, 8)

        ScrollView {
          // PROFILES — top of the menu, full width (OWRX only). Shows the active profile + listener
          // count; opens the grouped picker. Switching is EXPLICIT (etiquette) — never automatic.
          if !link.profiles.isEmpty {
            Button { showProfiles = true } label: {
              HStack(spacing: 8) {
                Image(systemName: "dial.medium.fill").font(.system(size: 18)).foregroundColor(.orange)
                VStack(alignment: .leading, spacing: 1) {
                  Text("PROFILES").font(.system(size: 11, weight: .bold)).foregroundColor(.orange)
                  Text("\(activeProfileName) · \(link.clients) listening").font(.system(size: 11)).foregroundColor(.white.opacity(0.7)).lineLimit(1)
                }
                Spacer(); Image(systemName: "chevron.right").foregroundColor(.white.opacity(0.4))
              }
              .padding(.horizontal, 10).padding(.vertical, 8)
              .frame(maxWidth: .infinity)
              .background(.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 10))
            }
            .buttonStyle(.plain).padding(.bottom, 5)
          }
          // BOOKMARKS — full-width, above the tile grid. Save the current spot and recall saved ones.
          // Standalone Jr needs its own dial memory (Buddy just reaches for the phone). Local to the watch.
          Button { showBookmarks = true } label: {
            HStack(spacing: 8) {
              Image(systemName: "bookmark.fill").font(.system(size: 17)).foregroundColor(.orange)
              VStack(alignment: .leading, spacing: 1) {
                Text("BOOKMARKS").font(.system(size: 11, weight: .bold)).foregroundColor(.orange)
                Text(bookmarks.bookmarks.isEmpty ? "Save this frequency"
                     : "\(bookmarks.bookmarks.count) saved").font(.system(size: 11)).foregroundColor(.white.opacity(0.7)).lineLimit(1)
              }
              Spacer(); Image(systemName: "chevron.right").foregroundColor(.white.opacity(0.4))
            }
            .padding(.horizontal, 10).padding(.vertical, 8)
            .frame(maxWidth: .infinity)
            .background(.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 10))
          }
          .buttonStyle(.plain).padding(.bottom, 5)
          LazyVGrid(columns: cols, spacing: 5) {
          // ORDER (Stuart): Demod, Step first; then Zoom, Volume, Mute, Bandwidth, Display, Wrist down,
          // Servers, Stop. DAB (conditional) sits with Bandwidth; CROWN (sensitivity) just before Display.
          //
          // NAME the control, then show its VALUE — a tile reading just "USB"/"9k" leaves you guessing
          // what it's the setting FOR. The name makes it a control; the value makes the menu a readout.
          tile(name: "DEMOD", value: link.mode.uppercased(), h: h) { showModes = true }
          tile(name: "STEP",  value: stepLabel(link.step), h: h) { showSteps = true }
          tile(icon: "magnifyingglass", label: "Zoom", h: h) {
            dismiss(); onPickCrown(.zoom)
          }
          // The iPhone's SYSTEM volume, not an app gain. The wrist shows the phone's real level —
          // including changes made ON the phone — so the two can never disagree.
          tile(icon: "speaker.wave.2.fill", label: "Volume", h: h) {
            dismiss(); onPickCrown(.volume)
          }
          // Mute is NOT volume-to-zero: that would lose the level you were listening at, so unmuting
          // could not put it back. This gates playback and leaves the volume where it is.
          tile(icon: link.muted ? "speaker.slash.fill" : "speaker.fill",
               label: link.muted ? "Unmute" : "Mute", h: h) {
            dismiss(); link.setMuted(!link.muted)
          }
          // Passband: tap → LSB/USB crown editor. Value = total width in kHz.
          tile(name: "BW", value: bwLabel, h: h) { showBw = true }
          // Squelch: SNR audio gate. Value = threshold in dB (Off = open).
          tile(name: "SQL", value: link.sql < 0 ? "Off" : "On", h: h) { showSquelch = true }
          // (DAB tile removed — the programme picker + speed fix live on the main DAB screen now.)
          tile(name: "CROWN", value: crownLabel, h: h) { showCrown = true }
          // DISPLAY — auto contrast + brightness + contrast (crown tweaks), palette + VFO colour
          // (crown-preview pickers), the peak-hold toggle and a reset. Watch-local look.
          tile(icon: "display", label: "Display", h: h) { showDisplay = true }
          // Wrist-down spectrum timeout — battery vs "always live". Off keeps the waterfall running
          // with the wrist down (costs power); the timed options drop it after N and reconnect on return.
          tile(name: "WRIST DOWN", value: wristLabel, h: h) { showWrist = true }

          // (RTL-SDR hardware controls live on their OWN button top-left of the waterfall screen — see
          // ContentView — so this grid stays uncluttered for the remote backends that have no dongle.)

          // SERVERS — back to the instance picker to switch server (or manage favourites).
          tile(icon: "antenna.radiowaves.left.and.right", label: "Servers", h: h) {
            dismiss(); link.backToPicker()
          }

          // STOP — the ONLY in-app way to actually stop the audio. Background-audio mode keeps the app
          // playing through a wrist-flick / crown press, so without this the only ways to silence it are
          // force-quit or the Now Playing screen. Drops audio + sockets and lands back on the picker.
          tile(icon: "stop.circle.fill", label: "Stop", h: h) {
            dismiss(); link.backToPicker()
          }
          // (Reset view moved INTO the Display menu — it resets all the display settings now, not
          // just brightness/contrast, so it belongs beside them.)
        }

        // ★★ NO LINK MANAGEMENT ON THE WRIST. Jr is pinned to LOW DATA and offers no
        //    choice. Measured on a Series 6, 2026-07-29: under Full or Auto the spectrum
        //    repeatedly stuck on "reconnecting", while pinned Low Data ran for minutes on
        //    end and the row interpolation covered the 5 fps convincingly. Stuart: "low
        //    data is good enough for the wrist… on the bigger screen its needed but the
        //    watch not so much." The ladder still runs on the PHONE, where a bad link is
        //    worth hunting; on a watch the hunting IS the fault. See LinkManager.mode.

        // Room to scroll the LAST row clear of the rounded corner — as content,
        // not as a bar. Control Centre lets its tiles run off the bottom edge and
        // simply keeps scrolling; a fixed bottom padding on the outer stack instead
        // drew a hard black band across the screen, which reads as a broken layout
        // rather than as "there is more below".
        .padding(.bottom, 18)

        // ★★ LAST BREATH — the tail of the run that ended, on the wrist, because this watch
        //    produces no crash report and Xcode cannot see it. Newest lines FIRST: the answer is
        //    always the last thing that happened, and nobody scrolls to the bottom of a log on a
        //    38mm screen. Gated with the CPU meter — both go before any public build.
        if CpuMeter.enabled, !Vitals.lastBreath.isEmpty {
          VStack(spacing: 3) {
            Text("LAST RUN (NEWEST FIRST)")
              .font(.system(size: 9, weight: .bold)).foregroundColor(.orange.opacity(0.7))
            Text(Vitals.lastBreath
                  .split(separator: "\n").reversed().prefix(24)
                  .joined(separator: "\n"))
              .font(.system(size: 8, design: .monospaced))
              .foregroundColor(.white.opacity(0.65))
              .frame(maxWidth: .infinity, alignment: .leading)
          }.padding(.top, 3).padding(.bottom, 18)
        }

        }
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }
    .padding(.horizontal, 6)
    // BOTTOM ONLY.
    //
    // It used to ignore the TOP safe area too, to buy the X a free row in the clock's
    // band. But that band is also where watchOS runs the back-swipe gesture on a pushed
    // view, and it SWALLOWED THE TAPS: the X did nothing at all, and the only way out of
    // this menu was to pick a crown mode you didn't want and then cancel that. A control
    // that cannot be pressed is not worth the height it saves.
    .ignoresSafeArea(edges: .bottom)
    .toolbar(.hidden, for: .navigationBar)
    .sheet(isPresented: $showCrown) {
      CrownPicker(current: $crownSens) { showCrown = false; dismiss() }
    }
    .sheet(isPresented: $showProfiles) {
      ProfileSheet { id in link.selectProfile(id); showProfiles = false; dismiss() }
        .environmentObject(link)
    }
    .sheet(isPresented: $showModes) {
      PickerList(title: "Demod", items: Self.modes, current: link.mode) { m in
        link.setMode(m); showModes = false; dismiss()
      }
    }
    .sheet(isPresented: $showSteps) {
      PickerList(title: "Step",
                 items: Self.steps.map(stepLabel),
                 current: stepLabel(link.step)) { label in
        if let hz = Self.steps.first(where: { stepLabel($0) == label }) {
          link.setStep(hz)
        }
        showSteps = false; dismiss()
      }
    }
    .sheet(isPresented: $showWrist) {
      PickerList(title: "Wrist down",
                 items: Self.wristOptions.map(\.label),
                 current: wristLabel) { label in
        if let secs = Self.wristOptions.first(where: { $0.label == label })?.secs {
          wristTimeout = secs
        }
        showWrist = false; dismiss()
      }
    }
    .sheet(isPresented: $showBw) {
      BandwidthView().environmentObject(link)
    }
    .sheet(isPresented: $showSquelch) {
      SquelchView().environmentObject(link)
    }
    .sheet(isPresented: $showBookmarks) {
      BookmarksView { dismiss() }.environmentObject(link).environmentObject(bookmarks)
    }
    .sheet(isPresented: $showDisplay) {
      DisplaySheet(
        onPickCrown: { m in showDisplay = false; dismiss(); onPickCrown(m) },
        closeAll:    { showDisplay = false; dismiss() }
      ).environmentObject(link)
    }
    // ★ Straight into Display when the caller says the user is mid-adjustment.
    .onAppear { if openDisplay { showDisplay = true } }
  }

  /// Total passband width in kHz, for the BW tile readout.
  private var bwLabel: String {
    let k = (link.filtHi - link.filtLo) / 1000
    return k >= 10 ? String(format: "%.0fk", k) : String(format: "%.1fk", k)
  }

  /// Off (never drop) + the timed steps. Kept here so ContentView and the picker agree.
  static let wristOptions: [(label: String, secs: Double)] = [
    ("Off", 0), ("30s", 30), ("60s", 60), ("90s", 90), ("3m", 180), ("5m", 300),
  ]
  private var wristLabel: String {
    ControlMenu.wristOptions.first(where: { $0.secs == wristTimeout })?.label
      ?? "\(Int(wristTimeout))s"
  }

  /// A named setting: the control's name small on top, its current value big below.
  private func tile(name: String, value: String, h: CGFloat,
                    action: @escaping () -> Void) -> some View {
    Button(action: action) {
      VStack(spacing: 1) {
        Text(name)
          .font(.system(size: max(9, h * 0.13), weight: .semibold, design: .rounded))
          .foregroundStyle(.secondary)
          .lineLimit(1)
        Text(value)
          .font(.system(size: h * 0.24, weight: .semibold, design: .rounded))
          .lineLimit(1)
          .minimumScaleFactor(0.6)
      }
      .foregroundStyle(.white)
      .frame(maxWidth: .infinity)
      .frame(height: h)
      .background(RoundedRectangle(cornerRadius: h * 0.30).fill(.white.opacity(0.16)))
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }

  /// An ACTION tile (it arms the crown, it isn't a setting) — icon over label.
  private func tile(icon: String?, label: String, h: CGFloat,
                    action: @escaping () -> Void) -> some View {
    Button(action: action) {
      VStack(spacing: 2) {
        if let icon {
          Image(systemName: icon).font(.system(size: h * 0.30, weight: .semibold))
        }
        Text(label)
          .font(.system(size: icon == nil ? h * 0.26 : h * 0.16,
                        weight: .semibold, design: .rounded))
          .lineLimit(1)
          .minimumScaleFactor(0.6)
      }
      .foregroundStyle(.white)
      .frame(maxWidth: .infinity)
      .frame(height: h)
      .background(RoundedRectangle(cornerRadius: h * 0.30).fill(.white.opacity(0.16)))
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }

  // Mirrors sdrTypes.ts. Kept in the order the phone lists them.
  // FM (narrow) + WFM (wide), matching the phone. `nfm` was a second NARROW-FM entry (redundant
  // with `fm`) and there was no wide FM at all — so broadcast FM couldn't be selected on the watch.
  static let modes = ["usb", "lsb", "am", "sam", "fm", "wfm", "cwu", "cwl", "dab", "adsb"]
  static let steps: [Double] = [10, 100, 500, 1_000, 9_000, 10_000, 12_500, 25_000, 100_000]

  private var crownLabel: String {
    CrownSens(rawValue: crownSens)?.label ?? "Normal"
  }

  private func stepLabel(_ hz: Double) -> String {
    if hz <= 0 { return "—" }
    if hz >= 1_000 {
      let k = hz / 1_000
      return k == k.rounded() ? String(format: "%.0fk", k) : String(format: "%.1fk", k)
    }
    return String(format: "%.0fHz", hz)
  }
}

/// VibeServer HARDWARE controls — the client drives the physical RTL-SDR over the WS. Observes the
/// UberClient directly (the values live there, not mirrored through SpikeLink). Gain steps + offered
/// sample rates come from the server's `hwinfo`; the sample-rate picker hides when the host pinned the rate.
struct HardwareSheet: View {
  @ObservedObject var radio: UberClient
  @State private var adminPass = ""
  @State private var gainArmed = false
  @State private var attArmed = false
  @State private var gainCrown = 0.0
  @State private var lastGainDetent = 0
  @State private var showSpan = false
  @FocusState private var crownFocused: Bool
  // (Link Management is NOT here — it lives at the bottom of the main menu, because this sheet is
  // VibeServer-only and the ladder is for the remote backends that have no dongle.)

  /// A grid cell: small title, big value, glass when off / lit (green on-state, cyan armed) when active.
  private func cell(title: String, value: String, lit: Bool, litColor: Color = .green, dim: Bool = false) -> some View {
    VStack(spacing: 2) {
      Text(title).font(.system(size: 9, weight: .bold)).foregroundColor(lit ? .black.opacity(0.65) : .white.opacity(0.55))
      Text(value).font(.system(size: 15, weight: .semibold, design: .rounded)).monospacedDigit().lineLimit(1).minimumScaleFactor(0.7)
    }
    .frame(maxWidth: .infinity).frame(height: cellH)
    .foregroundColor(lit ? .black : (dim ? .white.opacity(0.3) : .white))
    .background(RoundedRectangle(cornerRadius: 10).fill(lit ? AnyShapeStyle(litColor) : AnyShapeStyle(.white.opacity(0.14))))
    .overlay(RoundedRectangle(cornerRadius: 10).stroke(lit && litColor == .cyan ? Color.cyan : .clear, lineWidth: 1.5))
  }

  /// PPM correction — inline − / value / + (a tuner-clock trim, not a toggle).
  private var ppmCell: some View {
    VStack(spacing: 2) {
      Text("PPM").font(.system(size: 9, weight: .bold)).foregroundColor(.white.opacity(0.55))
      HStack(spacing: 12) {
        Button { radio.setPpm(max(-200, radio.ppm - 1)) } label: { Image(systemName: "minus") }.buttonStyle(.plain)
        Text("\(radio.ppm)").font(.system(size: 15, weight: .semibold, design: .rounded)).monospacedDigit().frame(minWidth: 26)
        Button { radio.setPpm(min(200, radio.ppm + 1)) } label: { Image(systemName: "plus") }.buttonStyle(.plain)
      }.font(.system(size: 13, weight: .semibold))
    }
    .frame(maxWidth: .infinity).frame(height: cellH).foregroundColor(.white)
    .background(RoundedRectangle(cornerRadius: 10).fill(.white.opacity(0.14)))
  }

  /// One broadcast-FM treatment. Lit ORANGE rather than green, deliberately: green on this sheet
  /// means "a hardware control is engaged", and these are DSP the server is applying to everyone's
  /// audio — a different kind of thing, and worth looking different.
  private func fmCell(_ label: String, on: Bool, action: @escaping () -> Void) -> some View {
    Button(action: action) {
      Text(label).font(.system(size: 13, weight: .semibold))
        .frame(maxWidth: .infinity).padding(.vertical, 7)
        .background(on ? Color.orange : Color.white.opacity(0.12), in: RoundedRectangle(cornerRadius: 8))
        .foregroundColor(on ? .black : .white)
    }.buttonStyle(.plain)
  }

  /// An on/off control drawn as an ILLUMINATED button — glass when off, lit green when on (clearer on a
  /// wrist than a switch). Tapping toggles.
  private func onOff(_ label: String, on: Bool, action: @escaping () -> Void) -> some View {
    Button(action: action) {
      HStack {
        Text(label).font(.system(size: 15, weight: .semibold))
        Spacer()
        Text(on ? "ON" : "OFF").font(.system(size: 12, weight: .bold))
          .foregroundColor(on ? .black.opacity(0.7) : .white.opacity(0.5))
      }
      .padding(.horizontal, 12).padding(.vertical, 9)
      .foregroundColor(on ? .black : .white)
      .background(RoundedRectangle(cornerRadius: 10)
        .fill(on ? AnyShapeStyle(Color.green) : AnyShapeStyle(.white.opacity(0.14))))
      .contentShape(Rectangle())
    }.buttonStyle(.plain).listRowInsets(EdgeInsets())
  }

  /// A full-width − / value / + row. Same shape the old PPM cell used — big targets,
  /// no dragging, and the value never moves under your thumb.
  private func stepCell(title: String, value: String,
                        dec: @escaping () -> Void, inc: @escaping () -> Void) -> some View {
    VStack(spacing: 2) {
      Text(title).font(.system(size: 9, weight: .bold)).foregroundColor(.white.opacity(0.55))
      HStack(spacing: 14) {
        Button(action: dec) { Image(systemName: "minus") }.buttonStyle(.plain)
        Text(value).font(.system(size: 15, weight: .semibold, design: .rounded))
          .monospacedDigit().frame(minWidth: 62)
        Button(action: inc) { Image(systemName: "plus") }.buttonStyle(.plain)
      }.font(.system(size: 13, weight: .semibold))
    }
    .frame(maxWidth: .infinity).frame(height: cellH).foregroundColor(.white)
    .background(RoundedRectangle(cornerRadius: 10).fill(.white.opacity(0.14)))
  }

  private let cellH: CGFloat = 56

  /// The owner has locked the radio and we are not through it. Everything the
  /// server's adminGate refuses is drawn dim and inert while this holds.
  private var ownerLocked: Bool { radio.adminSet && !radio.adminOk }

  var body: some View {
    ScrollView {
      VStack(spacing: 7) {
        // ★★ THE ADMIN LOCK, WHERE THE LOCKED CONTROLS ARE. The server enforces it —
        // bias-T, PPM, direct sampling and calibration all go through adminGate — but
        // a client that is not told draws every control as normal and the user finds
        // out only when one silently does nothing. So: say it, and put the way in
        // right here rather than back in a menu, because this is where the
        // frustration happens.
        //
        // ★ Only shown when the owner HAS set a password (`adminSet`) and we are not
        // already through it: an unlock box on a server with nothing to unlock is a
        // puzzle, which is exactly why the shim advertises the flag.
        // ★ SAY WHICH RADIO. Without it the sheet is a set of controls with no subject,
        // and on a shared server you cannot tell whether you are looking at the dongle
        // you expected or something else entirely.
        if !radio.radioName.isEmpty && radio.radioName != "—" {
          HStack(spacing: 5) {
            Image(systemName: "antenna.radiowaves.left.and.right").font(.system(size: 10, weight: .bold))
            Text(radio.radioName).font(.system(size: 12, weight: .semibold))
            Spacer()
            // ★ TOTAL SYSTEM GAIN, LIVE. On an RSP this is the only way to SEE the AGC
            // working — the number moves as the loop tracks. Without it a working AGC
            // and a stuck one look identical, which is exactly how the original
            // "AGC never engages" bug hid (fixed server-side in 05f330f).
            if radio.radioDriver == "sdrplay" && radio.sysGainDb != 0 {
              Text(String(format: "%.1f dB", radio.sysGainDb))
                .font(.system(size: 12, weight: .semibold, design: .rounded)).monospacedDigit()
                .foregroundColor(radio.rspOverload ? .red : .green)
            }
            // ★★ SETTLING — the gain loop is still moving, so the number beside it is not yet
            // a reading. Without this an AGC that is working and one that came up STUCK look
            // identical for the first few seconds, and "stuck on initial load" is a bug that
            // has bitten this project twice (Stuart, 2026-07-28).
            if radio.radioSettling {
              Text("SETTLING").font(.system(size: 9, weight: .bold))
                .foregroundColor(.orange)
            }
            // OVERLOAD is worth shouting about: it means the front end is being driven
            // too hard and everything you hear is suspect.
            if radio.rspOverload {
              Text("OVLD").font(.system(size: 10, weight: .bold)).foregroundColor(.red)
            }
          }
          .foregroundColor(.white.opacity(0.75))
          .padding(.horizontal, 2)
        }
        // Row 1 — GAIN (crown-armed) · AUTO
        // ★ Hidden entirely on an RSP (its gain is LNA state + IF gain reduction, not one
        // slider) and on an HF+ (no variable gain stage AT ALL). A slider that cannot
        // move the radio is worse than no slider.
        if radio.radioHasSimpleGain {
        HStack(spacing: 7) {
          Button {
            if !radio.gainAuto { gainArmed.toggle(); crownFocused = gainArmed; if gainArmed { attArmed = false } }
          } label: {
            cell(title: "GAIN", value: radio.gainAuto ? "Auto" : String(format: "%.1f dB", radio.gainValue / 10),
                 lit: gainArmed && !radio.gainAuto, litColor: .cyan, dim: radio.gainAuto)
          }.buttonStyle(.plain).disabled(radio.gainAuto)
          Button {
            let a = !radio.gainAuto; radio.setGainAuto(a)
            if a { gainArmed = false; crownFocused = false }
          } label: { cell(title: "AUTO GAIN", value: radio.gainAuto ? "ON" : "OFF", lit: radio.gainAuto) }
            .buttonStyle(.plain)
        }
        }
        // Row 2 — DIGITAL AGC. ★★ RTL-SDR ONLY, despite the old comment here calling it "a
        // client-side control, always yours". It is not: `setAgc` reaches
        // `rtlsdr_set_agc_mode(p->dev, …)` on the server, and on an Airspy or an RSP `p->dev` is
        // null so the call returns having done NOTHING (Stuart, 2026-07-29 — "digital AGC is an
        // RTL item"). A control that silently does nothing is the exact fault this menu has been
        // cleaned of twice already.
        // ★ Tested on the DRIVER NAME, not on "not an RSP and not an HF+" — that "else means
        //   dongle" shape has produced a bug every single time a third radio appeared
        //   ([[else_means_dongle_trap]]).
        if radio.radioDriver == "rtl" {
          HStack(spacing: 7) {
            Button { radio.setAgc(!radio.agc) } label: { cell(title: "DIGITAL AGC", value: radio.agc ? "ON" : "OFF", lit: radio.agc) }.buttonStyle(.plain)
          }
        }
        // Row 3 — SPAN (picker) · PPM (inline ±)
        // ★★ HIDDEN WHEN THERE IS NOTHING TO CHOOSE. The server sends each radio's OWN rate
        // list, and an Airspy HF+ Discovery offers a single rate — so the picker opened on a
        // list of one, which is a control in name only. Decided from the LIST rather than from
        // the driver, so an HF+ that does offer several keeps a working control and any future
        // radio needs no special case.
        if radio.offeredRates.count > 1 {
          HStack(spacing: 7) {
            Button { if radio.lockedRate == 0 { showSpan = true } } label: {
              cell(title: "SPAN", value: radio.sampleRate > 0 ? String(format: "%.1f MHz", Double(radio.sampleRate) / 1_000_000) : "—",
                   lit: false, dim: radio.lockedRate != 0)
            }.buttonStyle(.plain).disabled(radio.lockedRate != 0)
          }
        }
        // ★ CALIBRATION IS NOT A WRIST CONTROL. ppm (and the HF+'s parts-per-BILLION
        // equivalent) is an extremely fine trim you set once against a known
        // reference — not something anyone adjusts from a watch, and the one control
        // where a mis-tap is both easy and hard to notice. Deliberately absent; it
        // lives on the phone and the web client. (Stuart, 2026-07-28.)

        // ── The radio's OWN gain controls ──────────────────────────────────────
        // ★ These are the opposite of calibration: you reach for them because of what
        // you are hearing right now, which is exactly the wrist case.
        if radio.radioDriver == "sdrplay" && radio.lnaStates > 0 {
          stepCell(title: "LNA GAIN", value: "\(radio.rspLna)/\(max(0, radio.lnaStates - 1))",
                   dec: { radio.setRspLna(radio.rspLna - 1) },
                   inc: { radio.setRspLna(radio.rspLna + 1) })
          onOff("IF AGC", on: radio.rspIfAgc) { radio.setRspIfAgc(!radio.rspIfAgc) }
          // Manual IF gain reduction is the AGC's to own while it is on — dimmed, not
          // hidden: it is still the right control, just not yours at that moment.
          stepCell(title: "IF GAIN REDUCTION", value: "\(radio.rspIfGr) dB",
                   dec: { radio.setRspIfGr(radio.rspIfGr - 1) },
                   inc: { radio.setRspIfGr(radio.rspIfGr + 1) })
            .opacity(radio.rspIfAgc ? 0.35 : 1).disabled(radio.rspIfAgc)
          // ★ The two broadcast notches. Offered ONLY where the model advertises them —
          //   not every RSP has both, and a control that cannot work is worse than absent.
          if radio.radioHasRfNotch {
            onOff("FM/MW NOTCH", on: radio.rspRfNotch) { radio.setRspRfNotch(!radio.rspRfNotch) }
          }
          if radio.radioHasDabNotch {
            onOff("DAB NOTCH", on: radio.rspDabNotch) { radio.setRspDabNotch(!radio.rspDabNotch) }
          }
        }
        if radio.radioDriver == "airspyhf" {
          if radio.hasRadioAgc {
            onOff("AGC", on: radio.ahfAgc) { radio.setAhfAgc(!radio.ahfAgc) }
          }
          // ★ ON THE CROWN, like the dongle's gain. ± buttons felt unresponsive here:
          // the attenuator is a range you SWEEP while listening to the effect, and a
          // watch's crown is the precise control for that — a tap-tap-tap on a 41 mm
          // target is not (Stuart, 2026-07-28). Tap to arm, then turn.
          if radio.attSteps > 0 {
            Button {
              if !radio.ahfAgc { attArmed.toggle(); crownFocused = attArmed; if attArmed { gainArmed = false } }
            } label: {
              // ★ "set by AGC", not a stale number: the AGC owns the front end and the driver
              // has no getter to tell us what it picked.
              cell(title: attArmed ? "ATTENUATOR — TURN CROWN" : "ATTENUATOR",
                   value: radio.ahfAgc ? "set by AGC" : "\(radio.ahfAtt * radio.attStepDb) dB",
                   lit: attArmed && !radio.ahfAgc, litColor: .cyan, dim: radio.ahfAgc)
            }.buttonStyle(.plain).disabled(radio.ahfAgc)
          }
          if radio.hasPreamp {
            onOff("PREAMP (+6 dB)", on: radio.ahfPreamp) { radio.setAhfPreamp(!radio.ahfPreamp) }
          }
        }
        // FM de-emphasis — full width
        VStack(spacing: 3) {
          Text("FM DE-EMPHASIS").font(.system(size: 9, weight: .bold)).foregroundColor(.white.opacity(0.5))
          HStack(spacing: 6) {
            ForEach([(0, "Off"), (50, "50µs"), (75, "75µs")], id: \.0) { tau, label in
              let active = radio.deemph == tau
              Button { radio.setDeemph(tau) } label: {
                Text(label).font(.system(size: 13, weight: .semibold))
                  .frame(maxWidth: .infinity).padding(.vertical, 7)
                  .background(active ? Color.orange : Color.white.opacity(0.12), in: RoundedRectangle(cornerRadius: 8))
                  .foregroundColor(active ? .black : .white)
              }.buttonStyle(.plain)
            }
          }
        }.padding(.top, 3)

        // ══ BROADCAST FM — the four weak-signal treatments ═══════════════════
        // ★★★ FOUR FAULTS, FOUR SWITCHES, and they are NOT interchangeable: NR answers continuous
        //     NOISE, IMS a strong NEIGHBOUR, CEQ a REFLECTION, NB IMPULSES. Measured on the server
        //     they want OPPOSITE actions — narrowing the IF costs up to 10 dB against noise and
        //     gains 10 dB against a close neighbour — so one combined control would be wrong as
        //     well as unhelpful.
        // ★★ ALL FOUR ARE ON BY DEFAULT AND EACH DECLINES TO ACT unless its own measurements say it
        //    will help, so a strong station is untouched. The switches exist for A/B — which on a
        //    wrist is exactly the right size of job: three taps to hear what the receiver is doing.
        // ★ Shown only when the server says it HAS them (hasFmDsp). An older VibeServer never
        //   reports them and this section does not draw, rather than offering dead controls — the
        //   same rule as the gain slider that only suits one radio.
        if radio.hasFmDsp {
          VStack(spacing: 3) {
            Text("BROADCAST FM").font(.system(size: 9, weight: .bold)).foregroundColor(.white.opacity(0.5))
            // ★ Two rows of two, not one row of four: at 9-point type on a 41 mm watch, four cells
            //   across leaves each label too narrow to read at a glance — and this sheet is read
            //   at arm's length, often outdoors.
            HStack(spacing: 6) {
              fmCell("NR",  on: radio.fmNr)  { radio.setFmNr(!radio.fmNr) }
              fmCell("IMS", on: radio.fmIms) { radio.setFmIms(!radio.fmIms) }
            }
            HStack(spacing: 6) {
              fmCell("CEQ", on: radio.fmCeq) { radio.setFmCeq(!radio.fmCeq) }
              fmCell("NB",  on: radio.fmNb)  { radio.setFmNb(!radio.fmNb) }
            }
          }.padding(.top, 3)
        }

        if gainArmed, !radio.gainAuto {
          Text("Turn the crown to set gain").font(.system(size: 10)).foregroundColor(.cyan)
        }

        // ══ OWNER-ONLY, BELOW THE LINE ══════════════════════════════════════
        // ★ ORDER IS THE POINT: what you can use, then the lock, then what you
        // cannot. The unlock box sat at the TOP, so the first thing a visitor met
        // was a password prompt for controls they had not looked for yet — it read
        // as "this radio is not for you" rather than "most of this is". Everything
        // below the line goes through the server's adminGate.
        if radio.adminSet && !radio.adminOk {
          VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 5) {
              Image(systemName: "lock.fill").font(.system(size: 11, weight: .bold))
              Text("Owner-locked").font(.system(size: 12, weight: .semibold))
            }.foregroundColor(.orange)
            Text("These controls belong to the owner. Enter the admin password to unlock them.")
              .font(.system(size: 10)).foregroundColor(.white.opacity(0.6))
            SecureField("Admin password", text: $adminPass)
              .font(.system(size: 14, design: .rounded))
            Button {
              let p = adminPass.trimmingCharacters(in: .whitespaces)
              adminPass = ""
              radio.adminUnlock(p)
            } label: {
              Text("Unlock").font(.system(size: 13, weight: .semibold)).frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent).tint(.orange)
            .disabled(adminPass.trimmingCharacters(in: .whitespaces).isEmpty)
          }
          .padding(10)
          .background(RoundedRectangle(cornerRadius: 10).fill(.white.opacity(0.10)))
        }
        if radio.adminSet || radio.radioHasBiasT {
          if radio.radioHasBiasT {
            HStack(spacing: 7) {
              Button { radio.setBiasT(!radio.biasT) } label: {
                cell(title: "BIAS-T", value: radio.biasT ? "ON" : "OFF",
                     lit: radio.biasT && !ownerLocked, dim: ownerLocked)
              }.buttonStyle(.plain).disabled(ownerLocked)
              Spacer(minLength: 0)
            }
          }
        }
      }
      .padding(.horizontal, 6).padding(.bottom, 10)
    }
    .navigationTitle("Radio")
    .sheet(isPresented: $showSpan) {
      List {
        ForEach(radio.offeredRates, id: \.self) { r in
          Button { radio.setCaptureRate(r); showSpan = false } label: {
            HStack {
              Text(String(format: "%.2f MHz", Double(r) / 1_000_000)).font(.system(size: 15))
              Spacer()
              if radio.sampleRate == r { Image(systemName: "checkmark").foregroundStyle(.green) }
            }
          }.buttonStyle(.plain)
        }
      }.navigationTitle("Span")
    }
    // ★ ONE crown, either armed control. Two separate crown bindings in one view do
    // not work — SwiftUI gives the crown to whichever is focused, so arming one must
    // disarm the other (see the arming buttons) and the handler routes by whichever
    // is live.
    .focusable((gainArmed && !radio.gainAuto) || (attArmed && !radio.ahfAgc))
    .focused($crownFocused)
    .digitalCrownRotation($gainCrown, from: 0, through: 1000, by: 1, sensitivity: .low, isContinuous: true)
    .onChange(of: gainCrown) { _, new in
      let detent = Int(new.rounded())
      var delta = detent - lastGainDetent
      if delta > 500 { delta -= 1000 }; if delta < -500 { delta += 1000 }
      lastGainDetent = detent
      guard delta != 0 else { return }
      if attArmed, !radio.ahfAgc, radio.attSteps > 0 {
        radio.setAhfAtt(radio.ahfAtt + delta)
        return
      }
      guard gainArmed, !radio.gainAuto, !radio.offeredGains.isEmpty else { return }
      let gains = radio.offeredGains
      let cur = gains.firstIndex(where: { abs(radio.gainValue - Double($0)) < 0.5 }) ?? gains.count / 2
      let ni = min(gains.count - 1, max(0, cur + delta))
      radio.setGainValue(Double(gains[ni]))
    }
  }
}

/// The crown-sensitivity picker. A list, like Step and Demod — you tap the thing you
/// want, which is the right gesture on a surface your finger is already covering.
struct CrownPicker: View {
  @Binding var current: String
  let onPick: () -> Void

  var body: some View {
    List {
      ForEach(CrownSens.allCases, id: \.rawValue) { s in
        Button {
          current = s.rawValue
          onPick()
        } label: {
          HStack {
            VStack(alignment: .leading, spacing: 1) {
              Text(s.label)
                .font(.system(size: 16, weight: .semibold, design: .rounded))
              Text(s.detail)
                .font(.caption2)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            }
            Spacer()
            if current == s.rawValue {
              Image(systemName: "checkmark").foregroundStyle(.green)
            }
          }
        }
        .buttonStyle(.plain)
      }
    }
    .navigationTitle("Crown")
  }
}

/// A plain scrollable list. Deliberately dull: you tap the thing you want and it
/// happens, with no chance of passing through anything you didn't.
struct PickerList: View {
  let title: String
  let items: [String]
  let current: String
  let onPick: (String) -> Void

  var body: some View {
    List {
      ForEach(items, id: \.self) { item in
        Button {
          onPick(item)
        } label: {
          HStack {
            Text(item.uppercased())
              .font(.system(size: 16, weight: .semibold, design: .rounded))
            Spacer()
            if item.lowercased() == current.lowercased() {
              Image(systemName: "checkmark").foregroundStyle(.green)
            }
          }
        }
        .buttonStyle(.plain)
      }
    }
    .navigationTitle(title)
  }
}

/// DAB controls — programme picker (services in the tuned ensemble) + the speed-fix presets that work
/// around the dablin/OWRX "chipmunk" (a station whose sample rate the server misreads). Only reachable
/// on a DAB profile. Speed presets mirror the phone's set (Off / ×0.67 / ×0.50 / ×0.33 / ×0.25).
struct DabSheet: View {
  @EnvironmentObject var link: SpikeLink
  @Environment(\.dismiss) private var dismiss
  private let speeds: [(v: Double, l: String)] = [
    (1, "Off"), (0.6667, "×0.67"), (0.5, "×0.50"), (0.3333, "×0.33"), (0.25, "×0.25"),
  ]
  var body: some View {
    List {
      Section("Speed fix") {
        ScrollView(.horizontal, showsIndicators: false) {
          HStack(spacing: 6) {
            ForEach(speeds, id: \.l) { o in
              let active = abs(link.dabScale - o.v) < 0.001
              Button { link.setDabScale(o.v) } label: {
                Text(o.l).font(.system(size: 13, weight: .semibold))
                  .padding(.horizontal, 10).padding(.vertical, 6)
                  .background(active ? Color.orange : Color.white.opacity(0.12), in: Capsule())
                  .foregroundColor(active ? .black : .white)
              }.buttonStyle(.plain)
            }
          }
        }
      }
      Section("Station") {
        if link.dabProgrammes.isEmpty {
          Text("Waiting for the ensemble…").font(.system(size: 12)).foregroundColor(.white.opacity(0.5))
        }
        ForEach(link.dabProgrammes) { p in
          Button { link.selectDabService(p.id); dismiss() } label: {
            HStack(spacing: 8) {
              Text(p.name).font(.system(size: 14)).foregroundColor(p.name == link.stationName ? .green : .white).lineLimit(1)
              Spacer()
              if p.name == link.stationName { Image(systemName: "dot.radiowaves.left.and.right").font(.system(size: 13)).foregroundColor(.green) }
            }
          }.buttonStyle(.plain)
        }
      }
    }
    .navigationTitle("DAB")
  }
}

/// OWRX profile picker — grouped SDR → profiles, matching the phone. Active profile flagged; opens
/// with a brief etiquette reminder (switching retunes the SHARED receiver for everyone here).
struct ProfileSheet: View {
  @EnvironmentObject var link: SpikeLink
  let onSelect: (String) -> Void

  private var sdrs: [String] {
    var seen = Set<String>(); var out = [String]()
    for p in link.profiles where !seen.contains(p.sdrName) { seen.insert(p.sdrName); out.append(p.sdrName) }
    return out
  }

  var body: some View {
    // Open scrolled to the CURRENT profile (71-profile lists are painful to scroll from the top to
    // reach the neighbour of the one you're on — the phone opens on the active one too).
    ScrollViewReader { proxy in
      List {
        Section {
          Text("⚠︎ Switching retunes this receiver for everyone (\(link.clients) listening). Please ask in chat first.")
            .font(.system(size: 10.5)).foregroundColor(.orange).lineLimit(nil)
        }
        ForEach(sdrs, id: \.self) { sdr in
          Section(sdr) {
            ForEach(link.profiles.filter { $0.sdrName == sdr }) { p in
              Button { onSelect(p.id) } label: {
                HStack(spacing: 8) {
                  Text(p.name).font(.system(size: 14)).foregroundColor(p.active ? .green : .white).lineLimit(1)
                  Spacer()
                  // In-use / active indicator (the profile we're currently on).
                  if p.active { Image(systemName: "dot.radiowaves.left.and.right").font(.system(size: 13)).foregroundColor(.green) }
                }
              }.buttonStyle(.plain)
              .id(p.id)
            }
          }
        }
      }
      .navigationTitle("Profiles")
      .onAppear {
        guard let active = link.profiles.first(where: { $0.active })?.id else { return }
        // A tick after layout, else the List hasn't built its rows and scrollTo is a no-op.
        DispatchQueue.main.async { proxy.scrollTo(active, anchor: .center) }
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
/// Bookmarks — save the current spot on the dial and recall saved ones. LOCAL to the watch.
/// The list is filtered to the current server's coverage (a +30 MHz OWRX bookmark won't clutter a
/// 0–30 MHz Kiwi). Recall NEVER switches an OWRX profile: a frequency outside the current profile
/// window is refused with a "Frequency Range Not Available" pill (see SpikeLink.recall/notify).
struct BookmarksView: View {
  @EnvironmentObject var link: SpikeLink
  @EnvironmentObject var store: BookmarkStore
  @Environment(\.dismiss) private var dismiss
  /// Closes the CONTROL MENU behind this sheet, so a recall lands straight on the waterfall.
  let closeMenu: () -> Void

  @State private var showSave = false
  @State private var draftName = ""
  @State private var confirmDeleteAll = false

  /// Only bookmarks this server can actually reach — the rest are hidden, not greyed.
  private var shown: [Bookmark] { store.bookmarks.filter { link.inCoverage($0.frequency) } }

  var body: some View {
    List {
      Section {
        Button {
          draftName = Self.freqLabel(link.frequency)
          showSave = true
        } label: {
          HStack(spacing: 8) {
            Image(systemName: "plus.circle.fill").foregroundColor(.orange)
            Text("Save this frequency").font(.system(size: 14, weight: .semibold))
          }
        }
        .buttonStyle(.plain)
        // Deletion is a two-step, deliberate act (swipe → tap the trash) — say so, once, here.
        if !shown.isEmpty {
          Text("Swipe a bookmark left to delete")
            .font(.system(size: 10)).foregroundColor(.secondary)
        }
      }

      if shown.isEmpty {
        Section {
          Text(store.bookmarks.isEmpty ? "No bookmarks yet." : "None of your bookmarks are within this server's range.")
            .font(.system(size: 12)).foregroundColor(.secondary)
        }
      } else {
        Section("Saved") {
          ForEach(shown) { b in
            let reachable = link.canTune(b.frequency)
            Button {
              link.recall(b)      // tunes if reachable, else raises the warning pill on the waterfall
              dismiss(); closeMenu()
            } label: {
              HStack(spacing: 10) {
                VStack(alignment: .leading, spacing: 2) {
                  Text(b.name).font(.system(size: 16, weight: .semibold)).lineLimit(1)
                  HStack(spacing: 6) {
                    Text(Self.freqLabel(b.frequency)).font(.system(size: 11)).foregroundColor(.white.opacity(0.65))
                    if !b.mode.isEmpty {
                      Text(b.mode.uppercased()).font(.system(size: 10, weight: .bold))
                        .padding(.horizontal, 5).padding(.vertical, 1)
                        .background(.white.opacity(0.12), in: Capsule()).foregroundColor(.white.opacity(0.8))
                    }
                  }
                }
                Spacer()
                // Out of the current OWRX profile — still listed, but flag that recall will need a
                // profile change (which we don't do automatically).
                if !reachable {
                  Image(systemName: "exclamationmark.triangle.fill")
                    .font(.system(size: 11)).foregroundColor(.yellow.opacity(0.8))
                }
              }
              .opacity(reachable ? 1 : 0.55)
            }
            .buttonStyle(.plain)
            // Two-step delete: swipe reveals the trash, tapping it removes. No full-swipe auto-delete.
            .swipeActions(edge: .trailing, allowsFullSwipe: false) {
              Button(role: .destructive) {
                if let b2 = shown.first(where: { $0.id == b.id }) { store.remove(b2) }
              } label: { Image(systemName: "trash") }
            }
          }
        }

        // DELETE ALL — a rare, heavy action, so it lives at the very bottom and asks first.
        Section {
          Button(role: .destructive) { confirmDeleteAll = true } label: {
            HStack(spacing: 8) {
              Image(systemName: "trash")
              Text("Delete all bookmarks").font(.system(size: 13, weight: .semibold))
            }
          }
        }
      }
    }
    .navigationTitle("Bookmarks")
    .confirmationDialog("Delete all bookmarks?", isPresented: $confirmDeleteAll, titleVisibility: .visible) {
      Button("Delete all", role: .destructive) { store.removeAll() }
      Button("Cancel", role: .cancel) {}
    } message: {
      Text("This removes every saved bookmark on this watch. It can't be undone.")
    }
    .sheet(isPresented: $showSave) {
      VStack(spacing: 10) {
        Text("Name this bookmark").font(.system(size: 13, weight: .semibold))
        TextField("Name", text: $draftName)
          .textInputAutocapitalization(.words)
        HStack(spacing: 8) {
          Button("Cancel") { showSave = false }.buttonStyle(.bordered)
          Button("Save") {
            let name = draftName.trimmingCharacters(in: .whitespaces)
            store.add(name: name.isEmpty ? Self.freqLabel(link.frequency) : name,
                      frequency: link.frequency, mode: link.mode)
            showSave = false
          }.buttonStyle(.borderedProminent)
        }
      }
      .padding()
    }
  }

  /// 145.500 MHz / 7.100 MHz / 198 kHz — mirrors how the readout speaks frequency.
  static func freqLabel(_ hz: Double) -> String {
    if hz >= 1_000_000 { return String(format: "%.3f MHz", hz / 1_000_000) }
    if hz >= 1_000     { return String(format: "%.0f kHz", hz / 1_000) }
    return String(format: "%.0f Hz", hz)
  }
}

// ─────────────────────────────────────────────────────────────────────────────
/// DISPLAY — one home for the waterfall's look. Auto contrast / Brightness / Contrast are CROWN
/// tweaks (tap → dismiss to the waterfall and adjust live), all WATCH-LOCAL (the wrist varies in
/// readability far more than the phone). Palette + VFO colour open a crown-preview picker and CAN
/// track the phone via "Sync". Peak hold is a plain toggle (exposes a control that was hardwired on).
struct DisplaySheet: View {
  /// ★ Which crown modes belong to this sheet. Used by the waterfall to decide whether a
  ///   hold-menu should come back HERE. Deliberately ALL of them, not just the range pair:
  ///   brightness and contrast are adjusted in the same look-tweak-look loop.
  static func isDisplayMode(_ m: CrownMode) -> Bool {
    switch m {
    case .brightness, .contrast, .autoContrast, .wfFloor, .wfCeil: return true
    default: return false
    }
  }

  @EnvironmentObject var link: SpikeLink
  let onPickCrown: (CrownMode) -> Void   // arms a waterfall crown mode + closes the menu
  let closeAll: () -> Void               // closes the Display sheet + the menu, back to the spectrum

  @AppStorage("wfAutoContrast") private var wfAutoContrast = 5.0
  @AppStorage("wfBright")       private var wfBright       = 0.0
  @AppStorage("wfContrast")     private var wfContrast     = 0.0
  // ★ Defaults are real palettes now, not "sync" — that option promised to follow the phone
  // and never did (it resolved to Sonar Green). The picker offers the phone's full list instead.
  @AppStorage("wfPalette")      private var wfPalette      = "sonar"
  @AppStorage("wfVfoColour")    private var wfVfoColour    = "orange"
  @AppStorage("wfPeakHold")     private var wfPeakHold     = true
  // ★ MANUAL RANGE — watch-GLOBAL, like brightness and contrast, because it is a readability
  //   setting rather than a property of the aerial (BRIEF-jr-display-manual-range.md, open Q3).
  @AppStorage("wfManualRange")  private var wfManualRange  = false
  @AppStorage("wfFloorDb")      private var wfFloorDb      = -110.0
  @AppStorage("wfCeilDb")       private var wfCeilDb       = -30.0
  @AppStorage("meterUnit")      private var meterUnit      = "snr"

  @State private var showPalette = false
  @State private var showVfo     = false

  /// Crown steps as a signed integer, matching how the phone speaks brightness/contrast ("+5").
  private func steps(_ v: Double) -> String { let n = Int((v / 0.04).rounded()); return n > 0 ? "+\(n)" : "\(n)" }

  private var palette: WFPalette { SpikeLink.palettes.first { $0.id == wfPalette } ?? .sonar }
  private var vfo: VfoColour { SpikeLink.vfoColours.first { $0.id == wfVfoColour } ?? SpikeLink.vfoColours[0] }
  /// ★ "sync" counts as default: a wrist upgrading from the old build has it stored, and it always
  /// rendered as Sonar Green / Orange anyway. Without this, Reset would look enabled on a watch the
  /// user has never touched.
  private var isDefaultPalette: Bool { wfPalette == "sonar" || wfPalette == "sync" }
  private var isDefaultVfo: Bool { wfVfoColour == "orange" || wfVfoColour == "sync" }
  private var displayDirty: Bool {
    wfAutoContrast != 5 || wfBright != 0 || wfContrast != 0 || wfManualRange
      || !isDefaultPalette || !isDefaultVfo || !wfPeakHold || meterUnit != "snr"
  }

  var body: some View {
    List {
      Section("Tone") {
        // ★ MAN sits one crown detent BELOW 0 on this same row — same gesture, no new menu to
        //   find. See handleCrown(.autoContrast).
        toneRow(icon: "wand.and.stars",        label: "Auto contrast",
                value: wfManualRange ? "MAN" : "\(Int(wfAutoContrast))") { onPickCrown(.autoContrast) }
        // ★★ Floor and Ceiling appear ONLY in manual — in auto they are not merely inactive,
        //    they are meaningless, and an inert row still reads as an offer.
        if wfManualRange {
          toneRow(icon: "arrow.down.to.line", label: "Floor",
                  value: "\(Int(wfFloorDb)) dBFS") { onPickCrown(.wfFloor) }
          toneRow(icon: "arrow.up.to.line",   label: "Ceiling",
                  value: "\(Int(wfCeilDb)) dBFS") { onPickCrown(.wfCeil) }
        }
        toneRow(icon: "sun.max.fill",          label: "Brightness",    value: steps(wfBright))          { onPickCrown(.brightness) }
        toneRow(icon: "circle.lefthalf.filled", label: "Contrast",      value: steps(wfContrast))        { onPickCrown(.contrast) }
      }
      Section("Colour") {
        Button { showPalette = true } label: {
          colourRow(label: "Palette", detail: palette.name, swatches: palette.swatches)
        }.buttonStyle(.plain)
        Button { showVfo = true } label: {
          colourRow(label: "VFO colour", detail: vfo.name, swatches: [vfo.color])
        }.buttonStyle(.plain)
      }
      // METER UNIT — the readout in the frequency pill only. The squelch bar is deliberately NOT
      // affected: it is a unitless "point the needle above the noise" control that works, and
      // putting units on it would re-open a thing that's already right.
      Section("Signal meter") {
        Picker("Units", selection: $meterUnit) {
          Text("SNR").tag("snr")
          Text("S-units").tag("smeter")
          Text("dBFS").tag("dbfs")
        }
        .onChange(of: meterUnit) { _, u in link.meterUnit = u }
      }
      Section {
        Toggle(isOn: $wfPeakHold) {
          Label("Peak hold", systemImage: "chart.line.uptrend.xyaxis")
        }
        .onChange(of: wfPeakHold) { _, on in link.peakHold = on; link.waterfall.peakHold = on }
      }

      // RESET — the escape hatch. These are all watch-local, so a user who cranks brightness to a
      // white slab (or picks a palette they hate) has no phone setting to undo it; this puts the
      // whole Display menu back to defaults. Dimmed/disabled when already at defaults.
      Section {
        Button(role: .destructive) {
          wfAutoContrast = 5; wfBright = 0; wfContrast = 0
          // ★ Reset must cover the new state too — back to AUTO at 5, floor/ceiling cleared.
          wfManualRange = false; wfFloorDb = -110; wfCeilDb = -30
          wfPalette = "sonar"; wfVfoColour = "orange"; wfPeakHold = true
          link.setManualRange(false, floor: -110, ceil: -30)
          link.setAutoContrast(5); link.waterfall.brightness = 0; link.waterfall.contrast = 0
          link.applyPalette("sonar"); link.applyVfo("orange")
          link.peakHold = true; link.waterfall.peakHold = true
          meterUnit = "snr"; link.meterUnit = "snr"
          WKInterfaceDevice.current().play(.success)
        } label: {
          Label("Reset display", systemImage: "arrow.counterclockwise")
        }
        .disabled(!displayDirty)
      }
    }
    .navigationTitle("Display")
    .sheet(isPresented: $showPalette) {
      ColourCrownPicker(title: "Palette", current: wfPalette,
        options: SpikeLink.palettes.map { .init(id: $0.id, name: $0.name, colors: $0.swatches) }) { id in
          wfPalette = id; link.applyPalette(id); closeAll()
      }
    }
    .sheet(isPresented: $showVfo) {
      ColourCrownPicker(title: "VFO colour", current: wfVfoColour,
        options: SpikeLink.vfoColours.map { .init(id: $0.id, name: $0.name, colors: [$0.color]) }) { id in
          wfVfoColour = id; link.applyVfo(id); closeAll()
      }
    }
  }

  private func toneRow(icon: String, label: String, value: String, tap: @escaping () -> Void) -> some View {
    Button(action: tap) {
      HStack(spacing: 10) {
        Image(systemName: icon).frame(width: 22).foregroundColor(.orange)
        Text(label).font(.system(size: 14))
        Spacer()
        Text(value).font(.system(size: 14, weight: .semibold)).monospacedDigit().foregroundColor(.white.opacity(0.7))
        Image(systemName: "digitalcrown.horizontal.arrow.clockwise").font(.system(size: 11)).foregroundColor(.white.opacity(0.35))
      }
    }.buttonStyle(.plain)
  }

  private func colourRow(label: String, detail: String, swatches: [Color]) -> some View {
    HStack(spacing: 10) {
      SwatchStripe(colors: swatches).frame(width: 34, height: 22)
      VStack(alignment: .leading, spacing: 1) {
        Text(label).font(.system(size: 14))
        Text(detail).font(.system(size: 11)).foregroundColor(.white.opacity(0.6))
      }
      Spacer(); Image(systemName: "chevron.right").foregroundColor(.white.opacity(0.4))
    }
  }
}

/// Little stack of colour bands — the palette preview drawn on the Display rows and the picker.
struct SwatchStripe: View {
  let colors: [Color]
  var body: some View {
    HStack(spacing: 0) { ForEach(Array(colors.enumerated()), id: \.offset) { _, c in c } }
      .clipShape(RoundedRectangle(cornerRadius: 4))
      .overlay(RoundedRectangle(cornerRadius: 4).stroke(.white.opacity(0.2), lineWidth: 0.5))
  }
}

/// A crown-driven colour/palette picker. Spin the crown to move through the options, watch the big
/// preview change live, tap to apply and return to the spectrum. Handles both palettes (many bands)
/// and VFO colours (one). "Sync" is just the first option — it tracks the phone once iCloud lands.
struct ColourCrownPicker: View {
  struct Option: Identifiable { let id: String; let name: String; let colors: [Color] }
  let title: String
  let current: String
  let options: [Option]
  let onApply: (String) -> Void

  @State private var sel = 0.0
  @FocusState private var focused: Bool

  private var idx: Int { min(options.count - 1, max(0, Int(sel.rounded()))) }

  var body: some View {
    VStack(spacing: 10) {
      Text(title).font(.system(size: 13, weight: .semibold)).foregroundColor(.white.opacity(0.7))
      // The big live preview — this IS the button.
      Button { onApply(options[idx].id) } label: {
        ZStack(alignment: .bottom) {
          SwatchStripe(colors: options[idx].colors).frame(height: 70)
          Text(options[idx].name)
            .font(.system(size: 15, weight: .bold)).foregroundColor(.white)
            .padding(.horizontal, 8).padding(.vertical, 3)
            .background(.black.opacity(0.55), in: Capsule())
            .padding(.bottom, 8)
        }
      }
      .buttonStyle(.plain)
      .overlay(RoundedRectangle(cornerRadius: 8).stroke(.orange.opacity(0.8), lineWidth: 2))
      Text("Turn crown · tap to set").font(.system(size: 10)).foregroundColor(.secondary)
    }
    .padding()
    .focusable(true)
    .focused($focused)
    .digitalCrownRotation($sel, from: 0, through: Double(max(0, options.count - 1)), by: 1,
                          sensitivity: .low, isContinuous: false, isHapticFeedbackEnabled: true)
    .onAppear {
      sel = Double(options.firstIndex { $0.id == current } ?? 0)
      focused = true
    }
  }
}

/// SNR squelch editor — crown sets the threshold (dB) live so you hear the gate and see the line
/// move as you turn; 0 = Off (open). Tap Done to close. The audio gate is sent to the server live.
/// Visual squelch: a live signal bar you point a red needle at. Because the gate acts on the SAME
/// bar (client-side, see SpikeLink.setSquelch), what you see is what you get — no numbers, no scale to
/// guess. Crown moves the needle; ≤ 2% = Off. State word shows Passing/Muting live.
struct SquelchView: View {
  @EnvironmentObject var link: SpikeLink
  @Environment(\.dismiss) private var dismiss
  @State private var pos = 0.0
  @FocusState private var focused: Bool

  var body: some View {
    VStack(spacing: 10) {
      Text("SQUELCH").font(.system(size: 12, weight: .bold)).foregroundColor(.orange)
      Text(pos < 0.02 ? "Off" : (link.sqlSignal < pos ? "Muting" : "Passing"))
        .font(.system(size: 13, weight: .bold))
        .foregroundColor(pos < 0.02 ? .secondary : (link.sqlSignal < pos ? Color(red: 1, green: 0.3, blue: 0.3) : .green))
      SquelchBar(level: link.sqlSignal, pos: pos)
      Text("Point the needle just above the noise.\nTurn the crown · tap Done.")
        .font(.system(size: 10)).foregroundColor(.secondary).multilineTextAlignment(.center)
      Button("Done") { dismiss() }.buttonStyle(.borderedProminent).tint(.orange)
    }
    .padding()
    .focusable(true)
    .focused($focused)
    .digitalCrownRotation($pos, from: 0, through: 1, by: 0.02,
                          sensitivity: .low, isContinuous: false, isHapticFeedbackEnabled: true)
    .onChange(of: pos) { _, p in link.setSquelch(p < 0.02 ? -1 : p) }
    .onReceive(Timer.publish(every: 1.0 / 15.0, on: .main, in: .common).autoconnect()) { _ in
      link.pollSignal()   // the waterfall render driver pauses under the sheet — keep the bar live
    }
    .onAppear { pos = link.sql < 0 ? 0 : link.sql; focused = true }
  }
}

/// The live signal bar with a red squelch needle — the shared shape of the visual squelch control.
/// White fill = the live signal; the red needle is the threshold you point at it.
struct SquelchBar: View {
  let level: Double
  let pos: Double
  var body: some View {
    GeometryReader { geo in
      let w = geo.size.width
      let closed = pos >= 0.02 && level < pos   // signal below the needle → muting
      ZStack(alignment: .leading) {
        Capsule().fill(.white.opacity(0.15))
        Capsule().fill(closed ? Color(red: 1, green: 0.3, blue: 0.3).opacity(0.9) : .white.opacity(0.9))
          .frame(width: w * min(1, max(0, level)))
          .animation(.easeOut(duration: 0.1), value: level)
        if pos >= 0.02 {
          let x = w * min(1, max(0, pos))
          Rectangle().fill(.green).frame(width: 2).offset(x: x - 1)
          Image(systemName: "arrowtriangle.down.fill")
            .font(.system(size: 9)).foregroundColor(.green).offset(x: x - 5, y: -11)
        }
      }
    }
    .frame(height: 14)
    .padding(.top, 10)   // headroom for the arrow
  }
}
