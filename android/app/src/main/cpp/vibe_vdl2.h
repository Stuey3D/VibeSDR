// vibe_vdl2.h — V12 VDL Mode 2: every 25 kHz channel in the capture from one shared channeliser, D8PSK at 10.5 ksym/s,
// the burst (header, RS, interleave, HDLC), AVLC and ACARS over AVLC. docs/v12/AVIATION.md §VDL2.
// The coding layers are vibe_vdl2_fec.h; the front end vibe_avi_front.h; the ACARS block parser vibe_acars.h.
//
// ★★★ FROM ETSI EN 301 841-1/-2 (free): D8PSK, phase CHANGES of k·π/4 carrying 3 bits (Gray), 10 500 symbols/s, raised
//     cosine α = 0.6; a burst = ramp-up, a 16-symbol unique word, the header, the data. The unique word's symbols are
//     published (Jamal & Matolak, arXiv:2005.11456); the Gray table and the paid-only parameters are facts read in
//     dumpvdl2 (GPL-3.0-or-later, read only). Written fresh.
// ★★ THE RECEIVER, per channel at exactly 4 samples a symbol (42 kS/s, a cubic resampler from the channel rate):
//    1. a root-raised-cosine matched filter (α 0.6; vibedsp FirDecimator: NEON / SSE2 / scalar);
//    2. a cheap POWER GATE: nothing else runs on an idle channel (VDL2 is CSMA; most channels are quiet most of the time);
//    3. SYNC by the unique word's sixteen phase steps, correlated against y[n]·y*[n−4] — the magnitude finds the burst,
//       the angle IS the carrier's frequency error (no regression), a parabola through three samples the fine timing;
//    4. symbols read by differential detection with the frequency error taken out and tracked decision-directed, plus a
//       Gardner timing loop (a burst may run 4 s);
//    5. each symbol's margin to its decision boundary is kept: the weakest bytes become RS ERASURES when errors alone
//       do not decode a block.
#pragma once
#include "vibe_avi_front.h"
#include "vibe_vdl2_fec.h"
#include "vibe_vdl2_simd.h"
#include "vibe_acars.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <memory>
#include <string>
#include <vector>

