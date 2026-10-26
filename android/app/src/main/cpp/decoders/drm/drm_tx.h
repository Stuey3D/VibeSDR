// decoders/drm/drm_tx.h — a DRM TEST TRANSMITTER built from ETSI ES 201 980 V4.3.1, and the channel models of its annex B.
//
// ★★★ FOR TESTS AND THE BENCHMARK ONLY — nothing on a server transmits. It exists so the receiver is checked against
//     signals whose every bit is known: FAC, SDC and MSC built by the standard's own arithmetic (drm_coding/mlc/info),
//     mapped onto the cell map (drm_params), OFDM-modulated at 24 kHz, then passed through annex B's channels (AWGN,
//     Rice, US Consortium, CCIR Poor, channel 5) with a frequency offset and a sample-clock error.
// ★ The MSC carries one audio stream whose logical frames are a COUNTER and a PRBS pattern, plus a real text message,
//   so the test checks every byte that arrives, not just "something decoded".
#pragma once
#include "drm_info.h"
#include "drm_mlc.h"
#include "drm_params.h"
#include <functional>
#include <random>

namespace vibedrm {

struct TxConfig {
    Mode mode = Mode::B;
    int so = 3;
    int mscLevels = 3;            // 3 = 64-QAM, 2 = 16-QAM
    int sdcLevels = 1;            // 1 = 4-QAM, 2 = 16-QAM
    bool longInterleave = true;
    int protA = 0, protB = 1;
    int lenA = 0;                 // part A bytes of the audio stream (0 = EEP)
    std::string label = "VIBE TEST";
    std::string text = "Hello from the VibeSDR DRM test transmitter";
    uint32_t serviceId = 0xABCDEF;
    int language = 5, programmeType = 9;
    AudioInfo audio;              // what the SDC declares (coding/sbr/mode/rate/text)
    /** The logical frame payload for mux frame n (without the 4 text bytes) — default: counter + pattern. */
    std::function<std::vector<uint8_t>(int64_t n, size_t len)> payload;
};

/** The default payload: 4-byte big-endian counter, then a byte pattern that depends on it. */
inline std::vector<uint8_t> testPayload(int64_t n, size_t len) {
    std::vector<uint8_t> b(len);
    for (size_t i = 0; i < len; ++i) b[i] = uint8_t((i < 4) ? (uint64_t(n) >> (8 * (3 - i))) : (uint32_t(i) * 37u + uint32_t(n) * 101u));
    return b;
}

class Transmitter {
public:
    explicit Transmitter(const TxConfig& c) : cfg_(c) {
        map_.build(c.mode, c.so);
        const ModeInfo& mi = modeInfo(c.mode);
        tu_ = mi.tu; tg_ = mi.tg;
        // FAC
        facL_.buildFac();
        // SDC
        sdcL_.buildSdc(c.sdcLevels, map_.nSdc);
        sdcBytes_ = sdcDataBytesTable(c.mode, c.sdcLevels == 2 ? 0 : 1, c.so);
        // MSC
        mscL_.buildMsc(c.mscLevels, map_.nMux, c.protA, c.protB, c.lenA);
        // the one audio stream fills part A (lenA) and as much of part B as the profile carries
        lenB_ = (mscL_.l1 + mscL_.l2) / 8 - c.lenA;
        cil_.reset(map_.nMux, c.longInterleave ? 5 : 1);
        textPieces_ = textMessagePieces(c.text, false);
        if (!cfg_.audio.present) { cfg_.audio.present = true; cfg_.audio.coding = 3; cfg_.audio.srCode = 4; cfg_.audio.audioMode = 0; cfg_.audio.text = 1; }
    }
    int lenB() const { return lenB_; }
    const MlcLayout& mscLayout() const { return mscL_; }
    const CellMap& map() const { return map_; }

