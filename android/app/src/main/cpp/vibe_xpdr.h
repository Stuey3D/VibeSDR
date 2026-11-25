// vibe_xpdr.h — ★★★ beta94 THE TRANSPONDER SIGNAL FINDER's detectors (docs/v12/TRANSPONDER-FINDER.md). Pure: no threads,
// no sockets, no clock — vibeserver/test-xpdr.cpp drives it with synthetic IQ.
//
// Stuart, 2026-11-24, RS-44 with CW and SSB contacts all over 435.61–435.67 MHz: "with satellites like rs44 which can host
// multiple signals we need a voice skimmer, open the satellite in iq mode and as soon as a carrier is detected it gives you a
// button to listen to it". The CW half is the CW skimmer we have (decoders/cw_skimmer.h — it already drops speech as
// "broadband"); this file is the other half and the follow's measurements:
//
//  • VoiceFinder — SSB VOICE in a complex IQ segment. One windowed FFT (≈ 23 Hz bins) per block, powers averaged over
//    ≈ 0.25 s frames; per frame the noise floor is the segment's median bin (the transponder is mostly empty — a few
//    3 kHz voices in 60 kHz never move a median), and a run of bins ≥ kVoiceDb above it, gaps of ≤ ~100 Hz bridged, 400 Hz
//    to 4 kHz wide, is a voice frame. A carrier or a CW signal is one or two bins: never a voice. Runs are tracked by their
//    energy centre; four frames (≈ 1 s of speech) confirm one. Each voice's SUPPRESSED CARRIER — where a USB dial puts it
//    (an LSB one: above it) — is estimated from the low edge of its speech (speech starts ~300 Hz above the carrier on any
//    transmitter's filter), smoothed, and it is what LISTEN tunes to and what the follow measures while they speak.
//  • ToneTracker — an FLL on ONE CW tone (the followed CW row): mix to the tone, boxcar to ~400 Hz, the lag-1 product's
//    angle over 0.25 s windows with the tone keyed (power well above its own floor) → the tone's frequency, to a few Hz.
//
// ★ SIMD: the per-bin power accumulation and the window multiply are plain counted loops over float arrays (the compilers
//   vectorise them: NEON / SSE2); the FFT is vibedab::Fft (PFFFT where the build links it — the server and the app do).
//   Cost on a 96 kHz slice: ~23 FFTs of 4096 points a second — well under 1 % of a Pi 4 core.
#pragma once

#include "vibe_dab_fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

namespace vibexpdr {

using cf32 = std::complex<float>;

/** One voice, as a list row. Offsets are from the segment's centre (Hz). */
struct Voice {
    int id = 0;
    double centreHz = 0;          ///< the energy centre (smoothed)
    double loHz = 0, hiHz = 0;    ///< the speech's edges (smoothed)
    double carrierHz = 0;         ///< the suppressed carrier: where the listening dial goes
    float snrDb = 0;
    double firstT = 0, lastT = 0; ///< stream seconds
    int frames = 0;               ///< frames heard
    bool confirmed = false;
    bool active = false;          ///< heard in the last kActiveSec
    double measuredAt = 0;        ///< the last frame that measured it (stream seconds)
};

class VoiceFinder {
public:
    static constexpr double kBinHz = 25.0;        ///< target bin width
    static constexpr double kFrameSec = 0.25;     ///< power averaged over this long
    static constexpr double kVoiceDb = 6.0;       ///< a bin this far above the floor is "occupied"
    static constexpr double kMinWideHz = 400.0;   ///< narrower = a carrier / CW, never a voice
    static constexpr double kMaxWideHz = 4000.0;  ///< wider = two voices touching, or something else
    static constexpr double kBridgeHz = 110.0;    ///< gaps in speech this narrow are bridged
    static constexpr double kPeakShare = 0.5;     ///< a run whose strongest 100 Hz holds more of its energy = a carrier
    static constexpr double kMatchHz = 1500.0;    ///< a frame's run within this of a voice's centre is that voice
    static constexpr int kConfirmFrames = 4;      ///< ≈ 1 s of speech
    static constexpr double kTentativeSec = 3.0;  ///< an unconfirmed voice not heard again within this goes
    static constexpr double kActiveSec = 2.0;     ///< "on air now"
    static constexpr double kKeepSec = 600.0;     ///< a confirmed voice is listed this long after it was last heard
    static constexpr double kSpeechAboveCarrierHz = 300.0;
    static constexpr int kMaxVoices = 32;

