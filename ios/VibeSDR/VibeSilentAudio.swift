import AVFoundation

private var _audioEngine:  AVAudioEngine?
private var _playerNode:   AVAudioPlayerNode?
private var _silentBuffer: AVAudioPCMBuffer?

func vibeStartSilentAudio() {
  _setupObservers()
  _startEngine()
}

private func _startEngine() {
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
