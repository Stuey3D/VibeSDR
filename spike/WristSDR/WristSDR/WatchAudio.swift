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
  private let maxQueued: Double = 0.6

  private(set) var lastError: String = ""
  private(set) var route: String = "—"
  private(set) var packets = 0

  /// True once audio is genuinely running — i.e. the session activated AND the engine
  /// started. Anything less is a finding.
  private(set) var live = false

  func start(_ done: @escaping (Bool, String) -> Void) {
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

    session.activate(options: []) { [weak self] ok, err in
      guard let self else { return }
      guard ok else {
        // watchOS says no route. On older watches with no headphones connected this is
        // the EXPECTED answer, and it is the whole reason JR has a device-class question.
        self.lastError = "activate refused: \(err?.localizedDescription ?? "no route")"
        DispatchQueue.main.async { done(false, self.lastError) }
        return
      }
      DispatchQueue.main.async {
        do {
          try self.startEngine()
          self.live = true
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
      self?.q.async { self?.player.play() }
      return .success
    }
    c.pauseCommand.addTarget { [weak self] _ in
      self?.q.async { self?.player.pause() }
      return .success
    }
    c.togglePlayPauseCommand.addTarget { [weak self] _ in
      self?.q.async {
        guard let self else { return }
        self.player.isPlaying ? self.player.pause() : self.player.play()
      }
      return .success
    }

    MPNowPlayingInfoCenter.default().nowPlayingInfo = [
      MPMediaItemPropertyTitle: "648 kHz · AM",
      MPMediaItemPropertyArtist: "WristSDR",
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
    ) { n in
      let t = (n.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt).flatMap(
        AVAudioSession.InterruptionType.init(rawValue:))
      Vitals.crumb("AUDIO interruption: \(t == .began ? "BEGAN" : "ended")")
      if t == .ended {
        // Come back. An interruption that ends and is never resumed is silence forever.
        try? s.setCategory(.playback, mode: .default, policy: .longFormAudio, options: [])
        s.activate(options: []) { ok, err in
          Vitals.crumb("AUDIO reactivate after interruption: ok=\(ok) err=\(err?.localizedDescription ?? "-")")
        }
      }
    }
    NotificationCenter.default.addObserver(
      forName: AVAudioSession.routeChangeNotification, object: s, queue: nil
    ) { n in
      let r = (n.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt) ?? 0
      let out = s.currentRoute.outputs.first?.portType.rawValue ?? "NONE"
      Vitals.crumb("AUDIO route change (reason \(r)) → \(out)")
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
    engine.prepare()
    try engine.start()
    player.play()
    started = true
    watchSession()
    becomeNowPlaying()
    Vitals.crumb("AUDIO engine started · out=\(out)")

    // WHEN THE ENGINE RECONFIGURES, THE GRAPH IS ALREADY TORN DOWN. AVAudioEngine posts this
    // on a route change (speaker → Bluetooth, headphones pulled) and every cached format,
    // converter and connection is stale from that moment. Rebuild rather than play on into a
    // graph that no longer exists.
    NotificationCenter.default.addObserver(
      forName: .AVAudioEngineConfigurationChange, object: engine, queue: nil
    ) { [weak self] _ in
      guard let self else { return }
      self.q.async {
        self.converter = nil
        self.srcFormat = nil
        self.dstFormat = nil
        self.queuedSeconds = 0
        let out = self.engine.outputNode.outputFormat(forBus: 0)
        guard out.sampleRate > 0, out.channelCount > 0 else { return }
        self.engine.connect(self.player, to: self.engine.mainMixerNode, format: out)
        if !self.engine.isRunning { try? self.engine.start() }
        self.player.play()
        self.started = true
      }
    }
  }

  func stop() {
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

    let outFmt = engine.outputNode.outputFormat(forBus: 0)

    // GUARD THE OUTPUT FORMAT. If the route is not ready, `outputFormat(forBus:)` hands
    // back 0 Hz / 0 channels — and `AVAudioPCMBuffer(pcmFormat:frameCapacity:)` TRAPS on a
    // zero-channel format. That is the crash: audio starts, the first frames arrive, and
    // the app dies. On relaunch the audio never starts at all, so nothing crashes and the
    // waterfall runs perfectly — which is exactly the pattern that made it look like a
    // SPECTRUM bug. It was never the spectrum.
    guard outFmt.sampleRate > 0, outFmt.channelCount > 0 else {
      lastError = "output format not ready (\(outFmt.sampleRate)Hz/\(outFmt.channelCount)ch)"
      return
    }

    guard let inFmt = AVAudioFormat(commonFormat: .pcmFormatInt16,
                                    sampleRate: Double(rate),
                                    channels: AVAudioChannelCount(channels),
                                    interleaved: true) else { return }

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
    player.scheduleBuffer(outBuf) { [weak self] in
      // BACK ONTO THE QUEUE. This completion fires on the AUDIO thread, and it was
      // decrementing a counter that `play` increments on the WebSocket thread — one
      // counter, two threads, no lock. Moving `play` onto a serial queue and leaving its
      // completion off it fixed nothing at all.
      self?.q.async { self?.queuedSeconds -= dur }
    }
  }
}
