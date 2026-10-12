// VibeSDR — SpecDecoder: the VibeServer / UberSDR binary spectrum frame ("SPEC"), decoded natively.
//
// A line-for-line port of VibeServerWsClient._parseBinaryFrame and its _apply* helpers (the UberSDR client's are the
// same format), so the native spectrum socket (docs/NATIVE-SPECTRUM-ENGINE.md, stage 2) keeps exactly the bins the JS
// kept — including its two guards that cost a black waterfall when they were missing:
//   ★ a DELTA cannot create the array it writes into: an empty array is sized from the config's bin count
//     (setBinCountHint) — UberSDR sends nothing but deltas, and a reconnect need not re-send the config;
//   ★ a FULL frame with no bins is dropped, not applied (one truncated frame would blank the display).
// Layout (little-endian): u32 magic "SPEC", u8 version, u8 flags, u64 timestamp (unused), u64 frequency Hz, body.
//   0x01 full f32 · 0x02 delta f32 (u16 count, {u16 idx, f32}) · 0x03 full u8 · 0x04 delta u8 (u16 count, {u16 idx, u8})
//   u8 → dBFS = u8 − 256.  Gzipped JSON control frames (1f 8b) are NOT spectrum — the caller hands those to JS.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vibespec {

class SpecDecoder {
public:
    enum Result { kSpectrum, kNotSpec, kFault, kIgnored };   // kIgnored: the JS returns silently (delta body < 2)

    /** Decode one binary frame. kSpectrum: bins() now holds the frame (raw server order) and `frequency` its Hz. */
    Result feed(const uint8_t* p, size_t len, double& frequency);
    const std::vector<float>& bins() const { return bins_; }
    /** The config's binCount. ★ As the JS does on every config: a DIFFERENT count reallocates (zeroed) at once. */
    void setBinCount(int n) { binCount_ = n; if (n > 0 && (int)bins_.size() != n) bins_.assign(n, 0.f); }
    int binCount() const { return binCount_; }   // updated by full frames, as status.binCount is
    static bool isGzip(const uint8_t* p, size_t len) { return len >= 2 && p[0] == 0x1f && p[1] == 0x8b; }

private:
    std::vector<float> bins_ = std::vector<float>(1024, 0.f);   // the JS starts at Float32Array(1024), binCount 1024
    int binCount_ = 1024;
};

}  // namespace vibespec
