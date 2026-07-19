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

  static func inflate(_ data: Data) -> Data? {
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
    let n = data.count
    let isize = Int(data[n - 4]) | (Int(data[n - 3]) << 8)
              | (Int(data[n - 2]) << 16) | (Int(data[n - 1]) << 24)
    let capacity = max(isize, 1024) + 1024

    let deflated = data.subdata(in: idx..<(n - 8))
    var out = Data(count: capacity)

    let written: Int = out.withUnsafeMutableBytes { dst -> Int in
      guard let dp = dst.bindMemory(to: UInt8.self).baseAddress else { return 0 }
      return deflated.withUnsafeBytes { src -> Int in
        guard let sp = src.bindMemory(to: UInt8.self).baseAddress else { return 0 }
        return compression_decode_buffer(dp, capacity, sp, deflated.count, nil, COMPRESSION_ZLIB)
      }
    }
    guard written > 0 else { return nil }
    return out.prefix(written)
  }
}
