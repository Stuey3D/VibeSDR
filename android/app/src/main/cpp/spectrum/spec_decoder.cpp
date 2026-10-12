// VibeSDR — SpecDecoder. See spec_decoder.h; mirrors VibeServerWsClient._parseBinaryFrame (src/services).
#include "spec_decoder.h"
#include <cstring>

namespace vibespec {

namespace {
constexpr uint32_t SPEC_MAGIC = 0x43455053;   // "SPEC" little-endian
constexpr int U8_DBFS_OFFSET = -256;
inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline float rdf(const uint8_t* p) { uint32_t u = rd32(p); float f; std::memcpy(&f, &u, 4); return f; }
}  // namespace

SpecDecoder::Result SpecDecoder::feed(const uint8_t* p, size_t len, double& frequency) {
    if (isGzip(p, len)) return kNotSpec;
    if (len < 22) return kFault;                                   // short-frame
    if (rd32(p) != SPEC_MAGIC) return kFault;                      // bad-magic
    const uint8_t flags = p[5];
    frequency = (double)rd32(p + 14) + (double)rd32(p + 18) * 4294967296.0;
    if ((flags == 0x03 || flags == 0x01) && len == 22) return kFault;   // full frame with no bins
    const uint8_t* body = p + 22;
    const size_t blen = len - 22;
    switch (flags) {
        case 0x03: {                                                // full u8
            if ((size_t)bins_.size() != blen) { bins_.assign(blen, 0.f); binCount_ = (int)blen; }
            for (size_t i = 0; i < blen; i++) bins_[i] = (float)(body[i] + U8_DBFS_OFFSET);
            return kSpectrum;
        }
        case 0x01: {                                                // full f32
            if (blen % 4) return kFault;
            const size_t n = blen / 4;
            if (bins_.size() != n) { bins_.assign(n, 0.f); binCount_ = (int)n; }
            for (size_t i = 0; i < n; i++) bins_[i] = rdf(body + 4 * i);
            return kSpectrum;
        }
        case 0x02:                                                  // delta f32
        case 0x04: {                                                // delta u8
            if (blen < 2) return kIgnored;   // the JS returns without emitting OR counting a fault
            if (bins_.empty() && binCount_ > 0) bins_.assign(binCount_, 0.f);
            const uint16_t count = rd16(body);
            size_t off = 2;
            const size_t rec = flags == 0x02 ? 6 : 3;
            for (uint16_t i = 0; i < count; i++) {
                if (off + rec > blen) break;
                const uint16_t idx = rd16(body + off);
                const float v = flags == 0x02 ? rdf(body + off + 2) : (float)(body[off + 2] + U8_DBFS_OFFSET);
                off += rec;
                if (idx < bins_.size()) bins_[idx] = v;
            }
            return kSpectrum;
        }
        default:
            return kFault;                                          // unknown-flags
    }
}

}  // namespace vibespec
