import AVFoundation

private var _audioEngine:  AVAudioEngine?
private var _playerNode:   AVAudioPlayerNode?
private var _silentBuffer: AVAudioPCMBuffer?

func vibeStartSilentAudio() {
  _setupObservers()
  _startEngine()
}

/* ★★★ THE HOLD HAS TO START BEFORE JAVASCRIPT DOES — AND TWO OWNERS NOW ASK FOR IT.
 *
 *  Everything that used to start this loop ran downstream of the RN bridge mounting: the watch's
 *  command is queued in VibeWatchModule until `hasListeners`, JS then routes it to
 *  watchProvider.noteWatchAwake(), and only THEN do we produce audio. The entire cold launch —
 *  bundle load, mount, subscribe — happens with nothing playing, which is precisely the window
 *  iOS suspends us in. So the first watch-driven connect after a cold boot dies part-way and the
 *  second works, because by then the app is warm. That is the symptom exactly.
 *  ★★ SO THE WAKE ITSELF TAKES A HOLD, in didReceiveMessage, before anything is parsed.
 *  ★★ WHICH MEANS TWO OWNERS, and a naked start/stop pair cannot serve both: JS releasing its
 *     hold at the end of a connect must not silence a loop the wake is still holding, and the
 *     wake's own deadline must not silence one JS is holding. Reasons, not a boolean — the same
 *     shape watchProvider already uses on its side for the same reason.
 *  ★ Idempotent by construction: a second hold from a reason already held changes nothing, and
 *    _startEngine() refuses to build a second engine over a running one (it used to leak one). */
private var _holds = Set<String>()

func vibeHoldSilentAudio(_ reason: String, _ on: Bool) {
  let work = {
    let had = !_holds.isEmpty
    if on { _holds.insert(reason) } else { _holds.remove(reason) }
    let want = !_holds.isEmpty
    guard want != had else { return }
    if want { vibeStartSilentAudio() } else { vibeStopSilentAudio() }
  }
  if Thread.isMainThread { work() } else { DispatchQueue.main.async(execute: work) }
}

/* ★★★ AND A DEADLINE ON THE WAKE HOLD, because nothing else will ever release it.
 *   The wake hold is taken by a delegate callback that has no idea whether a connect follows; if
 *   JS never mounts (the user never asked for anything, the bundle fails) there is no code path
 *   left to turn it off, and an inaudible loop held for ever is the battery cost and the Now
 *   Playing hijack this file's stop() was written to prevent.
 * ★★ REFRESHED, NOT STACKED: every further watch message pushes the deadline out, so an active
 *   Buddy keeps a live phone underneath it and an abandoned wake lapses. Bounds INACTIVITY.
 * ★ 45 s is comfortably longer than a cold launch plus a connect, and JS's own `watch` hold
 *   (renewed by Buddy's 4 s ping) takes over long before it expires. */
private var _wakeTimer: Timer?

func vibeWakeHoldSilentAudio() {
  let work = {
    _wakeTimer?.invalidate()
    _wakeTimer = Timer.scheduledTimer(withTimeInterval: 45, repeats: false) { _ in
      _wakeTimer = nil
      vibeHoldSilentAudio("wake", false)
    }
    vibeHoldSilentAudio("wake", true)
  }
  if Thread.isMainThread { work() } else { DispatchQueue.main.async(execute: work) }
}

private func _startEngine() {
  // ★ NEVER BUILD A SECOND ENGINE OVER A RUNNING ONE. With one caller this was merely a leak;
  //   with the wake hold and the JS hold both able to start it, the second engine would orphan
  //   the first — still attached to the session, still playing, and no longer stoppable because
  //   vibeStopSilentAudio() only knows about the reference it just overwrote.
  if _audioEngine?.isRunning == true { _playerNode?.play(); return }
  let engine = AVAudioEngine()
  let player = AVAudioPlayerNode()
  engine.attach(player)

  let format = AVAudioFormat(standardFormatWithSampleRate: 44100, channels: 1)!
  engine.connect(player, to: engine.mainMixerNode, format: format)
  engine.mainMixerNode.outputVolume = 0.01  // non-zero so iOS counts it as audio

  if _silentBuffer == nil {
    let buf = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: 44100)!
    buf.frameLength = 44100
    _silentBuffer = buf
  }

  do {
    try AVAudioSession.sharedInstance().setActive(true)
    try engine.start()
    player.scheduleBuffer(_silentBuffer!, at: nil, options: .loops, completionHandler: nil)
    player.play()
    _audioEngine = engine
    _playerNode  = player
  } catch {
    _audioEngine = nil
    _playerNode  = nil
  }
}

/* ★★★ AND A WAY TO PUT IT DOWN. This file only ever had a start — which is half a lifecycle, and
 *   the wrong half to be missing: an inaudible loop held for ever keeps the audio focus, parks
 *   VibeSDR in Now Playing over whatever the user was actually listening to, and costs battery for
 *   a job that finished minutes ago. The caller starts it for the length of a connect and stops it
 *   the moment the real audio takes over (or the connect fails).
 * ★ The session is NOT deactivated: AppDelegate makes it active at launch so we appear in Now
 *   Playing at all, and tearing that down here would be reaching outside this file's business. */
func vibeStopSilentAudio() {
  _wakeTimer?.invalidate()
  _wakeTimer = nil
  _holds.removeAll()
  _playerNode?.stop()
  _audioEngine?.stop()
  _playerNode  = nil
  _audioEngine = nil
}

private var _observersAdded = false
private func _setupObservers() {
  guard !_observersAdded else { return }
  _observersAdded = true

  NotificationCenter.default.addObserver(
    forName: AVAudioSession.interruptionNotification, object: nil, queue: .main
  ) { note in
    guard let type = (note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt)
            .flatMap(AVAudioSession.InterruptionType.init) else { return }
    if type == .ended {
      try? AVAudioSession.sharedInstance().setActive(true)
      if _audioEngine?.isRunning == false { _startEngine() }
      else { _playerNode?.play() }
    }
  }

  NotificationCenter.default.addObserver(
    forName: AVAudioSession.mediaServicesWereResetNotification, object: nil, queue: .main
  ) { _ in
    _audioEngine = nil
    _playerNode  = nil
    _startEngine()
  }
}
