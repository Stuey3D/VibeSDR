import Foundation
import Compression

/// UberSDR sends its JSON control messages as GZIPPED BINARY WebSocket frames, not text —
/// which is why the client has to sniff for the gzip magic before deciding what a binary
/// frame even is.
///
/// Apple's `COMPRESSION_ZLIB` is RAW DEFLATE despite the name: it does not understand the
/// gzip wrapper. So the 10-byte gzip header (plus any optional fields) is stripped by hand
/// and the deflate stream underneath is handed to the framework.
enum Gzip {

  /// ★★★ THE LARGEST MESSAGE WE WILL INFLATE. ISIZE is the SENDER's claim — four bytes the
  ///     server (or anything in between) chooses — and the output buffer used to be sized from it
  ///     unchecked: up to 4 GB allocated on a watch from one frame. No control message we read is
  ///     anywhere near this; a frame whose output would exceed it is dropped, never truncated.
  static let maxInflated = 4 * 1024 * 1024

  static func inflate(_ raw: Data) -> Data? {
    // The byte subscripts below assume index 0 is the first byte; a slice does not promise that.
    let data = raw.startIndex == 0 ? raw : Data(raw)
    guard data.count > 18, data[0] == 0x1f, data[1] == 0x8b, data[2] == 0x08 else { return nil }

    let flags = data[3]
    var idx = 10                                  // fixed gzip header

    if flags & 0x04 != 0 {                        // FEXTRA
      guard idx + 2 <= data.count else { return nil }
      let xlen = Int(data[idx]) | (Int(data[idx + 1]) << 8)
      idx += 2 + xlen
    }
    if flags & 0x08 != 0 {                        // FNAME (NUL-terminated)
      while idx < data.count, data[idx] != 0 { idx += 1 }
      idx += 1
    }
    if flags & 0x10 != 0 {                        // FCOMMENT (NUL-terminated)
      while idx < data.count, data[idx] != 0 { idx += 1 }
      idx += 1
    }
    if flags & 0x02 != 0 { idx += 2 }             // FHCRC

    guard idx < data.count - 8 else { return nil }

    // The gzip trailer's ISIZE is the uncompressed length — use it, so the output buffer
    // is right first time instead of being guessed and grown.
    // ★ Assembled as UInt32, never Int: on arm64_32 (32-bit Int below watchOS 27) the top byte
    //   shifted into an Int went NEGATIVE. Capped before it becomes a size.
    let n = data.count
    let isize = UInt32(data[n - 4]) | (UInt32(data[n - 3]) << 8)
              | (UInt32(data[n - 2]) << 16) | (UInt32(data[n - 1]) << 24)
    if isize > UInt32(maxInflated) { return nil }      // claims more than we will ever accept
    let capacity = max(Int(isize), 1024) + 1024         // ≤ 4 MB + 1 KB: fits any Int

    let deflated = data.subdata(in: idx..<(n - 8))
    var out = Data(count: capacity)

    let written: Int = out.withUnsafeMutableBytes { dst -> Int in
      guard let dp = dst.bindMemory(to: UInt8.self).baseAddress else { return 0 }
      return deflated.withUnsafeBytes { src -> Int in
        guard let sp = src.bindMemory(to: UInt8.self).baseAddress else { return 0 }
        return compression_decode_buffer(dp, capacity, sp, deflated.count, nil, COMPRESSION_ZLIB)
      }
    }
    // compression_decode_buffer stops silently at `capacity`; a FULL buffer means the real
    // output was larger than ISIZE claimed (or than the cap) — drop it rather than hand back a
    // truncated message.
    guard written > 0, written < capacity, written <= maxInflated else { return nil }
    return out.prefix(written)
  }
}