    /** `rate`: the segment's sample rate; `loHz`/`hiHz`: the offsets searched; `usb`: the downlink's sideband. */
    VoiceFinder(double rate, double loHz, double hiHz, bool usb = true) : rate_(rate), lo_(loHz), hi_(hiHz), usb_(usb) {
        size_t n = 256;
        while ((double)n < rate / kBinHz && n < 16384) n <<= 1;
        n_ = n;
        fft_.reset(new vibedab::Fft(n_));
        win_.resize(n_);
        for (size_t i = 0; i < n_; ++i) win_[i] = (float)(0.5 - 0.5 * std::cos(2.0 * M_PI * (double)i / (double)n_));
        buf_.resize(n_); work_.resize(n_); acc_.assign(n_, 0.0f);
        perFrame_ = std::max(1, (int)std::lround(kFrameSec * rate / (double)n_));
        binHz_ = rate / (double)n_;
    }
    double binHz() const { return binHz_; }
    int fftSize() const { return (int)n_; }
    double now() const { return t_; }

    void feed(const cf32* x, int n) {
        for (int i = 0; i < n; ++i) {
            buf_[fill_++] = x[i];
            if (fill_ == n_) { block_(); fill_ = 0; }
        }
        t_ += (double)n / rate_;
    }
    /** Confirmed voices (active first, strongest first), then the recently heard. */
    std::vector<Voice> voices() const {
        std::vector<Voice> v;
        for (const auto& x : vs_) if (x.confirmed) { Voice y = x; y.active = t_ - x.lastT <= kActiveSec; v.push_back(y); }
        std::sort(v.begin(), v.end(), [](const Voice& a, const Voice& b) {
            if (a.active != b.active) return a.active;
            return a.active ? a.snrDb > b.snrDb : a.lastT > b.lastT;
        });
        return v;
    }
    bool voice(int id, Voice& out) const {
        for (const auto& x : vs_) if (x.id == id && x.confirmed) { out = x; out.active = t_ - x.lastT <= kActiveSec; return true; }
        return false;
    }
    uint64_t frames() const { return frames_; }
    /** The last frame's floor (power per bin, linear) — for the tests. */
    double floorNow() const { return floor_; }

private:
    void block_() {
        // ★ the window multiply and the power accumulation: counted loops over contiguous floats (vectorised)
        float* w = reinterpret_cast<float*>(work_.data());
        const float* b = reinterpret_cast<const float*>(buf_.data());
        for (size_t i = 0; i < n_; ++i) { w[2 * i] = b[2 * i] * win_[i]; w[2 * i + 1] = b[2 * i + 1] * win_[i]; }
        fft_->forward(work_.data());
        float* a = acc_.data();
        for (size_t i = 0; i < n_; ++i) a[i] += w[2 * i] * w[2 * i] + w[2 * i + 1] * w[2 * i + 1];
        if (++inFrame_ >= perFrame_) { frame_(); std::fill(acc_.begin(), acc_.end(), 0.0f); inFrame_ = 0; }
    }
    /** FFT bin k (natural order) → offset Hz. */
    double binOff_(int k) const { return (k < (int)n_ / 2 ? k : k - (int)n_) * binHz_; }
    int offBin_(double hz) const { int k = (int)std::lround(hz / binHz_); return k < 0 ? k + (int)n_ : k; }