namespace vibevdl2 {

using cf32 = vibedsp::cf32;
static constexpr double kSymRate = 10500.0;
static constexpr int kSps = 4;
static constexpr double kWorkRate = kSymRate * kSps;   ///< 42 kS/s
static constexpr double kCscHz = 136.975e6;           ///< the Common Signalling Channel

/** The unique word's 16 symbols (3 bits each, as on the air). */
static constexpr uint8_t kUw[16] = { 0b000, 0b010, 0b011, 0b110, 0b000, 0b001, 0b101, 0b110,
                                     0b001, 0b100, 0b011, 0b111, 0b101, 0b111, 0b100, 0b010 };
/** Gray: phase step k (×π/4) → the 3 bits; and back. */
inline int grayOf(int k) { return k ^ (k >> 1); }
inline int stepOf(int g) { int k = g; for (int s = g >> 1; s; s >>= 1) k ^= s; return k & 7; }

struct Tuning { float gate = 2.0f; int interpTaps = 12; float kaiserBeta = 6.f; int fdShape = 0; bool polyInterp = true; float binHz = 300.f; bool fdFilter = true; bool deepStop = true; float rxBw = 4800.f, rxTr = 4500.f; int rxKind = 0; bool rrc = false; bool erasures = true; bool gardner = true; bool coherent = true; float pllA = 0.15f, pllB = 0.004f; float syncQ = 0.75f; float freqGain = 0.03f; float tGain = 0.01f; };
inline Tuning& tuning() { static Tuning t; return t; }
inline bool debug() { static const bool d = std::getenv("VDL2_DEBUG") != nullptr; return d; }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  AVLC
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Addr { uint32_t addr = 0; int type = 0; int status = 0; };
/** Address field: 4 octets, 7 bits each (bit 0 = the HDLC extension bit), sent most significant first. */
inline Addr parseAddr(const uint8_t* b) {
    const uint32_t raw = uint32_t(b[0] >> 1) | (uint32_t(b[1] >> 1) << 7) | (uint32_t(b[2] >> 1) << 14) | (uint32_t(b[3] >> 1) << 21);
    uint32_t r = 0;
    for (int i = 0; i < 28; ++i) r |= ((raw >> i) & 1u) << (27 - i);
    Addr a; a.addr = r & 0xFFFFFFu; a.type = int((r >> 24) & 7u); a.status = int((r >> 27) & 1u);
    return a;
}
inline const char* addrKind(int t) { return t == 1 ? "aircraft" : (t == 4 || t == 5) ? "ground" : t == 7 ? "all" : "reserved"; }

struct Frame {
    std::vector<uint8_t> bytes;        ///< the AVLC frame, FCS removed
    Addr src, dst;
    bool cr = false;                   ///< command (0) / response (1): the destination's status bit
    bool airborne = true;              ///< the source's status bit (0 = airborne)
    uint8_t ctrl = 0;
    char kind = '?';                   ///< 'I' | 'S' | 'U'
    std::string uName;                 ///< U frames: XID, UI, DM, …
    bool hasAcars = false;
    vibeacars::Block acars;            ///< ACARS over AVLC (BCS-checked)
    bool acarsCrcOk = false;
    int channel = -1; double hz = 0, t = 0;
    float levelDb = -120, snrDb = 0, freqErrHz = 0;
    int rsFixed = 0, hdrFixed = 0, erased = 0;
};

inline bool parseFrame(const std::vector<uint8_t>& f, Frame& o) {
    if (f.size() < 11) return false;
    o.bytes.assign(f.begin(), f.end() - 2);
    o.dst = parseAddr(&f[0]);
    o.src = parseAddr(&f[4]);
    o.cr = o.dst.status; o.airborne = o.src.status == 0;
    o.ctrl = f[8];
    const uint8_t* info = &f[9];
    const size_t ni = f.size() - 2 - 9;
    if ((o.ctrl & 1) == 0) {
        o.kind = 'I';
        if (ni > 3 && info[0] == 0xFF && info[1] == 0xFF && info[2] == 0x01) {
            // ACARS: mode … suffix, BCS, DEL — the VHF block after SOH
            std::vector<uint8_t> a(info + 3, info + ni);
            if (a.size() >= 16 && (a.back() & 0x7F) == vibeacars::DEL) {
                a.pop_back();
                o.acarsCrcOk = vibeacars::bcs(a.data(), a.size()) == 0;
                std::vector<uint8_t> b;
                for (size_t i = 0; i + 2 < a.size(); ++i) b.push_back(a[i] & 0x7F);
                o.hasAcars = vibeacars::parse(b, o.acars);
            }
        }
    } else if ((o.ctrl & 3) == 1) o.kind = 'S';
    else {
        o.kind = 'U';
        switch (o.ctrl & 0xEF) {        // without the P/F bit
            case 0x03: o.uName = "UI"; break;   case 0x0F: o.uName = "DM"; break;   case 0x43: o.uName = "DISC"; break;
            case 0x63: o.uName = "UA"; break;   case 0x87: o.uName = "FRMR"; break; case 0xAF: o.uName = "XID"; break;
            case 0xE3: o.uName = "TEST"; break; default: o.uName = "U"; break;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ONE CHANNEL
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Stats { uint64_t searched = 0, samples = 0, syncs = 0, hdrBad = 0, bursts = 0, rsFail = 0, frames = 0, fcsBad = 0, rsFixed = 0, erasureSaves = 0; };

class Demod {
public:
    explicit Demod(double fs) : fs_(fs), step_(fs / kWorkRate) {
        // the matched filter: root raised cosine, α = 0.6, ±6 symbols at 4 samples a symbol
        const double a = 0.6;
        std::vector<float> h;
        for (int i = -6 * kSps; i <= 6 * kSps; ++i) {
            const double t = double(i) / kSps;
            double v;
            if (i == 0) v = 1 - a + 4 * a / M_PI;
            else if (std::fabs(std::fabs(4 * a * t) - 1) < 1e-9) v = a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
            else v = (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - (4 * a * t) * (4 * a * t)));
            h.push_back(float(v));
        }
        /* ★★ MEASURED: THE TRANSMIT PULSE IS A FULL RAISED COSINE, so a root-raised-cosine here leaves ISI (±0.5-step
         *  residuals on a 33 dB burst, its FCS failed). Without a filter the channeliser's slice passes 3.6× the symbol
         *  rate of noise (5.5 dB more than needed); the RC itself (the textbook matched filter) is worse still (RC·RC is not
         *  Nyquist). A plain low-pass between the two wins — flat to 5 kHz, shut by 9 kHz (−6 dB at 7 kHz), swept on both
         *  recordings: no filter 301 / 126 frames, 9/3 kHz 311 / 132, 7/4 347 / 142, 6/4 363 / 153, 5/4 381 / 159,
         *  4.5/4 362 / 151 (Pi 500 / Pi 2). */
        if (tuning().rxKind == 1) {   // the transmit pulse itself (raised cosine, α 0.6): the matched filter, with ISI
            h.clear();
            for (int i = -4 * kSps; i <= 4 * kSps; ++i) {
                const double t = double(i) / kSps, x = 4 * a * a * t * t;
                const double sc = i == 0 ? 1 : std::sin(M_PI * t) / (M_PI * t);
                h.push_back(float(std::fabs(1 - x) < 1e-6 ? M_PI / 4 * std::sin(M_PI / (2 * a)) / (M_PI / (2 * a)) : sc * std::cos(M_PI * a * t) / (1 - x)));
            }
            float s = 0; for (float v : h) s += v; for (float& v : h) v /= s;
        } else if (!tuning().rrc) h = vibedsp::designLowpass(double(tuning().rxBw + 0.5f * tuning().rxTr) / kWorkRate, double(tuning().rxTr) / kWorkRate, tuning().deepStop);
        else { float s = 0; for (float v : h) s += v * v; for (float& v : h) v /= std::sqrt(s); }
        if (tuning().fdFilter && !tuning().rrc && tuning().rxKind == 0) h.assign(1, 1.0f);   // ★ the low-pass is the slicer's (Receiver)
        mf_.reset(new vibedsp::FirDecimator(h, 1));
        passThrough_ = h.size() == 1;
        // the unique word's expected step between symbols i−1 and i (i = 0: from the ramp-up's 000)
        for (int i = 0; i < 16; ++i) {
            const int k = stepOf(kUw[i]);
            uwConj_[i] = std::polar(1.0f, -float(k) * float(M_PI / 4));
        }
    }
    void setChannel(int c, double hz) { chan_ = c; hz_ = hz; }
    const Stats& stats() const { return st_; }
    float floorDb() const { return 10 * std::log10(floor_ + 1e-20f); }

    template <class Out> void feed(const cf32* z, int n, Out&& out) {
        // 1. resample to 42 kS/s and filter
        if (tuning().polyInterp) resamplePoly_(z, n);
        else {
            rs_.clear();
            for (int i = 0; i < n; ++i) {
                h_[0] = h_[1]; h_[1] = h_[2]; h_[2] = h_[3]; h_[3] = z[i];
                while (mu_ < 1.0) {
                    const float m = float(mu_);
                    const cf32 p0 = h_[0], p1 = h_[1], p2 = h_[2], p3 = h_[3];
                    const cf32 a0 = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3, a1 = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3,
                               a2 = -0.5f * p0 + 0.5f * p2;
                    rs_.push_back(((a0 * m + a1) * m + a2) * m + p1);
                    mu_ += step_;
                }
                mu_ -= 1.0;
            }
        }
        if (passThrough_) { for (const cf32& v : rs_) sample_(v, out); return; }
        y_.resize(rs_.size() + 4);
        const int k = mf_->process(rs_.data(), int(rs_.size()), y_.data());
        for (int i = 0; i < k; ++i) sample_(y_[size_t(i)], out);
    }

private:
    /* ★★ THE RESAMPLER IS A FILTER TOO (2026-11-01). Catmull–Rom's 4 taps let the images of a ±9 kHz signal at 37.5 kS/s
     *  fold back to −4.5…−9 kHz of the 42 kS/s stream at only ~25 dB down; the 58-tap FIR after it used to clean that up.
     *  With the low-pass now in the slicer, the interpolator must reject the images itself: an 8-tap windowed sinc
     *  (cut at the input's Nyquist, Kaiser β 6), 256 phases, the nearest phase per output. 8 complex MACs an output,
     *  NEON / SSE2 (vibeavi::dot8). */
    void resamplePoly_(const cf32* z, int n) {
        if (pt_.empty()) {
            pt_.resize(size_t(kPh) * size_t(nt_()));
            auto bessel0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 25; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
            const double beta = double(tuning().kaiserBeta);
            for (int p = 0; p < kPh; ++p) {
                const double mu = p / double(kPh);
                double sum = 0;
                const int NT = nt_(), H = NT / 2;
                for (int j = 0; j < NT; ++j) {
                    const double t = double(j - (H - 1)) - mu;         // taps at −(H−1)…+H around the output point
                    const double sc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(M_PI * t) / (M_PI * t);
                    const double r = t / double(H);
                    const double w = std::fabs(r) <= 1 ? bessel0(beta * std::sqrt(1 - r * r)) / bessel0(beta) : 0;
                    pt_[size_t(p * NT + j)] = float(sc * w); sum += sc * w;
                }
                for (int j = 0; j < NT; ++j) pt_[size_t(p * NT + j)] = float(pt_[size_t(p * NT + j)] / sum);
            }
        }
        // the input with 7 samples of history in front
        const int NT = nt_(), HB = NT - 1;
        if (int(hist_.size()) < HB) hist_.assign(size_t(HB), cf32(0, 0));
        hist_.resize(size_t(HB) + size_t(n));
        std::copy(z, z + n, hist_.begin() + HB);
        rs_.clear();
        /* ★ A RATIONAL RATE (every rate the front end makes: 37.5 → 42 kS/s is 28/25, 32 → 42 is 21/16): the output
         *  positions repeat every L outputs, so each phase has its own EXACT taps, the position is an integer count of
         *  1/L samples (no double → int per output), and the 12-tap dot product is vibevdl2::dot12 (NEON / SSE2). */
        if (ratL_ == 0) {
            const long long fi = std::llround(fs_), wi = std::llround(kWorkRate);
            const long long g = std::gcd(fi, wi);
            if (std::fabs(fs_ - double(fi)) < 1e-6 && NT == 12 && wi / g <= 1024) {
                ratL_ = int(wi / g); ratM_ = int(fi / g);
                rt_.resize(size_t(ratL_) * 12);
                auto bessel0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 25; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
                const double beta = double(tuning().kaiserBeta);
                for (int r = 0; r < ratL_; ++r) {
                    const double mu = double(r) / ratL_;
                    double sum = 0, v[12];
                    for (int j = 0; j < 12; ++j) {
                        const double t = double(j - 5) - mu, sc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(M_PI * t) / (M_PI * t), q = t / 6.0;
                        v[j] = sc * (std::fabs(q) <= 1 ? bessel0(beta * std::sqrt(1 - q * q)) / bessel0(beta) : 0); sum += v[j];
                    }
                    for (int j = 0; j < 12; ++j) rt_[size_t(r * 12 + j)] = float(v[j] / sum);
                }
            } else ratL_ = -1;
        }
        if (ratL_ > 0) {
            rs_.resize(size_t(double(n) * ratL_ / ratM_) + 4);
            size_t k = 0;
            // ★ base + r/L, stepped by M/L without a divide (a 64-bit divide per output was 20 % of VDL2 on ARMv7)
            while (rBase_ < n) {
                rs_[k++] = dot12(rt_.data() + rPh_ * 12, hist_.data() + rBase_);
                rPh_ += ratM_;
                while (rPh_ >= ratL_) { rPh_ -= ratL_; ++rBase_; }
            }
            rs_.resize(k);
            rBase_ -= n;
            std::copy(hist_.end() - HB, hist_.end(), hist_.begin());
            hist_.resize(size_t(HB));
            return;
        }
        // output k sits at input position pos (relative to hist_[3] = the first of the last-call's tail): taps hist_[i..i+7]
        while (pos_ < double(n)) {
            const int i = int(pos_);
            int ph = int((pos_ - i) * kPh + 0.5), base = i;
            if (ph == kPh) { ph = 0; ++base; }
            rs_.push_back(NT == 8 ? vibeavi::dot8(pt_.data() + ph * 8, hist_.data() + base)
                                  : vibedsp::dotCplx(pt_.data() + size_t(ph) * size_t(NT), reinterpret_cast<const float*>(hist_.data() + base), NT));
            pos_ += step_;
        }
        pos_ -= double(n);
        std::copy(hist_.end() - HB, hist_.end(), hist_.begin());
        hist_.resize(size_t(HB));
    }
    static int nt_() { return tuning().interpTaps; }
    static constexpr int kPh = 256;
    std::vector<float> pt_;
    std::vector<cf32> hist_;
    double pos_ = 0;
    int ratL_ = 0, ratM_ = 1, rBase_ = 0, rPh_ = 0; std::vector<float> rt_;
    bool passThrough_ = false;
    static constexpr int kRing = 512;      // samples kept (12 ms)
    cf32 at_(int64_t n) const { return ring_[size_t(n & (kRing - 1))]; }
    /** y at a fractional time (samples, absolute): linear between the two neighbours (4 samples a symbol). */
    cf32 atf_(double t) const {
        const int64_t i = int64_t(std::floor(t));
        const float f = float(t - double(i));
        return at_(i) * (1 - f) + at_(i + 1) * f;
    }

    template <class Out> void sample_(cf32 v, Out& out) {
        const int64_t n = ++n_;
        ring_[size_t(n & (kRing - 1))] = v;
        /* ★ THE LAG PRODUCTS AND THE SYNC CORRELATOR RUN IN GROUPS OF FOUR SAMPLES (NEON / SSE2: lagProd4, corr16x4).
         *  A group is worked when its last sample arrives, so the search sees each sample up to 3 samples late — the same
         *  numbers, decided a little later; the first data symbol is still ≥ 1 sample in the future then. Groups are
         *  4-aligned and every lag is a multiple of 4, so no group crosses the ring's end. */
        const bool groupDone = (n & 3) == 3;
        if (groupDone) {
            const size_t g0 = size_t((n - 3) & (kRing - 1)), gl = size_t((n - 3 - kSps) & (kRing - 1));
            lagProd4(&ring_[g0], &ring_[gl], &prod_[g0], &absP_[g0]);
        }
        /* power gate: 64-sample energy against the MEAN of the quiet windows (it was a minimum-tracker over 16 samples:
         *  noise's own spread kept it crossing 2.5× its low tail, and the sync search ran 97 % of the time on an idle
         *  channel — 54 % of the VDL2 bench). A burst is ≥ 3 dB over the channel's noise here; the hold covers 4
         *  windows (6 symbols) so the unique word is searched across the edge. */
        pw_ += std::norm(v);
        if (++pwN_ == 64) {
            const float e = pw_ / 64;
            if (floor_ <= 0) floor_ = e;
            else if (e < tuning().gate * floor_) floor_ += (e - floor_) * 0.02f;
            else floor_ *= 1.0002f;                          // a channel that stays busy still lets its floor rise
            lastPw_ = e;
            active_ = e > tuning().gate * floor_ ? 4 : std::max(0, active_ - 1);
            pw_ = 0; pwN_ = 0;
        }
        ++st_.samples;
        if (state_ == kSearch) {
            if (!groupDone) return;
            if (!active_ && !cand_) return;
            st_.searched += 4;
            int g[16];
            for (int i = 0; i < 16; ++i) g[i] = int((n - 3 - int64_t(kSps) * (15 - i)) & (kRing - 1));
            float dr[4], di[4], sm[4], mn[4];
            corr16x4(prod_, absP_, g, uwConj_, dr, di, sm, mn);
            for (int l = 0; l < 4 && state_ == kSearch; ++l) searchAt_(n - 3 + l, cf32(dr[l], di[l]), sm[l], mn[l]);
            return;
        }
        if (state_ == kData && double(n) >= nextT_ + 1.0) symbol_(out);
    }

    /** ★ SYNC: D(n) = Σ_i prod(n − 4·(15−i))·conj(step_i); Q = |D| / Σ|prod|. A burst = Q above the threshold at its peak
     *  (the sample after shows it falling). */
    void searchAt_(int64_t n, cf32 d, float s, float mn) {
        /* ★★ EVERY STEP MUST BE THERE. |D|/Σ|p| alone is ~1 whenever ONE product dominates — the burst's own leading
         *  edge, where one symbol × another is far above the noise × noise terms (it synced 40 000 times on noise and
         *  edges in the first run). A real unique word has all sixteen products of a similar size. */
        const float q = (s > 0 && mn > 0.2f * s / 16) ? std::abs(d) / s : 0;
        /* ★ THE BEST PEAK, NOT THE FIRST. The window slides into a burst one sample at a time and a partial alignment
         *  (a symbol early, the ramp-up's steady phase standing in for the first steps) can cross the threshold before
         *  the true word arrives. A candidate is held for 6 samples (1.5 symbols) and the highest Q in that span wins; the
         *  parabola is through its neighbours. */
        if (cand_ > 0) {
            if (q > bestQ_) { bestQ_ = q; bestN_ = n; bestD_ = d; bestL_ = qPrev_; bestR_ = -1; }
            else if (n == bestN_ + 1) bestR_ = q;
            if (n - candAt_ >= 6 && bestR_ >= 0) {
                const float den = bestL_ - 2 * bestQ_ + bestR_;
                const float frac = std::fabs(den) > 1e-9f ? 0.5f * (bestL_ - bestR_) / den : 0;
                start_(double(bestN_) + std::max(-0.5f, std::min(0.5f, frac)), bestD_);
                cand_ = 0; qPrev_ = 0;
                return;
            }
        } else if (q > tuning().syncQ) {
            cand_ = 1; candAt_ = n; bestQ_ = q; bestN_ = n; bestD_ = d; bestL_ = qPrev_; bestR_ = -1;
        }
        qPrev2_ = qPrev_; qPrev_ = q; dPrev_ = d;
    }

    void start_(double tLast, cf32 d) {
        st_.syncs++;
        state_ = kData;
        w_ = std::arg(d);                              // radians per symbol: the carrier's frequency error
        prevT_ = tLast; prevZ_ = atf_(tLast);
        nextT_ = tLast + kSps;
        bits_.clear(); rel_.clear();
        need_ = 25; hdrDone_ = false;
        sigPw_ = 0; sigN_ = 0;
        syncAt_ = n_;
        /* ★★ THE CARRIER'S PHASE FROM THE UNIQUE WORD (coherent mode). The 16 symbols' absolute points are known up to
         *  one common rotation: point(i) = Σ steps up to i (the ramp-up's 000 before them is point 0). Each symbol, turned
         *  back by its point and by the frequency error over the distance to the last, votes for the phase at the last. */
        int a = 0;
        cf32 acc(0, 0);
        for (int i = 0; i < 16; ++i) {
            a = (a + stepOf(kUw[i])) & 7;
            const cf32 zi = atf_(tLast - double(kSps) * (15 - i));
            acc += zi * std::polar(1.0f, -float(a) * float(M_PI / 4) + w_ * float(15 - i));
        }
        phi_ = std::arg(acc) + float(a) * float(M_PI / 4);   // the carrier's phase at the last UW symbol, point a included
        aPrev_ = a;
        phi_ -= float(a) * float(M_PI / 4);                  // so that point = round((arg z − phi)/(π/4))
        prevMargin_ = float(M_PI / 8);
    }

    template <class Out> void symbol_(Out& out) {
        const double t = nextT_;
        const cf32 z = atf_(t);
        // ★ Gardner: the mid-point between this symbol and the last, against their difference
        if (tuning().gardner) {
            const cf32 mid = atf_(t - kSps / 2.0);
            const float e = ((prevZ_ - z) * std::conj(mid)).real() / (std::norm(z) + std::norm(prevZ_) + 1e-12f);
            tAdj_ = std::max(-0.5, std::min(0.5, double(tuning().tGain) * e * 4));
        } else tAdj_ = 0;
        int k; float r;
        if (tuning().coherent) {
            /* ★★★ COHERENT: the symbol's own point, against a decision-directed carrier (phase + frequency loop), and the
             *  data step = this point − the last. Its noise is the symbol's alone (√2 less than z·z*prev), which at
             *  15–20 dB is the difference between ~5 % and ~0.5 % wrong symbols — RS(255,249) mends only the second. */
            phi_ += w_;
            const float ang = std::arg(z * std::polar(1.0f, -phi_));
            int a = int(std::lround(ang / float(M_PI / 4)));
            const float res = ang - float(a) * float(M_PI / 4);
            a &= 7;
            phi_ += tuning().pllA * res;
            w_ += tuning().pllB * res;
            if (phi_ > float(M_PI)) phi_ -= float(2 * M_PI); else if (phi_ < -float(M_PI)) phi_ += float(2 * M_PI);
            k = (a - aPrev_) & 7;
            aPrev_ = a;
            const float m = float(M_PI / 8) - std::fabs(res);
            r = std::min(m, prevMargin_);                   // a step is as weak as the weaker of its two points
            prevMargin_ = m;
        } else {
            cf32 dz = z * std::conj(prevZ_) * std::polar(1.0f, -w_);
            const float ang = std::arg(dz);
            k = int(std::lround(ang / float(M_PI / 4)));
            const float res = ang - float(k) * float(M_PI / 4);
            k = ((k % 8) + 8) % 8;
            w_ += tuning().freqGain * res;
            r = float(M_PI / 8) - std::fabs(res);           // margin to the decision boundary
        }
        sigPw_ += std::norm(z); ++sigN_;
        const int g = grayOf(k);
        for (int b = 2; b >= 0; --b) { bits_.push_back(uint8_t((g >> b) & 1)); rel_.push_back(r); }
        prevZ_ = z; prevT_ = t;
        nextT_ = t + kSps + tAdj_;
        if (int(bits_.size()) >= need_) progress_(out);
        if (int(bits_.size()) > 8 * 2200 * 8) state_ = kSearch;   // runaway guard
    }

    template <class Out> void progress_(Out& out) {
        if (!hdrDone_) {
            Lfsr l;
            uint32_t w = 0;
            for (int i = 0; i < 25; ++i) w = (w << 1) | uint32_t(bits_[size_t(i)] ^ l.next());
            w &= (1u << 22) - 1;                   // the reserved bits are zero by definition
            const int fixed = hdrCorrect(w);
            const uint32_t len = fixed < 0 ? 0 : hdrLength(w);
            if (fixed < 0 || len < 8 * 11 || len > (fixed ? 0x1FFFu : 0x3FFFu)) {
                st_.hdrBad++; state_ = kSearch;
                if (debug()) std::fprintf(stderr, "  [vdl2 ch%d t=%.3f] header rejected (fixed %d len %u)\n", chan_, double(n_) / kWorkRate, fixed, len);
                return;
            }
            hdrFixed_ = fixed; len_ = int(len);
            const Layout L((len_ + 7) / 8);
            need_ = 25 + 8 * (L.octets + L.fecOctets);
            hdrDone_ = true;
            return;
        }
        decode_(out);
        state_ = kSearch;
    }

    template <class Out> void decode_(Out& out) {
        st_.bursts++;
        if (debug()) {
            double m = 0; for (size_t i = 0; i < rel_.size(); i += 3) m += rel_[i];
            std::fprintf(stderr, "  [vdl2 ch%d t=%.3f] burst len %d bits, snr %.1f dB, hdr fixed %d, mean margin %.1f deg, w %.3f, symbols:", chan_, double(syncAt_) / kWorkRate, len_,
                         10 * std::log10((sigN_ ? sigPw_ / float(sigN_) : 0) / (floor_ + 1e-20f) + 1e-20f), hdrFixed_, m / (rel_.size() / 3) * 180 / M_PI, w_);
            for (size_t i = 0; i < rel_.size() && i < 90; i += 3) std::fprintf(stderr, " %.0f", (M_PI / 8 - rel_[i]) * 180 / M_PI);
            std::fprintf(stderr, "\n");
        }
        const Layout L((len_ + 7) / 8);
        // descramble everything after the unique word, then the octets (LSB first) with their weakest margin
        Lfsr l;
        std::vector<uint8_t> b(bits_.size());
        for (size_t i = 0; i < bits_.size(); ++i) b[i] = bits_[i] ^ uint8_t(l.next());
        const int nOct = L.octets + L.fecOctets;
        std::vector<uint8_t> oct(size_t(nOct), 0);
        std::vector<float> orel(size_t(nOct), 1e9f);
        for (int o = 0; o < nOct; ++o)
            for (int j = 0; j < 8; ++j) {
                const size_t bi = 25 + size_t(o) * 8 + size_t(j);
                oct[size_t(o)] |= uint8_t(b[bi] << j);
                orel[size_t(o)] = std::min(orel[size_t(o)], rel_[bi]);
            }
        // deinterleave into blocks
        std::vector<std::array<uint8_t, 255>> blk(size_t(L.blocks));
        std::vector<std::array<float, 255>> brel(size_t(L.blocks));
        for (auto& x : blk) x.fill(0);
        for (auto& x : brel) x.fill(1e9f);
        dealColumns(L.octets, L.blocks, Rs::K, L.lastLen, 0, [&](int r, int c, int i) { blk[size_t(r)][size_t(c)] = oct[size_t(i)]; brel[size_t(r)][size_t(c)] = orel[size_t(i)]; });
        const int fecRows = L.lastPar ? L.blocks : L.blocks - 1;
        dealColumns(L.fecOctets, fecRows, Rs::R, L.lastPar ? L.lastPar : Rs::R, Rs::K,
                    [&](int r, int c, int i) { blk[size_t(r)][size_t(c)] = oct[size_t(L.octets + i)]; brel[size_t(r)][size_t(c)] = orel[size_t(L.octets + i)]; });
        // RS per block
        const Rs& rs = Rs::get();
        int fixedTot = 0, erasedTot = 0;
        for (int r = 0; r < L.blocks; ++r) {
            const int len = L.lenOf(r), par = L.parOf(r);
            if (par == 0) continue;
            auto valid = [&](int i) { return i < len || i >= Rs::K; };   // data, parity, and the erased missing parity
            int eras[6], ne = 0;
            for (int i = Rs::K + par; i < Rs::N; ++i) eras[ne++] = i;     // the parity this short block never had
            std::array<uint8_t, 255> cw = blk[size_t(r)];
            int res = rs.decode(cw.data(), eras, ne, valid);
            if (res < 0 && tuning().erasures) {
                // ★ the weakest bytes erased, 2 at a time, as far as the parity allows
                std::vector<std::pair<float, int>> w;
                for (int i = 0; i < Rs::N; ++i) if (valid(i)) w.push_back({ brel[size_t(r)][size_t(i)], i });
                std::sort(w.begin(), w.end());
                for (int extra = 2; extra + ne <= Rs::R - 2 && res < 0; extra += 2) {   // ★ ≥ 2 checks always left: 6 erasures 'decode' anything
                    int e2[6], n2 = ne;
                    std::copy(eras, eras + ne, e2);
                    for (int q = 0; q < extra && q < int(w.size()); ++q) e2[n2++] = w[size_t(q)].second;
                    cw = blk[size_t(r)];
                    res = rs.decode(cw.data(), e2, n2, valid);
                    if (res >= 0) { erasedTot += extra; st_.erasureSaves++; }
                }
            }
            if (res < 0) {
                st_.rsFail++;
                if (debug()) std::fprintf(stderr, "  [vdl2 ch%d t=%.3f] RS block %d/%d failed (len %d par %d)\n", chan_, double(n_) / kWorkRate, r, L.blocks, len, par);
                return;
            }
            for (int i = Rs::K + par; i < Rs::N; ++i) if (cw[size_t(i)] != blk[size_t(r)][size_t(i)]) --res;   // the missing parity, filled
            fixedTot += res;
            if (debug()) {
                std::fprintf(stderr, "  [vdl2 ch%d t=%.3f] len %d bits: RS block %d/%d len %d par %d fixed %d at", chan_, double(syncAt_) / kWorkRate, len_, r, L.blocks, len, par, res);
                for (int i = 0; i < Rs::N; ++i) if (cw[size_t(i)] != blk[size_t(r)][size_t(i)] && valid(i) && !(i >= Rs::K + par)) std::fprintf(stderr, " %d(m%.0f)", i, brel[size_t(r)][size_t(i)] * 180 / M_PI);
                std::fprintf(stderr, "\n");
            }
            blk[size_t(r)] = cw;
        }
        st_.rsFixed += uint64_t(fixedTot);
        // the data bits, LSB first, cut to the transmission length; then the HDLC frames
        std::vector<uint8_t> db;
        db.reserve(size_t(len_));
        for (int r = 0; r < L.blocks && int(db.size()) < len_; ++r)
            for (int c = 0; c < L.lenOf(r) && int(db.size()) < len_; ++c)
                for (int j = 0; j < 8 && int(db.size()) < len_; ++j) db.push_back(uint8_t((blk[size_t(r)][size_t(c)] >> j) & 1));
        const auto frames = hdlcFrames(db);
        const float sig = sigN_ ? sigPw_ / float(sigN_) : 0;
        for (const auto& f : frames) {
            if (!fcsGood(f.data(), f.size())) {
                st_.fcsBad++;
                if (debug()) { std::fprintf(stderr, "  [vdl2 ch%d] FCS bad, %zu bytes:", chan_, f.size()); for (uint8_t x : f) std::fprintf(stderr, " %02x", x); std::fprintf(stderr, "\n"); }
                continue;
            }
            Frame fr;
            if (!parseFrame(f, fr)) continue;
            fr.channel = chan_; fr.hz = hz_; fr.t = double(syncAt_) / kWorkRate;
            fr.levelDb = 10 * std::log10(sig + 1e-20f);
            fr.snrDb = 10 * std::log10((sig + 1e-20f) / (floor_ + 1e-20f));
            fr.freqErrHz = float(w_ / (2 * M_PI) * kSymRate);
            fr.rsFixed = fixedTot; fr.hdrFixed = hdrFixed_; fr.erased = erasedTot;
            st_.frames++;
            out(fr);
        }
    }

    enum { kSearch, kData };
    double fs_, step_, mu_ = 0;
    cf32 h_[4] = {};
    std::vector<cf32> rs_, y_;
    std::unique_ptr<vibedsp::FirDecimator> mf_;
    alignas(16) cf32 ring_[kRing] = {}, prod_[kRing] = {};
    alignas(16) float absP_[kRing] = {};
    cf32 uwConj_[16];
    int64_t n_ = 0, syncAt_ = 0;
    float pw_ = 0, floor_ = 0, lastPw_ = 0; int pwN_ = 0, active_ = 0;
    int state_ = kSearch;
    float qPrev_ = 0, qPrev2_ = 0; cf32 dPrev_{0, 0};
    int cand_ = 0; int64_t candAt_ = 0, bestN_ = 0; float bestQ_ = 0, bestL_ = 0, bestR_ = -1; cf32 bestD_{0, 0};
    double nextT_ = 0, prevT_ = 0, tAdj_ = 0;
    float w_ = 0, phi_ = 0, prevMargin_ = 0; int aPrev_ = 0;
    cf32 prevZ_{0, 0};
    std::vector<uint8_t> bits_;
    std::vector<float> rel_;
    int need_ = 25, len_ = 0, hdrFixed_ = 0; bool hdrDone_ = false;
    float sigPw_ = 0; int sigN_ = 0;
    Stats st_;
    int chan_ = -1; double hz_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE WHOLE CAPTURE
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** Every 25 kHz VDL2 channel from 136.700 to 136.975 MHz inside the capture (Europe's set; 136.975 is worldwide). */
inline std::vector<double> defaultChannels() {
    std::vector<double> v;
    for (int k = 0; k < 12; ++k) v.push_back(136.700e6 + 25e3 * k);
    return v;
}

class Receiver {
public:
    Receiver(double rate, double hwCentreHz, const std::vector<double>& chanHz)
        : mc_(rate, hwCentreHz, chanHz, 12500, 30000, tuning().binHz) {
        /* ★★ THE RECEIVE LOW-PASS IN THE FREQUENCY DOMAIN (2026-11-01): flat to rxBw, a raised-cosine roll to rxBw + rxTr,
         *  nothing beyond — the same corners the 58-tap Blackman FIR had, applied to each channel's slice (bins of 293 Hz:
         *  the 1024-point FFT keeps the response's tails inside overlap-save's discarded head). */
        if (tuning().fdFilter) {
            // ★ the magnitude of the very FIR it replaces (designLowpass, Blackman), so the swept corners hold exactly:
            //   a raised-cosine 5→9 kHz in its place lost 13 frames on the Pi 500 capture and 7 on the Pi 2's
            const auto taps = vibedsp::designLowpass(double(tuning().rxBw + 0.5f * tuning().rxTr) / kWorkRate, double(tuning().rxTr) / kWorkRate, tuning().deepStop);
            for (size_t c = 0; c < chanHz.size(); ++c)
                mc_.setResponse(c, [&](double f) {
                    if (tuning().fdShape == 1) {
                        const double a = std::fabs(f), p = tuning().rxBw, s = tuning().rxBw + tuning().rxTr;
                        return a <= p ? 1.0 : a >= s ? 0.0 : 0.5 * (1 + std::cos(M_PI * (a - p) / (s - p)));
                    }
                    std::complex<double> acc(0, 0);
                    for (size_t k = 0; k < taps.size(); ++k) acc += double(taps[k]) * std::polar(1.0, -2 * M_PI * f * double(k) / kWorkRate);
                    return std::abs(acc);
                });
        }
        for (size_t c = 0; c < chanHz.size(); ++c) {
            demod_.emplace_back(new Demod(mc_.chanRate()));
            demod_.back()->setChannel(int(c), chanHz[c]);
        }
    }
    template <class Out> void feed(const cf32* iq, size_t n, Out&& out) {
        mc_.feed(iq, n, [&](int c, const cf32* z, int k) { demod_[size_t(c)]->feed(z, k, out); });
    }
    const vibeavi::MultiChannel& front() const { return mc_; }
    const Demod& demod(size_t c) const { return *demod_[c]; }
    size_t channels() const { return demod_.size(); }

private:
    vibeavi::MultiChannel mc_;
    std::vector<std::unique_ptr<Demod>> demod_;
};

}  // namespace vibevdl2