    /** One transmission super frame of baseband samples at 24 kHz (unit average power per carrier cell). */
    std::vector<cf> superFrame() {
        std::vector<cf> cells(map_.type.size(), cf(0, 0));
        // pilots
        for (size_t i = 0; i < cells.size(); ++i)
            if (map_.type[i] == kFreqRef || map_.type[i] == kTimeRef || map_.type[i] == kGainRef) cells[i] = map_.pilot[i];
        // FAC per frame
        for (int f = 0; f < 3; ++f) {
            Fac fac; fac.identity = f == 0 ? 0 : f == 1 ? 1 : 2; fac.so = cfg_.so; fac.longInterleave = cfg_.longInterleave;
            fac.mscMode = cfg_.mscLevels == 3 ? 0 : 3; fac.sdcMode = cfg_.sdcLevels == 2 ? 0 : 1; fac.services = 4;
            fac.serviceId = cfg_.serviceId; fac.language = cfg_.language; fac.descriptor = cfg_.programmeType;
            std::vector<uint8_t> u = fac.toBits();
            prbs(u.data(), u.size());
            const auto z = mlcEncode(u, facL_);
            size_t q = 0;
            for (int s = 0; s < map_.ns; ++s) for (int k = map_.kmin; k <= map_.kmax; ++k) {
                const int i = map_.idx(f, s, k);
                if (map_.type[size_t(i)] == kFac) cells[size_t(i)] = z[q++];
            }
        }
        // SDC
        {
            SdcBuilder sb;
            sb.mux(cfg_.lenA > 0 ? cfg_.protA : 0, cfg_.protB, { StreamDesc{cfg_.lenA, lenB_} });
            sb.label(0, cfg_.label);
            sb.audio(0, cfg_.audio);
            if (sfCount_ % 2 == 0) sb.langCountry(0, "eng", "gb"); else sb.time(61340, 17, int(sfCount_ % 60));
            std::vector<uint8_t> u = sb.block(0, sdcBytes_, sdcL_.l1 + sdcL_.l2);
            prbs(u.data(), u.size());
            const auto z = mlcEncode(u, sdcL_);
            size_t q = 0;
            for (int s = 0; s < map_.ns; ++s) for (int k = map_.kmin; k <= map_.kmax; ++k) {
                const int i = map_.idx(0, s, k);
                if (map_.type[size_t(i)] == kSdc) cells[size_t(i)] = z[q++];
            }
        }
        // MSC: three multiplex frames
        std::vector<cf> msc;
        for (int m = 0; m < 3; ++m) {
            const int64_t n = muxCount_++;
            std::vector<uint8_t> lf = cfg_.payload ? cfg_.payload(n, size_t(cfg_.lenA + lenB_ - 4)) : testPayload(n, size_t(cfg_.lenA + lenB_ - 4));
            const auto& tp = textPieces_[size_t(n % int64_t(textPieces_.size()))];
            lf.insert(lf.end(), tp.begin(), tp.end());
            std::vector<uint8_t> u; for (uint8_t b : lf) putBits(u, b, 8);
            u.resize(size_t(mscL_.l1 + mscL_.l2), 0);
            prbs(u.data(), u.size());
            const auto z = mlcEncode(u, mscL_);
            const auto zi = cil_.push(z);
            msc.insert(msc.end(), zi.begin(), zi.end());
        }
        size_t q = 0;
        const float a = qamScale(cfg_.mscLevels);
        int dummy = 0;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (map_.type[i] == kMsc) cells[i] = msc[q++];
            else if (map_.type[i] == kMscDummy) { cells[i] = dummy++ == 0 ? cf(a, a) : cf(a, -a); }
        }
        ++sfCount_;
        // OFDM
        std::vector<cf> out;
        out.reserve(size_t(3 * map_.ns * (tu_ + tg_)));
        std::vector<cf> bins(static_cast<size_t>(tu_));
        for (int f = 0; f < 3; ++f)
            for (int s = 0; s < map_.ns; ++s) {
                std::fill(bins.begin(), bins.end(), cf(0, 0));
                for (int k = map_.kmin; k <= map_.kmax; ++k) bins[size_t((k + tu_) % tu_)] = cells[size_t(map_.idx(f, s, k))];
                const std::vector<cf> t = idft(bins);
                out.insert(out.end(), t.end() - tg_, t.end());
                out.insert(out.end(), t.begin(), t.end());
            }
        return out;
    }

private:
    /** Inverse DFT, scaled so one unit cell gives unit power over the useful part of the symbol / K. Direct but
     *  cached-twiddle — the transmitter is test code, the receiver is where speed matters. */
    std::vector<cf> idft(const std::vector<cf>& X) {
        const int n = tu_;
        if (tw_.size() != size_t(n)) { tw_.resize(size_t(n)); for (int i = 0; i < n; ++i) tw_[size_t(i)] = std::polar(1.0f, float(2.0 * M_PI * i / n)); }
        std::vector<cf> x(size_t(n), cf(0, 0));
        std::vector<int> nz;
        for (int k = 0; k < n; ++k) if (X[size_t(k)] != cf(0, 0)) nz.push_back(k);
        const float sc = 1.0f / std::sqrt(float(map_.nk));
        for (int t = 0; t < n; ++t) {
            cf acc(0, 0);
            for (int k : nz) acc += X[size_t(k)] * tw_[size_t((int64_t(k) * t) % n)];
            x[size_t(t)] = acc * sc;
        }
        return x;
    }
    TxConfig cfg_;
    CellMap map_;
    MlcLayout facL_, sdcL_, mscL_;
    int sdcBytes_ = 0, lenB_ = 0, tu_ = 0, tg_ = 0;
    CellInterleaver cil_;
    std::vector<std::array<uint8_t, 4>> textPieces_;
    std::vector<cf> tw_;
    int64_t sfCount_ = 0, muxCount_ = 0;
};

