// vibe_orbcomm_sky.h — THE LIGHT ORBCOMM SKY PLOT (V12 beta94, Stuart 11-25): unattended ORBCOMM monitoring that only
// logs WHERE each bird was heard and HOW STRONGLY — no demodulation, no packets.
//
// ★★★ WHY (Stuart: "if orbcomm is drawing an antenna plot it only needs to log the position and snr"). The full decoder
//     (vibe_orbcomm_run.h) demodulates every carrier at 4800 Bd to read the satellites' own position reports. A sky
//     plot's dot needs none of that: its azimuth and elevation come from the ORBIT (the SGP4 elements we already hold —
//     the full decoder's dots take them from the same place; the position report only NAMES the carrier), and its SNR
//     from the spectrum. So this looks at the capture four times a second, one FFT each, and that is all.
//
// ★★ HOW A CARRIER IS NAMED WITHOUT A PACKET — ITS DOPPLER CURVE. Every ORBCOMM downlink sits on the 2.5 kHz channel
//    raster (137.0 MHz + N × 2.5 kHz, vibe_orbcomm.h downlinkHz). For a peak at f and a bird b above the horizon,
//    f − Doppler_b(t) is that bird's NOMINAL frequency if the carrier is b's: it lands on the raster (within kMatchHz —
//    a dongle's ppm and the elements' age) and STAYS on the same channel with the same small residual while b's predicted
//    Doppler swings by kConfirmDopHz. A spur does not move (its residual drifts with the Doppler), another bird's carrier
//    follows another curve (likewise). Only after that does a bird's channel count as LEARNT, and only then are dots
//    drawn: each look, the peak nearest the prediction (channel + learnt residual + Doppler now) within kDotTolHz.
//    Channel listings are not trusted (they go stale — FM110's 11-24 packets listed other channels than its listing).
// ★★ REFUSED: narrow lines (the full decoder's spur cut, a bin over 4× its neighbours' median); the tuner's DC
//    (± kDcGuardHz); a carrier two learnt birds both claim (ambiguous: no dot for either); a bird whose elements are older
//    than kMaxElementAgeSec (the orbit, and so the dot's place and the Doppler match, would be untrustworthy).
// ★★ THE SNR is the full decoder's measure's equivalent: the carrier's power over ±3.4 kHz above the floor, divided by
//    the floor's power in 4.8 kHz (the symbol rate — the noise a matched filter lets through), so the two modes' dots
//    sit on one colour scale. tool-orbcomm-sky-compare.cpp measured both on the FM116 recording of 11-24.
// ★ COST: kFramesPerSec FFTs of ≤ kMaxFft points a second, and an SGP4 look per bird every 2 s. On a 2.4 MS/s capture it
//   reads 2 × 32768 of the 2.4 M samples a second (under 3 %) — vs the full decoder's whole-band slice demodulated
//   sample by sample (test-orbcomm-sky §6 and tool-orbcomm-sky-compare give the figures).
#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "vibe_orbcomm.h"
#include "vibe_sat_orbit.h"
#include "vibe_skyplot.h"