    void frame_() {
        ++frames_;
        const double tNow = t_ + (double)fill_ / rate_;
        // the searched bins in frequency order
        const int k0 = (int)std::ceil(std::max(lo_, -0.47 * rate_) / binHz_), k1 = (int)std::floor(std::min(hi_, 0.47 * rate_) / binHz_);
        if (k1 - k0 < 8) return;
        p_.resize((size_t)(k1 - k0 + 1));
        for (int k = k0; k <= k1; ++k) p_[(size_t)(k - k0)] = acc_[(size_t)offBin_(k * binHz_)];
        // the floor: the median bin (robust against a few voices and carriers)
        tmp_ = p_;
        std::nth_element(tmp_.begin(), tmp_.begin() + tmp_.size() / 2, tmp_.end());
        floor_ = std::max(1e-20, (double)tmp_[tmp_.size() / 2]);
        const double thr = floor_ * std::pow(10.0, kVoiceDb / 10.0);
        const int bridge = std::max(1, (int)std::lround(kBridgeHz / binHz_));
        // runs of occupied bins, gaps ≤ bridge filled
        struct Run { int a, b; };
        std::vector<Run> runs;
        int start = -1, lastOcc = -100;
        for (int i = 0; i < (int)p_.size(); ++i) {
            if (p_[(size_t)i] < thr) continue;
            if (start >= 0 && i - lastOcc <= bridge + 1) { lastOcc = i; continue; }
            if (start >= 0) runs.push_back({start, lastOcc});
            start = i; lastOcc = i;
        }
        if (start >= 0) runs.push_back({start, lastOcc});
        std::vector<bool> hit(vs_.size(), false);
        for (const Run& r : runs) {
            const double wHz = (r.b - r.a + 1) * binHz_;
            if (wHz < kMinWideHz || wHz > kMaxWideHz) continue;
            // occupied fraction (speech fills its band; two carriers 1 kHz apart do not)
            int occ = 0; double sw = 0, swf = 0, sp = 0;
            for (int i = r.a; i <= r.b; ++i) {
                const double v = p_[(size_t)i];
                if (v >= thr) ++occ;
                const double e = std::max(0.0, v - floor_);
                sw += e; swf += e * ((k0 + i) * binHz_); sp += v;
            }
            if (occ < 0.4 * (r.b - r.a + 1) || !(sw > 0)) continue;
            /* ★ ONE PEAK IS NOT SPEECH: a strong keyed carrier's skirts and key clicks can stand 6 dB over the floor for a few
             *  hundred Hz either side, but nearly all its energy is in its own few bins; speech spreads it over the band.
             *  The run's strongest 100 Hz holding more than kPeakShare of its energy above the floor = a carrier / CW. */
            {
                const int pk = std::max(1, (int)std::lround(100.0 / binHz_));
                double best = 0, cur = 0;
                for (int i = r.a; i <= r.b; ++i) {
                    cur += std::max(0.0, (double)p_[(size_t)i] - floor_);
                    if (i - r.a >= pk) cur -= std::max(0.0, (double)p_[(size_t)(i - pk)] - floor_);
                    best = std::max(best, cur);
                }
                if (best > kPeakShare * sw) continue;
            }
            const double centre = swf / sw, lo = (k0 + r.a) * binHz_, hi = (k0 + r.b) * binHz_;
            const float snr = (float)(10.0 * std::log10(std::max(1e-12, sp / (r.b - r.a + 1) / floor_)));
            // match: the nearest voice by centre
            int best = -1; double bd = kMatchHz;
            for (size_t j = 0; j < vs_.size(); ++j) {
                const double d = std::fabs(vs_[j].centreHz - centre);
                if (d < bd) { bd = d; best = (int)j; }
            }
            if (best < 0) {
                if ((int)vs_.size() >= kMaxVoices) dropOldest_();
                if ((int)vs_.size() >= kMaxVoices) continue;
                Voice v; v.id = ++nextId_; v.centreHz = centre; v.loHz = lo; v.hiHz = hi; v.snrDb = snr;
                v.firstT = v.lastT = v.measuredAt = tNow; v.frames = 1;
                v.carrierHz = usb_ ? lo - kSpeechAboveCarrierHz : hi + kSpeechAboveCarrierHz;
                vs_.push_back(v); hit.push_back(true);
                continue;
            }
            Voice& v = vs_[(size_t)best];
            if (hit[(size_t)best]) continue;   // one run a voice a frame
            hit[(size_t)best] = true;
            // ★ smoothed — speech's edges move syllable to syllable; the carrier is the slow part of them
            const double a = v.frames < kConfirmFrames ? 0.5 : 0.15;
            v.centreHz += a * (centre - v.centreHz);
            v.loHz += a * (lo - v.loHz); v.hiHz += a * (hi - v.hiHz);
            v.carrierHz = usb_ ? v.loHz - kSpeechAboveCarrierHz : v.hiHz + kSpeechAboveCarrierHz;
            v.snrDb += 0.3f * (snr - v.snrDb);
            v.lastT = v.measuredAt = tNow; ++v.frames;
            if (v.frames >= kConfirmFrames) v.confirmed = true;
        }
        // forget: tentative ones not heard again, confirmed ones after kKeepSec
        vs_.erase(std::remove_if(vs_.begin(), vs_.end(), [&](const Voice& v) {
            return (!v.confirmed && tNow - v.lastT > kTentativeSec) || (v.confirmed && tNow - v.lastT > kKeepSec);
        }), vs_.end());
    }
    void dropOldest_() {
        auto it = std::min_element(vs_.begin(), vs_.end(), [](const Voice& a, const Voice& b) { return a.lastT < b.lastT; });
        if (it != vs_.end()) vs_.erase(it);
    }