// ── Channels (annex B.1) ──────────────────────────────────────────────────────────────────────────────────────────

struct PathSpec { double delayMs, gain, dopplerShiftHz, dopplerSpreadHz; };
inline std::vector<PathSpec> channelProfile(int ch) {
    switch (ch) {
        case 2: return { {0, 1, 0, 0}, {1.0, 0.5, 0, 0.1} };
        case 3: return { {0, 1, 0.1, 0.1}, {0.7, 0.7, 0.2, 0.5}, {1.5, 0.5, 0.5, 1.0}, {2.2, 0.25, 1.0, 2.0} };
        case 4: return { {0, 1, 0, 1}, {2.0, 1, 0, 1} };
        case 5: return { {0, 1, 0, 2}, {4.0, 1, 0, 2} };
        case 40: return { {0, 1, 0, 0.05}, {2.0, 1, 0, 0.05} };   // ★ not annex B: channel 4 nearly frozen (diagnostics)
        case 41: return { {0, 1, 0, 0.3}, {2.0, 1, 0, 0.3} };
        case 50: return { {0, 1, 0, 0}, {4.0, 0.9, 0, 0} };   // ★ not annex B: channel 5's delay without its fading (diagnostics)
        default: return { {0, 1, 0, 0} };
    }
}

/** ★ A WSSUS tapped delay line (equation B.1): each path a Rayleigh process with a Gaussian Doppler spectrum (Dsp = 2σ),
 *  made as a sum of sinusoids with Gaussian frequencies and random phases; a zero-spread path is a fixed tap. Then a
 *  frequency offset, a sample-clock error (ppm, by cubic interpolation) and AWGN at `snrDb` in the occupied band. */