namespace vibeorb {

struct SkyBird {
    long long norad = 0;
    std::string name;
    vibe::sat::Sgp4 sgp;
    bool ok = false;
    double epochUnix = 0;   ///< the elements' epoch (0 = not known: not judged on age)
};

class SkyWatch {
public:
    static constexpr double kBandLoHz = 137.19e6, kBandHiHz = 137.81e6;   ///< the channels in use + Doppler
    static constexpr double kMatchHz = 400;        ///< f − Doppler within this of a raster channel (ppm + elements' age)
    /* ★★ THE CONFIRMATION IS A FIT, NOT A SPREAD (the FM116 recording of 11-24: its true carriers' residual wanders
     *  ±90 Hz look to look — a 4.8 kHz-wide carrier's centre in 1 s of spectrum — so "spread under 60 Hz" never named it).
     *  The residual e is fitted against the bird's predicted Doppler d: e = a + b·d. The bird's own carrier: b ≈ 0. A
     *  spur: b = −1 (it does not move). Another bird's carrier: b = (its Doppler's change ÷ this one's) − 1, far from 0
     *  unless both birds' curves run in step. */
    static constexpr double kConfirmDopHz = 1000;  ///< the predicted Doppler must swing this much over the looks
    static constexpr double kConfirmSlope = 0.15;  ///< |b| under this
    static constexpr double kConfirmRmsHz = 150;   ///< …and the looks within this of the fitted line (RMS)
    static constexpr int kConfirmLooks = 15;
    static constexpr int kMaxChansPerBird = 3;     ///< ★ a bird can send on two channels at once (FM116 11-24: 137.6625 + 137.800)
    /** ★ The SNR's calibration against the full decoder's (tool-orbcomm-sky-compare on the FM116 recording of 11-24:
     *  light − full = +1.3 dB mean, 0.7 dB sd over the shared 5 s buckets) — so both modes' dots share one colour scale. */
    static constexpr double kSnrCalDb = -1.3;
    static constexpr int kBackfillLooks = 600;
    /** ★ The full decoder's dots start at ~4 dB (it cannot decode below); a carrier the light mode still sees under this
     *  is not drawn, so the two modes' plots mean the same thing (FM116 11-24: 18 extra 0–2 dB dots at 18°→7° otherwise). */
    static constexpr int kMinDotSnrDb = 3;
    static constexpr double kDotTolHz = 300;       ///< a learnt bird's peak within this of its prediction = a dot
    static constexpr double kForgetSec = 120;      ///< a learnt channel not seen this long (bird above 5°) is forgotten
    static constexpr double kDcGuardHz = 2000;
    static constexpr double kMinDb = 3.0;          ///< a peak over the floor (the full decoder's Scanner::kMinDb)
    static constexpr double kMaxElementAgeSec = 7 * 86400.0;
    static constexpr int kFramesPerLook = 4;
    static constexpr double kFramesPerSec = 2;     ///< ★ a look (kFramesPerLook frames) every 2 s
    static constexpr int kMaxFft = 131072;

    struct Opts {
        std::function<std::vector<SkyBird>()> birds;
        std::function<bool(vibe::sat::Observer&)> observer;
        std::function<void(const std::vector<vibe::skyplot::Dot>&)> dots;
        std::function<void(const std::string&)> log;
    };
    struct Peak { double rfHz = 0, snrDb = 0, db = 0; };
    struct Learnt { double offsetHz = 0, lastHit = 0, since = 0; };
    using ChanKey = std::pair<long long, int>;   ///< (bird, downlink channel number)

    explicit SkyWatch(Opts o) : o_(std::move(o)) {}

    /** The FFT size for a capture rate: bins of ≤ ~80 Hz (the carrier's ±3.4 kHz is ~85 bins), at most kMaxFft. */
    static int fftFor(double rate) { int n = 1024; while (rate / double(n) > 80.0 && n < kMaxFft) n <<= 1; return n; }

    /** One frame of `n` (= fftFor(rate)) capture samples whose last sample is at `utc`, the tuner at `hwCentreHz`. */
    void frame(const std::complex<float>* x, int n, double rate, double hwCentreHz, double utc) {
        if (n < 256 || !(rate > 0)) return;
        if (size_t(n) != win_.size()) {
            win_.resize(size_t(n));
            for (int i = 0; i < n; i++) win_[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(n - 1)));
            acc_.assign(size_t(n), 0.0); frames_ = 0;
        }
        if (frames_ && (rate != rate_ || hwCentreHz != centre_)) { std::fill(acc_.begin(), acc_.end(), 0.0); frames_ = 0; }
        rate_ = rate; centre_ = hwCentreHz;
        buf_.resize(size_t(n));
        for (int i = 0; i < n; i++) buf_[size_t(i)] = x[i] * win_[size_t(i)];
        vibelrpt::fftInPlace(buf_);
        for (int i = 0; i < n; i++) acc_[size_t(i)] += std::norm(buf_[size_t(i)]);
        if (++frames_ >= kFramesPerLook) { look_(utc); std::fill(acc_.begin(), acc_.end(), 0.0); frames_ = 0; }
    }
    /** Write out what is bucketed (a stop). */
    void flush() { flushDots_(1e18); }