    double rate_, lo_, hi_;
    bool usb_;
    size_t n_ = 4096, fill_ = 0;
    std::unique_ptr<vibedab::Fft> fft_;
    std::vector<float> win_, acc_, p_, tmp_;
    std::vector<cf32> buf_, work_;
    int perFrame_ = 1, inFrame_ = 0;
    double binHz_ = 25, t_ = 0, floor_ = 0;
    uint64_t frames_ = 0;
    std::vector<Voice> vs_;
    int nextId_ = 0;
};

/** ★ An FLL on one CW tone: where exactly it is (Hz from the segment's centre), measured while it is keyed. */
class ToneTracker {
public:
    static constexpr double kOutHz = 400.0;      ///< the boxcar's output rate
    static constexpr double kWindowSec = 0.25;   ///< one measurement
    static constexpr double kKeyedDb = 8.0;      ///< a window this far above the tone's own floor is "keyed"

    ToneTracker(double rate, double offsetHz) : rate_(rate), f_(offsetHz) {
        dec_ = std::max(1, (int)std::lround(rate / kOutHz));
        perWin_ = std::max(8, (int)std::lround(kWindowSec * rate / dec_));
    }
    double hz() const { return f_; }
    double measuredAt() const { return measAt_; }
    bool measured() const { return measAt_ > 0; }
    /** Retarget (a new tone). */
    void reset(double offsetHz) { f_ = offsetHz; measAt_ = 0; floor_ = 0; ph_ = {1, 0}; prev_ = {0, 0}; box_ = {0, 0}; k_ = 0; wN_ = 0; lr_ = li_ = wp_ = 0; }
    void feed(const cf32* x, int n) {
        const double w = -2.0 * M_PI * f_ / rate_;
        const std::complex<double> step(std::cos(w), std::sin(w));
        std::complex<double> ph = ph_;
        for (int i = 0; i < n; ++i) {
            const std::complex<double> y = std::complex<double>(x[i].real(), x[i].imag()) * ph;
            ph *= step;
            box_ += y;
            if (++k_ < dec_) continue;
            k_ = 0;
            const std::complex<double> z = box_ / (double)dec_;
            box_ = {0, 0};
            const std::complex<double> lag = z * std::conj(prev_);
            prev_ = z;
            lr_ += lag.real(); li_ += lag.imag(); wp_ += std::norm(z);
            if (++wN_ >= perWin_) window_((double)(t_ + (double)i / rate_));
        }
        ph_ = ph / std::abs(ph);
        t_ += (double)n / rate_;
    }
private:
    void window_(double t) {
        const double p = wp_ / wN_;
        if (floor_ <= 0) floor_ = p;
        const bool keyed = p > floor_ * std::pow(10.0, kKeyedDb / 10.0);
        // the floor falls at once, rises slowly (a keyed tone must not lift its own)
        floor_ = p < floor_ ? p : floor_ * 1.02;
        if (keyed && (lr_ != 0 || li_ != 0)) {
            const double err = std::atan2(li_, lr_) * (rate_ / dec_) / (2.0 * M_PI);
            if (std::fabs(err) < 0.4 * (rate_ / dec_)) {
                f_ += std::max(-40.0, std::min(40.0, 0.6 * err));
                measAt_ = t;
            }
        }
        lr_ = li_ = wp_ = 0; wN_ = 0;
    }
    double rate_, f_;
    int dec_ = 1, perWin_ = 100, k_ = 0, wN_ = 0;
    std::complex<double> ph_{1, 0}, box_{0, 0}, prev_{0, 0};
    double lr_ = 0, li_ = 0, wp_ = 0, floor_ = 0, t_ = 0, measAt_ = 0;
};

/** ★ The first guess of a station's own drift (Stuart: "can we compensate for reverse doppler with vfo a"): most stations
 *  heard are near us, so their uncorrected uplink sees OUR range rate. Our downlink Doppler is d = −ṙ·f_down/c; theirs on
 *  the uplink is −ṙ·f_up/c = d·f_up/f_down. An inverting transponder turns an uplink rise into a downlink fall, so in the
 *  satellite frame their signal moves by −(f_up/f_down)·d (2 m up, 70 cm down: a third of our drift, the other way); a
 *  non-inverting one by +(f_up/f_down)·d. Returns that drift (Hz) for our downlink Doppler `dDownHz` now. */
inline double uplinkDriftHz(double dDownHz, double upHz, double downHz, bool inverting) {
    if (!(upHz > 0) || !(downHz > 0)) return 0.0;
    const double r = upHz / downHz;
    return (inverting ? -r : r) * dDownHz;
}

} // namespace vibexpdr
