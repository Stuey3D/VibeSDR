// vibe_wide_row.h — the full-capture row, "SPWB" v2 (V12, Stuart 2026-10-28): the WHOLE capture, for the layer the clients
// draw UNDER the waterfall's history so a zoom out or a pan never shows black inside what the radio was capturing.
//
// ★★ v2 — "the same waterfall, just less sharp" (Stuart's review of v1: big hot blobs, a hotter floor, vertical blocks).
//    v1 sent 1024 MAX-held bins: magnified to a 240 kHz view each 2.3 kHz bin became a fat plateau at the PEAK level of
//    everything in it — every SSB voice a blob, every floor bin raised by the max of 32 noise bins. A coarse row cannot be
//    made sharp, but it can be made TRUE: a floor that is the floor, and signals where they really are, as thin as they are.
//    So the row is two parts:
//      • the FLOOR — kFloorBins bins, each the MEDIAN of its FFT bins (the noise between signals, not their peak), and its
//        SPREAD (median − 16th percentile: the noise's σ, from the half no signal reaches). A detailed row's floor is not
//        one level but a speckle above and below it, and through the palette's clip that speckle is most of what the eye
//        sees; the client draws the floor as the speckle's AVERAGE colour (the same field, blurred), not its median's;
//      • the PEAKS — every cell (kCells across the capture) whose strongest FFT bin stands kPeakDb above the local floor
//        (its width: the bins within kWidthDb of that max):
//        its level, WHERE in the cell it is (1/16 of a cell), and how WIDE the part standing out is (in 1/16ths). A thin
//        carrier is drawn as a thin line at its frequency; a voice as the band it occupies — at the MEAN of its standing
//        bins blended with its max, never a plateau at the max.
//      Only the strongest kMaxPeaks cells go (the budget: ~+15 % of a listener's spectrum bytes); the rest are floor.
// Wire (little-endian): 'S','P','W','B', 2, 0, u64 ts ns, u64 centre Hz, u32 span Hz, u16 F, u16 C, u16 P,
//   F floor bytes (low → high, dB + 256, 0 = no data), F spread bytes (σ in 0.1 dB), then P triples (u8 cell delta from the previous peak's cell — the
//   first from −1 — u8 level dB + 256 (≥ 1), u8 shape = offset16 | (width16 − 1) << 4). A delta over 255 is carried by
//   escape triples (255, 0, 0): advance 255 cells, no peak. docs/PROTOCOL.md.
// PURE (no server state) — exercised through scripts/test-server-wide-rows.mjs and the web's test_wf_wide.ts decoder.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vibe { namespace wide {

constexpr int kFloorBins = 128;   // 18.75 kHz each across 2.4 MHz — the floor is smooth; linear between them
constexpr int kCells     = 1024;  // 2.34 kHz at 2.4 MS/s; position within a cell to 1/16 (one or two FFT bins)
constexpr int kMaxPeaks  = 110;   // the budget — see the header note: 32 + 2·128 + 3·110 = 618 bytes every 4th frame
constexpr float kPeakDb  = 6.0f;  // a cell is a signal when its strongest bin stands this far over the local floor — and
constexpr float kPeakSig = 3.5f;  //   at least this many σ of the local noise (a flat 6 dB let the max of a cell's own
                                  //   noise through: dozens of false dots on a flat band)
constexpr float kWidthDb = 12.0f; // its width: the bins within this of its max (a carrier's skirts are not its width)
constexpr size_t kHdr    = 32;

struct Scratch { std::vector<float> tmp; std::vector<float> floorDb, sigDb; struct Pk { int cell; float ex; uint8_t lvl, shape; }; std::vector<Pk> pk; };

inline uint8_t toByte(float db, float comp) {
    const int v = (int)std::lround(db + 256.0f + comp);
    return (uint8_t)(v < 1 ? 1 : (v > 255 ? 255 : v));
}

