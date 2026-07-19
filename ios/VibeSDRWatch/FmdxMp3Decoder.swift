import Foundation
import AVFoundation

/// Streaming MP3 → interleaved LE Int16 PCM decoder for the FM-DX Webserver `/audio` path (3LAS: raw MP3
/// frames over a WebSocket, no container, frame-independent).
///
/// ── watchOS has NO AudioToolbox ── The phone's decoder used `AudioFileStream` + `AudioConverter`, neither
/// of which exists on the watch SDK (only AVFAudio/CoreAudio ship). So this is a from-scratch watchOS
/// build: a manual MP3 frame splitter (the header format is fixed and well-defined) feeding `AVAudioConverter`
/// with `kAudioFormatMPEGLayer3` input. STEREO comes through natively — the header's channel mode sets the
/// output channel count, so a stereo broadcast decodes to interleaved L/R and `channels: 2` reaches WatchAudio.
///
/// Single-threaded: the caller serialises `feed(_:)` on its audio queue.
final class FmdxMp3Decoder {

  /// (interleaved LE Int16 PCM, channels, sampleRate). Emitted per decoded frame.
  var onPcm: ((Data, Int, Double) -> Void)?

  private var buf = Data()                 // undecoded byte accumulator
  private var converter: AVAudioConverter?
  private var inFormat: AVAudioFormat?
  private var outFormat: AVAudioFormat?
  private var curRate = 0.0
  private var curChannels = 0

  // ── MPEG audio tables (Layer III) ──
  private static let brV1: [Int] = [0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,-1]   // MPEG1
  private static let brV2: [Int] = [0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,-1]        // MPEG2/2.5
  private static let srV1: [Double] = [44100, 48000, 32000, 0]
  private static let srV2: [Double] = [22050, 24000, 16000, 0]
  private static let srV25: [Double] = [11025, 12000, 8000, 0]

  /// Feed raw MP3 bytes (any size / boundary).
  func feed(_ data: Data) {
    guard !data.isEmpty else { return }
    buf.append(data)
    parseFrames()
  }

  // MARK: - Frame splitter

  private func parseFrames() {
    var consumedTo = 0
    let n = buf.count
    buf.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
      let bytes = raw.bindMemory(to: UInt8.self)
      var i = 0
      while i + 4 <= n {
        // Sync: 11 bits set (0xFF Ex).
        guard bytes[i] == 0xFF, (bytes[i+1] & 0xE0) == 0xE0 else { i += 1; continue }
        guard let f = frameInfo(bytes[i+1], bytes[i+2], bytes[i+3]) else { i += 1; continue }
        if i + f.length > n { break }   // frame not fully arrived yet — wait for more
        let frame = Data(bytes: raw.baseAddress!.advanced(by: i), count: f.length)
        decode(frame, rate: f.rate, channels: f.channels, samples: f.samples)
        i += f.length
        consumedTo = i
      }
    }
    // Drop what we consumed; keep the trailing partial frame. If a big buffer has no sync at all, resync
    // by keeping only the tail (a stray byte can't be a frame start).
    if consumedTo > 0 { buf.removeSubrange(0..<consumedTo) }
    else if n > 4096 { buf.removeSubrange(0..<(n - 3)) }
  }

  /// Parse the 3 header bytes after sync → (byte length, sample rate, channels, samples/frame). nil = invalid.
  private func frameInfo(_ b1: UInt8, _ b2: UInt8, _ b3: UInt8) -> (length: Int, rate: Double, channels: Int, samples: Int)? {
    let versionBits = (b1 >> 3) & 0x03     // 00=2.5 01=reserved 10=2 11=1
    let layerBits   = (b1 >> 1) & 0x03     // 01 = Layer III
    guard versionBits != 0b01, layerBits == 0b01 else { return nil }
    let brIndex = Int((b2 >> 4) & 0x0F)
    let srIndex = Int((b2 >> 2) & 0x03)
    let padding = Int((b2 >> 1) & 0x01)
    guard brIndex != 0, brIndex != 15, srIndex != 3 else { return nil }

    let mpeg1 = (versionBits == 0b11)
    let bitrate = (mpeg1 ? Self.brV1 : Self.brV2)[brIndex] * 1000
    let rate: Double = versionBits == 0b11 ? Self.srV1[srIndex]
                     : versionBits == 0b10 ? Self.srV2[srIndex]
                     : Self.srV25[srIndex]
    guard bitrate > 0, rate > 0 else { return nil }

    let samples = mpeg1 ? 1152 : 576
    let length = mpeg1 ? (144 * bitrate / Int(rate) + padding)
                       : (72 * bitrate / Int(rate) + padding)
    guard length > 4 else { return nil }
    let channels = ((b3 >> 6) & 0x03) == 0b11 ? 1 : 2   // channel mode 11 = mono
    return (length, rate, channels, samples)
  }

  // MARK: - Decode one frame via AVAudioConverter

  private func decode(_ frame: Data, rate: Double, channels: Int, samples: Int) {
    if converter == nil || rate != curRate || channels != curChannels {
      rebuild(rate: rate, channels: channels, samples: samples)
    }
    guard let conv = converter, let inFmt = inFormat, let outFmt = outFormat else { return }

    let comp = AVAudioCompressedBuffer(format: inFmt, packetCapacity: 1, maximumPacketSize: frame.count)
    comp.byteLength = UInt32(frame.count)
    comp.packetCount = 1
    frame.withUnsafeBytes { src in memcpy(comp.data, src.baseAddress!, frame.count) }
    if let descs = comp.packetDescriptions {
      descs[0].mStartOffset = 0
      descs[0].mVariableFramesInPacket = 0        // 0 = constant frames/packet (from the format); NOT the sample count
      descs[0].mDataByteSize = UInt32(frame.count)
    }

    guard let out = AVAudioPCMBuffer(pcmFormat: outFmt, frameCapacity: AVAudioFrameCount(samples)) else { return }
    var supplied = false
    var err: NSError?
    let status = conv.convert(to: out, error: &err) { _, outStatus in
      if supplied { outStatus.pointee = .noDataNow; return nil }
      supplied = true
      outStatus.pointee = .haveData
      return comp
    }
    guard status != .error, out.frameLength > 0, let ch = out.int16ChannelData else { return }
    let count = Int(out.frameLength) * channels     // interleaved samples
    let data = Data(bytes: ch[0], count: count * 2)
    onPcm?(data, channels, rate)
  }

  private func rebuild(rate: Double, channels: Int, samples: Int) {
    var asbd = AudioStreamBasicDescription(
      mSampleRate: rate, mFormatID: kAudioFormatMPEGLayer3, mFormatFlags: 0,
      mBytesPerPacket: 0, mFramesPerPacket: UInt32(samples), mBytesPerFrame: 0,
      mChannelsPerFrame: UInt32(channels), mBitsPerChannel: 0, mReserved: 0)
    guard let inFmt = AVAudioFormat(streamDescription: &asbd),
          let outFmt = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: rate,
                                     channels: AVAudioChannelCount(channels), interleaved: true),
          let conv = AVAudioConverter(from: inFmt, to: outFmt) else {
      NSLog("[FmdxMp3Decoder] AVAudioConverter build failed (%.0fHz %dch)", rate, channels)
      Vitals.crumb("FMDX decoder BUILD FAILED \(Int(rate))Hz \(channels)ch — watchOS may lack MP3 decode")
      return
    }
    inFormat = inFmt; outFormat = outFmt; converter = conv
    curRate = rate; curChannels = channels
  }
}
