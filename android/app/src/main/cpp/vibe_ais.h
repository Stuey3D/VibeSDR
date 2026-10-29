// vibe_ais.h — V12 AIS: both VHF channels from one capture, GMSK demodulation, the M.1371 messages, readable ships.
// docs/v12/AIS-APRS.md. The service (thread, queue, list, JSON) is vibe_ais_service.h; the framing is vibe_hdlc.h.
//
// ★★★ FROM THE PUBLISHED STANDARD, written fresh: ITU-R M.1371-5 (2014) Annex 2 — GMSK, BT 0.4, 9600 bit/s, NRZI, HDLC
//     with the FCS-16, the 24-bit training sequence; Annex 8 — the message table (types 1–27, field by field). The MID
//     table (an MMSI → its flag) is vibe_maritime_mid.h, shared with DSC.
//     GPL decoders were read for behaviour only (none copied — code, tables or structure).
//
// ★★ THE FRONT END (Front): the radio sits on 162.000 MHz, so its DC remnant falls BETWEEN the two channels
//    (161.975 = −25 kHz, 162.025 = +25 kHz). One shared chain cuts the capture down to ~100 kHz holding both, then each
//    channel is mixed to 0 Hz and cut to ~40–80 kHz on its own (vibedsp's NCO + FirDecimator: NEON on ARM, SSE2 on x86).
//    Any capture ≥ ~100 kS/s works; an integer factor is found for every rate, never a resampler.
//    ★★ AFC (Front::afcStep_): the dongle's crystal error, measured from the bursts and taken out of both channels' NCOs.
// ★★ THE DEMODULATOR (GmskDemod), 2026-10-29: COHERENT (CohPath) at FOUR fixed sampling phases a quarter of a bit apart,
//    each with its own deframer; a frame any of them gets through the FCS counts once. The receive filter is matched to
//    GMSK's main Laurent pulse, the carrier's frequency and phase are estimated data-free from the squared signal over a
//    few dozen bits, and each symbol is read as a real ±1. 50 % of frames at ~8.5 dB Eb/N0 where the discriminator below
//    needed 13 dB (test-ais), and on a real recording it decodes everything the discriminator does with ~3 dB of noise
//    added on top (docs/v12/AIS-APRS.md).
//    • The DISCRIMINATOR (polar discriminator, post-detection low-pass, a peak/valley DC tracker) is kept behind
//      Tuning::disc, OFF: with the AFC it found nothing the coherent detector missed (one frame in the compound case at
//      16 dB) and it costs as much CPU again; it is there to compare against.
//    ★ A soft-decision repair of ONE symbol (vibe_hdlc.h), accepted only if the message's length fits its type AND a
//      second deframer produced the same bytes (emit_).
#pragma once
#include "vibe_hdlc.h"
#include "vibe_maritime_mid.h"   // ★ the MID → country table DSC uses too
#include "vibedsp/vibedsp.h"
#include "vibedsp/simd_internal.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace vibeais {

using cf = std::complex<float>;

