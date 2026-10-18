import Foundation

/**
 * RecordBufferRing — the V12 RECORD BUFFER's store: the last 30 or 60 seconds of exactly what a
 * recording captures, so ⏺ REC can start the file with the audio from before the tap.
 *
 * ★★ WHY CHUNKS AND NOT ONE BIG ARRAY. A recording starting must never make the audio thread wait
 *    (Network > Audio > Spectrum > Decoders). Each appended buffer becomes an immutable chunk, so
 *    `snapshot()` is a copy of an array of references — no audio is copied on audioQ — and the
 *    recorder's own queue converts and encodes it at leisure.
 * ★★ MEMORY. Int16, not Float (half), and MONO whenever L == R — which is every mode but WFM stereo
 *    or binaural — so 60 s is ≈ 5.8 MB mono, ≈ 11.5 MB stereo, at 48 kHz. Nothing is held while the
 *    setting is Off: capacity 0 frees every chunk and `isOn` gates the call sites, so Off costs one
 *    Bool read per buffer.
 * ★ audioQ only (VibePowerModule) — no locking inside. Foundation only, so the headless test
 *   (scripts/test_recordBuffer_swift.sh) compiles it on its own.
 */
final class RecordBufferRing {
  struct Chunk {
    /** Interleaved when `channels == 2`. */
    let samples: [Int16]
    let channels: Int
    let frames: Int
  }

  let rate: Double
  private(set) var capacitySeconds: Double = 0
  private var chunks: [Chunk] = []
  private var head = 0
  private var framesHeld = 0
  private var capacityFrames = 0

  init(rate: Double) { self.rate = rate }

  var isOn: Bool { capacityFrames > 0 }
  /** Seconds of audio currently held. */
  var seconds: Double { Double(framesHeld) / rate }
  /** Bytes of sample data held — for the debug line and the test. */
  var bytesHeld: Int {
    var n = 0
    for i in head..<chunks.count { n += chunks[i].samples.count * 2 }
    return n
  }

  /** 0 = Off: frees everything. Shrinking trims the oldest audio; growing keeps what is there. */
  func setCapacity(seconds: Double) {
    capacitySeconds = max(0, seconds)
    capacityFrames = Int(capacitySeconds * rate)
    if capacityFrames == 0 { clear(); chunks = []; return }
    trim()
  }

  func clear() {
    chunks.removeAll(keepingCapacity: true)
    head = 0
    framesHeld = 0
  }

  /** One planar float buffer (the post-converter 48 kHz feed). Stored mono when the channels match. */
  func append(left: UnsafePointer<Float>, right: UnsafePointer<Float>?, frames n: Int) {
    guard capacityFrames > 0, n > 0 else { return }
    var mono = true
    if let r = right {
      for i in 0..<n where left[i] != r[i] { mono = false; break }
    }
    @inline(__always) func s16(_ v: Float) -> Int16 {
      let x = v * 32767
      return x >= 32767 ? 32767 : x <= -32768 ? -32768 : Int16(x)
    }
    let samples: [Int16]
    if mono {
      samples = [Int16](unsafeUninitializedCapacity: n) { b, count in
        for i in 0..<n { b[i] = s16(left[i]) }
        count = n
      }
    } else {
      let r = right!
      samples = [Int16](unsafeUninitializedCapacity: n * 2) { b, count in
        for i in 0..<n { b[i * 2] = s16(left[i]); b[i * 2 + 1] = s16(r[i]) }
        count = n * 2
      }
    }
    chunks.append(Chunk(samples: samples, channels: mono ? 1 : 2, frames: n))
    framesHeld += n
    trim()
  }

  /** The held audio, oldest first. Cheap: chunks are immutable and shared, not copied. */
  func snapshot() -> [Chunk] { head == 0 ? chunks : Array(chunks[head...]) }

  private func trim() {
    // Drop whole chunks from the front while what remains still covers the capacity.
    while head < chunks.count, framesHeld - chunks[head].frames >= capacityFrames {
      framesHeld -= chunks[head].frames
      head += 1
    }
    // Compact now and then rather than shifting the array on every drop.
    if head > 512 && head * 2 > chunks.count {
      chunks.removeFirst(head)
      head = 0
    }
  }
}