    // ── for the status line and the tests ──
    const std::vector<Peak>& peaks() const { return peaks_; }
    const std::map<ChanKey, Learnt>& learnt() const { return learnt_; }
    /** The channel numbers learnt for `norad` (empty = not named yet). */
    std::vector<int> channelsOf(long long norad) const { std::vector<int> v; for (auto& kv : learnt_) if (kv.first.first == norad) v.push_back(kv.first.second); return v; }
    /** The candidates still being judged, in words (a developer's replay prints them). */
    std::vector<std::string> candidates() const {
        std::vector<std::string> v;
        for (auto& kv : cands_) {
            const Cand& c = kv.second; double sl = 0, rms = 0; c.fit(sl, rms);
            char b[200]; std::snprintf(b, sizeof b, "%lld ch %.4f: %d looks, Doppler %+.0f .. %+.0f Hz, residual %+.0f, slope %+.2f, rms %.0f Hz", kv.first.first, downlinkHz(kv.first.second) / 1e6, c.n, c.dLo, c.dHi, c.mean(), sl, rms);
            v.push_back(b);
        }
        return v;
    }
    uint64_t looks() const { return looks_; }
    uint64_t dotsWritten() const { return dotsWritten_; }
    uint64_t refusedAmbiguous() const { return ambiguous_; }
    int birdsUp() const { return up_; }
    std::string brief() const {
        char b[160];
        std::set<long long> named; for (auto& kv : learnt_) named.insert(kv.first.first);
        std::snprintf(b, sizeof b, "%d birds up, %zu named by their Doppler, %llu sky dots", up_, named.size(), (unsigned long long)dotsWritten_);
        return b;
    }

private:
    struct Cand {
        double t0 = 0, tLast = 0, dLo = 1e18, dHi = -1e18, sD = 0, sD2 = 0, sE = 0, sE2 = 0, sDE = 0; int n = 0;
        struct Seen { double t; int snr; float az, el; };
        std::vector<Seen> seen;   ///< ★ the looks before it was named — drawn once it is (≤ kBackfillLooks)
        void add(double d, double e) { n++; sD += d; sD2 += d * d; sE += e; sE2 += e * e; sDE += d * e; dLo = std::min(dLo, d); dHi = std::max(dHi, d); }
        double mean() const { return n ? sE / n : 0; }
        /** The fit e = a + b·d: slope b, the residual at the mean Doppler, and the RMS about the line. */
        bool fit(double& b, double& rms) const {
            const double den = n * sD2 - sD * sD;
            if (n < 3 || den <= 0) return false;
            b = (n * sDE - sD * sE) / den;
            const double a = (sE - b * sD) / n;
            const double ss = sE2 - 2 * a * sE - 2 * b * sDE + n * a * a + 2 * a * b * sD + b * b * sD2;
            rms = std::sqrt(std::max(0.0, ss / n));
            return true;
        }
    };
    struct Bucket { double t0 = 0; int snr = -127; int n = 0; float az = 0, el = 0; };
    struct UpBird { SkyBird* b; vibe::sat::Look L; };

