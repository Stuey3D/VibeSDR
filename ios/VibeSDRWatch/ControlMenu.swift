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
  case tune, zoom, brightness, contrast, volume

  var glyph: String {
    switch self {
    case .tune:       return "dial.medium"
    case .zoom:       return "magnifyingglass"
    case .brightness: return "sun.max.fill"
    case .contrast:   return "circle.lefthalf.filled"
    case .volume:     return "speaker.wave.2.fill"
    }
  }
  var label: String {
    switch self {
    case .tune: return "Tune"; case .zoom: return "Zoom"; case .volume: return "Volume"
    case .brightness: return "Bright"; case .contrast: return "Contrast"
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
  @Environment(\.dismiss) private var dismiss

  /// Set by the caller when Volume or Zoom is chosen: the menu closes and the
  /// waterfall returns with the crown in that mode.
  let onPickCrown: (CrownMode) -> Void

  @State private var showModes = false
  @State private var showSteps = false
  @State private var showCrown = false
  @State private var showProfiles = false
  private var activeProfileName: String { link.profiles.first(where: { $0.active })?.name ?? "—" }
  @State private var showWrist = false
  @State private var showBw = false
  @State private var showDab = false
  @AppStorage("vibeLinkMode") private var linkMode = LinkManager.Mode.adaptive.rawValue
  private var linkModeBlurb: String {
    switch LinkManager.Mode(rawValue: linkMode) ?? .adaptive {
    case .full:     return "Always asks for the full frame rate — may stutter on a poor link."
    case .adaptive: return "Adjusts the frame rate requested from the server to suit the connection. Results in slower waterfall draws."
    case .lowData:  return "Holds the lowest frame rate to save data. Waterfall draws slowly; audio is unaffected."
    }
  }
  @AppStorage("crownSens") private var crownSens = CrownSens.medium.rawValue
  /// Wrist-down spectrum timeout (seconds; 0 = never drop, keep it running at the cost of
  /// battery). ContentView reads the SAME key to time its suspend. See wristOptions.
  @AppStorage("jrWristTimeout") private var wristTimeout = 30.0
  /// WATCH-LOCAL waterfall offsets — the same keys ContentView drives. Mirrored here
  /// only so Reset can clear them.
  @AppStorage("wfBright")   private var wfBright   = 0.0
  @AppStorage("wfContrast") private var wfContrast = 0.0

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
          LazyVGrid(columns: cols, spacing: 5) {
          tile(icon: "magnifyingglass", label: "Zoom", h: h) {
            dismiss(); onPickCrown(.zoom)
          }
          // The iPhone's SYSTEM volume, not an app gain. The wrist shows the phone's
          // real level — including changes made ON the phone — so the two can never
          // disagree. (An app gain was the first attempt: delivered loudness is
          // appGain × systemVolume, the watch could only see one of the two, and with
          // the phone at 50% the meter read full while delivering half.)
          tile(icon: "speaker.wave.2.fill", label: "Volume", h: h) {
            dismiss(); onPickCrown(.volume)
          }
          // Mute is NOT volume-to-zero: that would lose the level you were listening
          // at, so unmuting could not put it back. This gates playback and leaves the
          // volume where it is.
          tile(icon: link.muted ? "speaker.slash.fill" : "speaker.fill",
               label: link.muted ? "Unmute" : "Mute", h: h) {
            dismiss(); link.setMuted(!link.muted)
          }
          // NAME the control, then show its VALUE. A tile reading just "Fine" (or
          // "9k", or "USB") shows you the setting while leaving you to guess what
          // it's the setting FOR. The name makes the tile a control; the value makes
          // the menu double as a status readout. You need both.
          // The WATCH's own brightness/contrast — the phone's settings are mirrored
          // as the base, but the same numbers don't serve both screens: a waterfall
          // that reads fine on a big bright phone can be near-black on a wrist held
          // at an angle outdoors. These are watch-local and persist.
          tile(icon: "sun.max.fill", label: "Bright", h: h) {
            dismiss(); onPickCrown(.brightness)
          }
          tile(icon: "circle.lefthalf.filled", label: "Contrast", h: h) {
            dismiss(); onPickCrown(.contrast)
          }
          tile(name: "CROWN", value: crownLabel, h: h) { showCrown = true }
          tile(name: "STEP",  value: stepLabel(link.step), h: h) { showSteps = true }
          tile(name: "DEMOD", value: link.mode.uppercased(), h: h) { showModes = true }
          // Passband: tap → LSB/USB crown editor. Value = total width in kHz.
          tile(name: "BW", value: bwLabel, h: h) { showBw = true }
          // DAB — only on a DAB profile. Programme picker (OWRX plays nothing until a service is
          // chosen; we auto-pick the first) + the speed-fix presets for the dablin chipmunk.
          if link.mode == "dab" {
            tile(name: "DAB", value: link.stationName.isEmpty ? "\(link.dabProgrammes.count) svc" : link.stationName, h: h) { showDab = true }
          }
          // Wrist-down spectrum timeout — battery vs "always live". Off keeps the waterfall
          // running with the wrist down (costs power); the timed options drop it after N and
          // reconnect on the way back.
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

          // A WAY BACK. Brightness and contrast are watch-local, so a user who
          // cranks them until the waterfall is a white slab has no phone setting to
          // undo it with — and no obvious way to tell that the WATCH is what they
          // broke. Resets ONLY the watch's own offsets; the phone is untouched.
          resetTile(h: h)
        }

        // LINK MANAGEMENT — three states, not a switch. "Slow because I chose to" and "slow
        // because the link is bad" are different situations and must not share a control.
        VStack(spacing: 3) {
          Text("LINK MANAGEMENT").font(.system(size: 9, weight: .bold)).foregroundColor(.white.opacity(0.5))
          HStack(spacing: 6) {
            ForEach([(LinkManager.Mode.full, "Full"),
                     (LinkManager.Mode.adaptive, "Auto"),
                     (LinkManager.Mode.lowData, "Low data")], id: \.0.rawValue) { m, label in
              let active = linkMode == m.rawValue
              Button { linkMode = m.rawValue } label: {
                Text(label)
                  .font(.system(size: 11, weight: .semibold))
                  .frame(maxWidth: .infinity).padding(.vertical, 6)
                  .foregroundColor(active ? .black : .white)
                  .background(RoundedRectangle(cornerRadius: 8)
                    .fill(active ? AnyShapeStyle(Color.green) : AnyShapeStyle(.white.opacity(0.14))))
              }.buttonStyle(.plain)
            }
          }
          Text(linkModeBlurb)
            .font(.system(size: 9)).foregroundColor(.white.opacity(0.5))
            .multilineTextAlignment(.center).fixedSize(horizontal: false, vertical: true)
        }.padding(.top, 3)

        // Room to scroll the LAST row clear of the rounded corner — as content,
        // not as a bar. Control Centre lets its tiles run off the bottom edge and
        // simply keeps scrolling; a fixed bottom padding on the outer stack instead
        // drew a hard black band across the screen, which reads as a broken layout
        // rather than as "there is more below".
        .padding(.bottom, 18)
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
    .sheet(isPresented: $showDab) {
      DabSheet().environmentObject(link)
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

  /// Reset the WATCH's waterfall offsets. Disabled (and dimmed) when they're already
  /// at default, so it reads as a status as much as a button.
  private func resetTile(h: CGFloat) -> some View {
    let dirty = wfBright != 0 || wfContrast != 0
    return Button {
      wfBright = 0
      wfContrast = 0
      link.waterfall.brightness = 0
      link.waterfall.contrast = 0
      WKInterfaceDevice.current().play(.success)
    } label: {
      VStack(spacing: 2) {
        Image(systemName: "arrow.counterclockwise")
          .font(.system(size: h * 0.26, weight: .semibold))
        Text("Reset view")
          .font(.system(size: h * 0.15, weight: .semibold, design: .rounded))
          .lineLimit(1)
          .minimumScaleFactor(0.6)
      }
      .foregroundStyle(dirty ? .white : .white.opacity(0.35))
      .frame(maxWidth: .infinity)
      .frame(height: h)
      .background(RoundedRectangle(cornerRadius: h * 0.30)
        .fill(.white.opacity(dirty ? 0.16 : 0.06)))
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .disabled(!dirty)
  }

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
  @State private var gainArmed = false
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

  private let cellH: CGFloat = 56

  var body: some View {
    ScrollView {
      VStack(spacing: 7) {
        // Row 1 — GAIN (crown-armed) · AUTO
        HStack(spacing: 7) {
          Button {
            if !radio.gainAuto { gainArmed.toggle(); crownFocused = gainArmed }
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
        // Row 2 — BIAS-T · DIGITAL AGC
        HStack(spacing: 7) {
          Button { radio.setBiasT(!radio.biasT) } label: { cell(title: "BIAS-T", value: radio.biasT ? "ON" : "OFF", lit: radio.biasT) }.buttonStyle(.plain)
          Button { radio.setAgc(!radio.agc) } label: { cell(title: "DIGITAL AGC", value: radio.agc ? "ON" : "OFF", lit: radio.agc) }.buttonStyle(.plain)
        }
        // Row 3 — SPAN (picker) · PPM (inline ±)
        HStack(spacing: 7) {
          Button { if radio.lockedRate == 0 { showSpan = true } } label: {
            cell(title: "SPAN", value: radio.sampleRate > 0 ? String(format: "%.1f MHz", Double(radio.sampleRate) / 1_000_000) : "—",
                 lit: false, dim: radio.lockedRate != 0)
          }.buttonStyle(.plain).disabled(radio.lockedRate != 0)
          ppmCell
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

        if gainArmed, !radio.gainAuto {
          Text("Turn the crown to set gain").font(.system(size: 10)).foregroundColor(.cyan)
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
    .focusable(gainArmed && !radio.gainAuto)
    .focused($crownFocused)
    .digitalCrownRotation($gainCrown, from: 0, through: 1000, by: 1, sensitivity: .low, isContinuous: true)
    .onChange(of: gainCrown) { _, new in
      guard gainArmed, !radio.gainAuto, !radio.offeredGains.isEmpty else { return }
      let detent = Int(new.rounded())
      var delta = detent - lastGainDetent
      if delta > 500 { delta -= 1000 }; if delta < -500 { delta += 1000 }
      lastGainDetent = detent
      guard delta != 0 else { return }
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
