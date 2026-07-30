import Foundation
import AVFoundation
import WatchKit
import MediaPlayer

/// AUDIO ON THE WATCH ITSELF — speaker or paired headphones, with no phone involved.
///
/// This is the half of JR that cannot be inferred from the companion app, because in the
/// companion app the PHONE plays everything and the watch never touches audio at all.
///
/// watchOS is not iOS here:
///  - You cannot just `setActive(true)`. watchOS wants `activate(options:)` with a
///    completion, and it will REFUSE if there is no usable route — that refusal is a
///    real answer, not an error to paper over.
///  - `.longFormAudio` is the route-sharing policy that lets audio keep playing with the
///    wrist DOWN. Without it the watch is happy to stop the moment the screen sleeps,
///    which for a radio is the same as not working.
///  - The built-in SPEAKER is only a media route on the newer watches (Series 9+/Ultra).
///    On older ones this will land on Bluetooth or fail — which is precisely the device
///    split the JR brief predicted, and precisely what this spike exists to confirm.
final class WatchAudio {

  private let engine = AVAudioEngine()
  private let player = AVAudioPlayerNode()
  private var converter: AVAudioConverter?
  private var srcFormat: AVAudioFormat?
  /// THE OUTPUT FORMAT IS NOT A CONSTANT, and pretending it was is what killed the app.
  ///
  /// The converter was rebuilt only when the INPUT format changed. But the output changes
  /// too — the watch speaker is 48kHz mono, Bluetooth is 48kHz stereo, and the engine
  /// reconfigures itself whenever the route moves or an interruption ends. A converter still
  /// aimed at the old output, handed a buffer allocated in the new one, does not return an
  /// error: it traps.
  private var dstFormat: AVAudioFormat?
  private var started = false

  /// Seconds of audio scheduled but not yet played. Left unbounded, `scheduleBuffer`
  /// happily lets the queue grow after any delivery burst, and playback then runs
  /// permanently behind live — you hear the backlog, and tuning feels laggy. (Learned on
  /// the phone, 2026-06-11; the same trap is waiting here.)
  private var queuedSeconds: Double = 0
  /// Bumped by flush() (on tune). Buffer completions capture the generation at schedule time and
  /// skip their queuedSeconds decrement if a flush happened since — so discarded buffers can't
  /// corrupt the counter.
  private var flushGen = 0
  /// Feeds the player node silence whenever the stream is late, so the audio hardware
  /// never idles — see startSilenceKeeper(). Idle hardware = suspended app = dead socket.
  private var keeper: DispatchSourceTimer?
  /// One crumb per engine stop, not one per 100ms tick.
  private var loggedEngineStop = false
  /// ★★★ WAS THE SESSION STARTED IN THE FOREGROUND? Apple: "start the session while your app is
  /// running in the foreground" — an activation taken outside the foreground yields a session that
  /// PLAYS but carries no background grant, and the app is suspended a few seconds after the wrist
  /// drops. We activate deliberately early (watchOS refuses the very first activate() before the
  /// app is fully foreground, see activateAttempt), so the activation that finally succeeds is
  /// often taken while INACTIVE — and `restartAudio` only ever restarts the ENGINE, never the
  /// session, so nothing afterwards repaired it.
  ///
  /// ★★ Series 6, 2026-07-29: `activate ok=true` at 23:40:55 with the scene INACTIVE (it went
  /// inactive at :53, active at :58), then death 5s after a later inactive — Apple's documented
  /// "few seconds of background execution time" to the letter. Invisible on the Ultra.
  private var activatedInForeground = false
  /// Throttle for the "ran dry" crumb.
  private var lastDryCrumb: Double = 0
  /// THE CUSHION. There was only ever a CEILING here — drop packets if the queue grows too
  /// deep — and no FLOOR. So the queue sat wherever the network left it, which on a flaky
  /// tunnel is sometimes empty, and an empty queue on watchOS is fatal (see
  /// startSilenceKeeper: idle hardware -> suspended app -> dead socket).
  ///
  /// So hold a deliberate head start. Real audio then rides ON TOP of it, and a late packet
  /// eats into the cushion instead of starving the node — nothing is heard, nothing is
  /// suspended. The silence keeper stops being a thing that patches audible holes and
  /// becomes a thing that should essentially never fire.
  ///
  /// The price is latency, and we pay it DELIBERATELY. A second of cushion means a second
  /// between the air and your ear — but a watch radio is for listening on a walk, not for
  /// chasing a weak signal, and a stall costs far more than a second of delay ever will.
  /// (The phone runs ~0.25s on a LAN; the watch is talking over a tunnel to a server on the
  /// internet, from a wrist, over Bluetooth. It needs the room.)
  ///
  /// Tuning latency is the thing this costs, and it is recoverable later: flush the queue on
  /// retune and re-prime, so the dial stays instant while steady listening stays solid.
  ///
  /// LOWERED 0.55 -> 0.35 (2026-07-17). The cushion was sized big to survive "connection
  /// blips" on wrist-down — but that cause was WRONG: wrist-down drops were BEDTIME FOCUS,
  /// not the link. And a 20-min cellular drive (incl. a known mobile black spot) proved the
  /// link stable: a real dropout gave a little stutter and recovered. So the big cushion was
  /// paying a permanent latency tax against a threat the link shrugs off. Tune-to-audio lag
  /// was still noticeable at 0.55. This is a by-ear starting point; push lower (0.30/0.25) if
  /// it holds, back off if stutter on jitter gets annoying. Keep spectrumDelay coupled.
  private let targetQueued: Double = 0.35
  /// Below this we are about to run dry — top back up to the cushion. Kept ~0.2s under the
  /// target so a normal delivery gap doesn't trip a re-prime, but above the fatal-empty floor.
  private let floorQueued: Double = 0.15
  /// Above this, a delivery burst is just building latency for no benefit. Drop it.
  private let maxQueued: Double = 1.60

  private(set) var lastError: String = ""
  private(set) var route: String = "—"
  private(set) var packets = 0