static constexpr double kCentreHz = 162.000e6;          ///< where the radio sits (between the channels)
static constexpr double kChanHz[2] = { 161.975e6, 162.025e6 };   ///< AIS 1 (ch 87B), AIS 2 (ch 88B)
static constexpr double kBaud = 9600.0;
static constexpr double kDevHz = 2400.0;                ///< GMSK h = 0.5 → ±2400 Hz
static constexpr double kMinRateHz = 96e3;              ///< below this the two channels do not fit

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ONE CHANNEL'S DEMODULATOR — complex baseband at fs (≈ 40–80 kS/s), centred on the channel
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** ★ Experiment switches (test-ais only; the defaults are the shipped receiver — measured choices, docs/v12/AIS-APRS.md:
 *  W/Wf the phase/frequency windows in bits, 4 coherent phases (5 gained ~5 % at 9 dB for +20 % CPU; 3 lost ~15 %). */
struct Tuning { bool disc = false, coh = true, afc = true; int W = 6, Wf = 24, cohPhases = 4, discPhases = 3; };
inline Tuning& tuning() { static Tuning t; return t; }

struct RawFrame { std::vector<uint8_t> bytes; int repaired = 0; float quality = 0, flipConf = 0; float levelDb = -120, snrDb = 0; };

/** The message's length fits its type (M.1371 Annex 8). The check a REPAIRED frame must pass, and a clean one too. */
inline bool plausible(const std::vector<uint8_t>& b) {
    if (b.empty()) return false;
    const int type = b[0] >> 2;
    const size_t bits = b.size() * 8;
    switch (type) {
        case 1: case 2: case 3: case 4: case 11: case 9: case 18: return bits == 168;
        case 5: return bits == 424 || bits == 416 || bits == 432;   // some sets pad a byte, some lose the spare
        case 19: return bits == 312;
        case 21: return bits >= 272 && bits <= 360;
        case 24: return bits == 160 || bits == 168;
        case 27: return bits == 96 || bits == 168;
        case 6: case 8: case 12: case 14: case 25: case 26: return bits >= 56 && bits <= 1008;
        case 7: case 13: return bits >= 72 && bits <= 168;
        case 10: return bits == 72;
        case 15: return bits >= 88 && bits <= 168;
        case 16: return bits == 96 || bits == 144;
        case 17: return bits >= 80 && bits <= 816;
        case 20: return bits >= 72 && bits <= 160;
        case 22: case 23: return bits == 168;
        default: return false;
    }
}

/** ★ GMSK's main Laurent pulse C0 (P. A. Laurent, IEEE Trans. Commun. 1986) for BT 0.4, h = ½, sampled at `sps` samples
 *  a bit — the receive filter matched to the part of the signal that carries ~99 % of its energy. The frequency pulse
 *  g(t) (the bit's rectangle through the Gaussian, area ½) is truncated to L = 4 bits; q(t) = ∫g; S(t) = sin(π·q(t)) on
 *  [0, L), mirrored on [L, 2L); C0(t) = Π S(t + i), i = 0…L−1, which lasts L+1 bits. Tails under 1 % are trimmed. */
inline std::vector<float> laurentC0(double sps) {
    const int L = 4, fine = 64;
    const double BT = 0.4, k = std::sqrt(2.0) * std::sqrt(std::log(2.0)) / (2 * M_PI * BT);
    const int nq = L * fine;
    std::vector<double> q(size_t(nq) + 1, 0.0);
    for (int i = 1; i <= nq; ++i) {
        const double t = (i - 0.5) / fine - L / 2.0;
        q[size_t(i)] = q[size_t(i - 1)] + 0.25 * (std::erf((t + 0.5) / k) - std::erf((t - 0.5) / k)) / fine;
    }
    const double qEnd = q[size_t(nq)];
    auto S = [&](double t) -> double {
        if (t < 0 || t >= 2 * L) return 0.0;
        if (t >= L) t = 2 * L - t;
        const double x = t * fine; const int i = std::min(nq - 1, int(x)); const double fr = x - i;
        const double qq = (q[size_t(i)] + (q[size_t(i + 1)] - q[size_t(i)]) * fr) * 0.5 / qEnd;   // q(∞) = ½ exactly
        return std::sin(M_PI * qq);
    };
    const int n = int(std::ceil((L + 1) * sps));
    std::vector<float> c(static_cast<size_t>(n));
    float peak = 0;
    for (int j = 0; j < n; ++j) {
        double v = 1; const double t = j / sps;
        for (int i = 0; i < L; ++i) v *= S(t + i);
        c[size_t(j)] = float(v); peak = std::max(peak, float(v));
    }
    size_t a = 0, b = c.size();
    while (a < b && c[a] < 0.01f * peak) ++a;
    while (b > a && c[b - 1] < 0.01f * peak) --b;
    if ((b - a) % 2 == 0) { if (b < c.size()) ++b; else if (a > 0) --a; }   // ★ odd: a whole-sample group delay
    std::vector<float> o(c.begin() + long(a), c.begin() + long(b));
    float sum = 0; for (float v : o) sum += v;
    for (float& v : o) v /= sum;
    return o;
}

/** ★★★ A COHERENT DETECTOR FOR ONE SAMPLING PHASE — no training needed, no carrier loop to lose lock.
 *  MSK (and GMSK, near enough) at a bit BOUNDARY sits on an axis: the phase is θ + k·π/2 + (a multiple of π), so after
 *  turning sample k back by k quarter-turns, y_k = ±A·e^{jθ}. Squaring takes the data out: y² = A²e^{j2θ}. So:
 *   • the carrier's FREQUENCY is the turn from one y² to the next, averaged over ±Wf bits (data-free);
 *   • after removing it, the carrier's PHASE is half the angle of Σy² over ±W bits (data-free), followed continuously
 *     so the half-angle never jumps by π for nothing;
 *   • the symbol is Re(y·e^{−jθ}) — a real ±1 with all the signal's energy, where a discriminator sees the noise twice.
 *  A π ambiguity is left, and does not matter: AIS is NRZI, and on this detector the data bit is symbol k vs symbol k−2
 *  (vibehdlc pairDist = 2), which a sign flip of every symbol leaves alone.
 *  Delay: Wf + W bits. All running sums, so the cost is the same whatever the windows (a few dozen flops a bit). */
class CohPath {
public:
    CohPath(int W, int Wf) : W_(W), Wf_(Wf), N_(2 * (W + Wf) + 4) {
        invWin_ = 1.f / float(2 * W + 1); invWinF_ = 1.f / float(2 * Wf + 1);
        y_.assign(size_t(N_), cf(0, 0)); r_ = u_ = yr_ = q2_ = y_;
        g_.assign(size_t(N_), 0.f);
    }
    /** One sample at this phase's bit instant: `x` matched-filtered (the symbol), `r` the same instant unfiltered (the
     *  estimates — see below). Calls `out(soft)` for the symbol Wf + W bits back.
     *  ★★ WHY TWO: the matched filter's output at a boundary carries its NEIGHBOURS in quadrature (C0 at ±T is ~45 % of its
     *     peak, and the neighbours sit on the other axis). The symbol decision takes the real part, so that is harmless
     *     there — but squared, it is not data-free any more (the training sequence's 1+0.9j squares to 84°). The channel-
     *     filtered sample at the same instant is on its axis to within GMSK's own few degrees, so the carrier is
     *     estimated from that, and the symbol read from the matched filter. */
    template <class Out> void push(cf x, cf r, Out&& out) {
        // turn back by k quarter-turns: (−j)^k
        auto turn = [this](cf v) {
            switch (k_ & 3) { case 1: return cf(v.imag(), -v.real()); case 2: return -v; case 3: return cf(-v.imag(), v.real()); default: return v; }
        };
        // ★ the coarse frequency: the mean turn a bit of the UNturned sample (±90° of data each bit, averaging out)
        const cf dr = r * std::conj(rawPrev_);
        const float dph = vibedsp::fastAtan2(dr.imag(), dr.real());
        rawPrev_ = r;
        x = turn(x); r = turn(r);
        ++k_;
        if (++i_ == N_) i_ = 0;
        const int i = i_;
        auto at = [&](int back) { int j = i - back; if (j < 0) j += N_; return size_t(j); };
        G_ += dph - g_[at(2 * Wf_ + 1)];
        g_[size_t(i)] = dph;
        const cf q = r * r;
        const cf u = q * std::conj(qPrev2_);   // ★ two bits apart — see the note on the frequency below
        qPrev2_ = qPrev_; qPrev_ = q;
        y_[size_t(i)] = x; r_[size_t(i)] = r; u_[size_t(i)] = u;
        // ── frequency: Σu over the last 2Wf+1 bits, centred Wf back ──
        F_ += u - u_[at(2 * Wf_ + 1)];
        // ★ running sums in float drift: rebuilt from the window every 1024 bits
        if (++fresh_ > 1024) { fresh_ = 0; F_ = 0; G_ = 0; for (int b = 0; b <= 2 * Wf_; ++b) { F_ += u_[at(b)]; G_ += g_[at(b)]; } }
        /* ★★ TWO BITS APART, NOT ONE. GMSK's Gaussian shrinks the swing of alternating levels (data 0s — the padding of
         *  every name) from ±45° to about ±21°, so at a boundary those samples sit 24° off their axis, alternately either
         *  side: squared, consecutive ones differ by ~96°, and a one-bit average of y²·conj(y²) turns NEGATIVE — the
         *  estimate jumped by 2400 Hz in the middle of every type 5 (measured). Two bits apart they are on the same
         *  side. The price is the range: ±π/4 a bit, ±1200 Hz — so the alias (every 2400 Hz) is chosen nearest the
         *  coarse estimate above, which is good to a few hundred Hz over the window. */
        // ★ the estimate moves slowly (a ±Wf-bit window): worked out every 4th bit, the step kept in between (the CPU)
        if ((k_ & 3) == 0) {
            float w = 0.25f * vibedsp::fastAtan2(F_.imag(), F_.real());   // rad a bit, ±π/4
            const float coarse = G_ * invWinF_;
            w += float(M_PI / 2) * std::floor((coarse - w) * float(2 / M_PI) + 0.5f);
            w = std::max(-2.4f, std::min(2.4f, w));
            const float c = 1 - w * w * (0.5f - w * w * (1.f / 24 - w * w / 720.f)), s = w * (1 - w * w * (1.f / 6 - w * w * (1.f / 120 - w * w / 5040.f)));
            step_ = cf(c, -s);
        }
        // the rotor turns back by ψ; advanced by the frequency estimate
        rot_ *= step_;
        rot_ *= 1.5f - 0.5f * std::norm(rot_);   // ★ back to unit length (first order: it never strays far in one step)
        const cf yr = y_[at(Wf_)] * rot_, rr = r_[at(Wf_)] * rot_;
        yr_[size_t(i)] = yr;
        // ── phase: Σ(rr²) over the last 2W+1 rotated samples, centred W back ──
        const cf q2 = rr * rr;
        q2_[size_t(i)] = q2;
        P_ += q2 - q2_[at(2 * W_ + 1)];
        if (fresh_ == 0) { P_ = 0; for (int b = 0; b <= 2 * W_; ++b) P_ += q2_[at(b)]; }
        const float pm = std::sqrt(std::norm(P_));
        if (pm > 0) {
            const float c2 = P_.real() / pm, s2 = P_.imag() / pm;
            float ec = std::sqrt(std::max(0.f, 0.5f * (1 + c2))), es = std::sqrt(std::max(0.f, 0.5f * (1 - c2)));
            if (s2 < 0) es = -es;
            if (ec * e_.real() + es * e_.imag() < 0) { ec = -ec; es = -es; }   // ★ the branch nearest the last: no false π jumps
            e_ = cf(ec, es);
        }
        const cf v = yr_[at(W_)];
        const float amp = std::sqrt(pm * invWin_);   // the burst's amplitude: soft values near ±1
        const float sym = v.real() * e_.real() + v.imag() * e_.imag();
        out(amp > 0 ? sym / amp : 0.f);
    }
private:
    int W_, Wf_, N_;
    float invWin_ = 1, invWinF_ = 1;
    uint64_t k_ = 0;
    int fresh_ = 0;
    std::vector<cf> y_, r_, u_, yr_, q2_;
    std::vector<float> g_;
    float G_ = 0;
    cf rawPrev_{0, 0};
    cf qPrev_{0, 0}, qPrev2_{0, 0}, rot_{1, 0}, e_{1, 0}, step_{1, 0};
    cf F_{0, 0}, P_{0, 0};
    int i_ = 0;
};

class GmskDemod {
public:
    explicit GmskDemod(double fs) : fs_(fs), useCoh_(tuning().coh), useDisc_(tuning().disc) {
        // post-detection low-pass: a little over half the bit rate (the GMSK pulse is already smooth)
        lp_ = vibedsp::designLowpass(0.62 * kBaud / fs, 0.5 * kBaud / fs);
        std::reverse(lp_.begin(), lp_.end());
        hist_.assign(lp_.size() * 2, 0.f);
        scale_ = float(fs / (2.0 * M_PI * kDevHz));
        /* ★★ FIVE FIXED SAMPLING PHASES, NO PLL. A frame is at most ~1000 bits (26–110 ms) and a transmitter's clock is
         *  good to a few ppm, so the bit timing cannot drift a tenth of a bit inside one; what is unknown is only its
         *  PHASE. A loop that chases zero crossings is shaken by GMSK's own ISI (BT 0.4 moves a crossing ±0.1 bit with
         *  its neighbours) and by noise; five clocks a fifth of a bit apart need no loop at all — one of them is always
         *  within a tenth of a bit of the eye's centre, and the deduplication keeps the frame once. */
        nDisc_ = std::max(1, std::min(kSlicers, tuning().discPhases));
        for (int i = 0; i < nDisc_; ++i) {
            clk_[i].reset(new vibehdlc::BitClock(fs, kBaud, i / double(nDisc_)));
            vibehdlc::Deframer::Options o;
            o.minBytes = 12 + 2; o.maxBytes = 126 + 2;
            o.repairMax = 8;
            o.validate = plausible; o.validateClean = true;
            df_[i].reset(new vibehdlc::Deframer(o));
        }
        // ★ the coherent paths: the matched filter, then five phases, each its own CohPath + deframer (pairDist 2)
        mf_ = laurentC0(fs / kBaud);
        std::reverse(mf_.begin(), mf_.end());
        mfRe_.assign(mf_.size() * 2, 0.f); mfIm_.assign(mf_.size() * 2, 0.f);
        nCoh_ = std::max(1, std::min(kSlicers, tuning().cohPhases));
        for (int i = 0; i < nCoh_; ++i) {
            coh_[i].reset(new CohPath(tuning().W, tuning().Wf));
            cph_[i] = i / double(nCoh_);
            vibehdlc::Deframer::Options o;
            o.minBytes = 12 + 2; o.maxBytes = 126 + 2;
            o.repairMax = 8;
            o.validate = plausible; o.validateClean = true;
            o.pairDist = 2;
            cdf_[i].reset(new vibehdlc::Deframer(o));
        }
        dcFast_ = float(1.0 - std::exp(-1.0 / (fs / kBaud * 16)));    // hunting: ~16 bits
        dcSlow_ = float(1.0 - std::exp(-1.0 / (fs / kBaud * 200)));   // in a frame: ~200 bits
        pwA_ = float(1.0 - std::exp(-1.0 / (fs / kBaud * 4)));
        dcAcq_ = float(1.0 - std::exp(-1.0 / (fs / kBaud * 2)));
        pwSlowA_ = float(1.0 - std::exp(-1.0 / (fs / kBaud * 150)));
    }

    /** `n` samples of this channel. `out(RawFrame)` for each new frame. */
    template <class Out> void feed(const cf* z, int n, Out&& out) {
        for (int i = 0; i < n; ++i) {
            const cf s = z[i];
            const cf d = s * std::conj(prev_);
            prev_ = s;
            const float p = std::norm(s);
            pw_ += (p - pw_) * pwA_;
            // ★ the noise floor: a slow follower that falls fast and climbs very slowly (frames are short and rare)
            if (pw_ < floor_ || floor_ <= 0) floor_ += (pw_ - floor_) * 0.02f; else floor_ += (pw_ - floor_) * 2e-6f;
            afcSample_(d);
            if (useCoh_) cohSample_(s, out);
            if (!useDisc_) { ++t_; if (hasPend_ && t_ >= pendDue_) flush_(out); continue; }
            float f = vibedsp::fastAtan2(d.imag(), d.real()) * scale_;   // ±1 at the nominal deviation
            if (!(f == f)) f = 0;
            // the post-detection FIR on a doubled history (contiguous for the dot product)
            const int K = int(lp_.size());
            hist_[size_t(hp_)] = f; hist_[size_t(hp_ + K)] = f;
            hp_ = (hp_ + 1) % K;
            const float y = vibedsp::dotReal(&hist_[size_t(hp_)], lp_.data(), K);
            /* ★★ THE CRYSTAL'S OFFSET (a DC on the discriminator): the middle of a peak and a valley follower — each
             *  jumps to a new extreme within a bit and relaxes slowly towards the other, so a run of six equal levels (the
             *  flag, or five 1s and a stuffed 0) does not drag it as a plain average would. Faster while hunting (the
             *  training sequence settles it), slower inside a frame. */
            bool inFrame = false;
            for (int k = 0; k < nDisc_; ++k) inFrame = inFrame || df_[k]->inFrame();
            float rel = inFrame ? dcSlow_ : dcFast_;
            /* ★ RESTARTED BY A RISE IN POWER: between frames the discriminator of noise swings to ±fs/2, and followers
             *  that took those extremes would start every frame far off. A burst arriving (the short-term power 4 dB over
             *  the long-term) restarts them round the first sample; they never take a value more than two deviations
             *  from the middle. */
            pwSlow_ += (pw_ - pwSlow_) * pwSlowA_;
            if (!rise_ && pw_ > 2.5f * pwSlow_) { rise_ = true; hi_ = y + 0.6f; lo_ = y - 0.6f; dc_ = y; acq_ = int(fs_ / kBaud * 16); }
            // ★ …and for the first 16 bits (the training sequence) they relax within two bits, so an odd first sample
            //   (the burst's ramp, a noise spike) cannot hold one of them out for the rest of the frame
            if (acq_ > 0) { --acq_; rel = dcAcq_; }
            else if (rise_ && pw_ < 1.6f * pwSlow_) rise_ = false;
            {
                const float c = std::max(dc_ - 2.2f, std::min(dc_ + 2.2f, y));
                if (c > hi_) hi_ += (c - hi_) * 0.5f; else hi_ += (lo_ - hi_) * rel;
                if (c < lo_) lo_ += (c - lo_) * 0.5f; else lo_ += (hi_ - lo_) * rel;
            }
            dc_ = 0.5f * (hi_ + lo_);
            const float v = y - dc_;
            for (int k = 0; k < nDisc_; ++k) {
                clk_[k]->push(v, 1.0f, [&](float lvl) {
                    df_[k]->push(lvl, [&](const vibehdlc::Frame& fr) { emit_(fr, out); });
                });
            }
            ++t_;
            if (hasPend_ && t_ >= pendDue_) flush_(out);
        }
    }
    double noiseDb() const { return floor_ > 0 ? 10 * std::log10(floor_) : -120; }
    /** ★ AFC: a burst's mean frequency (Hz from this channel's centre) since the last call, and how many bits it rests
     *  on; false = none. GMSK's spectrum is symmetric, so a burst's mean frequency IS its carrier's offset. */
    bool takeBurstFreq(double& hz, double& bits) {
        if (afcN_ <= 0) return false;
        hz = afcHz_; bits = afcN_; afcN_ = 0; return true;
    }
    bool inBurst() const { return afcRun_ > 0; }
    uint64_t unconfirmed() const { return unconfirmed_; }   ///< repaired frames dropped for want of a second witness
    uint64_t repaired() const { uint64_t n = 0; for (int k = 0; k < kSlicers; ++k) n += (df_[k] ? df_[k]->framesRepaired() : 0) + (cdf_[k] ? cdf_[k]->framesRepaired() : 0); return n; }

private:
    static constexpr int kSlicers = 5;
    /** ★★ THE BEST COPY, NOT THE FIRST. The five phases close the same frame within about a bit of each other; the
     *  first to finish may be the worst phase (one wrong level, repaired) while a better one is clean a few samples
     *  later. So a frame is held for two bits: a copy that arrives meanwhile replaces it if it is cleaner (not repaired,
     *  or a higher mean |soft|), and the best is sent when the time is up. Same content = same frame (FNV-1a hash). */
    template <class Out> void emit_(const vibehdlc::Frame& fr, Out& out) {
        // ★ A repair that had to flip a level as confident as the frame's average is a guess, not a repair. (The
        //   stronger rule — a repaired frame must name a ship already heard cleanly — is the service's: Service::accept_.)
        if (fr.repaired && fr.flipConf > 0.45f) return;
        uint32_t h = 2166136261u;
        for (uint8_t c : fr.bytes) h = (h ^ c) * 16777619u;
        for (auto& r : recent_) if (r.h == h && t_ - r.t < uint64_t(fs_ * 0.03)) return;   // already sent
        if (hasPend_ && pendH_ == h) {
            const bool better = (pend_.repaired && !fr.repaired) || (pend_.repaired == fr.repaired && fr.quality > pend_.quality);
            if (better) fill_(fr);
            ++copies_;
            if (!pend_.repaired) pendDue_ = std::min(pendDue_, t_ + uint64_t(2 * fs_ / kBaud) + 1);
            return;
        }
        if (hasPend_) flush_(out);
        pendH_ = h; fill_(fr); hasPend_ = true; copies_ = 1;
        /* ★★★ A REPAIRED FRAME NEEDS A SECOND WITNESS. Sixteen bits of FCS against eight candidate flips lets about one
         *  failed frame in 8000 through as a false "repair" — and with ten deframers a channel, a weak busy coast fails
         *  hundreds a minute. The service's rule (the MMSI must be known) does not catch it when the error is past the
         *  MMSI: test-ais's back-to-back case produced a type 5 of a known ship with a wrong field. A real single error is
         *  seen alike by neighbouring sampling phases and by the other detector, which repair to the SAME bytes; a chance
         *  FCS match is not reproduced. So a repaired frame waits (the coherent detector is W + Wf bits behind the
         *  discriminator) and is sent only if another deframer produced the same bytes, repaired or clean. */
        const int holdBits = fr.repaired ? tuning().W + tuning().Wf + 6 : 2;
        pendDue_ = t_ + uint64_t(holdBits * fs_ / kBaud) + 1;
    }
    void fill_(const vibehdlc::Frame& fr) {
        pend_.bytes = fr.bytes; pend_.repaired = fr.repaired; pend_.quality = fr.quality; pend_.flipConf = fr.flipConf;
        pend_.levelDb = pw_ > 0 ? float(10 * std::log10(pw_)) : -120.f;
        pend_.snrDb = pw_ > 0 && floor_ > 0 ? float(10 * std::log10(pw_ / floor_)) : 0.f;
    }
    template <class Out> void flush_(Out& out) {
        hasPend_ = false;
        if (pend_.repaired && copies_ < 2) { ++unconfirmed_; return; }
        recent_[rp_] = { pendH_, t_ }; rp_ = (rp_ + 1) % kRecent;
        out(pend_);
    }
    int copies_ = 0;
    uint64_t unconfirmed_ = 0;
    /* ★★ BURST FREQUENCY for the AFC: the discriminator's complex product summed over a run of samples well over the
     *  floor (3×); a run 120–1400 bits long is burst-shaped (an AIS burst is 256 bits a slot, at most 5 slots) and is
     *  reported; longer is a carrier, not a ship, and is ignored. */
    void afcSample_(const cf& d) {
        if (floor_ > 0 && pw_ > 3.f * floor_) { afcSum_ += std::complex<double>(d); ++afcRun_; return; }
        if (afcRun_ > 0) {
            const double bits = afcRun_ * kBaud / fs_;
            if (bits >= 120 && bits <= 1400 && std::abs(afcSum_) > 0) {
                afcHz_ = std::arg(afcSum_) * fs_ / (2 * M_PI); afcN_ = bits;
            }
            afcSum_ = 0; afcRun_ = 0;
        }
    }
    /** The matched filter, then each phase's bit instant (linear interpolation) into its CohPath and deframer. */
    template <class Out> void cohSample_(const cf& s, Out& out) {
        const int K = int(mf_.size());
        mfRe_[size_t(mp_)] = s.real(); mfRe_[size_t(mp_ + K)] = s.real();
        mfIm_[size_t(mp_)] = s.imag(); mfIm_[size_t(mp_ + K)] = s.imag();
        mp_ = (mp_ + 1) % K;
        const cf m(vibedsp::dotReal(&mfRe_[size_t(mp_)], mf_.data(), K), vibedsp::dotReal(&mfIm_[size_t(mp_)], mf_.data(), K));
        const size_t c0 = size_t(mp_ + K / 2);   // ★ the raw sample at the filter's centre: the same instant
        const cf r(mfRe_[c0], mfIm_[c0]);
        const double step = kBaud / fs_;
        for (int k = 0; k < nCoh_; ++k) {
            cph_[k] += step;
            if (cph_[k] >= 1.0) {
                cph_[k] -= 1.0;
                const float back = float(cph_[k] / step);   // samples since the instant (0…1)
                const cf x = m - (m - mfPrev_) * back, xr = r - (r - rawPrev_) * back;
                cohPush_(k, x, xr, out);
            }
        }
        mfPrev_ = m; rawPrev_ = r;
    }
    template <class Out> void cohPush_(int k, const cf& x, const cf& xr, Out& out) {
        coh_[k]->push(x, xr, [&](float soft) {
            cdf_[k]->push(soft, [&](const vibehdlc::Frame& fr) { emit_(fr, out); });
        });
    }
    std::vector<float> mf_, mfRe_, mfIm_;
    int mp_ = 0;
    cf mfPrev_{0, 0}, rawPrev_{0, 0};
    double cph_[5] = {0, 0, 0, 0, 0};
    int nCoh_ = 5, nDisc_ = 5;
    std::unique_ptr<CohPath> coh_[5];
    std::unique_ptr<vibehdlc::Deframer> cdf_[5];
    std::complex<double> afcSum_{0, 0};
    long afcRun_ = 0;
    double afcHz_ = 0, afcN_ = 0;
    RawFrame pend_;
    uint32_t pendH_ = 0;
    bool hasPend_ = false;
    uint64_t pendDue_ = 0;
    double fs_;
    bool useCoh_, useDisc_;
    std::vector<float> lp_, hist_;
    int hp_ = 0;
    float scale_;
    cf prev_{1, 0};
    float hi_ = 0.5f, lo_ = -0.5f;
    bool rise_ = false;
    int acq_ = 0;
    float dcAcq_ = 0;
    float pwSlow_ = 0, pwSlowA_ = 0;
    float dc_ = 0, dcFast_, dcSlow_, pw_ = 0, pwA_, floor_ = 0;
    std::unique_ptr<vibehdlc::BitClock> clk_[kSlicers];
    std::unique_ptr<vibehdlc::Deframer> df_[kSlicers];
    struct Rec { uint32_t h = 0; uint64_t t = 0; };
    static constexpr int kRecent = 8;
    Rec recent_[kRecent];
    int rp_ = 0;
    uint64_t t_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE FRONT END — a whole capture in, both channels' frames out
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Front {
public:
    /** `rate` the capture rate, `hwCentreHz` where the radio is tuned (normally kCentreHz). */
    Front(double rate, double hwCentreHz) : rate_(rate) {
        // ★ THE SHARED CHAIN: integer stages of ≤ 8 down to ≥ 100 kS/s, each filter only as long as its own transition
        //   needs (the wanted span is ±40 kHz around 162.000: both channels and a crystal's error).
        const double pass = 40e3;
        double cur = rate;
        mixHz_ = kCentreHz - hwCentreHz;
        while (cur / 2 >= 100e3) {
            int d = int(std::min(8.0, std::floor(cur / 100e3)));
            while (d > 1 && cur / d < 100e3) --d;
            if (d < 2) break;
            const double outR = cur / d, stop = outR - pass;
            auto taps = vibedsp::designLowpass(0.5 * (pass + stop) / cur, (stop - pass) / cur);
            stages_.emplace_back(new vibedsp::FirDecimator(taps, d));
            cur = outR;
        }
        shared_ = cur;
        // ★ EACH CHANNEL: mixed to 0 Hz, then cut to the first integer rate ≥ 38.4 kS/s (4 samples a bit).
        int d2 = std::max(1, int(std::floor(cur / 38.4e3)));
        chRate_ = cur / d2;
        for (int c = 0; c < 2; ++c) {
            const double off = kChanHz[c] - kCentreHz;
            nco_[c].setFreq(off / cur);
            /* ★ ±7 kHz passed (the GMSK main lobe and ~2 kHz of crystal), stopped by ±14 kHz — measured best of five
             *  widths on the synthetic captures (test-ais): ±9/22 kHz let in 3 dB more noise and lost a tenth of the
             *  frames at 14 dB Eb/N0; ±6/12 kHz began to cut the signal. */
            const double pe = 7e3, se = 14e3;
            const double stop = std::min(chRate_ - pe, se);
            chDec_[c].reset(new vibedsp::FirDecimator(vibedsp::designLowpass(0.5 * (pe + stop) / cur, (stop - pe) / cur, true), d2));
            demod_[c].reset(new GmskDemod(chRate_));
        }
        if (std::fabs(mixHz_) > 1) mix_.setFreq(mixHz_ / rate);   // ★ NCO::mix multiplies by exp(−j·phase): 162.000 MHz down to 0 Hz
    }
    double sharedRate() const { return shared_; }
    double channelRate() const { return chRate_; }
    int stages() const { return int(stages_.size()); }

    /** `out(int channel, RawFrame)` — channel 0 = AIS 1 (161.975), 1 = AIS 2 (162.025). */
    template <class Out> void feed(const cf* iq, size_t n, Out&& out) {
        const size_t kBlk = 16384;
        for (size_t o = 0; o < n; o += kBlk) {
            const int m = int(std::min(kBlk, n - o));
            a_.resize(size_t(m) + 16);
            const vibedsp::cf32* src = reinterpret_cast<const vibedsp::cf32*>(iq + o);
            if (std::fabs(mixHz_) > 1) { mix_.mix(src, a_.data(), m); src = a_.data(); }
            int cnt = m;
            for (auto& st : stages_) {
                b_.resize(size_t(st->maxOut(cnt)) + 16);
                cnt = st->process(src, cnt, b_.data());
                a_.swap(b_); src = a_.data();
            }
            for (int c = 0; c < 2; ++c) {
                mixed_.resize(size_t(cnt) + 16);
                nco_[c].mix(src, mixed_.data(), cnt);
                ch_.resize(size_t(chDec_[c]->maxOut(cnt)) + 16);
                const int k = chDec_[c]->process(mixed_.data(), cnt, ch_.data());
                demod_[c]->feed(reinterpret_cast<const cf*>(ch_.data()), k, [&](const RawFrame& f) { out(c, f); });
            }
            afcStep_();
        }
    }
    double afcHz() const { return afc_; }
    /** Start the AFC where a previous run left it (Service: the same dongle's crystal). */
    void seedAfc(double hz) {
        afc_ = std::max(-12e3, std::min(12e3, hz));
        for (int c = 0; c < 2; ++c) { applied_[c] = afc_; nco_[c].setFreq((kChanHz[c] - kCentreHz + afc_) / shared_); }
    }
    uint64_t unconfirmed() const { return demod_[0]->unconfirmed() + demod_[1]->unconfirmed(); }
    double noiseDb(int c) const { return demod_[c]->noiseDb(); }
    uint64_t repaired() const { return demod_[0]->repaired() + demod_[1]->repaired(); }

private:
    /* ★★★ AFC — THE DONGLE'S CRYSTAL. An RTL dongle without a TCXO is commonly 20–60 ppm off, which at 162 MHz is
     *  3–10 kHz: a burst pushed against (or past) the ±7 kHz channel filter. Measured before this (test-ais conditions):
     *  +6 kHz lost half the frames at 14 dB, −9 kHz decoded NOTHING even at 20 dB. Both channels share the crystal, so
     *  every burst on either channel (its mean frequency, GmskDemod::takeBurstFreq) moves ONE correction half of the
     *  way; it is applied to both channels' NCOs between bursts (never mid-burst), up to ±12 kHz. A burst far off is only
     *  partly inside the filter, so its estimate falls short — repeated, it converges in a handful of bursts. Each ship's
     *  own ±500 Hz averages out; what remains the coherent detector follows burst by burst (±1200 Hz).
     *  ★ Measured (test-ais, 20 dB): −9 kHz settles within 100 Hz in ~10 bursts. The service keeps the last value and
     *    starts the next Front there (seedAfc): a crystal does not change between sessions, so only the first ever start
     *    pays the settling. */
    void afcStep_() {
        if (!tuning().afc) return;
        for (int c = 0; c < 2; ++c) {
            double hz = 0, bits = 0;
            // ★ the estimate is relative to where THIS channel's NCO sits now
            if (demod_[c]->takeBurstFreq(hz, bits)) afc_ = std::max(-12e3, std::min(12e3, afc_ + (applied_[c] + hz - afc_) / 2));
        }
        for (int c = 0; c < 2; ++c)
            if (applied_[c] != afc_ && !demod_[c]->inBurst()) {   // ★ each channel between its own bursts
                applied_[c] = afc_;
                nco_[c].setFreq((kChanHz[c] - kCentreHz + afc_) / shared_);
            }
    }
    double afc_ = 0, applied_[2] = {0, 0};
    double rate_, shared_ = 0, chRate_ = 0, mixHz_ = 0;
    vibedsp::NCO mix_;
    std::vector<std::unique_ptr<vibedsp::FirDecimator>> stages_;
    vibedsp::NCO nco_[2];
    std::unique_ptr<vibedsp::FirDecimator> chDec_[2];
    std::unique_ptr<GmskDemod> demod_[2];
    std::vector<vibedsp::cf32> a_, b_, mixed_, ch_;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE MESSAGES (M.1371-5 Annex 8)
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** The message as bits, MSB first: an AIS byte is sent LSB first by HDLC, so bit k is (byte k/8) bit (7 − k%8). */
struct Bits {
    const std::vector<uint8_t>& b;
    size_t n() const { return b.size() * 8; }
    uint32_t u(size_t at, int len) const {
        uint32_t v = 0;
        for (int i = 0; i < len; ++i) {
            const size_t k = at + size_t(i);
            const int bit = k < n() ? (b[k / 8] >> (7 - k % 8)) & 1 : 0;
            v = (v << 1) | uint32_t(bit);
        }
        return v;
    }
    int32_t s(size_t at, int len) const {
        uint32_t v = u(at, len);
        if (v & (1u << (len - 1))) return int32_t(v) - int32_t(1u << len);   // len < 32 everywhere it is used
        return int32_t(v);
    }
    /** Six-bit ASCII (Table 47): 0–31 → '@'…'_', 32–63 → ' '…'?'. Trailing '@' and spaces dropped. */
    std::string text(size_t at, int chars) const {
        std::string o;
        for (int i = 0; i < chars; ++i) {
            if (at + size_t(i) * 6 + 6 > n()) break;
            const uint32_t v = u(at + size_t(i) * 6, 6);
            o += char(v < 32 ? v + 64 : v);
        }
        const size_t at0 = o.find('@');
        if (at0 != std::string::npos) o.resize(at0);
        while (!o.empty() && o.back() == ' ') o.pop_back();
        while (!o.empty() && o.front() == ' ') o.erase(0, 1);
        return o;
    }
};

struct Msg {
    int type = 0, repeat = 0;
    uint32_t mmsi = 0;
    // position
    bool hasPos = false; double lat = NAN, lon = NAN;
    double sog = NAN, cog = NAN; int heading = -1;   ///< knots, degrees true; heading 0–359
    int rot = -128;                                  ///< raw ROT_AIS (−127…127; −128 = none)
    int nav = -1;                                    ///< navigational status 0–15
    bool accuracy = false;
    int second = -1;
    // static / voyage
    std::string name, callsign, dest;
    uint32_t imo = 0;
    int shipType = -1;
    int bow = -1, stern = -1, port = -1, stbd = -1;
    double draught = NAN;
    int etaMonth = 0, etaDay = 0, etaHour = 24, etaMin = 60;
    // AtoN
    int atonType = -1; bool virt = false, offPos = false;
    // base station / SAR
    int year = 0, month = 0, day = 0, hour = 24, minute = 60, sec = 60;
    int altM = -1;   ///< SAR aircraft altitude, metres (4095 = none)
    // class B static (type 24)
    int part = -1;
    // safety-related text (12, 14)
    std::string text;
    uint32_t destMmsi = 0;
};

inline double posOrNan(int32_t raw, double scale, double limit) {
    const double v = raw / scale;
    return std::fabs(v) > limit ? NAN : v;
}

/** Decode one frame. False = a type we do not read (counted, not shown) or a field out of range. */
inline bool decode(const std::vector<uint8_t>& bytes, Msg& m) {
    if (!plausible(bytes)) return false;
    Bits B{bytes};
    m = Msg();
    m.type = int(B.u(0, 6)); m.repeat = int(B.u(6, 2)); m.mmsi = B.u(8, 30);
    auto pos28 = [&](size_t lonAt, size_t latAt) {
        const double lon = posOrNan(B.s(lonAt, 28), 600000.0, 180.0), lat = posOrNan(B.s(latAt, 27), 600000.0, 90.0);
        if (lon == lon && lat == lat && !(lon == 0 && lat == 0)) { m.lon = lon; m.lat = lat; m.hasPos = true; }
    };
    auto sog10 = [&](size_t at) { const uint32_t v = B.u(at, 10); if (v < 1023) m.sog = v / 10.0; };
    auto cog12 = [&](size_t at) { const uint32_t v = B.u(at, 12); if (v < 3600) m.cog = v / 10.0; };
    auto hdg9 = [&](size_t at) { const uint32_t v = B.u(at, 9); if (v < 360) m.heading = int(v); };
    auto dims = [&](size_t at) { m.bow = int(B.u(at, 9)); m.stern = int(B.u(at + 9, 9)); m.port = int(B.u(at + 18, 6)); m.stbd = int(B.u(at + 24, 6)); };
    switch (m.type) {
        case 1: case 2: case 3:
            m.nav = int(B.u(38, 4)); m.rot = B.s(42, 8); sog10(50); m.accuracy = B.u(60, 1);
            pos28(61, 89); cog12(116); hdg9(128); m.second = int(B.u(137, 6));
            return true;
        case 4: case 11:
            m.year = int(B.u(38, 14)); m.month = int(B.u(52, 4)); m.day = int(B.u(56, 5));
            m.hour = int(B.u(61, 5)); m.minute = int(B.u(66, 6)); m.sec = int(B.u(72, 6));
            m.accuracy = B.u(78, 1); pos28(79, 107);
            return true;
        case 5:
            m.imo = B.u(40, 30); m.callsign = B.text(70, 7); m.name = B.text(112, 20); m.shipType = int(B.u(232, 8));
            dims(240);
            m.etaMonth = int(B.u(274, 4)); m.etaDay = int(B.u(278, 5)); m.etaHour = int(B.u(283, 5)); m.etaMin = int(B.u(288, 6));
            { const uint32_t d = B.u(294, 8); if (d) m.draught = d / 10.0; }
            m.dest = B.text(302, 20);
            return true;
        case 9:
            { const uint32_t a = B.u(38, 12); m.altM = a < 4095 ? int(a) : -1; }
            { const uint32_t v = B.u(50, 10); if (v < 1023) m.sog = double(v); }   // ★ whole knots for aircraft
            m.accuracy = B.u(60, 1); pos28(61, 89); cog12(116); m.second = int(B.u(128, 6));
            return true;
        case 18:
            sog10(46); m.accuracy = B.u(56, 1); pos28(57, 85); cog12(112); hdg9(124); m.second = int(B.u(133, 6));
            return true;
        case 19:
            sog10(46); m.accuracy = B.u(56, 1); pos28(57, 85); cog12(112); hdg9(124); m.second = int(B.u(133, 6));
            m.name = B.text(143, 20); m.shipType = int(B.u(263, 8)); dims(271);
            return true;
        case 21: {
            m.atonType = int(B.u(38, 5)); m.name = B.text(43, 20); m.accuracy = B.u(163, 1); pos28(164, 192);
            dims(219); m.second = int(B.u(253, 6)); m.offPos = B.u(259, 1); m.virt = B.u(269, 1);
            if (B.n() > 272) m.name += B.text(272, int((B.n() - 272) / 6));
            return true;
        }
        case 24:
            m.part = int(B.u(38, 2));
            if (m.part == 0) { m.name = B.text(40, 20); return true; }
            if (m.part == 1) {
                m.shipType = int(B.u(40, 8)); m.callsign = B.text(90, 7);
                if (m.mmsi / 10000000 != 98) dims(132);   // ★ an auxiliary craft carries its mother ship's MMSI there
                return true;
            }
            return false;
        case 27: {
            m.accuracy = B.u(38, 1); m.nav = int(B.u(40, 4));
            const double lon = posOrNan(B.s(44, 18), 600.0, 180.0), lat = posOrNan(B.s(62, 17), 600.0, 90.0);
            if (lon == lon && lat == lat) { m.lon = lon; m.lat = lat; m.hasPos = true; }
            { const uint32_t v = B.u(79, 6); if (v < 63) m.sog = double(v); }
            { const uint32_t v = B.u(85, 9); if (v < 360) m.cog = double(v); }
            return true;
        }
        case 12:
            m.destMmsi = B.u(40, 30);
            m.text = B.text(72, int((B.n() - 72) / 6));
            return true;
        case 14:
            m.text = B.text(40, int((B.n() - 40) / 6));
            return true;
        default:
            return true;   // ★ a valid frame of a type we only count (binary messages, slot management …)
    }
}

// ── names ──────────────────────────────────────────────────────────────────────────────────────────────────────────
inline const char* navStatusName(int s) {
    static const char* n[16] = { "under way using engine", "at anchor", "not under command", "restricted manoeuvrability",
        "constrained by draught", "moored", "aground", "engaged in fishing", "under way sailing", "", "", "towing astern",
        "pushing ahead / towing alongside", "", "AIS-SART active", "" };
    return s >= 0 && s < 16 ? n[s] : "";
}
/** Ship and cargo type (Table 53), as one plain word or two. */
inline std::string shipTypeName(int t) {
    if (t < 0) return "";
    if (t >= 20 && t <= 29) return "wing in ground";
    switch (t) {
        case 30: return "fishing"; case 31: case 32: return "towing"; case 33: return "dredging";
        case 34: return "diving"; case 35: return "military"; case 36: return "sailing"; case 37: return "pleasure craft";
        case 50: return "pilot"; case 51: return "search and rescue"; case 52: return "tug"; case 53: return "port tender";
        case 54: return "anti-pollution"; case 55: return "law enforcement"; case 58: return "medical transport";
        case 59: return "non-combatant";
        default: break;
    }
    if (t >= 40 && t <= 49) return "high-speed craft";
    if (t >= 60 && t <= 69) return "passenger";
    if (t >= 70 && t <= 79) return "cargo";
    if (t >= 80 && t <= 89) return "tanker";
    if (t >= 90 && t <= 99) return "other";
    return "";
}
/** The map's colour family for a ship type — one word the layer keys on. */
inline const char* shipFamily(int t) {
    if (t == 30) return "fishing";
    if (t == 36 || t == 37) return "leisure";
    if ((t >= 31 && t <= 33) || t == 52 || t == 53) return "tug";
    if (t == 50 || t == 51 || t == 55 || t == 58 || t == 35) return "special";
    if (t >= 40 && t <= 49) return "hsc";
    if (t >= 60 && t <= 69) return "passenger";
    if (t >= 70 && t <= 79) return "cargo";
    if (t >= 80 && t <= 89) return "tanker";
    return "other";
}
inline const char* atonTypeName(int t) {
    static const char* n[32] = { "", "reference point", "RACON", "fixed structure", "", "light", "light with sectors",
        "leading light front", "leading light rear", "beacon, cardinal N", "beacon, cardinal E", "beacon, cardinal S",
        "beacon, cardinal W", "beacon, port hand", "beacon, starboard hand", "beacon, preferred channel port",
        "beacon, preferred channel starboard", "beacon, isolated danger", "beacon, safe water", "beacon, special mark",
        "cardinal mark N", "cardinal mark E", "cardinal mark S", "cardinal mark W", "port hand mark", "starboard hand mark",
        "preferred channel port", "preferred channel starboard", "isolated danger", "safe water", "special mark",
        "light vessel / LANBY / rig" };
    return t >= 0 && t < 32 ? n[t] : "";
}

// ── the flag: MMSI → MID → country — ONE TABLE for DSC and AIS (vibe_maritime_mid.h, ITU-R M.585 / MARS) ─────────────
using Mid = vibe::mid::Row;
inline std::string mmsi9(uint32_t mmsi) { char s[16]; std::snprintf(s, sizeof s, "%09u", mmsi); return s; }
/** What the MMSI is (M.585: ship, coast station, SAR aircraft, AtoN, SART/MOB/EPIRB …) and its MID (-1 = none). */
inline int midOf(uint32_t mmsi, const char** what = nullptr, vibe::mid::Kind* kind = nullptr) {
    int m = -1;
    const vibe::mid::Kind k = vibe::mid::kindOf(mmsi9(mmsi), m);
    if (what) *what = vibe::mid::kindWord(k);
    if (kind) *kind = k;
    return m;
}
inline const Mid* flagOf(uint32_t mmsi) { const int m = midOf(mmsi); return m >= 0 ? vibe::mid::find(m) : nullptr; }
inline bool isBeacon(uint32_t mmsi) {
    vibe::mid::Kind k; midOf(mmsi, nullptr, &k);
    return k == vibe::mid::Kind::Sart || k == vibe::mid::Kind::Mob || k == vibe::mid::Kind::Epirb;
}

} // namespace vibeais