class Channel {
public:
    Channel(int profile, double snrDb, double freqOffsetHz, double clockPpm, double bandwidthHz, uint32_t seed)
        : paths_(channelProfile(profile)), snrDb_(snrDb), fo_(freqOffsetHz), ppm_(clockPpm), bw_(bandwidthHz), rng_(seed) {
        std::normal_distribution<double> g(0.0, 1.0);
        std::uniform_real_distribution<double> u(0.0, 2.0 * M_PI);
        for (auto& p : paths_) {
            Tap t; t.delay = int(std::lround(p.delayMs * 1e-3 * kFs));
            t.fixed = p.dopplerSpreadHz <= 0.0;
            for (int i = 0; i < kSines; ++i) { t.f[i] = p.dopplerShiftHz + g(rng_) * p.dopplerSpreadHz / 2.0; t.ph[i] = u(rng_); }
            t.gain = p.gain; t.shift = p.dopplerShiftHz;
            taps_.push_back(t);
            maxDelay_ = std::max(maxDelay_, t.delay);
        }
        hist_.assign(size_t(maxDelay_ + 1), cf(0, 0));
    }
    std::vector<cf> run(const std::vector<cf>& in) {
        std::vector<cf> y(in.size());
        for (size_t n = 0; n < in.size(); ++n) {
            hist_[size_t(hp_)] = in[n];
            const double t = double(t0_ + int64_t(n)) / kFs;
            cf acc(0, 0);
            for (const Tap& tp : taps_) {
                cf c;
                if (tp.fixed) c = cf(float(tp.gain), 0) * std::polar(1.0f, float(2.0 * M_PI * tp.shift * t));
                else {
                    double re = 0, im = 0;
                    for (int i = 0; i < kSines; ++i) { const double a = 2.0 * M_PI * tp.f[i] * t + tp.ph[i]; re += std::cos(a); im += std::sin(a); }
                    const double sc = tp.gain / std::sqrt(double(kSines));
                    c = cf(float(re * sc), float(im * sc));
                }
                acc += c * hist_[size_t((hp_ - tp.delay + int(hist_.size())) % int(hist_.size()))];
            }
            hp_ = (hp_ + 1) % int(hist_.size());
            y[n] = acc * std::polar(1.0f, float(2.0 * M_PI * fo_ * t));
        }
        t0_ += int64_t(in.size());
        // clock error: resample by (1 + ppm·1e-6)
        if (ppm_ != 0.0) {
            for (const cf& v : y) rs_.push_back(v);
            std::vector<cf> o;
            const double step = 1.0 + ppm_ * 1e-6;
            while (rsPos_ + 2.0 < double(rs_.size())) {
                const int i = int(rsPos_); const float mu = float(rsPos_ - i);
                const cf y0 = i > 0 ? rs_[size_t(i - 1)] : rs_[0], y1 = rs_[size_t(i)], y2 = rs_[size_t(i + 1)], y3 = rs_[size_t(i + 2)];
                const cf a0 = y3 - y2 - y0 + y1, a1 = y0 - y1 - a0, a2 = y2 - y0, a3 = y1;
                o.push_back(((a0 * mu + a1) * mu + a2) * mu + a3);
                rsPos_ += step;
            }
            const int drop = std::max(0, int(rsPos_) - 2);
            rs_.erase(rs_.begin(), rs_.begin() + drop); rsPos_ -= drop;
            y.swap(o);
        }
        // AWGN: signal power measured on the clean input, noise scaled to the occupied band
        double ps = 0; for (const cf& v : in) ps += std::norm(v);
        ps /= std::max<size_t>(1, in.size());
        if (sigPow_ <= 0) sigPow_ = ps;
        const double nTotal = sigPow_ / std::pow(10.0, snrDb_ / 10.0) * (kFs / bw_);
        std::normal_distribution<float> g(0.0f, float(std::sqrt(nTotal / 2.0)));
        for (cf& v : y) v += cf(g(rng_), g(rng_));
        return y;
    }
private:
    static constexpr int kSines = 24;
    struct Tap { int delay = 0; bool fixed = true; double gain = 1, shift = 0; double f[kSines]{}, ph[kSines]{}; };
    std::vector<PathSpec> paths_;
    std::vector<Tap> taps_;
    double snrDb_, fo_, ppm_, bw_;
    std::mt19937 rng_;
    int maxDelay_ = 0, hp_ = 0;
    std::vector<cf> hist_;
    int64_t t0_ = 0;
    std::vector<cf> rs_; double rsPos_ = 1.0;
    double sigPow_ = 0;
};

}  // namespace vibedrm