  /// True once audio is genuinely running — i.e. the session activated AND the engine
  /// started. Anything less is a finding.
  private(set) var live = false

  /// ★★★ WHAT THE NOW PLAYING CARD SAYS. This was the STRING LITERAL "648 kHz · AM" — not stale
  /// metadata but a placeholder that had never once changed, and it read as correct only because
  /// 648 kHz AM is the app's DEFAULT tuning (UberClient: `frequency = 648_000  // Radio Caroline`).
  /// So the watch face confidently announced Radio Caroline while you listened to FM.
  ///
  /// It is a system surface — the Smart Stack, the Now Playing card, the lock screen — so it is
  /// arguably the most public text the app has, and nothing pointed at it because nothing in the
  /// app displays it.
  nonisolated(unsafe) private static var npTitle = ""

  /// Publish the real dial to the system. Cheap and idempotent: identical titles are dropped, so
  /// this can be called from a `didSet` on every tune without churning the card.
  static func setNowPlaying(freqHz: Double, mode: String) {
    guard freqHz > 0 else { return }
    let f: String = freqHz < 1_000_000
      ? String(format: "%.0f kHz", freqHz / 1_000)
      : String(format: "%@ MHz", String(format: "%.3f", freqHz / 1_000_000)
                                   .replacingOccurrences(of: "0+$", with: "", options: .regularExpression)
                                   .replacingOccurrences(of: "\\.$", with: "", options: .regularExpression))
    let title = mode.isEmpty ? f : "\(f) · \(mode.uppercased())"
    guard title != npTitle else { return }
    npTitle = title
    DispatchQueue.main.async {
      var info = MPNowPlayingInfoCenter.default().nowPlayingInfo ?? [:]
      guard !info.isEmpty else { return }   // not registered yet — start() will pick up npTitle
      info[MPMediaItemPropertyTitle] = title
      MPNowPlayingInfoCenter.default().nowPlayingInfo = info
    }
  }

  /// THE AUDIO THAT IS CURRENTLY PLAYING, for the scene crumb. Each backend client owns its own
  /// WatchAudio (UberClient, KiwiClient, OwrxClient, FmDxClient) and only one is ever live, so the
  /// alternative was threading a reference through four clients and the view purely so a log line
  /// could ask a question. Weak, so a torn-down client is not kept alive by a diagnostic.
  nonisolated(unsafe) private(set) static weak var current: WatchAudio?

  /// ★★★ WHAT THE AUDIO WAS DOING AT THE MOMENT THE WRIST DROPPED. The last-breath log's final
  /// line is `SCENE phase=inactive`, and until now that line said nothing about the one thing the
  /// suspension actually turns on: whether sound was still being produced. "It died a minute after
  /// wrist-down" and "it went silent, and a minute later watchOS reaped a silent app" are different
  /// bugs with the same last line. One string per scene change settles it.
  var stateLine: String {
    let out = AVAudioSession.sharedInstance().currentRoute.outputs
      .map { $0.portType.rawValue.replacingOccurrences(of: "AVAudioSessionPort", with: "") }
      .joined(separator: "+")
    return String(format: "live=%d eng=%d play=%d q=%.2fs route=[%@] pkts=%d",
                  live ? 1 : 0, engine.isRunning ? 1 : 0, player.isPlaying ? 1 : 0,
                  queuedSeconds, out.isEmpty ? "NONE" : out, packets)
  }

  /// ★★★ DID WE TAKE THE SESSION BEFORE THERE WAS ANYWHERE TO PLAY IT? Series 6, 2026-07-30:
  /// `activate ok=true route=[NONE]`, and the Bluetooth route only appeared a SECOND LATER. The
  /// retry ladder in activateAttempt() exists to wait for a usable route, but it only ever fires on
  /// `ok=false` — a success with an empty route sails straight through it. That matters because the
  /// long-form background grant is a property of a QUALIFYING ROUTE, and at the instant we asked
  /// there was no route to qualify.
  ///
  /// ★★ DIAGNOSTIC ONLY, and kept precisely because it disproved itself: the run that logged this
  /// went on to survive FIVE MINUTES backgrounded. So an activation with no route is NOT what ends
  /// the app, however plausible it looks, and nothing acts on this flag. Do not re-litigate it
  /// without a log line that contradicts 2026-07-30 09:43:36.
  private var activatedWithoutRoute = false

  /// Is the current OUTPUT route mono? The built-in watch speaker is 48k MONO; AirPods/BT are
  /// stereo. Drives Jr's audio request: on a mono route we ask VibeServer for a fullband mono Opus
  /// stream (all 64k in one channel) instead of receiving stereo and downmixing a band-limited pair.
  private(set) var outputIsMono = false
  /// Fires (on the main thread) when the route's mono-ness CHANGES — plug/unplug AirPods — so the
  /// client can re-request the matching channel count. Only the transition fires, so it's dedup'd.
  var onRouteChange: ((Bool) -> Void)?

  /// Re-read the output channel count and, if the mono/stereo-ness flipped, publish it. Called after
  /// the engine starts and on every route reconfigure (AVAudioEngineConfigurationChange).
  private func refreshRoute() {
    let ch = engine.outputNode.outputFormat(forBus: 0).channelCount
    guard ch > 0 else { return }                 // route not ready — don't guess
    let mono = ch == 1
    guard mono != outputIsMono else { return }
    outputIsMono = mono
    let cb = onRouteChange
    DispatchQueue.main.async { cb?(mono) }
  }

  /// Kept so cold-start recovery can re-run start() with the original callback (see hookColdStart()).
  private var startDone: ((Bool, String) -> Void)?
  private var lifecycleHooked = false