    void refreshBirds_(double t) {
        birdsAt_ = t;
        if (o_.birds) birds_ = o_.birds();
        vibe::sat::Observer ob;
        if (o_.observer && o_.observer(ob)) obs_ = ob;
    }
    /** The averaged spectrum → the peaks inside the ORBCOMM band (and the capture's flat part). */
    void findPeaks_() {
        peaks_.clear();
        const int n = int(acc_.size()), half = n / 2;
        const double bin = rate_ / double(n);
        const double lo = std::max(kBandLoHz, centre_ - 0.45 * rate_), hi = std::min(kBandHiHz, centre_ + 0.45 * rate_);
        if (hi - lo < 20000) return;
        const int k0 = int(std::ceil((lo - centre_) / bin)), k1 = int(std::floor((hi - centre_) / bin));
        std::vector<double> p;
        p.reserve(size_t(k1 - k0 + 1));
        for (int k = k0; k <= k1; k++) p.push_back(acc_[size_t((k + n) % n)] / double(frames_));
        (void)half;
        // ★ narrow lines out (the full decoder's Scanner rule): a bin over 4× the median of its ±6 neighbours → that median
        {
            std::vector<double> c = p, w;
            for (size_t i = 0; i < p.size(); i++) {
                w.clear();
                for (long j = long(i) - 6; j <= long(i) + 6; j++) if (j >= 0 && j < long(p.size()) && j != long(i)) w.push_back(p[size_t(j)]);
                std::nth_element(w.begin(), w.begin() + long(w.size() / 2), w.end());
                if (p[i] > 4 * w[w.size() / 2]) c[i] = w[w.size() / 2];
            }
            p.swap(c);
        }
        std::vector<double> s = p; std::nth_element(s.begin(), s.begin() + long(s.size() * 3 / 10), s.end());
        const double floor = std::max(1e-30, s[s.size() * 3 / 10]);
        // ★ the tuner's DC: its bins are the floor (a DC spike must never be a carrier)
        for (size_t i = 0; i < p.size(); i++) if (std::fabs(centre_ + (double(k0) + double(i)) * bin - centre_) < kDcGuardHz) p[i] = floor;
        const int w = std::max(1, int(std::lround(5000 / bin)));
        std::vector<double> sm(p.size(), 0.0);
        { double a = 0; int c = 0; long L = 0, R = -1;
          for (size_t i = 0; i < p.size(); i++) {
              const long want0 = long(i) - w / 2, want1 = long(i) + w / 2;
              while (R < want1 && R + 1 < long(p.size())) { ++R; a += p[size_t(R)]; c++; }
              while (L < want0) { a -= p[size_t(L)]; c--; ++L; }
              sm[i] = a / std::max(1, c);
          } }
        const int sep = std::max(1, int(6000 / bin));
        const double thr = floor * std::pow(10.0, kMinDb / 10.0);
        const int h = std::max(1, int(3400 / bin));
        const double nbw = 4800.0 / bin;   // the floor's power in 4.8 kHz
        for (size_t i = 0; i < sm.size(); i++) {
            if (sm[i] < thr) continue;
            bool top = true;
            for (int j = -sep; j <= sep && top; j++) { const long q = long(i) + j; if (j && q >= 0 && q < long(sm.size()) && sm[size_t(q)] > sm[i]) top = false; }
            if (!top) continue;
            double num = 0, den = 0;
            for (int j = -h; j <= h; j++) { const long q = long(i) + j; if (q < 0 || q >= long(p.size())) continue; const double ex = std::max(0.0, p[size_t(q)] - floor); num += ex * double(q); den += ex; }
            const double ci = den > 0 ? num / den : double(i);
            Peak pk;
            pk.rfHz = centre_ + (double(k0) + ci) * bin;
            pk.db = 10 * std::log10(sm[i] / floor);
            pk.snrDb = den > 0 ? 10 * std::log10(den / (floor * nbw)) : -10;
            peaks_.push_back(pk);
            i += size_t(sep);
        }
    }
    void look_(double t) {
        looks_++;
        if (t - birdsAt_ > 600 || birdsAt_ == 0) refreshBirds_(t);
        findPeaks_();
        flushDots_(t);
        if (!obs_.valid) return;
        std::vector<UpBird> up;
        for (auto& b : birds_) {
            if (!b.ok) continue;
            if (b.epochUnix > 0 && std::fabs(t - b.epochUnix) > kMaxElementAgeSec) continue;   // ★ stale orbit: no dot
            const vibe::sat::Look L = vibe::sat::look(b.sgp, obs_, t);
            if (L.ok && L.elDeg > 0) up.push_back({ &b, L });
        }
        up_ = int(up.size());
        // 1. what each learnt (bird, channel) predicts now — and which peak it explains
        struct Pred { ChanKey k; const UpBird* u; double hz; size_t peak; };
        std::vector<Pred> preds;
        std::vector<int> explained(peaks_.size(), 0);
        for (const UpBird& u : up)
            for (auto it = learnt_.lower_bound({ u.b->norad, INT_MIN }); it != learnt_.end() && it->first.first == u.b->norad; ++it) {
                const double nominal = downlinkHz(it->first.second) + it->second.offsetHz;
                const double pred = nominal + u.L.dopplerHz(nominal);
                size_t best = SIZE_MAX; double bd = 1e18;
                for (size_t i = 0; i < peaks_.size(); i++) { const double d = std::fabs(peaks_[i].rfHz - pred); if (d < bd) { bd = d; best = i; } }
                if (best == SIZE_MAX || bd > kDotTolHz) continue;
                explained[best]++;
                preds.push_back({ it->first, &u, pred, best });
            }
        // 2. learning: every (bird, channel) a peak NOT already explained fits (★ FM116's carrier must never teach FM20 —
        //    the FM116 recording of 11-24 did, before this: a neighbouring bird on 137.6600 at −236 Hz)
        for (size_t pi = 0; pi < peaks_.size(); pi++) {
            if (explained[pi]) continue;
            const Peak& pk = peaks_[pi];
            for (const UpBird& u : up) {
                const double dop = u.L.dopplerHz(pk.rfHz);
                const double nom = pk.rfHz - dop;
                const int dcn = int(std::lround((nom - kDownBaseHz) / kChanStepHz));
                const double e = nom - downlinkHz(dcn);
                if (std::fabs(e) > kMatchHz) continue;
                const ChanKey key{ u.b->norad, dcn };
                if (learnt_.count(key)) continue;
                Cand& c = cands_[key];
                if (c.n && t - c.tLast > 20) c = Cand();
                if (!c.n) c.t0 = t;
                c.tLast = t; c.add(dop, e);
                if (c.seen.size() < size_t(kBackfillLooks)) c.seen.push_back({ t, int(std::lround(pk.snrDb + kSnrCalDb)), float(u.L.azDeg), float(u.L.elDeg) });
                double slope = 0, rms = 0;
                if (c.n < kConfirmLooks || c.dHi - c.dLo < kConfirmDopHz || !c.fit(slope, rms)) continue;
                if (std::fabs(slope) > kConfirmSlope || rms > kConfirmRmsHz) continue;
                const double m = c.mean();
                const size_t had = channelsOf(u.b->norad).size();
                if (had >= size_t(kMaxChansPerBird)) continue;
                Learnt& l = learnt_[key];
                l.offsetHz = m; l.lastHit = t; l.since = t;
                if (o_.log) { char b[240]; std::snprintf(b, sizeof b, "ORBCOMM sky plot: %s named on %.4f MHz by its Doppler (residual %+.0f Hz, slope %+.2f, %.0f Hz about the fit, %d looks over %.0f Hz of Doppler)", u.b->name.c_str(), downlinkHz(dcn) / 1e6, m, slope, rms, c.n, c.dHi - c.dLo); o_.log(b); }
                /* ★ The looks that named it are drawn too (the full decoder draws from its first packet; without this the
                 *  first ~45 s of every bird's pass would be missing) — only for the bird's FIRST channel, whose bucket
                 *  holds nothing yet, and before this look's own dot. */
                if (had == 0) for (const Cand::Seen& sn : c.seen) if (sn.snr >= kMinDotSnrDb) addDot_(u.b->norad, sn.t, sn.snr, sn.az, sn.el);
                cands_.erase(key);
                break;   // one peak, one bird
            }
        }
        for (auto it = cands_.begin(); it != cands_.end();) it = t - it->second.tLast > 20 ? cands_.erase(it) : std::next(it);
        if (cands_.size() > 4096) cands_.clear();   // ★ bounded
        // 3. dots: each learnt carrier found where its channel + Doppler say — unless two birds claim one carrier
        std::map<long long, std::pair<int, const UpBird*>> best;   // bird → its strongest carrier's SNR this look
        for (const Pred& p : preds) {
            if (explained[p.peak] > 1) {
                bool sameBird = true;
                for (const Pred& q : preds) if (q.peak == p.peak && q.k.first != p.k.first) sameBird = false;
                if (!sameBird) { ambiguous_++; continue; }   // ★ two birds on one carrier: neither
            }
            Learnt& l = learnt_[p.k];
            const Peak& pk = peaks_[p.peak];
            // the residual follows slowly (the elements age through a day; a dongle warms)
            l.offsetHz += 0.1 * ((pk.rfHz - p.u->L.dopplerHz(pk.rfHz) - downlinkHz(p.k.second)) - l.offsetHz);
            l.lastHit = t;
            const int snr = int(std::lround(pk.snrDb + kSnrCalDb));
            auto bi = best.find(p.k.first);
            if (bi == best.end() || snr > bi->second.first) best[p.k.first] = { snr, p.u };
        }
        for (auto& kv : best) if (kv.second.first >= kMinDotSnrDb) addDot_(kv.first, t, kv.second.first, float(kv.second.second->L.azDeg), float(kv.second.second->L.elDeg));
        for (auto it = learnt_.begin(); it != learnt_.end();) {
            bool high = false;
            for (const UpBird& u : up) if (u.b->norad == it->first.first && u.L.elDeg > 5) high = true;
            it = (high && t - it->second.lastHit > kForgetSec) ? learnt_.erase(it) : std::next(it);
        }
    }
    void addDot_(long long norad, double t, int snr, float az, float el) {
        Bucket& b = buckets_[norad];
        if (b.n && t - b.t0 >= vibe::skyplot::kBucketSec) flushOne_(norad, b);
        // ★ buckets on the 5 s grid (floor), so a replay's dots line up with a clock's
        if (!b.n) { b.t0 = std::floor(t / vibe::skyplot::kBucketSec) * vibe::skyplot::kBucketSec; b.snr = snr; }
        b.snr = std::max(b.snr, snr); b.n++; b.az = az; b.el = el;
    }
    void flushOne_(long long norad, Bucket& b) {
        if (!b.n) return;
        vibe::skyplot::Dot d;
        d.t = uint32_t(b.t0); d.azDeg = b.az; d.elDeg = b.el; d.snr = int8_t(std::max(-127, std::min(127, b.snr)));
        d.kind = vibe::skyplot::kOrbcomm; d.count = uint16_t(std::min(65535, b.n)); d.norad = uint32_t(norad);
        pending_.push_back(d);
        b = Bucket();
    }
    void flushDots_(double t) {
        for (auto& kv : buckets_) if (kv.second.n && t - kv.second.t0 >= vibe::skyplot::kBucketSec) flushOne_(kv.first, kv.second);
        if (!pending_.empty() && o_.dots) o_.dots(pending_);
        dotsWritten_ += pending_.size();
        pending_.clear();
    }

    Opts o_;
    std::vector<float> win_;
    std::vector<std::complex<float>> buf_;
    std::vector<double> acc_;
    int frames_ = 0;
    double rate_ = 0, centre_ = 0, birdsAt_ = 0;
    std::vector<SkyBird> birds_;
    vibe::sat::Observer obs_;
    std::vector<Peak> peaks_;
    std::map<ChanKey, Cand> cands_;
    std::map<ChanKey, Learnt> learnt_;
    std::map<long long, Bucket> buckets_;
    std::vector<vibe::skyplot::Dot> pending_;
    uint64_t looks_ = 0, dotsWritten_ = 0, ambiguous_ = 0;
    int up_ = 0;
};

}  // namespace vibeorb
