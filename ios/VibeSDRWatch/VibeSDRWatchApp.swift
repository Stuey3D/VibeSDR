import SwiftUI
import Darwin

@main
struct VibeSDRWatchApp: App {
  @StateObject private var link = WatchLink.shared

  var body: some Scene {
    WindowGroup {
      // NavigationStack so the numpad can be PUSHED. As a sheet it came with a
      // mandatory header (X + clock + grab handle) that stole ~100pt and cut the
      // bottom row off; that chrome cannot be removed from a sheet.
      NavigationStack {
        // Routed by BACKEND, not by a strip swap: FM-DX has no spectrum at all, so
        // it gets its own screen rather than a waterfall with the waterfall taken
        // out. The phone never tells us which screen to be — what it SENDS already
        // says: rows mean a spectrum, an FM-DX blob means a station.
        Group {
          switch link.screen {
          case .sdr:  ContentView()
          case .fmdx: FmdxView()
          case .dab:  DabView()
          case .adsb: AircraftView()
          }
        }
        .environmentObject(link)
        .navigationBarHidden(true)   // both screens are full-bleed; no bar on either
      }
      .onAppear { link.activate() }
    }
  }
}

// ── Ported from Jr, so both apps teach the same gestures with the same words ──────────────

/// Whether this watch supports the Double Tap (double-pinch) gesture — Series 9 / Ultra 2 and later.
/// Used to omit the "double-pinch" tutorial line on watches that can't do it.
enum DoubleTap {
  static let isSupported: Bool = {
    var size = 0
    sysctlbyname("hw.machine", nil, &size, nil, 0)
    guard size > 0 else { return true }
    var machine = [CChar](repeating: 0, count: size)
    sysctlbyname("hw.machine", &machine, &size, nil, 0)
    let id = String(cString: machine)                       // e.g. "Watch7,5"
    let parts = id.replacingOccurrences(of: "Watch", with: "").split(separator: ",")
    guard parts.count == 2, let maj = Int(parts[0]), let min = Int(parts[1]) else { return true }
    return (maj == 6 && min >= 14) || maj >= 7               // S9/Ultra2 and later; default true for unknown/future
  }()
}

/// One tutorial line: an SF Symbol + a short explanation.
struct TutorialTip: Identifiable { let id = UUID(); let icon: String; let text: String }

/// A generic scroll-to-read first-use card: title, tip rows, and a dismiss button at the bottom so the
/// user scrolls past the tips to reach it. Themed for the wrist.
struct TutorialSheet: View {
  let title: String
  let tips: [TutorialTip]
  var dismissLabel: String = "Got it"
  let onDismiss: () -> Void
  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 12) {
        Text(title).font(.headline).foregroundStyle(.orange)
        ForEach(tips) { t in
          HStack(alignment: .top, spacing: 9) {
            Image(systemName: t.icon).font(.system(size: 15)).foregroundStyle(.cyan).frame(width: 20)
            Text(.init(t.text)).font(.system(size: 13)).foregroundStyle(.white.opacity(0.9))
              .fixedSize(horizontal: false, vertical: true)
          }
        }
        Button(action: onDismiss) {
          Text(dismissLabel).font(.system(size: 15, weight: .semibold)).frame(maxWidth: .infinity)
        }.tint(.orange).padding(.top, 6)
      }
      .padding(.horizontal, 4).padding(.bottom, 8)
    }
    .navigationTitle("Tutorial")
  }
}