  func start(_ done: @escaping (Bool, String) -> Void) {
    startDone = done
    hookColdStart()
    let session = AVAudioSession.sharedInstance()
    do {
      // .longFormAudio, AND THE ROUTE PICKER IS THE PRICE OF IT.
      //
      // I removed this earlier because it made watchOS demand you choose an output — the
      // same thing the Music app does — and .default played happily through the speaker with
      // no fuss. But that fuss IS the entitlement: `.longFormAudio` is the policy that keeps
      // audio alive when the wrist drops and the screen sleeps. Without it watchOS suspends
      // the app and cuts the speaker the moment you look away, which for a radio is the same
      // as not working at all. The picker was not a bug to route around; it was the system
      // telling us the audio was now long-form.
      //
      // Two things this ALONE will not do, both outside the app:
      //   - WKBackgroundModes = [audio] in Info.plist (we have it — note the iOS spelling
      //     UIBackgroundModes is silently IGNORED on watchOS, which cost us an earlier round).
      //   - Settings › General › Return to Clock › <app> › Return to App, which the USER must
      //     turn on for the app to still be there when the wrist comes back up.
      //
      // And the cost is real: speaker playback burns roughly an hour of watch battery per ten
      // minutes of audio. A genuine design constraint for JR, not a footnote.
      try session.setCategory(.playback, mode: .default, policy: .longFormAudio, options: [])
    } catch {
      lastError = "setCategory: \(error.localizedDescription)"
      done(false, lastError)
      return
    }

    activateAttempt(session, attempt: 1, done: done)
  }

  /// Activate the audio session, RETRYING a transient first-connect refusal.
  ///
  /// ★ watchOS often refuses the VERY FIRST activate() — before the app is fully foreground — even
  /// on a speaker-capable watch. With no retry that left JR SILENT until a force-quit (a fresh
  /// launch activates cleanly, which is exactly what the force-quit did). A bounded backoff heals
  /// it. A genuinely route-less watch (older, no headphones) still fails ALL attempts and gives up
  /// gracefully — no spin. ~3 attempts over ~1.2 s.
  /// ★ COLD-START RECOVERY. If the very first activate() is refused for the WHOLE retry window
  /// (watchOS can hold it off for several seconds while the app is still settling on a fresh
  /// connect), start() fails and — because startEngine() never ran — NONE of the lifecycle
  /// observers exist. Audio then cannot heal on its own: it stays dead until a force-quit. That
  /// was the "ages with no audio, then it sprang to life when I touched the watch" symptom.
  /// Register ONE didBecomeActive hook here, independent of activation, that re-runs start()
  /// whenever audio isn't live — so a glance at the wrist revives it. Guarded to register once.
  private func hookColdStart() {
    guard !lifecycleHooked else { return }
    lifecycleHooked = true
    NotificationCenter.default.addObserver(
      forName: WKApplication.didBecomeActiveNotification, object: nil, queue: .main
    ) { [weak self] _ in
      guard let self else { return }
      // ★★★ BUILD 11 RE-ACTIVATED THE SESSION HERE, on the theory that a grant taken outside the
      //    foreground could be repaired afterwards. REMOVED — see the didBecomeActive handler in
      //    watchSession(), which carries the evidence. The theory was wrong twice over: the log
      //    shows the app surviving five minutes backgrounded on a grant taken with the scene
      //    inactive AND no route, so the premise does not hold; and re-activating a live session
      //    is now our leading suspect for destroying a grant that was working. Nothing re-takes
      //    the session after start any more.
      guard !self.live, let done = self.startDone else { return }
      Vitals.crumb("AUDIO cold-start recovery: not live on didBecomeActive — retrying start()")
      self.start(done)
    }
  }

