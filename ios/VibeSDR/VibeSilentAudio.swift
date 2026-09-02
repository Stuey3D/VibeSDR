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

/* ★★★ REAL AUDIO OUTRANKS EVERY HOLD, and this is not an optimisation — two AVAudioEngines is a
 *   fault. The silent loop exists for ONE reason: to make iOS count us as an audio app during the
 *   seconds between being woken and the real audio starting. Once the real engine is playing, it
 *   does that job by itself and this one has nothing left to do.
 *  ★★★ BUT IT WAS NEVER STOPPING. watchProvider takes a `watch` hold on ANY message from the
 *   wrist and Buddy pings every 4 s, so with Buddy on the wrist the hold never lapsed — and the
 *   silent engine ran CONTINUOUSLY alongside the real one: two AVAudioEngines on one
 *   AVAudioSession, a 44.1 kHz loop beside a 48 kHz decode. This file's own opening comment says
 *   "the real audio replaces it moments later"; it did not, and nothing noticed because silence
 *   is not something you hear.
 *  ★★ AVAudioEngine IS THE ONE THING IN THIS APP THAT PUNISHES THAT — the crash notes say it is
 *   single-threaded and its faults are uncatchable. Stuttering, dropouts and silence are exactly
 *   what a contended session produces, and they would follow the SIGNAL rather than the code,
 *   which is what makes such a bug read as "this frequency is broken".
 *  ★ The holds are kept, not cleared: when the real audio stops, whatever was holding still is. */
private var _realAudioPlaying = false

func vibeSetRealAudioPlaying(_ on: Bool) {
  let work = {
    guard _realAudioPlaying != on else { return }
    _realAudioPlaying = on
    _applyHolds()
  }
  if Thread.isMainThread { work() } else { DispatchQueue.main.async(execute: work) }
}

/// The one place that decides whether the loop should be running: something wants it, and the
/// real audio is not already doing the job.
private func _applyHolds() {
  let want = !_holds.isEmpty && !_realAudioPlaying
  let running = _audioEngine?.isRunning == true
  if want && !running { vibeStartSilentAudio() }
  else if !want && running { vibeStopSilentAudioEngineOnly() }
}

/// Stop the engine WITHOUT forgetting who was holding — see vibeStopSilentAudio for the full stop.
private func vibeStopSilentAudioEngineOnly() {
  _playerNode?.stop()
  _audioEngine?.stop()
  _playerNode  = nil
  _audioEngine = nil
}

func vibeHoldSilentAudio(_ reason: String, _ on: Bool) {
  let work = {
    if on { _holds.insert(reason) } else { _holds.remove(reason) }
    if _holds.isEmpty {
      // ★ Nothing holds it any more — full stop, including the wake deadline.
      vibeStopSilentAudio()
    } else {
      _applyHolds()
    }
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

/* ★★★ A PING MUST NOT START A HOLD, BUT IT SHOULD KEEP ONE ALIVE.
 *   Excluding pings from the wake hold stopped VibeSDR stealing the audio route every time the
 *   wrist came up — but it also took away the thing that kept a COLD start alive: the wake hold
 *   runs on a 45 s deadline from the last real command, and Buddy's 4 s heartbeat was what used
 *   to keep pushing that out while the JS bundle loaded. Stuart, 2026-09-01: the spectrum "has
 *   reverted back to only working for a couple of frames then stopping" — the app being let go
 *   mid-start, exactly as before.
 * ★★ SO THE RULE SPLITS ON WHETHER WE ARE ALREADY HOLDING. A ping arriving with no hold in force
 *   means nothing is happening and we take nothing — no route stolen, which is the whole point of
 *   the exclusion. A ping arriving DURING a hold is the wearer still there while work we already
 *   agreed to is in flight, and it pushes the deadline out.
 * ★ Refresh only: it can never create a hold, so it cannot reintroduce the fault it sits beside. */
func vibeRefreshWakeHoldSilentAudio() {
  let work = {
    guard _holds.contains("wake") else { return }
    _wakeTimer?.invalidate()
    _wakeTimer = Timer.scheduledTimer(withTimeInterval: 45, repeats: false) { _ in
      _wakeTimer = nil
      vibeHoldSilentAudio("wake", false)
    }
  }
  if Thread.isMainThread { work() } else { DispatchQueue.main.async(execute: work) }
}

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
 * ★★★ AND IT NOW HANDS THE SESSION BACK, which is the other half of not stealing somebody's music.
 *   This used to say the session is deliberately left active "because AppDelegate makes it active
 *   at launch" — and that launch activation has been REMOVED, precisely because activating a
 *   non-mixing .playback session before we have anything to play interrupts whatever the phone was
 *   doing. With it gone, nothing else would ever release the session, so the file that takes it
 *   must be the file that returns it. Activation and deactivation now live together.
 * ★★ `.notifyOthersOnDeactivation` is the part the user actually feels: it is what tells Apple
 *    Music it may resume. Without it the other app stays paused and CarPlay keeps showing a
 *    receiver that stopped playing minutes ago. Stuart, 2026-09-02, on a drive: "the now playing
 *    keeps trying to default to VibeSDR even though I had been playing my Apple Music."
 * ★★★ ONLY WHEN NOTHING REAL IS PLAYING. The caller stops this the moment real audio takes over,
 *     so an unconditional deactivate here would cut the listener off at the exact moment their
 *     station started. audioIsLive covers all four paths. */
func vibeStopSilentAudio() {
  _wakeTimer?.invalidate()
  _wakeTimer = nil
  _holds.removeAll()
  _playerNode?.stop()
  _audioEngine?.stop()
  _playerNode  = nil
  _audioEngine = nil
  if VibePowerModule.shared?.audioIsLive != true {
    try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
  }
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