/// The SDR / waterfall control tutorial. Double-pinch line only on capable watches; the OWRX
/// Bluetooth-link warning only for OWRX servers (the runtime advisory pill still fires — this is a
/// one-time heads-up).
func sdrTutorialTips(isOwrx: Bool) -> [TutorialTip] {
  var t: [TutorialTip] = [
    .init(icon: "digitalcrown.horizontal.press", text: "**Turn the crown** to tune."),
  ]
  if DoubleTap.isSupported {
    t.append(.init(icon: "hand.pinch", text: "**Double-pinch** to switch the crown between Tune, Zoom and Volume."))
  }
  t += [
    .init(icon: "hand.tap", text: "**Press and hold** the waterfall for the menu — the demodulator and step rate live in there."),
    .init(icon: "keyboard", text: "**Tap the frequency** to type one in directly."),
    .init(icon: "lock", text: "**Tap the padlock** to lock the controls."),
    .init(icon: "antenna.radiowaves.left.and.right", text: "The **connection pill** shows how you're connected and the link quality to the server."),
    // ★ Explains a deliberate behaviour that would otherwise read as poor quality: with Automatic
    //   Link Management on we OPEN SLOWER on purpose, because connection setup is the moment a
    //   marginal link is most likely to drop. Without this line the first few seconds look like
    //   the app is bad rather than careful.
    .init(icon: "waveform.path.ecg", text: "With **Automatic Link Management** on, the waterfall starts slower while we work out how much the connection can carry. The link glyph cycles while it decides, then speeds up if the link is good."),
  ]
  if isOwrx {
    t.append(.init(icon: "wifi.exclamationmark", text: "Some OWRX servers stream heavily and can overwhelm the phone's Bluetooth link. If it stutters, use the watch's **own Wi-Fi or cellular** — you'll also see a warning pill when it happens."))
    t.append(.init(icon: "arrow.uturn.backward", text: "OWRX profiles are shared by everyone on the server, so leaving the app (flick to the watch face) drops the session — you'll come back on the server's **default profile**, not the one you were on."))
  }
  return t
}

/// DAB is a service LIST, not a band — its own gestures.
func dabTutorialTips() -> [TutorialTip] {
  var t: [TutorialTip] = [
    .init(icon: "square.stack.3d.up", text: "A DAB multiplex carries many stations — they build up as they decode."),
    .init(icon: "digitalcrown.horizontal.press", text: "**Turn the crown** to move the cursor through the list."),
  ]
  if DoubleTap.isSupported {
    t.append(.init(icon: "hand.pinch", text: "**Double-pinch** to tune the highlighted station — works in Water Lock."))
  }
  t += [
    .init(icon: "hand.tap", text: "Or **tap** a station to tune it."),
    .init(icon: "speaker.wave.2", text: "The **volume** button switches the crown to volume."),
    .init(icon: "gauge.with.dots.needle.bottom.50percent", text: "**Speed Fix** corrects a station that plays too fast (the DAB 'chipmunk') — remembered per station."),
    .init(icon: "line.3.horizontal", text: "The **menu** button has profiles, servers and the rest."),
  ]
  return t
}

/// FM-DX is a SHARED single tuner — the crown is armed deliberately so a wrist-nudge can't retune it for
/// everyone. That shared nature is why the chat here is tuning etiquette.
func fmdxTutorialTips() -> [TutorialTip] {
  [
    .init(icon: "dial.medium", text: "This receiver is **shared** — everyone hears the same tuner. So the crown is **off** until you arm it: tap the **dial button**, then turn to tune."),
    .init(icon: "timer", text: "The crown **disarms itself** after a few seconds — deliberate tuning only."),
    .init(icon: "speaker.wave.2", text: "Tap the **speaker** to give the crown the volume, with the native HUD."),
    .init(icon: "person.2.fill", text: "The **listener count** (top-left) tells you if you're alone. Tap it to **chat** — ask before you tune if others are on."),
    .init(icon: "rectangle.stack", text: "The **servers** button goes back to pick a different receiver."),
  ]
}

/// ADS-B is a whole-block mode — a live aircraft list/map, nothing to tune.
func adsbTutorialTips() -> [TutorialTip] {
  [
    .init(icon: "airplane", text: "Live aircraft decoded from 1090 MHz — **nearest first**, with distance from the receiver."),
    .init(icon: "map", text: "Tap **Map** to see them on a map with the receiver's home marker."),
    .init(icon: "digitalcrown.horizontal.press", text: "**Turn the crown** to scroll the list."),
    .init(icon: "line.3.horizontal", text: "The **menu** button has profiles and servers — there's nothing to tune here."),
  ]
}
