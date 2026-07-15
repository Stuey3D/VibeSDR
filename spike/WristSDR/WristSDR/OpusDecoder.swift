import Foundation

/// UberSDR's audio is Opus, and Apple ships no Opus decoder on ANY platform — so this is
/// the thing that made the spike a build problem before it was a code problem. The
/// libopus in the main app is an iOS arm64 static library; watchOS needed its own
/// (see scratchpad/build_opus_watchos.sh — arm64 only, because JR targets Series 9+).
///
/// Packet layout on /ws/audio (version 2, 21-byte header) — same as the phone's:
///   [0:8]   uint64 LE  timestamp
///   [8:12]  uint32 LE  sample rate
///   [12]    uint8      channels
///   [13:17] float32 LE baseband power
///   [17:21] float32 LE noise density
///   [21:]   Opus payload
final class OpusDecoder {
  private var dec: OpaquePointer?
  private(set) var sampleRate: Int32 = 0
  private(set) var channels: Int32 = 0

  /// Opus frames are at most 120 ms; at 48 kHz stereo that is 5760 samples per channel.
  private var pcm = [Int16](repeating: 0, count: 5760 * 2)

  deinit { if let d = dec { opus_decoder_destroy(d) } }

  /// The server creates its encoder ONCE per WebSocket at the then-current sample rate,
  /// so a rate change means a new decoder — and, on the phone, a whole new socket. Here
  /// we simply rebuild when the header's rate changes.
  private func ensure(rate: Int32, ch: Int32) {
    guard rate != sampleRate || ch != channels || dec == nil else { return }
    if let d = dec { opus_decoder_destroy(d); dec = nil }
    var err: Int32 = 0
    dec = opus_decoder_create(rate, ch, &err)
    if err != OPUS_OK || dec == nil {
      NSLog("[Opus] decoder_create failed: \(err)")
      dec = nil
      return
    }
    sampleRate = rate
    channels = ch
  }

  /// Decode one packet. Returns interleaved Int16 PCM, or nil if the packet is unusable.
  func decode(_ packet: Data) -> (pcm: [Int16], rate: Int32, channels: Int32)? {
    guard packet.count > 21 else { return nil }

    let rate: UInt32 = packet.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 8, as: UInt32.self) }
    let ch = packet[12]
    guard rate >= 8000, rate <= 48000, ch == 1 || ch == 2 else { return nil }

    ensure(rate: Int32(rate), ch: Int32(ch))
    guard let d = dec else { return nil }

    let payload = packet.subdata(in: 21..<packet.count)
    let maxPerChannel = Int32(pcm.count / Int(channels))

    let n: Int32 = payload.withUnsafeBytes { raw -> Int32 in
      guard let base = raw.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return -1 }
      return pcm.withUnsafeMutableBufferPointer { out -> Int32 in
        guard let dst = out.baseAddress else { return -1 }
        return opus_decode(d, base, Int32(payload.count), dst, maxPerChannel, 0)
      }
    }
    guard n > 0 else { return nil }

    let total = Int(n) * Int(channels)
    return (Array(pcm[0..<total]), sampleRate, channels)
  }
}