  private func activateAttempt(_ session: AVAudioSession, attempt: Int, done: @escaping (Bool, String) -> Void) {
    // ★ Persist longer than the old 3/1.2s. A fresh VibeServer connect settles the app for a few
    // seconds and watchOS refuses activate() the whole time — too short a window left JR silent
    // until it happened to be re-kicked. 6 attempts with a rising backoff cover ~7.5s; a genuinely
    // route-less watch still exhausts them and reports the refusal, just a few seconds later.
    let maxAttempts = 6
    session.activate(options: []) { [weak self] ok, err in
      guard let self else { return }
      if ok {
        let fg = WKApplication.shared().applicationState == .active
        self.activatedInForeground = fg
        if !fg { Vitals.crumb("AUDIO activate succeeded but NOT foreground — no background grant yet") }
        // ★★★ ACTIVATED INTO THIN AIR. See activatedWithoutRoute: a success with an empty route is
        //   a grant taken against nothing, and the route-change handler has to re-take it.
        if AVAudioSession.sharedInstance().currentRoute.outputs.isEmpty {
          self.activatedWithoutRoute = true
          Vitals.crumb("AUDIO activate succeeded with NO ROUTE — will re-take the session when one arrives")
        }
      }

      // SAY EXACTLY WHAT WE GOT, because the difference between "audio is playing" and
      // "audio is playing WITH A BACKGROUND GRANT" is invisible until the wrist drops —
      // and then it is too late to ask. Apple's rules for a long-form route on watchOS:
      //
      //   * Bluetooth always qualifies.
      //   * The BUILT-IN SPEAKER qualifies too — but only on watchOS 11+, only on a
      //     speaker-capable watch, and **NOT WHILE THE WATCH IS CHARGING**. On the
      //     charger, long-form simply cannot use the speaker, so there is no background
      //     grant and the app is suspended the instant you look away. Nothing in the app
      //     can fix that, and you will chase it for hours if you do not log it.
      //   * On a speaker-capable watch with no Bluetooth around, the system routes to the
      //     speaker AUTOMATICALLY and shows NO PICKER. The absent picker is not a bug —
      //     it is the system telling you the speaker was accepted.
      let dev = WKInterfaceDevice.current()
      let route = AVAudioSession.sharedInstance().currentRoute.outputs
        .map { $0.portType.rawValue.replacingOccurrences(of: "AVAudioSessionPort", with: "") }
        .joined(separator: "+")
      Vitals.crumb(
        "AUDIO activate ok=\(ok) err=\(err?.localizedDescription ?? "-") "
        + "route=[\(route.isEmpty ? "NONE" : route)] "
        + "batt=\(Int(dev.batteryLevel * 100))% state=\(dev.batteryState.rawValue) "
        + "(0=unknown 1=unplugged 2=charging 3=full) model=\(dev.model) sys=\(dev.systemVersion)")

      guard ok else {
        // A first-connect refusal is usually transient — retry with a short backoff before
        // treating it as final. (On older watches with no headphones a refusal is the EXPECTED,
        // permanent answer — the retries simply exhaust and we report it, which is the whole
        // reason JR asks the device-class question.)
        if attempt < maxAttempts {
          Vitals.crumb("AUDIO activate refused (attempt \(attempt)/\(maxAttempts)) — retrying")
          let delay = 0.5 * Double(attempt)   // 0.5s … 2.5s (rising)
          DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
            self.activateAttempt(session, attempt: attempt + 1, done: done)
          }
          return
        }
        self.lastError = "activate refused: \(err?.localizedDescription ?? "no route")"
        DispatchQueue.main.async { done(false, self.lastError) }
        return
      }
      DispatchQueue.main.async {
        do {
          try self.startEngine()
          self.live = true
          WatchAudio.current = self
          // SAY WHERE IT WENT AND HOW LOUD. "Audio is arriving" and "audio is audible" are
          // different claims, and the gap between them is where an hour disappears: the
          // packets were decoding perfectly and being played into a route with no volume.
          let out = session.currentRoute.outputs.first
          let fmt = self.engine.outputNode.outputFormat(forBus: 0)
          self.route = String(
            format: "%@ · vol %.0f%% · %.0fHz/%dch",
            out?.portType.rawValue.replacingOccurrences(of: "AVAudioSessionPort", with: "") ?? "NO ROUTE",
            session.outputVolume * 100,
            fmt.sampleRate, Int(fmt.channelCount))
          done(true, self.route)
        } catch {
          self.lastError = "engine: \(error.localizedDescription)"
          done(false, self.lastError)
        }
      }
    }
  }

  /// WHY DID THE AUDIO STOP? Two completely different answers look identical on the wrist:
  /// watchOS SUSPENDED the app (background audio was never really granted), or the app stayed
  /// alive and the SESSION was torn down (an interruption, or a route change). The log's
  /// `gap=` column separates them — a big gap means we were suspended — and these events say
  /// which flavour of the second case it was. Guessing between them has cost enough.
  /// BE A MEDIA PLAYER, not just an app that makes noise.
  ///
  /// The audio FADED OUT gracefully on wrist-down and the AirPods then wandered off to
  /// another device — that is not a crash and not a dead socket, it is watchOS deliberately
  /// DEACTIVATING our session. It does that to an app it does not consider entitled to
  /// background audio, and the entitlement is not just a plist key: the system grants it to
  /// the NOW PLAYING app. We were shoving PCM into an engine and declaring a background mode,
  /// while never telling the system we were playing anything.
  ///
  /// So: publish now-playing info and handle the remote commands. This is also what makes the
  /// crown volume and the Control Centre transport work — JR needs it regardless.
  private func becomeNowPlaying() {
    let c = MPRemoteCommandCenter.shared()
    c.playCommand.isEnabled = true
    c.pauseCommand.isEnabled = true
    c.togglePlayPauseCommand.isEnabled = true
    // A live radio does not seek, and advertising commands we cannot honour is how you get a
    // transport UI that lies.
    c.nextTrackCommand.isEnabled = false
    c.previousTrackCommand.isEnabled = false
    c.changePlaybackPositionCommand.isEnabled = false

    c.playCommand.addTarget { [weak self] _ in
      // ★★★ Through the GUARDED path. `player.play()` on a non-running engine raises an
      // UNCATCHABLE Obj-C exception — see restartAudio. These remote-command handlers were
      // the two call sites that bypassed the guard, and the system can deliver a play
      // command exactly when the engine is down (wrist-down/up, route change, interruption).
      self?.restartAudio("remote play")
      return .success
    }
    c.pauseCommand.addTarget { [weak self] _ in
      self?.q.async { self?.player.pause() }
      return .success
    }
    c.togglePlayPauseCommand.addTarget { [weak self] _ in
      guard let self else { return .success }
      // ★ Pause is safe unguarded; PLAY is not — same uncatchable exception, so it goes
      //   through restartAudio, which starts the engine first and skips play if it cannot.
      if self.player.isPlaying { self.q.async { self.player.pause() } }
      else                     { self.restartAudio("remote toggle") }
      return .success
    }

    MPNowPlayingInfoCenter.default().nowPlayingInfo = [
      MPMediaItemPropertyTitle: WatchAudio.npTitle.isEmpty ? "VibeSDR Jr" : WatchAudio.npTitle,
      MPMediaItemPropertyArtist: "VibeSDR Jr",
      // LIVE. Not a track with a position — say so, or the system draws a scrubber for a
      // stream that cannot be scrubbed.
      MPNowPlayingInfoPropertyIsLiveStream: true,
      MPNowPlayingInfoPropertyPlaybackRate: 1.0,
    ]
    MPNowPlayingInfoCenter.default().playbackState = .playing
    Vitals.crumb("AUDIO now-playing registered")
  }

  private func watchSession() {
    let s = AVAudioSession.sharedInstance()
    NotificationCenter.default.addObserver(
      forName: AVAudioSession.interruptionNotification, object: s, queue: nil
    ) { [weak self] n in
      let t = (n.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt).flatMap(
        AVAudioSession.InterruptionType.init(rawValue:))
      Vitals.crumb("AUDIO interruption: \(t == .began ? "BEGAN" : "ended")")
      if t == .ended {
        // Come back. An interruption that ends and is never resumed is silence forever.
        try? s.setCategory(.playback, mode: .default, policy: .longFormAudio, options: [])
        s.activate(options: []) { ok, err in
          Vitals.crumb("AUDIO reactivate after interruption: ok=\(ok) err=\(err?.localizedDescription ?? "-")")
          // Reactivating the SESSION isn't enough — WATER LOCK (and other interruptions) STOP the engine,
          // so restart the engine + player or it stays silent (esp. on AirPods, where the user saw this).
          guard let self, ok else { return }
          self.restartAudio("interruption ended")
        }
      }
    }
    NotificationCenter.default.addObserver(
      forName: AVAudioSession.routeChangeNotification, object: s, queue: nil
    ) { [weak self] n in
      let r = (n.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt) ?? 0
      let out = s.currentRoute.outputs.first?.portType.rawValue ?? "NONE"
      Vitals.crumb("AUDIO route change (reason \(r)) → \(out)")
      guard let self else { return }

      // ★★★ THIS HANDLER USED TO ONLY LOG. It now restarts the ENGINE — and deliberately does NOT
      //    re-take the SESSION, though an earlier draft of this change did.
      //
      //    That draft was built on `activate ok=true route=[NONE]`: we take the grant before any
      //    output exists (the Series 6 has no speaker media route, so Bluetooth is the only
      //    candidate and it attaches a second late), so re-taking it once a real route arrived
      //    looked obviously right. The log refuted it — the run that activated with NO ROUTE went
      //    on to survive FIVE MINUTES in the background. An empty route at activation is simply
      //    not the thing that kills us, and adding a third re-activation would have worked against
      //    the actual finding. Left here as a warning: this one is very convincing and it is wrong.
      //
      // RESTART THE ENGINE IF THE ROUTE CHANGE KILLED IT. A stopped engine means idle hardware,
      //    and idle hardware is precisely what gets the app suspended when the wrist drops — the
      //    silence keeper deliberately leaves a stopped engine alone, because it cannot tell this
      //    apart from the intentional watch-face stop. Here we CAN tell: a route change is not the
      //    user leaving. Guarded on `live` so we never start an engine we never started.
      if self.live { self.restartAudio("route change") }
    }
    // BACK TO THE FOREGROUND. A wrist-flick / dismiss backgrounds the app and can stop the engine
    // (audio then dead until a force-quit). Restart the engine + player whenever the app becomes active
    // again — global, so it covers every screen (waterfall, DAB, ADS-B), not just ContentView.
    NotificationCenter.default.addObserver(
      forName: WKApplication.didBecomeActiveNotification, object: nil, queue: .main
    ) { [weak self] _ in
      guard let self else { return }
      // ★★★ DO NOT RE-ACTIVATE THE SESSION HERE. There used to be a setCategory() + activate() on
      //    this line, on every single foreground, with the result discarded. It is the best
      //    candidate we have ever had for the random quits, and the Series 6 log of 2026-07-30 is
      //    why:
      //
      //      09:45:03  SCENE phase=inactive     ← wrist down
      //      09:46:25  keeper: ran dry          ← ALIVE, 82s into the background
      //      09:50:08  SCENE phase=active       ← wrist up, FIVE MINUTES later. Survived.
      //      09:50:13  SCENE phase=inactive     ← wrist down again. Dead in 5 seconds.
      //
      //    ★★★ It did NOT die after a minute of wrist-down. It survived the FIRST backgrounding
      //    for five minutes and died five seconds after the SECOND — i.e. after a round trip
      //    through the foreground. The background grant plainly WORKS; something in that round
      //    trip destroys it. This was the only thing in the round trip that touched the session,
      //    and re-activating a session that is already active and playing is exactly how you would
      //    lose a grant you already hold.
      //
      //    It also explains why this looked random and timing-related for months: it depends on
      //    how often you GLANCE at the watch, not on how long the wrist is down.
      //
      //    The engine still needs restarting on the way back — a wrist-flick can stop it — and
      //    restartAudio does that without going near the session. Interruption-ended still
      //    re-activates, which is legitimate: there the system took the session away from us.
      self.restartAudio("didBecomeActive")
    }
  }

  /// The ONE guarded path back to sound after an interruption / Water Lock / route change / foreground.
  /// Serialised on `q` so the interruption-ended and didBecomeActive handlers (which both fire on Water
  /// Lock exit) can't race a double engine.start()/play(). CRUCIALLY: `player.play()` raises an
  /// UNCATCHABLE Obj-C exception if the engine isn't running — so only play once the engine is CONFIRMED
  /// running. That is the Water-Lock-exit-on-speaker crash: the eject tone held the session, engine.start()
  /// quietly failed, and play() on the dead engine took the app down. Guard it and a later notification retries.
  private func restartAudio(_ why: String) {
    q.async { [weak self] in
      guard let self, self.started else { return }
      if !self.engine.isRunning { try? self.engine.start() }
      guard self.engine.isRunning else {
        Vitals.crumb("AUDIO restart (\(why)): engine NOT running — skip play, will retry")
        return
      }
      if !self.player.isPlaying { self.player.play() }
      // The config-change handler cancels the keeper before its surgery and, if engine.start() failed
      // there, bailed before re-arming it. Now that we ARE running again, re-arm a dead keeper (only if
      // nil, so we don't re-prime a silence burst on every didBecomeActive).
      if self.keeper == nil { self.startSilenceKeeper() }
      Vitals.crumb("AUDIO restart (\(why)): running=\(self.engine.isRunning) playing=\(self.player.isPlaying)")
    }
  }

  private func startEngine() throws {
    guard !started else { return }
    engine.attach(player)
    // Connect with the OUTPUT's own format and let AVAudioConverter do the resampling —
    // UberSDR's sample rate is whatever the server feels like (it changes with the demod),
    // and fighting the engine over formats is how you get silence.
    let out = engine.outputNode.outputFormat(forBus: 0)
    engine.connect(player, to: engine.mainMixerNode, format: out)

    // ── THIS IS THE WRIST-DOWN BUG, AND YOU CANNOT SWITCH IT OFF. ─────────────────
    //
    // AVAudioEngine auto-shutdown: "when the Engine detects it's running idle for a
    // certain duration, it stops the audio hardware" — and Apple built it FOR watchOS:
    // "on watchOS especially, not all apps properly pause or stop the Engine"
    // (WWDC 2017 §501).
    //
    // On iOS you would set `isAutoShutdownEnabled = false` and be done. On watchOS that
    // property is UNAVAILABLE — the compiler refuses it. Auto-shutdown is mandatory here.
    // Which is the whole bug:
    //
    //   one late Opus packet -> the player node has nothing to render
    //     -> the engine idles and powers the audio hardware DOWN
    //       -> watchOS no longer observes audio rendering. And the background grant is
    //          NOT the Info.plist key — it is a RUNTIME state, held only while the system
    //          can see audio actually flowing.
    //         -> the assertion evaporates -> the app is SUSPENDED
    //           -> the socket dies with it -> the packet we were waiting for can never
    //              arrive. The failure feeds itself, which is exactly why this looked like
    //              a hard platform ban rather than a starved buffer.
    //
    // It even explains the shape of the symptom: a FADE (the hardware draining its last
    // buffer) and only THEN the suspension, ~14s later.
    //
    // Everything else was right all along — WKBackgroundModes=[audio], .longFormAudio, the
    // Bluetooth route, Now Playing — and all of it was useless while the engine was free to
    // switch the hardware off underneath it.
    //
    // Since we cannot stop the engine idling, WE MUST NEVER BE IDLE. See
    // startSilenceKeeper(): the moment the queue runs dry, feed it silence. That is not a
    // workaround for a missing flag; on watchOS it IS the mechanism.

    engine.prepare()
    try engine.start()
    player.play()
    started = true
    startSilenceKeeper()
    refreshRoute()          // publish the initial route (mono speaker vs stereo AirPods)
    watchSession()
    becomeNowPlaying()
    // Keep the app FRONTMOST longer on wrist-down — extends the default ~2 min return-to-clock timeout to
    // ~8 min WITHOUT the user changing the OS "Return to App" setting. Not a full replacement (their
    // setting keeps it up indefinitely), but it makes wrist-down-and-resume work out of the box.
    DispatchQueue.main.async { WKExtension.shared().isFrontmostTimeoutExtended = true }
    Vitals.crumb("AUDIO engine started · out=\(out) · silence-keeper ON")

    // WHEN THE ENGINE RECONFIGURES, THE GRAPH IS ALREADY TORN DOWN. AVAudioEngine posts this
    // on a route change (speaker → Bluetooth, headphones pulled) and every cached format,
    // converter and connection is stale from that moment. Rebuild rather than play on into a
    // graph that no longer exists.
    NotificationCenter.default.addObserver(
      forName: .AVAudioEngineConfigurationChange, object: engine, queue: nil
    ) { [weak self] _ in
      guard let self else { return }
      self.q.async {
        // STOP THE KEEPER FIRST. It fires every 100ms on this same queue, and the graph is
        // about to be torn down and rebuilt — a buffer scheduled mid-surgery is a trap, not
        // an error. (startSilenceKeeper() below restarts it and re-primes the cushion.)
        self.keeper?.cancel()
        self.keeper = nil

        self.converter = nil
        self.srcFormat = nil
        self.dstFormat = nil
        self.queuedSeconds = 0
        let out = self.engine.outputNode.outputFormat(forBus: 0)
        guard out.sampleRate > 0, out.channelCount > 0 else { return }
        self.engine.connect(self.player, to: self.engine.mainMixerNode, format: out)
        if !self.engine.isRunning { try? self.engine.start() }
        // GUARD THE PLAY. This is the WATER-LOCK-EXIT crash path: the eject tone triggers a config
        // change here, but it also briefly holds the session, so engine.start() above fails and
        // player.play() on a dead engine is an UNCATCHABLE Obj-C exception → app gone. Only play once
        // the engine is confirmed running; the didBecomeActive/interruption handlers retry otherwise.
        guard self.engine.isRunning else {
          Vitals.crumb("AUDIO config-change: engine NOT running — skip play, will retry")
          return
        }
        self.player.play()
        self.started = true
        // A route change zeroed the queue above — so the cushion is gone, and an empty node
        // is exactly what lets the engine shut the hardware down. Restart the keeper against
        // the NEW format (it re-primes the cushion as it starts).
        self.startSilenceKeeper()
        self.refreshRoute()     // route may have flipped mono↔stereo — re-request channels if so
      }
    }
  }

  /// NEVER LET THE PLAYER NODE RUN DRY.
  ///
  /// Turning off auto-shutdown stops the engine powering the hardware down on its own, but
  /// an `AVAudioPlayerNode` with nothing scheduled still renders nothing — and "nothing
  /// rendering" is precisely what watchOS reads as "not actively streaming audio". Over a
  /// radio link a late packet is not an edge case, it is Tuesday: one stall on a flaky
  /// tunnel and the background grant is gone, and with it the socket that would have
  /// delivered the very packet we were waiting for. The failure feeds itself.
  ///
  /// So when the queue runs empty, feed the node SILENCE. It costs nothing, it is
  /// inaudible, and it keeps the hardware rendering — which keeps us alive to receive the
  /// audio that is merely late rather than gone. Silence is only ever scheduled while
  /// genuinely starved, so it adds no latency to a healthy stream.
  private func startSilenceKeeper() {
    keeper?.cancel()
    // Prime the cushion up front. This is the "wait a moment on connect for a stable stream"
    // trade, and it is silence rather than a stall — the hardware starts rendering
    // IMMEDIATELY, so the background grant exists from the first instant, before a single
    // Opus packet has arrived.
    q.async { [weak self] in self?.topUp(to: self?.targetQueued ?? 1.0) }

    let t = DispatchSource.makeTimerSource(queue: q)
    t.schedule(deadline: .now() + 0.1, repeating: 0.1)
    t.setEventHandler { [weak self] in
      // If the engine stopped (flick to the watch face / crown-home), LEAVE it stopped — letting the app
      // suspend saves battery, and didBecomeActive restarts audio on return. Wrist-down (screen off) keeps
      // the app frontmost so audio plays on; that's the case that matters. Chosen behaviour, not a bug.
      guard let self, self.started else { return }
      // ★★★ SAY WHICH IT WAS. This guard covers TWO situations that look identical from here and
      //    need opposite responses: the deliberate watch-face stop (fine, didBecomeActive brings
      //    it back) and the engine dying under us while we still expect to be playing (fatal —
      //    idle hardware means the app is suspended a few seconds later, which is Apple's
      //    documented grace period and exactly the ~5s the Series 6 dies in). Neither was ever
      //    written down, so the last-breath log went quiet at the one moment that mattered.
      if !self.engine.isRunning {
        if !self.loggedEngineStop { self.loggedEngineStop = true; Vitals.crumb("AUDIO keeper: engine STOPPED (queued=\(String(format: "%.2f", self.queuedSeconds))s)") }
        return
      }
      self.loggedEngineStop = false
      guard self.queuedSeconds < self.floorQueued else { return }   // healthy — leave it alone
      // ★ RAN DRY. The cushion is meant to make this essentially never fire, so every occurrence
      //   is evidence — throttled to one a second so a sustained stall leaves a trail, not a flood.
      let now = Date().timeIntervalSinceReferenceDate
      if now - self.lastDryCrumb > 1 {
        self.lastDryCrumb = now
        Vitals.crumb("AUDIO keeper: ran dry, queued=\(String(format: "%.2f", self.queuedSeconds))s → topping up")
      }
      // Running dry. Rebuild the whole cushion, not a token 50 ms: if the stream has stalled
      // we want to survive the WHOLE stall, and if it is merely late we want the head start
      // back so the next hiccup is absorbed too.
      self.topUp(to: self.targetQueued)
    }
    t.resume()
    keeper = t
  }

  /// ONE-SHOT extra cushion, for the wrist-up spectrum reconnect. The reopened spectrum socket dumps
  /// a burst that briefly hogs the (weak) link and starves audio → a stutter. Topping the buffer up
  /// once here gives audio the head start to ride that burst out. ★ This does NOT change targetQueued
  /// or floorQueued, so it adds ZERO latency to the TUNE path — a tune flush()es to floorQueued and
  /// rebuilds to the normal target regardless. The only cost is a touch more listening latency between
  /// wrist-up and the next tune, which self-clears on that tune. Keep `seconds` modest (snappy > deep).
  func primeCushion(_ seconds: Double) {
    q.async { [weak self] in
      guard let self, self.started, self.engine.isRunning else { return }
      self.topUp(to: min(seconds, self.maxQueued))
    }
  }

  /// Schedule silence until the queue holds `seconds`. Caller must be on `q`.
  ///
  /// NEVER USE A CACHED FORMAT HERE. `dstFormat` is the converter's destination, captured
  /// when the converter was last built — and the output format is NOT a constant. Switch
  /// AirPods -> speaker and the engine reconfigures underneath us: Bluetooth is 48k STEREO,
  /// the built-in speaker is 48k MONO. A keeper firing every 100ms would then hand a stereo
  /// buffer to a node that is now mono, and `scheduleBuffer` does not return an error for
  /// that — IT TRAPS. (It killed the app on a manual route switch. Same trap the audio path
  /// already learned once; I reintroduced it here.)
  ///
  /// So ask the PLAYER what it is actually connected with, every single time.
  private func topUp(to seconds: Double) {
    guard engine.isRunning else { return }
    let fmt = player.outputFormat(forBus: 0)
    guard fmt.sampleRate > 0, fmt.channelCount > 0 else { return }
    while queuedSeconds < seconds {
      let want = min(0.1, seconds - queuedSeconds)             // 100 ms chunks
      let frames = AVAudioFrameCount(fmt.sampleRate * want)
      guard frames > 0, let buf = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: frames)
      else { return }
      buf.frameLength = frames
      // AVAudioPCMBuffer does NOT zero its storage — silence has to be written, or you
      // schedule whatever was in that memory. (Which is noise, and loud.)
      if let ch = buf.floatChannelData {
        for c in 0..<Int(fmt.channelCount) { ch[c].update(repeating: 0, count: Int(frames)) }
      }
      let dur = Double(frames) / fmt.sampleRate
      queuedSeconds += dur
      let gen = flushGen
      player.scheduleBuffer(buf) { [weak self] in
        self?.q.async { guard self?.flushGen == gen else { return }; self?.queuedSeconds -= dur }
      }
    }
  }

  func stop() {
    keeper?.cancel()
    keeper = nil
    player.stop()
    engine.stop()
    started = false
    live = false
    try? AVAudioSession.sharedInstance().setActive(false)
  }

  /// Everything below runs on ONE queue.
  ///
  /// `play` is called from the WebSocket's thread ~50 times a second, and the completion
  /// handler that decrements `queuedSeconds` runs on the AUDIO thread. Two threads, one
  /// counter, no lock — that is a data race, and it is the kind that corrupts rather than
  /// merely miscounts. AVAudioEngine's graph mutation is not thread-safe either.
  private let q = DispatchQueue(label: "wristsdr.audio")

  /// App output gain, 0…1. watchOS exposes NO API to set the SYSTEM volume slider, so on the
  /// standalone watch "volume" means the engine's master output gain — which genuinely changes
  /// how loud the app plays (there's no phone here to defer real volume to, unlike the
  /// companion). Set on the audio queue alongside the other node ops.
  func setVolume(_ v: Float) {
    let g = max(0, min(1, v))
    q.async { [weak self] in self?.engine.mainMixerNode.outputVolume = g }
  }

  /// Feed one decoded packet. Interleaved Int16 at the server's rate.
  func play(pcm: [Int16], rate: Int32, channels: Int32) {
    q.async { [weak self] in self?.playLocked(pcm: pcm, rate: rate, channels: channels) }
  }

  private func playLocked(pcm: [Int16], rate: Int32, channels: Int32) {
    guard started, !pcm.isEmpty else { return }
    packets += 1

    // Drop rather than drift. If we are already behind, playing this makes it worse: the
    // listener would be hearing the past, and every tune would feel a second late.
    if queuedSeconds > maxQueued { return }

    // Build against the PLAYER'S OWN connected format, NOT the engine output node's. On a route
    // change (WATER LOCK exit, speaker↔Bluetooth) the output node can already report the NEW format
    // (e.g. speaker 48k MONO) while the player is still connected with the OLD one (48k STEREO) — and
    // `player.scheduleBuffer` TRAPS (an UNCATCHABLE Obj-C exception, SIGABRT) when the buffer's format
    // doesn't match the node's connection format. That is the Water-Lock-exit crash (WatchAudio.swift
    // :558 in the report). The player's connection format is precisely what scheduleBuffer accepts, so
    // convert to THAT and the two can never disagree. The reconnect on a config change updates it.
    let outFmt = player.outputFormat(forBus: 0)

    // GUARD THE OUTPUT FORMAT. If the route is not ready (or the player isn't connected yet),
    // `outputFormat(forBus:)` hands back 0 Hz / 0 channels — and `AVAudioPCMBuffer(pcmFormat:...)`
    // TRAPS on a zero-channel format. Skip the frame; the next one (post-reconnect) will be valid.
    guard outFmt.sampleRate > 0, outFmt.channelCount > 0 else {
      lastError = "output format not ready (\(outFmt.sampleRate)Hz/\(outFmt.channelCount)ch)"
      return
    }

    // STEREO → MONO SPEAKER: DOWNMIX IN SOFTWARE. AVAudioConverter simply does NOT fold stereo down to
    // the mono built-in speaker on watchOS — it returns pure silence (no error, no layout helps: stereo
    // →stereo on AirPods played fine, stereo→mono speaker stayed dead). So when the output is mono and
    // the source is stereo, average L/R ourselves and feed the proven mono→mono path (same as OWRX). On
    // a stereo output (AirPods) we leave it stereo and let the converter pass it through.
    var pcm = pcm
    var channels = channels
    if channels == 2, outFmt.channelCount == 1 {
      var mono = [Int16](); mono.reserveCapacity(pcm.count / 2)
      var i = 0
      while i + 1 < pcm.count { mono.append(Int16((Int(pcm[i]) + Int(pcm[i + 1])) / 2)); i += 2 }
      pcm = mono
      channels = 1
    }

    // STEREO NEEDS AN EXPLICIT LAYOUT (AirPods path). A bare 2-channel AVAudioFormat has no channel
    // layout; tag stereo so the converter treats L/R correctly on a stereo output.
    let inFmt: AVAudioFormat?
    if channels == 2, let layout = AVAudioChannelLayout(layoutTag: kAudioChannelLayoutTag_Stereo) {
      inFmt = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: Double(rate),
                            interleaved: true, channelLayout: layout)
    } else {
      inFmt = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: Double(rate),
                            channels: AVAudioChannelCount(channels), interleaved: true)
    }
    guard let inFmt else { return }

    if srcFormat != inFmt || dstFormat != outFmt {
      srcFormat = inFmt
      dstFormat = outFmt
      converter = AVAudioConverter(from: inFmt, to: outFmt)
    }
    guard let conv = converter else { return }

    // The engine can be stopped out from under us by an interruption or a route change.
    // Scheduling into a dead player is the other way this crashes.
    guard engine.isRunning else {
      started = false
      return
    }

    let frames = AVAudioFrameCount(pcm.count / Int(channels))
    guard let inBuf = AVAudioPCMBuffer(pcmFormat: inFmt, frameCapacity: frames) else { return }
    inBuf.frameLength = frames
    pcm.withUnsafeBufferPointer { src in
      if let dst = inBuf.int16ChannelData?[0] {
        dst.update(from: src.baseAddress!, count: pcm.count)
      }
    }

    let ratio = outFmt.sampleRate / Double(rate)
    let outCap = AVAudioFrameCount(Double(frames) * ratio + 512)
    guard let outBuf = AVAudioPCMBuffer(pcmFormat: outFmt, frameCapacity: outCap) else { return }

    var err: NSError?
    var supplied = false
    conv.convert(to: outBuf, error: &err) { _, status in
      if supplied { status.pointee = .noDataNow; return nil }
      supplied = true
      status.pointee = .haveData
      return inBuf
    }
    if err != nil || outBuf.frameLength == 0 { return }

    let dur = Double(outBuf.frameLength) / outFmt.sampleRate
    queuedSeconds += dur
    let gen = flushGen
    player.scheduleBuffer(outBuf) { [weak self] in
      // BACK ONTO THE QUEUE. This completion fires on the AUDIO thread, and it was
      // decrementing a counter that `play` increments on the WebSocket thread — one
      // counter, two threads, no lock. Moving `play` onto a serial queue and leaving its
      // completion off it fixed nothing at all.
      // `gen` guard: a flush() (tune) stops the player, which fires these completions for
      // buffers we've already discarded — without the guard they'd drive queuedSeconds
      // negative and the cushion logic would over-fill.
      self?.q.async { guard self?.flushGen == gen else { return }; self?.queuedSeconds -= dur }
    }
  }

  /// Drop everything currently queued and restart clean — called on TUNE so the listener stops
  /// hearing the OLD frequency play out of the cushion (the ~queue-depth of tune latency). The
  /// keeper refills a small silence floor immediately so the node never starves.
  func flush() {
    q.async { [weak self] in
      guard let self, self.started, self.engine.isRunning else { return }
      self.flushGen &+= 1            // stale completions from the stopped buffers become no-ops
      self.player.stop()            // discards all scheduled (old-frequency) buffers
      self.queuedSeconds = 0
      self.player.play()            // ready for the new-frequency packets
      self.topUp(to: self.floorQueued)
    }
  }
}
