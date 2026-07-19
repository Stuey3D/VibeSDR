import Foundation

/// IMA-ADPCM decoder — a Swift port of `src/services/imaAdpcm.ts` ('kiwi' flavour).
///
/// KiwiSDR uses the libcsdr/Kientzle variant: the step is sampled from the CURRENT index
/// BEFORE decoding a nibble, and the index adjusts AFTER. Nibble order is low-then-high.
/// Two streams need it: the audio (persistent state, s16 clamp, server may preset via
/// `MSG audio_adpcm_state=<index>,<prev>`) and the waterfall (u8 0..255 clamp, fresh per
/// frame, drop the first 10 ADPCM-settling samples).
final class ImaAdpcmDecoder {
  private static let indexTable = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
  private static let stepTable: [Int] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
    37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
  ]

  private var index = 0
  private var predictor = 0
  private let clampLo: Int
  private let clampHi: Int

  /// 'kiwi' (libcsdr): step from CURRENT index before decoding, index adjusts after.
  /// 'owrx' (openwebrx AudioEngine): index adjusts FIRST, diff uses the step LATCHED at the end of
  /// the previous nibble; first nibble after reset uses step=0; a state load leaves step stale.
  enum Flavor { case kiwi, owrx }
  private let flavor: Flavor
  private var step = 0   // owrx: latched step

  init(flavor: Flavor = .kiwi, clampLo: Int = -32768, clampHi: Int = 32767) {
    self.flavor = flavor
    self.clampLo = clampLo
    self.clampHi = clampHi
  }

  func reset() { index = 0; predictor = 0; step = 0 }

  /// Kiwi `MSG audio_adpcm_state=<index>,<prev>` / OWRX sync-frame state (owrx keeps `step` stale).
  func setState(index: Int, predictor: Int) {
    self.index = min(max(index, 0), 88)
    self.predictor = predictor
  }

  @inline(__always) func decodeNibble(_ nibble: Int) -> Int {
    let s: Int
    if flavor == .kiwi {
      s = Self.stepTable[index]
    } else {
      index = min(max(index + Self.indexTable[nibble], 0), 88)
      s = step
    }
    var diff = s >> 3
    if nibble & 1 != 0 { diff += s >> 2 }
    if nibble & 2 != 0 { diff += s >> 1 }
    if nibble & 4 != 0 { diff += s }
    if nibble & 8 != 0 { diff = -diff }
    predictor = min(max(predictor + diff, clampLo), clampHi)
    if flavor == .kiwi {
      index = min(max(index + Self.indexTable[nibble], 0), 88)
    } else {
      step = Self.stepTable[index]
    }
    return predictor
  }

  /// Decode a payload (2 samples/byte, low nibble first) into Int16 PCM.
  func decode(_ data: ArraySlice<UInt8>) -> [Int16] {
    var out = [Int16](repeating: 0, count: data.count * 2)
    var o = 0
    for b in data {
      out[o] = Int16(clamping: decodeNibble(Int(b) & 0x0f)); o += 1
      out[o] = Int16(clamping: decodeNibble((Int(b) >> 4) & 0x0f)); o += 1
    }
    return out
  }
  func decode(_ data: [UInt8]) -> [Int16] { decode(data[...]) }
}

/// Kiwi waterfall frame → u8 bins (dBm = bin − 255 upstream). Fresh state per frame; the
/// first 10 samples are the ADPCM settling pad (openwebrx.js waterfall_recv).
func decodeKiwiWaterfallFrame(_ data: ArraySlice<UInt8>) -> [UInt8] {
  let dec = ImaAdpcmDecoder(clampLo: 0, clampHi: 255)
  let all = dec.decode(data)
  let pad = 10
  guard all.count > pad else { return [] }
  return all[pad...].map { UInt8(clamping: $0) }
}

/// OWRX compressed FFT frame → Float32 dB row. Fresh 'owrx' state per frame; first 10 samples are
/// COMPRESS_FFT_PAD_N padding, then dB = int16 / 100. (Port of imaAdpcm.ts decodeOwrxFftFrame.)
func decodeOwrxFftFrame(_ data: ArraySlice<UInt8>) -> [Float] {
  let dec = ImaAdpcmDecoder(flavor: .owrx)
  let all = dec.decode(data)
  let pad = 10
  guard all.count > pad else { return [] }
  return all[pad...].map { Float($0) / 100.0 }
}

/// OWRX audio ADPCM stream with embedded "SYNC" framing — a port of AudioEngine.js decodeWithSync.
/// Every sync frame carries the codec state (stepIndex s16 LE, predictor s16 LE) then 1000 payload
/// bytes. Persistent across frames; reset on profile change.
final class OwrxAudioDecoder {
  private let codec = ImaAdpcmDecoder(flavor: .owrx)
  private var phase = 0            // 0=hunt SYNC, 1=read 4-byte state, 2=payload
  private var synchronized = 0
  private var syncBuffer = [UInt8](repeating: 0, count: 4)
  private var syncBufferIndex = 0
  private var syncCounter = 0
  private static let sync: [UInt8] = [0x53, 0x59, 0x4e, 0x43]   // "SYNC"

  func reset() { codec.reset(); phase = 0; synchronized = 0; syncBufferIndex = 0; syncCounter = 0 }

  func decode(_ data: ArraySlice<UInt8>) -> [Int16] {
    var out = [Int16](); out.reserveCapacity(data.count * 2)
    for b in data {
      switch phase {
      case 0:
        if b != Self.sync[synchronized] { synchronized = 0 } else { synchronized += 1 }
        if synchronized == 4 { syncBufferIndex = 0; phase = 1 }
      case 1:
        syncBuffer[syncBufferIndex] = b; syncBufferIndex += 1
        if syncBufferIndex == 4 {
          let stepIndex = Int(Int16(bitPattern: UInt16(syncBuffer[0]) | (UInt16(syncBuffer[1]) << 8)))
          let predictor = Int(Int16(bitPattern: UInt16(syncBuffer[2]) | (UInt16(syncBuffer[3]) << 8)))
          codec.setState(index: stepIndex, predictor: predictor)
          syncCounter = 1000
          phase = 2
        }
      default:
        out.append(Int16(clamping: codec.decodeNibble(Int(b) & 0x0f)))
        out.append(Int16(clamping: codec.decodeNibble(Int(b) >> 4)))
        syncCounter -= 1
        if syncCounter < 0 { synchronized = 0; phase = 0 }
      }
    }
    return out
  }
  func decode(_ data: [UInt8]) -> [Int16] { decode(data[...]) }
}
