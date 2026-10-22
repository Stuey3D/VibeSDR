// vibe_png.h — a PNG writer for the gallery's charts and pictures (V12, 2026-10-22).
//
// ★ NO NEW LIBRARY. The deflate is vibe_gzip.h's (the one the web page is already served with): a gzip member is a
//   10-byte header, the raw DEFLATE stream and an 8-byte trailer, so the stream is taken from between them and given
//   zlib's two-byte header and Adler-32 instead. Greyscale (WEFAX) or RGB (SSTV), 8 bits, the 'Sub' filter on every
//   row — a fax chart is long runs of the same grey, and Sub turns them into zeros the matcher eats.
// ★ Runs on a DECODE thread at the end of a chart (Network > Audio > Spectrum > Decoders): ~3 MB of WEFAX in tens of
//   milliseconds on a Pi 2, once per chart.
#pragma once
#include <algorithm>
#include "vibe_gzip.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vibe {
namespace png {

inline void put32(std::string& o, uint32_t v) {
    o += (char)(v >> 24); o += (char)(v >> 16); o += (char)(v >> 8); o += (char)v;
}
inline void chunk(std::string& o, const char* type, const std::string& data) {
    put32(o, (uint32_t)data.size());
    std::string td(type, 4); td += data;
    o += td;
    put32(o, vibegz::crc32(reinterpret_cast<const uint8_t*>(td.data()), td.size()));
}
/** `px` is w*h*channels bytes, rows top to bottom; channels 1 (grey) or 3 (RGB). Empty on nonsense input. */
inline std::string encode(int w, int h, int channels, const uint8_t* px) {
    if (w <= 0 || h <= 0 || (channels != 1 && channels != 3) || !px) return "";
    const size_t row = (size_t)w * (size_t)channels;
    std::string raw;
    raw.reserve((row + 1) * (size_t)h);
    for (int y = 0; y < h; y++) {
        const uint8_t* r = px + (size_t)y * row;
        raw += (char)1;                                    // Sub
        for (size_t i = 0; i < row; i++) raw += (char)(uint8_t)(r[i] - (i >= (size_t)channels ? r[i - channels] : 0));
    }
    const std::string gz = vibegz::gzip(raw);
    if (gz.size() < 18) return "";
    std::string z;
    z += (char)0x78; z += (char)0x01;                      // zlib: deflate, 32 KB window, no dictionary
    z.append(gz, 10, gz.size() - 18);
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    put32(z, (b << 16) | a);
    std::string o("\x89PNG\r\n\x1a\n", 8);
    std::string ih;
    put32(ih, (uint32_t)w); put32(ih, (uint32_t)h);
    ih += (char)8; ih += (char)(channels == 3 ? 2 : 0); ih += (char)0; ih += (char)0; ih += (char)0;
    chunk(o, "IHDR", ih);
    chunk(o, "IDAT", z);
    chunk(o, "IEND", "");
    return o;
}

} // namespace png
} // namespace vibe
