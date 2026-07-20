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
            .font(.system(size: 8, weight: .bold, design: .rounded))
            .monospacedDigit()
            .foregroundStyle(tint)
            .minimumScaleFactor(0.7)
            .lineLimit(1)
        }
        .frame(width: 26, height: 13)
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
  @EnvironmentObject var link: WatchLink
  @Environment(\.dismiss) private var dismiss

  /// Set by the caller when Volume or Zoom is chosen: the menu closes and the
  /// waterfall returns with the crown in that mode.
  let onPickCrown: (CrownMode) -> Void

  @State private var showModes = false
  @State private var showSteps = false
  @State private var showCrown = false
  @State private var showFavs = false
  @AppStorage("crownSens") private var crownSens = CrownSens.medium.rawValue
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

          // FAVOURITE INSTANCES. The watch can LAUNCH the phone app — but if there's
          // no default instance the phone lands on the picker and the wrist is left
          // looking at nothing. Bringing the whole directory over would be silly;
          // bringing the handful you actually use is exactly right.
          tile(icon: "server.rack", label: "Servers", h: h) { showFavs = true }

          // A WAY BACK. Brightness and contrast are watch-local, so a user who
          // cranks them until the waterfall is a white slab has no phone setting to
          // undo it with — and no obvious way to tell that the WATCH is what they
          // broke. Resets ONLY the watch's own offsets; the phone is untouched.
          resetTile(h: h)
        }
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
    .sheet(isPresented: $showFavs) {
      FavouritesList(favs: link.favourites) { url in
        link.selectInstance(url)
        showFavs = false
        dismiss()
      }
    }
    .sheet(isPresented: $showCrown) {
      CrownPicker(current: $crownSens) { showCrown = false; dismiss() }
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
  static let modes = ["usb", "lsb", "am", "sam", "fm", "nfm", "cwu", "cwl"]
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

/// A WAY OUT, for every screen that has no long-press menu.
///
/// FM-DX, DAB and ADS-B are CARDS and LISTS, not control surfaces — there's nothing on
/// them to zoom, step or demodulate, so none of them has a menu. Which meant that once
/// the phone landed on one of those backends, the wrist had no way to reach another
/// server: you had to take the phone out of your pocket, which is the one thing the
/// watch exists to avoid.
///
/// A button, not an invented menu. It lives in the CLOCK'S BAND — watchOS reserves that
/// strip whether we use it or not, so it costs no height — and it mirrors where the
/// phone puts the same control.
struct ServersButton: View {
  @Binding var show: Bool

  var body: some View {
    Button { show = true } label: {
      // A SERVER, not a star. A star says "favourite" — which is what the list holds,
      // not what the button DOES. The button switches receivers.
      Image(systemName: "server.rack")
        .font(.system(size: 11, weight: .semibold))
        .foregroundStyle(.white)
        .frame(width: 28, height: 26)      // the TAP TARGET is the frame, not the glyph
        .background(RoundedRectangle(cornerRadius: 7).fill(.white.opacity(0.14)))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
  }
}

/// The user's favourite instances, as a list. Tapping one switches the PHONE.
struct FavouritesList: View {
  let favs: [WatchLink.Favourite]
  let onPick: (String) -> Void

  var body: some View {
    List {
      if favs.isEmpty {
        // Honest about WHY it's empty, and where to fix it. An empty list with no
        // explanation reads as broken.
        // ♥ = favourite, ★ = default. Say the right one: this is the only instruction
        // a user with an empty list ever gets.
        Text("No favourites yet.\n♥ a server on the iPhone.")
          .font(.caption2)
          .foregroundStyle(.secondary)
          .multilineTextAlignment(.center)
          .frame(maxWidth: .infinity)
      }
      ForEach(favs) { f in
        Button {
          onPick(f.url)
        } label: {
          VStack(alignment: .leading, spacing: 1) {
            Text(f.name.isEmpty ? f.url : f.name)
              .font(.system(size: 15, weight: .semibold, design: .rounded))
              .lineLimit(1)
            if let t = f.type, !t.isEmpty {
              Text(t.uppercased())
                .font(.system(size: 9, weight: .semibold, design: .rounded))
                .foregroundStyle(.cyan)
            }
          }
        }
        .buttonStyle(.plain)
      }
    }
    .navigationTitle("Servers")
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
