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
  @Published fileprivate(set) var phoneActive = false
  /// The wait is over — decided one way or the other.
  @Published private(set) var settled = false
  fileprivate var poller: Task<Void, Never>?

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

  /// ★ NOT `WCSession.isReachable`. Reachability is true whenever the watch COULD reach the phone,
  ///   and it stays true for an app that is merely wakeable. Treating it as "open" would mean the
  ///   chooser essentially never appeared.
  ///
  /// ★★ PASSIVE LISTENING DOES NOT WORK, and this is why the first attempt always showed the
  ///    chooser (field-caught by Stuart on build 60). The phone sends `state` only from the SDR
  ///    screen, and only when something CHANGES — mode/step, or throttled while the frequency is
  ///    moving. A phone sitting open on a frequency, not being touched, sends nothing at all. So
  ///    "no state frame in 1.5s" was never evidence of a closed app; it was the normal idle case.
  ///
  ///    So we ASK. The phone answers every ping with state — that handshake already exists
  ///    precisely so the watch can tell "phone dead" from "phone fine, rows lost"
  ///    (`SDRScreen.tsx`: "Answer EVERY ping with state").
  ///
  ///    Doesn't that violate "never wake the phone to find out if it was awake"? No — because of
  ///    WHAT can answer. `sendState` is driven by the SDR screen's ping handler, which exists only
  ///    while that screen is mounted and running. A terminated app woken in the background comes up
  ///    without it and cannot reply, so the silence still reads as "not in use" and the chooser
  ///    still appears. The REPLY is the evidence, not the delivery.
  func decide() async {
    let link = WatchLink.shared
    link.activate()

    let clock = ContinuousClock()
    let start = clock.now
    // Activation is asynchronous; a ping fired before the session is up is simply dropped, so
    // repeat it rather than gambling the whole decision on one shot landing at the right moment.
    var nextPing = start
    while clock.now - start < Self.window {
      // Rows count too: a phone streaming a waterfall at us is unambiguously in use, and on a busy
      // link a row often lands before the state reply does.
      if link.lastStateAt != nil || link.lastRowAt != nil { phoneActive = true; break }
      if clock.now >= nextPing {
        link.ping()
        nextPing = clock.now.advanced(by: .milliseconds(500))
      }
      try? await Task.sleep(for: .milliseconds(100))
    }
    settled = true
  }
}

extension PhonePresence {
  /// Is the phone **actively running a server**, as opposed to merely being open?
  ///
  /// ★ THE TWO ARE DIFFERENT, and conflating them is the bug (Stuart, build 70): switching to
  ///   Companion always landed on the servers list, even when the phone was sitting on a live
  ///   waterfall. The rule he wants:
  ///
  ///     phone shut, or open with NO server   ->  servers list (there is nothing to show)
  ///     phone open AND running a server      ->  straight into that view, no list in the way
  ///
  /// ROWS ARE THE PROOF. A `state` frame only says the phone is on its SDR screen — which it also
  /// is while disconnected or failing. A row means a server is actually delivering a waterfall
  /// right now, which is exactly the condition for dropping the user into it.
  ///
  /// ★ Order matters. Standalone told the phone to stop sending (`cmd:stop`), so without resuming
  ///   we would be listening to a silence we caused ourselves and would always say "no server".
  ///   `ping` wakes the phone's spectrum (its `onHello`), `need` resumes rows and reflushes.
  ///
  /// ★★ ROWS ARE NOT THE ONLY PROOF, and insisting on them was a bug (Stuart, build 71). With the
  ///    phone LOCKED and playing in the background, its spectrum socket is closed for power — so
  ///    there are no rows to catch, and waiting for one concluded "no server" on a phone sitting
  ///    happily on a live one. Waking it takes a socket reopen and a first frame, which is longer
  ///    than anyone wants to stare at a spinner.
  ///
  ///    So ask the phone instead: `why` IS the answer, and it already distinguishes the cases —
  ///    'paused' (asleep for power), 'live', and 'reconnecting' all mean A SERVER IS THERE, while
  ///    'idle' means nothing is feeding it. The wake still happens; we just stop making the user
  ///    wait for its result before deciding.
  func probeSession() async -> Bool {
    let link = WatchLink.shared
    link.activate()
    link.ping()             // wakes a spectrum that was closed for power
    link.requestMissing()   // resume rows + reflush what is only sent on change

    let hasServer: Set<String> = ["live", "paused", "reconnecting"]
    let before = link.rowsPushed
    let clock = ContinuousClock()
    let start = clock.now
    while clock.now - start < .milliseconds(2500) {
      if link.rowsPushed > before { return true }        // definitive
      if let t = link.lastStateAt, Date().timeIntervalSince(t) < 3,
         hasServer.contains(link.why) { return true }    // the phone said so
      try? await Task.sleep(for: .milliseconds(100))
    }
    return false
  }

  /// Keep `phoneActive` live while a screen that offers the mode toggle is on show.
  ///
  /// The toggle must EXIST only when an iPhone is actually there (§"the mode toggle"), and that can
  /// change under us — the phone gets closed, or walks out of range in a pocket. So we re-ask on a
  /// slow cadence rather than trusting the launch answer forever. Same evidence as `decide()`: a
  /// reply, not a reachability flag.
  func startPolling() {
    guard poller == nil else { return }
    poller = Task { [weak self] in
      while !Task.isCancelled {
        guard let self else { return }
        let link = WatchLink.shared
        let fresh = [link.lastStateAt, link.lastRowAt].compactMap { $0 }
          .contains { Date().timeIntervalSince($0) < 8 }
        if self.phoneActive != fresh { self.phoneActive = fresh }
        link.ping()
        try? await Task.sleep(for: .seconds(3))
      }
    }
  }

  func stopPolling() {
    poller?.cancel()
    poller = nil
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

/// Which device the servers screen connects. Colour is the carrier of this distinction throughout
/// Jr — orange the watch, cyan the iPhone — so the launch chooser and the mode toggle agree without
/// anyone having to read the words twice.
enum PickerMode {
  case standalone, companion

  var title: String { self == .companion ? "Companion" : "Standalone" }
  var subtitle: String {
    self == .companion ? "servers connect your iPhone" : "servers connect this watch"
  }
  var tint: Color { self == .companion ? .cyan : .orange }
  var toggled: PickerMode { self == .companion ? .standalone : .companion }
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
