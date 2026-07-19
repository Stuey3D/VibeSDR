import SwiftUI
import WatchConnectivity

/// **Which VibeSDR Jr are you running today?** — the launch gate and the two-button chooser.
///
/// Jr is one app with two genuinely different shapes (`BRIEF-watch-app-merge.md` §0):
///
///   - **Companion** — remote control and remote waterfall for the iPhone app. The phone owns the
///     radio and the audio; the watch draws rows it is sent.
///   - **Standalone** — its own receiver. Own sockets, own DSP, own Opus, own audio, no phone.
///
/// The spec: phone app running → open straight into Companion, no chooser. Phone app not running →
/// offer the choice. What makes that subtle is the word RUNNING.
@MainActor
final class PhonePresence: ObservableObject {
  /// A state frame arrived, so the phone app is genuinely OPEN AND IN USE.
  @Published private(set) var phoneActive = false
  /// The wait is over — decided one way or the other.
  @Published private(set) var settled = false

  /// How long to listen before concluding the phone is shut.
  ///
  /// ★ THE BOOTSTRAP PROBLEM. On a cold launch "the phone app is closed" and "the phone app has not
  ///   sent its first state frame YET" look identical, because we have no frames either way. Decide
  ///   instantly and Jr shows the chooser, then contradicts itself a moment later when the first
  ///   frame lands — the screen changing under a thumb already reaching for a button, which is how
  ///   you tap Standalone when you meant Companion.
  ///
  ///   So we wait, briefly. The cost is paid ONLY when the phone app really is shut (with it open a
  ///   frame arrives almost immediately and we stop early), and it buys an answer that is never
  ///   wrong. Chosen with Stuart, 2026-07-19.
  private static let window: Duration = .milliseconds(1500)

  /// ★ NOT `WCSession.isReachable`. Reachability is true whenever the watch COULD reach the phone —
  ///   and the watch can WAKE the iOS app in the background to make it so. Treating that as "the
  ///   app is open" would mean the chooser essentially never appears, and choosing Companion would
  ///   silently wake an app the user had deliberately closed. "In use" is evidenced by the phone
  ///   actively SENDING, and nothing else. We never reach for it to find out.
  func decide() async {
    let link = WatchLink.shared
    link.activate()

    let clock = ContinuousClock()
    let start = clock.now
    while clock.now - start < Self.window {
      if link.lastStateAt != nil { phoneActive = true; break }
      try? await Task.sleep(for: .milliseconds(100))
    }
    settled = true
  }
}

/// The waiting state. Deliberately quiet and unlabelled with a mode — naming one here would be a
/// guess, and a guess that flickers to the other answer is worse than a moment of nothing.
struct ModeProbeView: View {
  var body: some View {
    VStack(spacing: 10) {
      Image(systemName: "applewatch.radiowaves.left.and.right")
        .font(.system(size: 30, weight: .light))
        .foregroundStyle(.white.opacity(0.7))
      ProgressView().scaleEffect(0.7)
      Text("VibeSDR Jr")
        .font(.system(size: 13, weight: .semibold))
        .foregroundStyle(.white.opacity(0.5))
    }
  }
}

/// The two-button chooser.
///
/// ★ THE SPEAKER IS THE WHOLE IDEA. Each glyph shows the signal path, and a speaker sits beside
///   whichever device produces the SOUND — the iPhone in Companion, the watch in Standalone. That
///   is the difference a user actually feels, said in a picture rather than a paragraph.
struct ModeChooserView: View {
  let onCompanion: () -> Void
  let onStandalone: () -> Void

  var body: some View {
    // ★ NO TITLE, and both cards must fit WITHOUT scrolling at 41mm. The first draft had a
    //   "VibeSDR Jr" heading, which pushed Standalone below the fold — turning a two-way choice
    //   into one obvious option and one you had to discover by scrolling. On the smallest screen
    //   the app's own name is the least useful thing present; you already know what you opened.
    //   (Same lesson as the small-screen splash bug: 41mm is the design target, not the edge case.)
    ScrollView {
      VStack(spacing: 8) {
        ModeButton(
          title: "Companion",
          subtitle: "all audio handled by the iPhone",
          tint: .cyan,
          path: [.init(symbol: "applewatch"), .init(symbol: "iphone", speaker: true),
                 .init(symbol: "antenna.radiowaves.left.and.right")],
          action: onCompanion)

        ModeButton(
          title: "Standalone",
          subtitle: "no iPhone required",
          tint: .orange,
          path: [.init(symbol: "applewatch", speaker: true),
                 .init(symbol: "antenna.radiowaves.left.and.right")],
          action: onStandalone)
      }
      .padding(.horizontal, 4)
      .padding(.bottom, 6)
    }
  }
}

/// One hop in the signal path. `speaker` marks the device the audio comes OUT of.
struct PathNode: Identifiable {
  let id = UUID()
  let symbol: String
  var speaker = false
}

private struct ModeButton: View {
  let title: String
  let subtitle: String
  let tint: Color
  let path: [PathNode]
  let action: () -> Void

  var body: some View {
    Button(action: action) {
      VStack(alignment: .leading, spacing: 5) {
        Text(title).font(.system(size: 14, weight: .bold)).foregroundStyle(tint)
        pathGlyph
        Text(subtitle)
          .font(.system(size: 10))
          .foregroundStyle(.white.opacity(0.65))
          .fixedSize(horizontal: false, vertical: true)   // wraps at 41mm rather than truncating
          .multilineTextAlignment(.leading)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      .padding(.vertical, 6)
      .padding(.horizontal, 8)
    }
    .buttonStyle(.plain)
    .background(RoundedRectangle(cornerRadius: 11).fill(tint.opacity(0.16)))
    .overlay(RoundedRectangle(cornerRadius: 11).stroke(tint.opacity(0.4), lineWidth: 1))
  }

  private var pathGlyph: some View {
    HStack(spacing: 3) {
      ForEach(Array(path.enumerated()), id: \.element.id) { i, node in
        if i > 0 {
          Image(systemName: "arrow.left.arrow.right")
            .font(.system(size: 7, weight: .semibold))
            .foregroundStyle(.white.opacity(0.45))
        }
        ZStack(alignment: .bottomTrailing) {
          Image(systemName: node.symbol)
            .font(.system(size: 15, weight: .regular))
            .foregroundStyle(.white.opacity(0.9))
          if node.speaker {
            // Sits ON the device that makes the sound — offset clear of the glyph so it reads as a
            // badge rather than part of the icon.
            Image(systemName: "speaker.wave.2.fill")
              .font(.system(size: 8, weight: .bold))
              .foregroundStyle(tint)
              .offset(x: 6, y: 3)
          }
        }
      }
    }
    .frame(height: 20)
  }
}