/** Encode the averaged wide row `db` (fftshifted: index 0 = −span/2, natural order), `bins` long, into `out`. */
inline void encode(const float* db, int bins, double centreHz, double spanHz, float comp, std::vector<uint8_t>& out, Scratch& s) {
    out.clear();
    if (!db || bins < kCells || !(spanHz > 0)) return;
    // ── the floor: the median of each floor bin's FFT bins ──
    s.floorDb.assign(kFloorBins, 0.0f); s.sigDb.assign(kFloorBins, 0.0f);
    for (int f = 0; f < kFloorBins; f++) {
        const int lo = (int)((long long)f * bins / kFloorBins), hi = (int)((long long)(f + 1) * bins / kFloorBins);
        // ★ at most 64 of its bins, evenly — a median of 64 noise samples is as good a floor as one of 256, at a quarter of
        //   the cost (this runs on the DSP thread: ~50 µs a row here, ~1 ms on a Pi 2)
        const int step = std::max(1, (hi - lo) / 64);
        s.tmp.clear();
        for (int i = lo; i < hi; i += step) s.tmp.push_back(db[i]);
        auto mid = s.tmp.begin() + (long)s.tmp.size() / 2;
        std::nth_element(s.tmp.begin(), mid, s.tmp.end());
        s.floorDb[f] = *mid;
        auto p16 = s.tmp.begin() + (long)(s.tmp.size() * 16 / 100);
        std::nth_element(s.tmp.begin(), p16, mid);
        s.sigDb[f] = std::max(0.0f, s.floorDb[f] - *p16);
    }
    auto sigAt = [&](double bin) { int i = (int)(bin / bins * kFloorBins); return s.sigDb[std::max(0, std::min(kFloorBins - 1, i))]; };
    auto floorAt = [&](double bin) {   // linear between floor-bin centres
        const double x = bin / bins * kFloorBins - 0.5;
        int i = (int)std::floor(x); double t = x - i;
        if (i < 0) { i = 0; t = 0; } if (i >= kFloorBins - 1) { i = kFloorBins - 2; t = 1; }
        return (float)(s.floorDb[i] + (s.floorDb[i + 1] - s.floorDb[i]) * t);
    };
    // ── the peaks: cells whose strongest bin stands out ──
    s.pk.clear();
    const int per = bins / kCells;
    for (int c = 0; c < kCells; c++) {
        const int lo = c * per;
        int bi = lo; float mx = db[lo];
        for (int i = lo + 1; i < lo + per; i++) if (db[i] > mx) { mx = db[i]; bi = i; }
        const float fl = floorAt(bi + 0.5);
        const float ex = mx - fl;
        if (ex < std::max(kPeakDb, kPeakSig * sigAt(bi + 0.5))) continue;
        // the part standing out: its width, its centre, its mean level
        int n = 0, first = -1, last = -1; double sum = 0;
        const float cut = std::max(fl + std::max(kPeakDb, kPeakSig * sigAt(bi + 0.5)), mx - kWidthDb);
        for (int i = lo; i < lo + per; i++) if (db[i] >= cut) { n++; sum += db[i]; if (first < 0) first = i; last = i; }
        const double centre = (first + last + 1) * 0.5 - lo;                 // in FFT bins from the cell's low edge
        int off = (int)std::floor(centre / per * 16.0); off = std::max(0, std::min(15, off));
        int w16 = (int)std::lround((double)n / per * 16.0); w16 = std::max(1, std::min(16, w16));
        const float lvl = n <= 2 ? mx : (float)(0.5 * (mx + sum / n));       // a voice: between its mean and its max
        s.pk.push_back({c, ex, toByte(lvl, comp), (uint8_t)(off | ((w16 - 1) << 4))});
    }
    if ((int)s.pk.size() > kMaxPeaks) {
        std::nth_element(s.pk.begin(), s.pk.begin() + kMaxPeaks, s.pk.end(), [](const Scratch::Pk& a, const Scratch::Pk& b) { return a.ex > b.ex; });
        s.pk.resize(kMaxPeaks);
        std::sort(s.pk.begin(), s.pk.end(), [](const Scratch::Pk& a, const Scratch::Pk& b) { return a.cell < b.cell; });
    }
    // ── the frame ──
    out.resize(kHdr + 2 * kFloorBins);
    out[0]='S'; out[1]='P'; out[2]='W'; out[3]='B'; out[4]=2; out[5]=0;
    const uint64_t ts = 0; std::memcpy(&out[6], &ts, 8);   // set by the caller
    const uint64_t c = (uint64_t)std::llround(centreHz); std::memcpy(&out[14], &c, 8);
    const uint32_t sp = (uint32_t)std::llround(spanHz);   std::memcpy(&out[22], &sp, 4);
    const uint16_t F = kFloorBins, C = kCells; std::memcpy(&out[26], &F, 2); std::memcpy(&out[28], &C, 2);
    for (int f = 0; f < kFloorBins; f++) out[kHdr + f] = toByte(s.floorDb[f], comp);
    for (int f = 0; f < kFloorBins; f++) out[kHdr + kFloorBins + f] = (uint8_t)std::min(255L, std::lround(s.sigDb[f] * 10.0f));
    uint16_t P = 0; int prev = -1;
    for (const auto& p : s.pk) {
        int d = p.cell - prev;
        while (d > 255) { out.push_back(255); out.push_back(0); out.push_back(0); d -= 255; P++; }
        out.push_back((uint8_t)d); out.push_back(p.lvl); out.push_back(p.shape); P++;
        prev = p.cell;
    }
    std::memcpy(&out[30], &P, 2);
}

}}  // namespace vibe::wide
