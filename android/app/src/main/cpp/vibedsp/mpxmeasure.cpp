// VibeSDR — MpxMeasure: the Advanced RDS instrument's own receiver. Original VibeSDR code.
//
// ★★★ READ THE CLASS NOTE IN vibedsp.h FIRST. In one line: every Advanced RDS measurement is taken
//     here, from the channel BEFORE the listener's passband filter, at ONE fixed rate through ONE
//     flat filter, so neither the listener's passband, auto bandwidth nor the capture rate can move
//     a transmitter constant. The listener's audio and RDS decoder are untouched by any of it.
#include "vibedsp.h"
#if defined(__linux__)
#include <pthread.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include "simd_internal.h"

namespace {
// ★ The instrument's pilot-PLL gear shift — see MpxMeasure::reset_. Fractions of the 19 kHz pilot.
constexpr double kPllWide = 0.01;            // ~190 Hz: acquisition, and the listener's own loop
constexpr double kPllNarrow = 0.002;         // ~38 Hz: measurement (programme no longer pulls the phase)
constexpr double kPllNarrowAfterSec = 1.5;   // of steady tracking before narrowing
}

namespace vibedsp {

namespace {

/** out/in as L/M with L capped — exact when both rates are whole hertz and the reduced L fits. */
void rationalFor(double inRate, double outRate, int maxL, int& L, int& M) {
    const long long a = std::llround(outRate), b = std::llround(inRate);
    if (std::fabs(inRate - (double)b) < 1e-6 && a > 0 && b > 0) {
        const long long g = std::gcd(a, b);
        if (a / g <= maxL) { L = (int)(a / g); M = (int)(b / g); return; }
    }
    // Best approximation with L <= maxL (a few ppm at worst): the channel rate is then reported
    // exactly by chanRate() and every design below uses the actual figure.
    double bestErr = 1e9; L = 1; M = std::max(1, (int)std::llround(inRate / outRate));
    for (int l = 1; l <= maxL; ++l) {
        const int m = std::max(1, (int)std::llround(inRate * l / outRate));
        const double err = std::fabs(inRate * l / m - outRate);
        if (err < bestErr - 1e-9) { bestErr = err; L = l; M = m; }
    }
}

/** sin(pi x)/(pi x) */
inline double sincPi(double x) { return (std::fabs(x) < 1e-12) ? 1.0 : std::sin(M_PI * x) / (M_PI * x); }

}  // namespace

MpxMeasure::~MpxMeasure() { stop(); }

void MpxMeasure::stop() {
    if (!running_) return;
    { std::lock_guard<std::mutex> lk(qM_); qStop_ = true; }
    qCv_.notify_all();
    qIdleCv_.notify_all();
    if (thr_.joinable()) thr_.join();
    running_ = false;
    std::lock_guard<std::mutex> lk(qM_);
    qStop_ = false; qCount_ = 0; qHead_ = 0; gapPending_ = false; draining_ = false;
}

void MpxMeasure::startWorker_() {
    if (running_) return;
    running_ = true;
    thr_ = std::thread([this] {
        // ★★ DECODER PRIORITY — the bottom of Network > Audio > Spectrum > Decoders. The host maps
        //    "vibe-mpx" to it (local_sdr_shim.cpp); an instrument must never be what makes audio late.
        if (RxPipeline::workerInit()) RxPipeline::workerInit()("vibe-mpx");
#if defined(__linux__)
        else pthread_setname_np(pthread_self(), "vibe-mpx");
#endif
        std::unique_lock<std::mutex> lk(qM_);
        for (;;) {
            qCv_.wait(lk, [this] { return qCount_ > 0 || qStop_; });
            if (qStop_) return;                         // pending blocks are measurements nobody waits for
            work_.swap(q_[qHead_]);
            const int n = qN_[qHead_];
            const unsigned gen = qGen_[qHead_];
            const bool gap = qGap_[qHead_];
            qHead_ = (qHead_ + 1) % kQ;
            --qCount_;
            qIdleCv_.notify_all();
            lk.unlock();
            process_(work_.data(), n, gen, gap);
            lk.lock();
        }
    });
}

void MpxMeasure::feed(const cf32* iq, int n, unsigned gen, bool gap) {
    if (n <= 0 || !(inRate_ > 0.0)) return;
    if (!threadedWant_) { process_(iq, n, gen, gap); return; }
    if (!running_) startWorker_();
    /* ★★★ THE QUEUE IS COUNTED IN BLOCKS, SO A BLOCK MUST BE WORTH SOMETHING IN TIME. The radio
     *  decides how big a block is: an RTL hands the DSP thread 16k samples (~7 ms), but the SDRplay
     *  API calls back ~2900 times a second with ~1000 samples (0.34 ms) and the pipeline feeds us each
     *  one. kQ = 6 of THOSE is two milliseconds of slack — and the DSP thread works in bursts (on the
     *  Lenovo's RSP1A it slept ~30 times a second and ran ~90 blocks back to back each time), so the
     *  queue filled in every burst while `vibe-mpx` sat at 4.8 % of a core waiting to be woken. It
     *  dropped ~700 blocks a second, every drop is a hole, and a hole holds every figure for
     *  kGapHoldSec — so the panel NEVER published: no pilot, no phase, empty eyes, on an i5 doing
     *  nothing (2026-10-01, measured on the box; the same build's scopes worked on every RTL).
     *  ★ So the feeder ACCUMULATES to kMinBlockSec before queueing. Six of those are >= 120 ms of
     *    slack whatever the radio's callback size, and the low-priority thread is woken ~50 times a
     *    second instead of ~3000. The instrument is a 6 Hz panel; 20 ms of latency is invisible.
     *  ★ The copy is still made OUTSIDE the lock, into the spare buffer that is then swapped in — so
     *    the DSP thread never holds the queue while copying, and never waits on a low-priority
     *    thread that does. The worker only ever holds the lock for a swap.
     *  ★ What is being accumulated belongs to ONE measurement and ONE side of any hole: a new `gen`
     *    makes it obsolete (the worker would reset on the new gen anyway), and a hole sends it on
     *    its way first so the gap flag lands on the samples AFTER the hole and no others. */
    if (!spare_.empty() && gen != spareGen_) { spare_.clear(); spareGap_ = false; }
    if (gap && !spare_.empty()) enqueue_();
    if (spare_.empty()) { spareGen_ = gen; spareGap_ = gap; }
    spare_.insert(spare_.end(), iq, iq + n);
    const size_t minBlock = (size_t)std::max(1.0, std::ceil(inRate_ * kMinBlockSec));
    if (spare_.size() >= minBlock) enqueue_();
}

void MpxMeasure::enqueue_() {
    const unsigned gen = spareGen_;
    const bool gap = spareGap_;
    const int n = (int)spare_.size();
    spareGap_ = false;
    std::unique_lock<std::mutex> lk(qM_);
    // ★ A hole UPSTREAM (the caller's) joins our own drops in gapPending_: if this very block is
    //   then dropped too, the flag waits for the next one that gets in, exactly as a drop does.
    if (gap) gapPending_ = true;
    /* ★★★ AND ONCE DROPPING, KEEP DROPPING UNTIL THE WORKER HAS CAUGHT UP. Dropping one block each
     *  time a slot frees turned a worker at half speed into a hole every other block — and a hole
     *  now costs a hold (kGapHoldSec), so the panel would never publish at all. Refusing until the
     *  queue has drained to kQ/2 loses the SAME share of the signal in fewer, longer runs, leaving
     *  contiguous stretches long enough to measure. */
    if (draining_ && qCount_ > kQ / 2 && !blocking_.load(std::memory_order_relaxed)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        gapPending_ = true;
        spare_.clear();
        return;
    }
    draining_ = false;
    if (qCount_ >= kQ) {
        if (blocking_.load(std::memory_order_relaxed)) {
            qIdleCv_.wait(lk, [this] { return qCount_ < kQ || qStop_; });
        } else {
            /* ★★★ DROP, NEVER WAIT. The instrument is behind; audio is not allowed to find out.
             *  The next block that does get in carries `gap`, and the deviation window that
             *  straddles the hole is discarded rather than read (see winTainted_). */
            dropped_.fetch_add(1, std::memory_order_relaxed);
            gapPending_ = true;
            draining_ = true;
            spare_.clear();
            return;
        }
    }
    const int slot = (qHead_ + qCount_) % kQ;
    q_[slot].swap(spare_);
    qN_[slot] = n; qGen_[slot] = gen; qGap_[slot] = gapPending_;
    gapPending_ = false;
    ++qCount_;
    lk.unlock();
    spare_.clear();                                  // the slot's old buffer, kept for its capacity
    qCv_.notify_one();
}

void MpxMeasure::configure(double inRate) {
    stop();
    spare_.clear(); spareGap_ = false;   // half a block at the old rate belongs to nobody
    inRate_ = inRate;
    built_ = false;      // ★ designed on first use, on the worker — see build()
    gen_ = ~0u;          // the first block restarts everything
}

/* ★★ THE CHAIN IS DESIGNED LAZILY, ON THE INSTRUMENT'S OWN THREAD. configure() runs from
 *  RxPipeline::start() — on the DSP thread, at radio start and on every per-listener channel rebuild —
 *  and the design below is not free (the 2:1 equaliser is iterated; a 10 MS/s channel filter is 16k
 *  taps). Nobody may pay for an instrument they have not opened, and nobody's audio may wait for it,
 *  so it is built by the first block that actually reaches process_(). */
void MpxMeasure::build() {
    if (built_) return;
    built_ = true;
    const double inRate = inRate_;
    decs_.clear();
    stageTaps_.clear();
    if (!(inRate > 0.0)) return;

    /* ── 1. INTEGER DECIMATION to fs1 in [600k, 1.2M) ─────────────────────────────────────────────
     *  Anti-alias only: each stage keeps ±160 kHz flat and stops what would fold into ±200 kHz at
     *  its own output. Blackman (≈74 dB): what folds here can never be removed, and the passband
     *  ripple of a Blackman stage (≈0.002 dB) is what keeps the whole instrument flat.
     *  ★★ DESIGNED WITH THE PASSBAND WHERE IT SAYS IT IS. designLowpass() places its −6 dB point
     *     at `cutoff`, and the transition straddles it — so cutoff is the MIDPOINT of pass and stop,
     *     never the pass edge. The listener's early stages put the −6 dB point AT the channel edge
     *     with a transition hundreds of kHz wide, which is why their passband droops from DC up and
     *     why the droop moved with the capture rate (see the class note, loss 2). */
    /* ★ THE DECIMATION IS CHOSEN BY COST, like the listener's (see rebuildAudio): every D that lands
     *  fs1 in [600k, 1.2M] is planned with the same formulas the build below uses, costed as taps x
     *  output rate for each stage plus the channel filter's taps per phase x kChanRate, and the
     *  cheapest taken. floor(fs/600k) alone put 8 MS/s on a PRIME 13 — one 173-tap stage, twice the
     *  cost of the whole path at 10 MS/s. */
    const auto tapsFor = [](double trans, bool deep) {
        int n = (int)std::ceil((deep ? 5.5 : 3.3) / std::max(trans, 1e-4));
        if ((n & 1) == 0) ++n;
        return std::max(n, 9);
    };
    const auto planFor = [&](int D, std::vector<int>& st) {
        st.clear();
        std::vector<int> primes;
        int r = D;
        for (int q = 2; q * q <= r; ++q) while (r % q == 0) { primes.push_back(q); r /= q; }
        if (r > 1) primes.push_back(r);
        int twos = 0;
        for (int q : primes) { if (q == 2) ++twos; else st.push_back(q); }
        for (; twos >= 2; twos -= 2) st.push_back(4);
        if (twos) st.push_back(2);
        std::sort(st.begin(), st.end(), [](int x, int y) { return x > y; });
        double cost = 0.0, f = inRate;
        for (int d : st) {
            const double fo = f / d;
            cost += tapsFor(((fo - kStopHz) - (kPassHz + 10000.0)) / f, true) * fo;
            f = fo;
        }
        cost += std::ceil(5.5 * f / (kStopHz - kPassHz)) * kChanRate;   // the channel filter, per phase
        return cost;
    };
    std::vector<int> st;
    {
        const int dLo = std::max(1, (int)std::ceil(inRate / 1200000.0));
        const int dHi = std::max(1, (int)std::floor(inRate / 600000.0));
        double best = -1.0;
        std::vector<int> tryst;
        for (int D = dLo; D <= dHi; ++D) {
            const double c = planFor(D, tryst);
            if (best < 0.0 || c < best) { best = c; st = tryst; }
        }
        if (best < 0.0) st.clear();                  // below 1.2 MS/s: straight into the channel filter
    }
    double fs = inRate;
    for (int d : st) {
        const double fsOut = fs / d;
        const double pass = kPassHz + 10000.0, stopHz = fsOut - kStopHz;
        std::vector<float> t = designLowpass(0.5 * (pass + stopHz) / fs, (stopHz - pass) / fs, /*deepStop=*/true);
        stageTaps_.emplace_back(t, fs);
        decs_.push_back(std::make_unique<PairDec<cf32>>(t, d));
        fs = fsOut;
    }
    fs1_ = fs;

    /* ── 2. THE CHANNEL FILTER, as the prototype of a rational resampler to kChanRate ────────────
     *  Flat to kPassHz, rejecting from kStopHz, Blackman windowed-sinc: passband ripple ≈ 0.003 dB,
     *  stopband ≈ 73 dB — characterised from its taps by test-mpx-measure. ONE filter, whatever came
     *  before it: designed at L·fs1 in hertz, so its shape in hertz is identical at every rate. */
    rationalFor(fs1_, kChanRate, 1024, rsL_, rsM_);
    rc_ = fs1_ * rsL_ / rsM_;
    {
        const double fsP = fs1_ * rsL_;
        std::vector<float> h = designLowpass(0.5 * (kPassHz + kStopHz) / fsP, (kStopHz - kPassHz) / fsP,
                                             /*deepStop=*/true);
        rsProto_ = h;
        rsK_ = (int)((h.size() + rsL_ - 1) / rsL_);
        h.resize((size_t)rsK_ * rsL_, 0.0f);
        rsTaps_.assign((size_t)rsK_ * rsL_, 0.0f);
        for (int p = 0; p < rsL_; ++p)
            for (int j = 0; j < rsK_; ++j)
                rsTaps_[(size_t)p * rsK_ + (rsK_ - 1 - j)] = h[(size_t)p + (size_t)j * rsL_] * (float)rsL_;
    }

    /* ── 3. THE MULTIPLEX: discriminator at rc_, then 2:1 to kMpxRate with its sinc UNDONE ─────────
     *  A phase difference per sample is the instantaneous frequency AVERAGED over that sample — a
     *  sinc(f/rc) response, −3.6 % at 57 kHz and −0.4 % at 19 kHz here. Known exactly, so it is
     *  equalised exactly: the 2:1 filter is designed (windowed frequency sampling) to 1/sinc across
     *  its passband. Flat to 85 kHz: RDS, its guard at 63 kHz and the deviation meter's noise guard
     *  at 80 kHz all sit inside it; what folds lands above 85 kHz. */
    fm_.setGain((float)(rc_ / (2.0 * M_PI * 75000.0)));
    dc_.configure(rc_);
    {
        const double pass = 85000.0;
        // The decimated rate is rc/2: content at f folds to rc/2 − f, so to keep 0..pass clean the
        // stop edge is rc/2 − pass.
        const double stopE = rc_ / 2.0 - pass;
        const double cut = 0.5 * (pass + stopE);
        int n = (int)std::ceil(3.3 / ((stopE - pass) / rc_));
        if ((n & 1) == 0) ++n;
        const double mid = (n - 1) / 2.0;
        const int NF = 2048;
        // The target on the design grid: 1/sinc to the cutoff, nothing above it.
        std::vector<double> want((size_t)NF + 1), D((size_t)NF + 1);
        for (int q = 0; q <= NF; ++q) want[(size_t)q] = D[(size_t)q] = 1.0 / sincPi(cut * q / NF / rc_);
        std::vector<float> taps((size_t)n);
        /* ★ WINDOWED FREQUENCY SAMPLING, THEN CORRECTED. A windowed design misses a non-flat target
         *  by a few tenths of a percent (the window smears it); twelve rounds of "add back what was
         *  missed" across the passband take the error to a few hundredths of a percent at every frequency
         *  that is measured — pilot, stereo, RDS and both guards. Once, at configure. */
        for (int it = 0; it < 12; ++it) {
            for (int i = 0; i < n; ++i) {
                const double k = i - mid;
                // (2/rc) ∫0^cut D(f) cos(2π f k / rc) df — the cosines by recurrence, not by libm
                const double step = 2.0 * M_PI * (cut / NF) * k / rc_, c1 = 2.0 * std::cos(step);
                double cPrev = std::cos(-step), cNow = 1.0, acc = 0.0;
                for (int q = 0; q <= NF; ++q) {
                    const double w = (q == 0 || q == NF) ? 0.5 : 1.0;
                    acc += w * D[(size_t)q] * cNow;
                    const double cNext = c1 * cNow - cPrev; cPrev = cNow; cNow = cNext;
                }
                acc *= 2.0 * (cut / NF) / rc_;
                taps[(size_t)i] = (float)(acc * (0.54 - 0.46 * std::cos(2.0 * M_PI * i / (n - 1))));
            }
            for (int q = 0; q <= NF; ++q) {
                const double f = cut * q / NF;
                if (f > pass) break;
                double re = 0.0;                          // symmetric taps: the response is real about mid
                for (int i = 0; i < n; ++i) re += taps[(size_t)i] * std::cos(2.0 * M_PI * f / rc_ * (i - mid));
                D[(size_t)q] += want[(size_t)q] - re;
            }
        }
        dec2Taps_ = taps;
        dec2_ = std::make_unique<PairDec<float>>(taps, 2);
    }
    // The MPX spectrum is taken at rc_ (0-100 kHz needs more than 192k), so it gets the same
    // correction per bin, in dB.
    sincDbCorr_.assign(kMpxFft / 2 + 1, 0.0f);
    for (int k = 0; k <= kMpxFft / 2; ++k)
        sincDbCorr_[(size_t)k] = (float)(-20.0 * std::log10(std::max(1e-6, sincPi((double)k / kMpxFft))));

    rds_.configure(kMpxRate, RdsDecoder::Callbacks{});   // ★ NO callbacks: this demod MEASURES, never names
    noise_.configure(kMpxRate);
    multipath_.configure(rc_);
    if (std::getenv("VIBE_DSP_PLAN")) {
        std::fprintf(stderr, "[mpxmeasure] in=%.0f stages:", inRate);
        for (auto& d : decs_) std::fprintf(stderr, " /%d(%d taps)", d->decim(), d->taps());
        std::fprintf(stderr, " -> %.0f, x%d/%d (%d taps/phase) -> chan %.3f, mpx /2 -> %.3f\n",
                     fs1_, rsL_, rsM_, rsK_, rc_, rc_ / 2.0);
    }
    reset_();
}

namespace {
double dtftMag(const std::vector<float>& h, double fNorm) {
    double re = 0.0, im = 0.0;
    for (size_t k = 0; k < h.size(); ++k) {
        re += h[k] * std::cos(2.0 * M_PI * fNorm * (double)k);
        im -= h[k] * std::sin(2.0 * M_PI * fNorm * (double)k);
    }
    return std::sqrt(re * re + im * im);
}
}  // namespace

/* ── The paired decimator ──────────────────────────────────────────────────────────────────────────
 *  ★★★ FirDecimator::process / RealFir::process, output for output: the same reversed taps, the same
 *  [K-1 history][block] buffer, the same countdown phase deciding which inputs produce an output — and
 *  the same dot product, because dotCplx2/dotReal2 keep each output's accumulation order exactly as
 *  dotCplx/dotReal have it. The only difference is that outputs are taken in PAIRS, so each tap vector
 *  is loaded once for two of them.
 *  ★ WHY THIS AND NOTHING CLEVERER (2026-09-30). The instrument's cost is spread: at 3 MS/s on the Mac
 *  the /4 front-end stage is 23 %, the channel resampler 16 %, the measuring RDS demod 18 %, the eye and
 *  deviation filters 18 %, the pilot PLL 11 %. The front end is already planned by cost (see build()),
 *  the resampler's consecutive outputs use different polyphase branches (no taps to share), and the
 *  eye's serial IIRs are the shape NEON was measured NOT to help (0.96x, 2026-09-13). What was left is
 *  tap-load sharing in the integer stages — and it is the only change here that cannot move a figure,
 *  because it does not change a single bit of one. */
template <typename T>
MpxMeasure::PairDec<T>::PairDec(const std::vector<float>& taps, int decim)
    : D_(decim), phase_(decim), K_((int)taps.size()) {
    rtaps_.assign(taps.rbegin(), taps.rend());
    buf_.assign((size_t)(K_ - 1), T());
}

template <typename T>
void MpxMeasure::PairDec<T>::reset() {
    std::fill(buf_.begin(), buf_.end(), T());
    buf_.resize((size_t)(K_ - 1));
    phase_ = D_;
}

namespace {
inline void dotPair(const float* t, const cf32* x, int K, int D, cf32& a, cf32& b) {
    dotCplx2(t, reinterpret_cast<const float*>(x), K, D, a, b);
}
inline void dotPair(const float* t, const float* x, int K, int D, float& a, float& b) { dotReal2(t, x, K, D, a, b); }
inline cf32  dotOne(const float* t, const cf32* x, int K) { return dotCplx(t, reinterpret_cast<const float*>(x), K); }
inline float dotOne(const float* t, const float* x, int K) { return dotReal(t, x, K); }
}  // namespace

template <typename T>
int MpxMeasure::PairDec<T>::process(const T* in, int n, T* out) {
    buf_.resize((size_t)(K_ - 1 + n));
    std::copy(in, in + n, buf_.begin() + (K_ - 1));
    const T* x = buf_.data();
    const float* h = rtaps_.data();
    int outn = 0;
    // FirDecimator's countdown, unrolled: it emits at i = phase_-1, then every D_.
    int i = phase_ - 1;
    for (; i + D_ < n; i += 2 * D_) { dotPair(h, x + i, K_, D_, out[outn], out[outn + 1]); outn += 2; }
    if (i < n) { out[outn++] = dotOne(h, x + i, K_); i += D_; }
    phase_ = i - n + 1;                              // the next output's countdown, as FirDecimator leaves it
    std::copy(buf_.end() - (K_ - 1), buf_.end(), buf_.begin());
    buf_.resize((size_t)(K_ - 1));
    return outn;
}

template struct MpxMeasure::PairDec<cf32>;
template struct MpxMeasure::PairDec<float>;

double MpxMeasure::channelGain(double hz) const {
    double g = 1.0;
    for (const auto& st : stageTaps_) g *= dtftMag(st.first, hz / st.second);
    if (!rsProto_.empty()) g *= dtftMag(rsProto_, hz / (fs1_ * rsL_));
    return g;
}

double MpxMeasure::mpxGain(double hz) const {
    if (dec2Taps_.empty() || !(rc_ > 0.0)) return 0.0;
    return dtftMag(dec2Taps_, hz / rc_) * sincPi(hz / rc_);
}

int MpxMeasure::resample_(const cf32* in, int n, std::vector<cf32>& out) {
    const int K = rsK_;
    rsBuf_.resize((size_t)(K - 1 + n));
    std::copy(in, in + n, rsBuf_.begin() + (K - 1));
    out.clear();
    out.reserve((size_t)((long long)n * rsL_ / rsM_ + 2));
    const float* z = reinterpret_cast<const float*>(rsBuf_.data());
    for (;;) {
        const long long base = rsT_ / rsL_;
        if (base >= n) break;
        const int p = (int)(rsT_ % rsL_);
        // y = Σ_j h[p + jL] · x[base − j] — with the branch reversed, a forward dot over
        // rsBuf_[base .. base+K-1] (history offset K-1 cancels the reach-back of K-1).
        out.push_back(dotCplx(rsTaps_.data() + (size_t)p * K, z + 2 * base, K));
        rsT_ += rsM_;
    }
    rsT_ -= (long long)n * rsL_;
    std::copy(rsBuf_.end() - (K - 1), rsBuf_.end(), rsBuf_.begin());
    rsBuf_.resize((size_t)(K - 1));
    return (int)out.size();
}

void MpxMeasure::reset_() {
    for (auto& d : decs_) d->reset();
    rsBuf_.assign((size_t)std::max(0, rsK_ - 1), cf32(0.0f, 0.0f));
    rsT_ = 0;
    chanQ_.clear();
    fm_.reset(); dc_.reset();
    if (dec2_) dec2_->reset();
    /* ★★ A NARROW LOOP FOR THE INSTRUMENT — ~38 Hz once tracking, the listener's ~190 only to acquire (2026-10-10). The wide loop is pulled by
     *  programme audio (its phase detector sees the whole composite), which biased the RDS-to-pilot phase by +1.5° at a
     *  normal level and +2.7° at 1.4× — in proportion to programme POWER, ≈0 with pilot + RDS alone — and read the pilot
     *  low (6.716 / 6.690 kHz against a true 6.75). Measured on the test generator (shaped RDS, stereo programme):
     *    ~190 Hz +1.5° / +2.7°  · ~38 Hz +0.3° / +0.5°, pilot 6.739, drift exact · ~9.5 Hz +0.1° but drift 7 % high
     *  and slower to settle. A broadcast pilot is crystal-stable; the instrument only has to follow it. Found comparing
     *  with mrwish7's sdrpp-mpx-analyzer (exact on the same signals). The listener's stereo PLL is untouched.
     *  ★★ GEAR-SHIFTED, because narrow from the start re-acquired too slowly: a steady station read a phantom drift of
     *     1.1–1.3°/s while it settled, and every hole in the input cost a slow re-lock (test-mpx-measure). So: acquire
     *     on the wide loop exactly as before, narrow after kPllNarrowAfterSec of tracking, and go wide again on a hole
     *     or a lost track. See process_. */
    pll_.configure(19000.0, kMpxRate, kPllWide); pll_.reset();
    pllTrackedSec_ = 0.0; pllNarrow_ = false;
    rds_.reset();
    rds_.setNoiseCorrection(noiseCorr_.load(std::memory_order_relaxed));
    noise_.reset(); multipath_.reset();
    snrDb_ = 99.0f; snrOk_ = false; multipathCorr_ = 0.0f; multipathOk_ = false;
    winTainted_ = false;
    holdChunks_ = 0;
    // ── the moved eye / deviation / panel state, cleared exactly as rebuildAudio() cleared it ──
    for (int b = 0; b < kEyeBands; ++b) {
        std::fill(eyeAcc_[b].begin(), eyeAcc_[b].end(), 0.0f);
        std::fill(eyeOut_[b].begin(), eyeOut_[b].end(), (unsigned char)0);
        eyeBandPk_[b] = 0.0f; eyeBmxSm_[b] = 0.0f;
    }
    eyePeak_ = 0.0f; eyeSince_ = 0.0;
    eyeHp1_ = eyeHp2_ = eyeHp3_ = 0.0f;
    eyeBandFs_ = 0.0; mpxLpFs_ = 0.0;             // redesign (and clear) the biquads on first use
    mpxDevSm_ = 0.0f; mpxDevAvg_ = 0.0f; mpxDevHold_ = 0.0f; mpxDevSettle_ = 0.0;
    mpxDevDwellMax_ = 0.0f; mpxDevDwellT_ = 0.0;
    mpxNoiseSm_ = 0.0f; mpxDevOut_ = 0.0f; mpxDevAvgOut_ = 0.0f; mpxDevNoise_ = 0.0f;
    devWinCnt_ = 0; devWinGp_ = 0.0; devHist_.assign(kDevHistN, 0u);
    devWinP_ = 0.0; powHead_ = 0; powN_ = 0; powSumPdt_ = 0.0; powSumDt_ = 0.0;
    mpxPowerDb_ = 0.0f; mpxPowerSecs_ = 0.0f;
    extAvgInit_ = false; extPilotDev_ = 0.0f; extRdsDev_ = 0.0f; extRdsDevRaw_ = 0.0f;
    extCoh_ = 0.0f; extDrift_ = 0.0f; extRdsBad_ = 0; extSettle_ = 0.0;
    mpxAccN_ = 0; mpxSkip_ = 0;
    std::lock_guard<std::mutex> lk(outM_);
    const unsigned seq = out_.gridSeq;
    out_ = Out{};
    out_.gridSeq = seq + 1;
    for (auto& e : pubEye_) e.clear();
    pubMpx_.clear();
}

void MpxMeasure::process_(const cf32* iq, int n, unsigned gen, bool gap) {
    if (!built_) build();
    if (gen != gen_) { gen_ = gen; reset_(); }
    if (gap) {
        // ★★★ A HOLE — hold every figure while the splice clears (see holdChunks_). The chunk in
        //     progress holds the samples either side of it, so the hold starts with that chunk.
        winTainted_ = true;
        ++gapsSeen_;
        holdChunks_ = (int)std::ceil(kGapHoldSec * kChanRate / (double)kChunk);
        rds_.noteGap(kGapHoldSec);    // re-find the block grid; hold its level/phase figures
        pll_.noteGap();               // and do not let the pilot's re-lock dent the pilot figure
        mpxAccN_ = 0;                    // the MPX spectrum frame in progress straddles it: discard
    }
    // Front end: integer stages, then the rational channel filter.
    const cf32* src = iq;
    int m = n;
    for (auto& d : decs_) {
        std::vector<cf32>& dst = (src == fe0_.data()) ? fe1_ : fe0_;
        dst.resize((size_t)d->maxOut(m));
        m = d->process(src, m, dst.data());
        src = dst.data();
    }
    resample_(src, m, feOut_);
    chanQ_.insert(chanQ_.end(), feOut_.begin(), feOut_.end());
    // ★ FIXED 10 ms CHUNKS. Every smoother downstream (S/N, multipath, the deviation window, the
    //   1.5 s panel average) steps per call; the listener's steps per RADIO block, whose size
    //   depends on the radio and its rate. Chunking makes each time constant the same everywhere.
    size_t off = 0;
    while (chanQ_.size() - off >= (size_t)kChunk) {
        chunk_(chanQ_.data() + off, kChunk);
        off += kChunk;
    }
    if (off) chanQ_.erase(chanQ_.begin(), chanQ_.begin() + (long)off);
}

void MpxMeasure::chunk_(const cf32* ch, int n) {
    // ★★★ THE HOLD AFTER A HOLE (holdChunks_): below, every filter, the discriminator and the pilot
    //     loop still run — they must, to flush the splice — but nothing is accumulated, averaged or
    //     published until it is over. Each gated line says so.
    const bool hold = holdChunks_ > 0;
    if (hold) --holdChunks_;
    // ── multipath: the envelope, measured on the flat channel ──
    multipath_.process(ch, n, /*measure=*/!hold);
    // ── the multiplex ──
    mpx384_.resize((size_t)n);
    fm_.process(ch, mpx384_.data(), n);
    dc_.process(mpx384_.data(), n);
    if (!hold) mpxSpectrum_(mpx384_.data(), n);      // ★ a display average: simply skip the hold
    mpx_.resize((size_t)dec2_->maxOut(n));
    const int nm = dec2_->process(mpx384_.data(), n, mpx_.data());
    const float* x = mpx_.data();

    // ── MPX S/N: the pilot against the 15-19 kHz gap — the listener's own formula ──
    noise_.process(x, nm, /*measure=*/!hold);
    if (noise_.ready() && !hold) {
        const float noise = noise_.level();
        const float pilot = std::fabs(pll_.lockAmp());
        snrOk_ = (pilot > 0.027f);                         // ~2 kHz: below it there is no yardstick
        if (snrOk_) {
            const float r = (noise > 1e-9f) ? (pilot / noise) : 1e6f;
            snrDb_ += 0.05f * (20.0f * std::log10(std::max(r, 1e-6f)) - snrDb_);
            if (!std::isfinite(snrDb_)) snrDb_ = 99.0f;
        }
    }
    // ── multipath, noise contribution removed — see kMpSnr / kMpDepth for this path's table ──
    if (!hold) {
        const float s = snrDb_;
        float expect = kMpDepth[0];
        if (s <= kMpSnr[kMpN - 1]) expect = kMpDepth[kMpN - 1];
        else if (s < kMpSnr[0]) {
            for (int i = 0; i < kMpN - 1; ++i)
                if (s <= kMpSnr[i] && s > kMpSnr[i + 1]) {
                    const float t = (kMpSnr[i] - s) / (kMpSnr[i] - kMpSnr[i + 1]);
                    expect = kMpDepth[i] + t * (kMpDepth[i + 1] - kMpDepth[i]);
                    break;
                }
        }
        const float d = multipath_.depth();
        const float p = d * d - expect * expect;
        multipathCorr_ = (p > 0.0f) ? std::sqrt(p) : 0.0f;
        const float need = multipathOk_ ? 10.0f : 13.0f;     // the listener's hysteresis, unchanged
        multipathOk_ = snrOk_ && multipath_.plausible() && (snrDb_ > need);
    }

    // ── pilot PLL, its 57 kHz references and the bit clock ──
    lmr_.resize((size_t)nm); ref57_.resize((size_t)nm); ref57q_.resize((size_t)nm); bitClk_.resize((size_t)nm);
    pll_.processBlock(x, nm, lmr_.data(), ref57_.data(), ref57q_.data(), bitClk_.data());
    // ★ The gear shift (see reset_): narrow once it has tracked steadily, wide again the moment it has not.
    if (pll_.trackable() && !hold) {
        pllTrackedSec_ += nm / kMpxRate;
        if (!pllNarrow_ && pllTrackedSec_ >= kPllNarrowAfterSec) { pll_.setLoopFrac(kPllNarrow); pllNarrow_ = true; }
    } else {
        pllTrackedSec_ = 0.0;
        if (pllNarrow_) { pll_.setLoopFrac(kPllWide); pllNarrow_ = false; }
    }

    eyeAndDeviation_(x, nm, hold);

    // ── RDS: amplitude, phase, coherence, drift — MEASURED, never decoded for the listener ──
    const bool nc = noiseCorr_.load(std::memory_order_relaxed);
    if (nc != rds_.noiseCorrection()) rds_.setNoiseCorrection(nc);
    rds_.setPilotRef(pll_.lockAmp());
    if (pll_.trackable()) rds_.process(x, ref57_.data(), ref57q_.data(), bitClk_.data(), nm);

    /* ★★ THE AGGREGATE IS WHAT SAYS "THIS STATION HAS RDS". Every deviation figure is gated on
     *  groupTotal > 0 (no subcarrier, no number — see rdsDeviationKHz), and the aggregate is only
     *  refreshed by mergedAf(). The listener's path calls that when it assembles the panel; nothing
     *  else would here, so the gate would stay shut for ever and every figure read "—". */
    { int af[RdsDecoder::kMaxAf]; rds_.mergedAf(af, RdsDecoder::kMaxAf); }
    if (hold) return;                    // ★ the panel keeps the figures from before the hole
    panelAverage_((double)nm / kMpxRate);
    publish_(gridsReady_);
    gridsReady_ = false;
}

void MpxMeasure::eyeAndDeviation_(const float* mpxIn, int n, bool hold) {
    const double fsM_ = kMpxRate;
    // ★★ 96 COLUMNS, FIXED. The grid was sized to the channel rate for one release
    //    and came out at 30 columns on every radio — see the note on eyeW_ for why
    //    the argument was wrong and what the wire measured.
    eyeW_ = kEyeWMax;
    for (int b = 0; b < kEyeBands; ++b) {
        if ((int)eyeAcc_[b].size() != eyeW_ * kEyeH) {
            eyeAcc_[b].assign((size_t)eyeW_ * kEyeH, 0.0f);
            eyeOut_[b].assign((size_t)eyeW_ * kEyeH, 0);
        }
    }
    // ★★ The three resonators, designed once per rate. Q from what each component
    //    actually occupies: the pilot is a tone, L-R carries the stereo sidebands
    //    (wide), RDS is +/-2.4 kHz about 57.
    if (eyeBandFs_ != fsM_) {
        for (int sct = 0; sct < 2; ++sct) {
            eyeBand_[0][sct].design(fsM_, 19000.0, 14.0);   // pilot — a tone
            eyeBand_[1][sct].design(fsM_, 38000.0,  1.6);   // L-R sidebands — wide
            eyeBand_[2][sct].design(fsM_, 57000.0,  9.0);   // RDS  — +/-2.4 kHz
        }
        eyeBandFs_ = fsM_;
    }
    /* ★★★ THE DEVIATION FILTERS, AND THE TWO NOISE INTEGRALS — see mpxLp_ and
     *   devNoiseK_ in the header. Designed once per channel rate. K and G are the
     *   noise power each filter passes from FM's triangular (∝ f²) noise, so the
     *   guard band's measured power scales to the measurement band's by K/G with no
     *   fitted number in it. The guard sits at 80 kHz (between the US SCA slots at
     *   67 and 92) and needs the channel to be flat there, so it is only trusted on
     *   a channel wider than 180 kHz — narrower, the reading goes uncorrected, which
     *   errs on the side it always did. */
    if (mpxLpFs_ != fsM_) {
        static const double kBw6[3] = { 0.51763809, 0.70710678, 1.93185165 };
        for (int k = 0; k < 3; ++k) {
            mpxLp_[k].designLp(fsM_, 66000.0, kBw6[k]);
            mpxGuard_[k].design(fsM_, 80000.0, 12.0);
        }
        devNoiseK_ = 0.0f;
        if (fsM_ > 180000.0) {
            double K = 0.0, G = 0.0;
            const int nGrid = 2048;
            for (int g = 1; g < nGrid; ++g) {
                const double f = 0.5 * fsM_ * g / nGrid, w = 2.0 * M_PI * f / fsM_;
                double hl = 1.0, hg = 1.0;
                for (int k = 0; k < 3; ++k) { hl *= mpxLp_[k].mag2(w); hg *= mpxGuard_[k].mag2(w); }
                K += f * f * hl; G += f * f * hg;
            }
            devNoiseK_ = (G > 0.0) ? (float)(K / G) : 0.0f;
        }
        devWinN_ = (int)(fsM_ * 0.05);          // 50 ms, whatever the block size
        devWinCnt_ = 0; devWinGp_ = 0.0; devHist_.assign(kDevHistN, 0u);
        devWinP_ = 0.0; powHead_ = 0; powN_ = 0; powSumPdt_ = 0.0; powSumDt_ = 0.0;
        mpxPowerDb_ = 0.0f; mpxPowerSecs_ = 0.0f;
        mpxLpFs_ = fsM_;
    }
    // ★ Maintenance runs at the send rate, not per block — see eyeSince_. The
    //   ACCUMULATION below is per block; only the display work is gated.
    eyeSince_ += n;
    const bool eyeMaint = (fsM_ > 0.0) && (eyeSince_ >= fsM_ / 6.0);
    // ── High-pass above the audio (see the note on eyeHpA_) ─────────────────
    if (eyeHpA_ <= 0.0f && fsM_ > 0.0) {
        eyeHpA_ = (float)(1.0 - std::exp(-2.0 * M_PI * 15000.0 / fsM_));
        // |H| of three cascaded one-pole high-passes at each band centre, so the
        // per-band kHz figures report the composite, not the filtered copy.
        static const double kBandHz[3] = { 19000.0, 38000.0, 57000.0 };
        for (int b = 0; b < kEyeBands; ++b) {
            const double r = kBandHz[b] / 15000.0;
            eyeHpGain_[b] = (float)std::pow(r / std::sqrt(1.0 + r * r), 3.0);
        }
    }
    // ★ Same reasoning as the biquads: a one-pole that goes non-finite stays there.
    if (!std::isfinite(eyeHp1_) || !std::isfinite(eyeHp2_) || !std::isfinite(eyeHp3_))
        eyeHp1_ = eyeHp2_ = eyeHp3_ = 0.0f;
    /* ★ THE AVERAGE JOINS THE NaN GUARD. It feeds the guard-band verdict and sizes the
     *  noise removal for BOTH figures now, so a NaN here would take the peak with it —
     *  a new state must be added to this list or it is a hole in it. */
    if (!std::isfinite(mpxDevSm_) || !std::isfinite(mpxDevAvg_) || !std::isfinite(mpxDevHold_)
        || !std::isfinite(mpxNoiseSm_) || !std::isfinite(devWinGp_) || !std::isfinite(mpxDevDwellMax_))
        { mpxDevSm_ = 0.0f; mpxDevAvg_ = 0.0f; mpxDevHold_ = 0.0f; mpxNoiseSm_ = 0.0f; devWinGp_ = 0.0;
          mpxDevDwellMax_ = 0.0f; mpxDevDwellT_ = 0.0; }
    // ★ MPX power joins the list: a NaN in the window sum or the ring's running sums would stick.
    if (!std::isfinite(devWinP_) || !std::isfinite(powSumPdt_) || !std::isfinite(powSumDt_))
        { devWinP_ = 0.0; powHead_ = 0; powN_ = 0; powSumPdt_ = 0.0; powSumDt_ = 0.0;
          mpxPowerDb_ = 0.0f; mpxPowerSecs_ = 0.0f; }
    if ((int)powMean_.size() != kPowSlots) { powMean_.assign(kPowSlots, 0.0f); powDt_.assign(kPowSlots, 0.0f);
                                             powHead_ = 0; powN_ = 0; powSumPdt_ = 0.0; powSumDt_ = 0.0; }
    if ((int)devHist_.size() != kDevHistN) devHist_.assign(kDevHistN, 0u);
    if (!std::isfinite(eyePeak_)) eyePeak_ = 0.0f;
    /* ★★★ IN THE HOLD AFTER A HOLE: step every filter the loop below steps — the high-pass, the
     *  three band resonators, the deviation low-pass and the guard — so their state is the signal's
     *  when the hold ends, and do nothing else: no peak, no histogram, no guard power, no eye
     *  deposit, no decay, no clock. A spike that is not measured cannot become a figure. */
    if (hold) {
        for (int i = 0; i < n; ++i) {
            const float x = mpxIn[i];
            eyeHp1_ += eyeHpA_ * (x - eyeHp1_);       const float h1 = x - eyeHp1_;
            eyeHp2_ += eyeHpA_ * (h1 - eyeHp2_);      const float h2 = h1 - eyeHp2_;
            eyeHp3_ += eyeHpA_ * (h2 - eyeHp3_);      const float h  = h2 - eyeHp3_;
            (void)eyeBand_[0][1].step(eyeBand_[0][0].step(h));
            (void)eyeBand_[1][1].step(eyeBand_[1][0].step(h));
            (void)eyeBand_[2][1].step(eyeBand_[2][0].step(h));
            (void)mpxLp_[2].step(mpxLp_[1].step(mpxLp_[0].step(x)));
            (void)mpxGuard_[2].step(mpxGuard_[1].step(mpxGuard_[0].step(x)));
        }
        return;
    }
    // ★ AUTOSCALE, with a slow decay so it cannot pump on every bass note. A quiet
    //   passage genuinely shrinks the composite, and a fixed full scale would hide
    //   the structure instead of magnifying it (Stuart, 2026-09-13).
    // ★ The scale used for THIS block is last block's peak (after its decay) — one
    //   block of lag on a 0.995/block autoscale is nothing, and it is what lets the
    //   whole thing run as one pass. A sample above it lands on the top row.
    // ★★ DECAY IN TIME, NOT PER BLOCK. 0.995 per block was 0.1 s on an RTL's 168-sample
    //    blocks and many seconds on a bench feeding 8192 at a time — the same
    //    block-size trap the deviation window fell into. 0.5 s is the constant.
    // ★ 2 s, not 0.5: at 0.5 s the scale followed the music bar by bar and the whole
    //   trace visibly breathed with it (Stuart, 2026-09-14).
    const float pkDecay = (fsM_ > 0.0) ? (float)std::exp(-(double)n / fsM_ / 2.0) : 1.0f;
    eyePeak_ *= pkDecay;
    float blockPk = eyePeak_;
    float bpk[3], binv[3];
    for (int b = 0; b < kEyeBands; ++b) {
        if (!std::isfinite(eyeBandPk_[b])) eyeBandPk_[b] = 0.0f;
        eyeBandPk_[b] *= pkDecay; bpk[b] = eyeBandPk_[b];
        binv[b] = (eyeBandPk_[b] > 1e-6f) ? (1.0f / eyeBandPk_[b]) : 0.0f;
    }
    // ★★ NO fmod. A fractional part is floor-and-subtract; working in TURNS (0..1)
    //    rather than radians removes the division too. bitClk = (cycle*2pi+phase)/16,
    //    so bitClk * 16/(4pi) is the position in two-pilot-cycle units.
    const float kTurns = (float)(16.0 / (2.0 * 2.0 * M_PI));
    const float halfH  = 0.5f * (float)kEyeH;
    float* acc0 = eyeAcc_[0].data(); float* acc1 = eyeAcc_[1].data(); float* acc2 = eyeAcc_[2].data();
    double devGp = devWinGp_;
    double devP = devWinP_;
    uint32_t* hist = devHist_.data();
    const float kHistScale = (float)kDevHistN / 1.28f;
    /* ★★★ ONE PASS. This was five sweeps over the block — high-pass into a copy,
     *   three band-passes into three more copies, a peak scan, the deviation
     *   low-pass, then the fold reading them all back. Every one of those is a serial
     *   IIR, and the earlier NEON/SSE attempt proved four lanes cannot help a chain
     *   like that (measured 0.96x — see the memory of 2026-09-13). What DOES help is
     *   letting the three independent band chains, the deviation chain and the guard
     *   chain sit in one loop body, where the core's out-of-order window overlaps
     *   them for free instead of finishing one chain before starting the next.
     * ★★★ BILINEAR SPLAT. Each sample lands at an exact (x, y) inside the grid and is
     *   shared between the four cells around it by distance — that is the sub-cell
     *   information the old nearest-cell deposit threw away, and it is what makes 96
     *   columns draw as a smooth trace rather than a staircase. x wraps (the sweep is
     *   periodic in the pilot), y clamps. */
    for (int i = 0; i < n; ++i) {
        const float x = mpxIn[i];
        eyeHp1_ += eyeHpA_ * (x - eyeHp1_);       const float h1 = x - eyeHp1_;
        eyeHp2_ += eyeHpA_ * (h1 - eyeHp2_);      const float h2 = h1 - eyeHp2_;
        eyeHp3_ += eyeHpA_ * (h2 - eyeHp3_);      const float h  = h2 - eyeHp3_;
        const float y0 = eyeBand_[0][1].step(eyeBand_[0][0].step(h));
        const float y1 = eyeBand_[1][1].step(eyeBand_[1][0].step(h));
        const float y2 = eyeBand_[2][1].step(eyeBand_[2][0].step(h));
        /* ★★★ THE SCALE COMES FROM WHAT IS DRAWN — the sum of the three band outputs —
         *   not from the raw high-passed composite. That peak included every bit of
         *   broadband noise above 15 kHz, so on a noisy station (Flex FM at 26 dB MPX
         *   S/N) it read ±28 kHz while the pilot was 6.6: three rows of 48, and
         *   Stuart saw "basically a straight line". On a clean station the two agree
         *   and nothing changes. */
        const float ah = std::fabs(y0 + y1 + y2);
        if (ah > blockPk) blockPk = ah;
        const float a0 = std::fabs(y0), a1 = std::fabs(y1), a2 = std::fabs(y2);
        if (a0 > bpk[0]) bpk[0] = a0;  if (a1 > bpk[1]) bpk[1] = a1;  if (a2 > bpk[2]) bpk[2] = a2;
        // Each band against ITS OWN peak — see eyeBandPk_. 0.92 keeps the crest inside the box.
        /* ★★★ ONE AXIS, SCALED BY THE PILOT. Three scalings were tried today: the
         *   composite's own peak (a loud stereo passage squashed the pilot to a white
         *   line), and each band to its own peak (RDS, a filled eye by nature, strobed
         *   the box — "a disco light"). What Stuart pointed at as the aim was the
         *   2026-09-13 BBC Northampton shot, which by accident was pilot-scaled: the
         *   pilot fills three quarters of the box, stereo draws at its TRUE size against
         *   it and clips at the edges when loud (which is information), RDS stays the
         *   small braid it really is. Every station has a pilot at ~6.75 kHz, so the
         *   scale barely moves and nothing breathes. */
        /* ★★★ AND THEN THE MUSIC STATION: Flex FM at 35 kHz of stereo on a pilot-sized
         *   axis clipped everywhere and filled the box white. The reference shot was
         *   SPEECH. So: each band on its own scale, at a FIXED share of the box — pilot
         *   0.75 (the wave), stereo 0.55 (a braid, never a wall), RDS 0.35 (a small
         *   braid) — with the reference's brightness rule keeping the spread bands dim
         *   under the pilot line. Heights no longer compare amplitudes; the caption's
         *   three kHz figures do. */
        // ★ Three boxes now (web): each band fills its OWN box on its own scale.
        const float u0 = y0 * binv[0] * 0.85f, u1 = y1 * binv[1] * 0.85f, u2 = y2 * binv[2] * 0.85f;
        // Deviation: the whole composite, audio included, through the 66 kHz cascade.
        const float d = mpxLp_[2].step(mpxLp_[1].step(mpxLp_[0].step(x)));
        // ★ UNSIGNED compare: a NaN casts to INT_MIN, and "hb >= N" would let it through
        //   to write kilobytes below the histogram. The step() guards return 0 for a
        //   non-finite output, but the index must not trust that alone.
        unsigned hb = (unsigned)(int)(std::fabs(d) * kHistScale);
        if (hb >= (unsigned)kDevHistN) hb = kDevHistN - 1;
        ++hist[hb];
        const float g = mpxGuard_[2].step(mpxGuard_[1].step(mpxGuard_[0].step(x)));
        devGp += (double)g * g;
        devP  += (double)d * d;              // MPX power — see devWinP_
        // The fold — one x for all three bands: they share the trigger.
        const float t = bitClk_[i] * kTurns;
        /* ★★★ A NaN HERE SEGFAULTED THE LENOVO's RSP CHILD (dev 5.6.0, 2026-09-14). The
         *   old nearest-cell fold clamped its index, which happened to swallow a NaN
         *   pilot clock (it casts to INT_MIN, and "< 0 → 0" caught it). The splat's
         *   wrap arithmetic did not, and INT_MIN + 96 is still a write a long way
         *   below the grid. The PLL can hand out a NaN clock briefly after a mode
         *   change (here AM 648 kHz → WFM 96.6). Skip the sample; never index on it. */
        if (!std::isfinite(t)) continue;
        /* ★★★ ONE CELL PER HIT, NOT A BILINEAR SPLAT. The splat was tried today and it is
         *   what made the traces fat: every hit spread over four cells, then bilinear
         *   upscaling softened them again, and the pilot came out as a thick blurred
         *   ribbon. The XCover's app eye — one cell per hit at 96x48 — is the look
         *   Stuart pointed at: "this older look is the aim". Crisp beats smooth here. */
        int cx = (int)((t - std::floor(t)) * (float)eyeW_);
        if (cx < 0) cx = 0; else if (cx >= eyeW_) cx = eyeW_ - 1;
        auto deposit = [&](float* acc, float u) {
            // Row 0 is the TOP, so +full scale is at the top like a scope.
            float fy = (1.0f - u) * halfH;
            if (!(fy >= 0.0f)) fy = 0.0f;                       // NaN lands on row 0
            int cy = (int)fy; if (cy >= kEyeH) cy = kEyeH - 1;
            acc[(size_t)cy * eyeW_ + cx] += 1.0f;
        };
        deposit(acc0, u0); deposit(acc1, u1); deposit(acc2, u2);
    }
    eyePeak_ = blockPk;
    for (int b = 0; b < kEyeBands; ++b) eyeBandPk_[b] = bpk[b];
    devWinGp_ = devGp; devWinP_ = devP; devWinCnt_ += n;
    /* ★★ THE 50 ms WINDOW CLOSES — see devWinN_. The bar is the AVERAGE of window
     *  maxima on the panel's 1.5 s clock (Stuart: "average it the same as the other
     *  measurements"); the tick is a slow peak-hold of the same corrected value. */
    if (devWinN_ > 0 && devWinCnt_ >= devWinN_) {
        const double dtW = (double)devWinCnt_ / fsM_;
        // The percentile — walk down from the top until `skip` samples have been
        // passed. The bin's UPPER edge, so a clean tone is not read low.
        float pk = 0.0f;
        {
            /* ★★★ A FIXED COUNT, NOT A FRACTION OF THE WINDOW. This was
             *  `devWinCnt_ * 0.0003` — 0.3 per mille, which is ~5 samples at 160 kHz
             *  but grows with the channel rate, so at 250 kHz it discarded the top
             *  ~4 samples and at higher rates more again. A SUSTAINED TONE does not
             *  care: it puts hundreds of samples in the top bin every window. A
             *  SPARSE TRANSIENT — one orchestral attack, a consonant — is only a few
             *  samples wide and was thrown away WHOLE, and the quieter the programme
             *  the larger the share of its peaks that are sparse. That is part of
             *  Onfliner's under-read (2026-09-25), and it is the half that hides
             *  inside the rate.
             *  ★ Two samples still rejects a lone impulse (the reason the skip
             *    exists), and the 66 kHz band-limit plus the guard-band correction
             *    are the other two defences. The bench figures that justified 0.3 per
             *    mille were taken on a spiky multi-tone, not on real programme.
             *  ✗ Do not restore a proportional skip: it makes the reading depend on
             *    the sample rate, which is exactly what §3 of the brief proved this
             *    measurement is otherwise free of.
             *
             *  ★★★ A FIXED *TIME*, NOT TWO SAMPLES: 125 µs — MPXtool's default PEAK
             *  RESPONSE TIME (Onfliner, 2026-10-01). Two samples was 10 µs: every
             *  processor overshoot a few samples wide counted as the station's peak, and
             *  side by side on seven Moscow stations we read 1-8 kHz above MPXtool
             *  (Business FM 86 vs 80, Kultura 86 vs 81, Sputnik 86 vs 84, Retro 80 vs 77)
             *  while RDS agreed to 0.1 kHz — the error was the peak detector alone, and
             *  it was largest on the less clean signals. The level published is now the
             *  one the composite stays at or above for 125 µs of the window (24 samples
             *  at 192 kHz) — still a time, so still free of the radio's rate.
             *  ★ An interpretation of "peak response", not a copy of MPXtool's code:
             *    CUMULATIVE time above the level, because a composite near its peak
             *    crosses back below it every pilot cycle (26 µs) and a CONTIGUOUS 125 µs
             *    would read a 75 kHz programme peak as almost nothing. Re-check on
             *    Onfliner's stations before tuning the figure. */
            constexpr double kPeakResponseSec = 125e-6;
            const uint32_t skip = (uint32_t)std::lround(kPeakResponseSec * fsM_);
            uint32_t seen = 0; int b = kDevHistN - 1;
            for (; b > 0; --b) { seen += hist[b]; if (seen > skip) break; }
            pk = (float)(b + 1) / kHistScale;
            devHist_.assign(kDevHistN, 0u);
        }
        float gp = (float)(devWinGp_ / (double)devWinCnt_);
        const double winP = devWinP_ / (double)devWinCnt_;     // this window's mean d² — MPX power
        devWinCnt_ = 0; devWinGp_ = 0.0; devWinP_ = 0.0;
        /* ★★ A WINDOW THAT STRADDLED A DROPPED BLOCK IS NOT A MEASUREMENT. The instrument drops
         *  rather than make audio wait (see MpxMeasure::feed), and a hole in the IQ is a phase
         *  step — a discriminator spike the peak meter would publish as the station's deviation.
         *  The whole window is thrown away: nothing smoothed moves, nothing is held. */
        if (winTainted_) { winTainted_ = false; pk = 0.0f; gp = 0.0f; }
        else {
        /* ★★ IGNORE THE FIRST 0.4 s AFTER A RETUNE — see the note at the reconfigure.
         *  ★★★ WAS 1.5 s, AND THAT WAS SIZED FOR THE OLD SLOW METER. With an
         *  instant-attack peak the reading is meaningful as soon as the DC blocker and
         *  the PLL have settled, so a 1.5 s blank is now just 1.5 s of the meter
         *  saying nothing after every tune — on top of the attack, it was ~5-6 s
         *  before the number meant anything (Onfliner: "the slow display of the
         *  deviation scale"). */
        const bool settling = mpxDevSettle_ < 0.4;   // ★ MPX power skips these windows too
        if (mpxDevSettle_ < 0.4) { mpxDevSettle_ += dtW; pk = 0.0f; gp = 0.0f;
                                   mpxDevSm_ = 0.0f; mpxDevAvg_ = 0.0f;
                                   mpxDevHold_ = 0.0f; mpxNoiseSm_ = 0.0f;
                                   mpxDevDwellMax_ = 0.0f; mpxDevDwellT_ = 0.0; }
        const float aSm = 1.0f - (float)std::exp(-dtW / 1.5);
        /* ★★★ TWO STATISTICS FROM ONE WINDOW ARRAY — THE PEAK AND THE AVERAGE.
         *
         *  ★★★ WHAT WAS WRONG. This line used to be the ONLY one, and it published the
         *  1.5 s SYMMETRIC AVERAGE of the window peaks while calling it peak deviation.
         *  On processed programme (crest factor 2-4 dB) nearly every 50 ms window peaks
         *  at the same value, so average ≈ peak and we read within 1 kHz of MPX Tool —
         *  which is exactly what tgcfabian measured. On jazz, classical and speech
         *  (crest 10-20 dB) most windows contain no peak at all, so a 75 kHz peak read
         *  ≈ 38 — which is exactly what Onfliner measured. ★ TWO TESTERS CONTRADICTED
         *  EACH OTHER AND BOTH WERE RIGHT; only the averaging predicts both reports,
         *  and Onfliner himself confirmed it by adding that "noisy stations playing
         *  loud music it was spot on" (2026-09-25).
         *
         *  ★★★ AND THE HEADER ABOVE THIS FIELD SAID IT WAS ALREADY A PEAK METER —
         *  vibedsp.h claimed "FAST ATTACK, SLOW DECAY … rises INSTANTLY to a new peak"
         *  for weeks after this became an average. One rule, two readers, and the
         *  stale reader was the DESIGN NOTE, so auditing the source said the meter was
         *  fine and it took an outside tester with a reference instrument to find it.
         *  That note is now true again.
         *
         *  ★★ NOTHING IS LOST: the average is still computed and still sent, because
         *  it is the steady number Stuart asked for ("average it the same as the other
         *  measurements", 2026-09-13) and the one tgcfabian validated. It is
         *  relabelled, not removed. PIRA's analysers do the same thing from a 50 ms
         *  window identical to ours — they publish MAX, AVE and MIN; we published the
         *  AVE alone and labelled it "deviation". */
        mpxDevAvg_ += aSm * (pk - mpxDevAvg_);
        /* ★★★ THE PEAK: INSTANT ATTACK, ~0.9 s DECAY. This is what a modulation
         *  monitor is. A peak that arrives is published immediately; between peaks it
         *  falls slowly enough that the bar reads as a level rather than a flicker.
         *  ★ THE DIGITS DO NOT FOLLOW THIS — see mpxDevHold_ below. Stuart, 2026-09-25:
         *    "if it is bouncing up and down like a yoyo then the number looks like a
         *    stopwatch, how do you read that?" He is right, and the answer is that the
         *    NUMBER must not be the fast thing: the bar moves, the digits hold. */
        const float kPk = (float)std::exp(-dtW / 0.9);
        mpxDevSm_ = (pk > mpxDevSm_) ? pk : mpxDevSm_ * kPk;
        mpxNoiseSm_ += aSm * (gp - mpxNoiseSm_);
        // σ² in the measurement band, then the quadrature removal — see devNoiseK_.
        const float sig2 = mpxNoiseSm_ * devNoiseK_;
        const float kC = 4.5f;   // fitted on the bench (bench_eye), see devNoiseK_
        /* ★★★ A NEIGHBOUR IN THE GUARD BAND IS NOT NOISE. The guard sits at 80 kHz,
         *  which is where an adjacent station 100 kHz away puts its sidebands. Classic
         *  FM on 100.4 beside a strong 100.3 (Stuart, 2026-09-14): the "noise" read
         *  7 kHz, the removal took 31 kHz off a 32 kHz raw peak, and the meter showed
         *  8 kHz falling to nothing while every other readout said the transmitter
         *  was fine. Real broadband noise never removes most of the reading on a
         *  station whose pilot and RDS are locked — so if the removal would take more
         *  than 60 % of the raw figure, the guard band is occupied: subtract nothing,
         *  and report the noise as NEGATIVE so the panel can say why. */
        const float removal = kC * std::sqrt(std::max(0.0f, sig2));
        /* ★★★ THE CORRECTION IS JUDGED AND SIZED ON THE AVERAGE, THEN APPLIED TO BOTH.
         *
         *  ★★★ WHY NOT JUST RUN THE SAME FORMULA ON THE PEAK. `kC = 4.5` is a bench fit
         *  (bench_eye) made against the AVERAGED statistic, and it is a quadrature
         *  removal — the right shape for a quantity that behaves like an rms. An
         *  instant-attack peak does not: the noise contribution to a single window
         *  maximum is not σ-like, so 4.5σ subtracted from a peak is a number with no
         *  derivation behind it. Getting that wrong on a weak signal is precisely the
         *  "106 kHz peak, OVERMODULATED" failure the band-limit was added to prevent
         *  (see the note on mpxDevSettle_ in vibedsp.h) — the fast attack is what makes
         *  that failure reachable again.
         *  ★★ So: correct the average with the fit that was made for it, take the
         *  ABSOLUTE kHz that removed, and subtract the same absolute amount from the
         *  peak. The noise floor under a peak and under an average of peaks is the
         *  same noise floor; what is not justified is re-deriving its size from a
         *  statistic the constant was never fitted against.
         *  ▶ Re-fitting kC for a true peak on the bench is the proper answer and is
         *    written up in briefs/BRIEF-deviation-meter.md. Until that is measured,
         *    this is the honest approximation, and it errs towards removing too
         *    little — which shows a reading as noisy rather than inventing silence.
         *  ★ The guard-band test also stays on the average, because it is the stable
         *    quantity: judging "is a neighbour sitting in my guard band?" off a value
         *    that moves with every syllable would make the verdict flicker. */
        const bool guardOccupied = mpxDevAvg_ > 0.02f && removal > 0.6f * mpxDevAvg_;
        /* ★★★ MPX POWER (BS.412) — see devWinP_. Power adds, so the noise power in the same band comes
         *  straight off — unless the guard band holds a neighbour, when its "noise" is not noise (the
         *  same verdict as the deviation's, for the same reason). One slot per measured window. */
        if (!settling && winP >= 0.0 && std::isfinite(winP) && !powMean_.empty()) {
            const double pClean = guardOccupied ? winP : std::max(0.0, winP - (double)sig2);
            if (powN_ == kPowSlots) {                      // full: the oldest slot leaves
                powSumPdt_ -= (double)powMean_[powHead_] * powDt_[powHead_];
                powSumDt_  -= powDt_[powHead_];
            } else ++powN_;
            powMean_[powHead_] = (float)pClean; powDt_[powHead_] = (float)dtW;
            powSumPdt_ += pClean * dtW; powSumDt_ += dtW;
            powHead_ = (powHead_ + 1) % kPowSlots;
            if (powSumDt_ < 0.0) powSumDt_ = 0.0;
            if (powSumDt_ >= 5.0 && powSumPdt_ > 0.0) {
                // 0 dBr = the power of a sine at ±19 kHz peak: (19/75)² / 2 in our ±1 = ±75 kHz units.
                constexpr double kRef = (19.0 / 75.0) * (19.0 / 75.0) * 0.5;
                mpxPowerDb_   = (float)(10.0 * std::log10((powSumPdt_ / powSumDt_) / kRef));
                mpxPowerSecs_ = (float)std::min(60.0, powSumDt_);
            } else { mpxPowerDb_ = 0.0f; mpxPowerSecs_ = 0.0f; }
        }
        if (guardOccupied) {
            mpxDevNoise_ = -std::sqrt(std::max(0.0f, sig2));
            mpxDevOut_    = mpxDevSm_;
            mpxDevAvgOut_ = mpxDevAvg_;
        } else {
            mpxDevNoise_ = std::sqrt(std::max(0.0f, sig2));
            const float s2 = mpxDevAvg_ * mpxDevAvg_ - kC * kC * sig2;
            mpxDevAvgOut_ = (s2 > 0.0f) ? std::sqrt(s2) : 0.0f;
            const float removedAbs = mpxDevAvg_ - mpxDevAvgOut_;   // ≥ 0 by construction
            mpxDevOut_ = std::max(0.0f, mpxDevSm_ - removedAbs);
        }
        /* ★★★ THE HOLD IS NOW THE FIGURE THE DIGITS SHOW, not a thin tick nobody reads.
         *  Instant attack, 6 s decay: it sits still long enough to be read and then
         *  steps down. This is the half of Stuart's objection that the fast bar cannot
         *  answer on its own, and it is what MPX Tool's peak flasher gives you.
         *  ★ It holds the CORRECTED peak, so it can never sit at a figure the bar has
         *    not actually reached. */
        /* ★★★ A DWELL, NOT A DECAY — THIS IS WHAT MAKES THE NUMBER READABLE.
         *
         *  ★★★ THE MISTAKE THIS REPLACES, BECAUSE IT IS AN EASY ONE TO MAKE AGAIN.
         *  This was `mpxDevHold_ * exp(-dtW/6)` — instant attack, 6 s exponential
         *  decay — and that is a perfectly good ballistic FOR A BAR and quite wrong
         *  for DIGITS: an exponential decay is a CONTINUOUSLY FALLING number, and it
         *  is repainted at the 6 Hz frame rate, so the readout counts itself down
         *  74, 71, 68, 65 and can never be read. Stuart, 2026-09-25: "deviation now
         *  changes far too quick I cannot see it to read it." Calling it a "hold"
         *  does not make it hold anything.
         *  ★★ AND IT HID WHERE IT WOULD DO LEAST HARM. On a strong processed station
         *  peaks arrive in almost every window, so the hold is re-armed at nearly the
         *  same value and LOOKS stable — "on the stronger Heart it seems better". The
         *  decay only becomes visible when peaks are SPARSE, which is exactly the
         *  classical/speech material this whole measurement was fixed for. A fault
         *  that hides on the easy case and appears on the important one.
         *
         *  ★★ WHAT A REAL PEAK-HOLD READOUT DOES: it sits FLAT for a dwell, then
         *  steps to the new maximum. So the figure is constant for kDwell seconds at
         *  a time and changes in readable steps — with one exception, below.
         *  ★ THE EXCEPTION IS THE WHOLE POINT OF THE INSTRUMENT: a peak HIGHER than
         *    what is displayed appears IMMEDIATELY. Waiting up to 3 s to report an
         *    overmodulation would be the one failure a modulation monitor must not
         *    have. So: rise at once, fall only on the step.
         *  ★ 3 s is long enough to read a three-digit number without hurrying and
         *    short enough that the figure still tracks the programme. */
        constexpr double kDwell = 3.0;
        if (mpxDevOut_ > mpxDevHold_) mpxDevHold_ = mpxDevOut_;   // an excursion never waits
        if (mpxDevOut_ > mpxDevDwellMax_) mpxDevDwellMax_ = mpxDevOut_;
        mpxDevDwellT_ += dtW;
        if (mpxDevDwellT_ >= kDwell) {
            /* ★ The step: the true maximum of the window that just closed. It can go
             *  DOWN (that is the point) and it can go up if the rise above happened
             *  to be missed by rounding — either way it is a measured figure, never
             *  a decayed one. */
            mpxDevHold_ = mpxDevDwellMax_;
            mpxDevDwellMax_ = 0.0f;
            mpxDevDwellT_ = 0.0;
        }
        }   // not tainted
    }
    /* ★★★ CONVERT WHAT WAS ACCUMULATED, *THEN* DECAY — order matters, and getting it
     *   wrong is invisible in code review (it shipped the other way round and the
     *   plot flickered; Stuart, 2026-09-13). */
    if (eyeMaint) {
        eyeSince_ = 0.0;
        gridsReady_ = true;
        /* ★★★ EACH BAND ON ITS OWN BRIGHTNESS. A shared intensity scale let the pilot
         *   — a pure tone that lands in the same cells on every one of ~14000 sweeps —
         *   set the scale for everything, and the stereo, spread across its envelope,
         *   came out at a mean of 3.5/63 with fewer than one cell in 1440 changing
         *   per frame (measured from the Pi's wire, 2026-09-14). That is the plot
         *   Stuart described as not responding. A 4x lift cap had been tried; it was
         *   not enough by a factor of five.
         * ★★ STRENGTH IS STILL HONEST, because the VERTICAL scale stays shared: a weak
         *   component draws small, and a DEAD one draws as a flat bright line at zero
         *   (its noise is tiny against the shared axis) — which is exactly what
         *   "nothing there" should look like. Scatter still reads as fuzz. */
        /* ★★ THE GEOMETRIC MEAN OF ITS OWN MAXIMUM AND THE SHARED ONE. Fully own-scaled
         *   (tried first today) every band saturated and the three added to white — the
         *   RDS, a spread band, drew as fat violet blobs over the pilot (Stuart: "the
         *   individual components are blending into each other too much"). Fully shared
         *   (5.5.5) hid weak stereo. sqrt(bmx·mx) lifts a band 25x below the leader by
         *   5x and one 100x below by 10x: a concentrated tone still reads brighter than
         *   a spread band, which is the "strong line vs speckle" reading the colours
         *   exist for, and nothing vanishes. */
        float mx = 0.0f;
        for (int b = 0; b < kEyeBands; ++b)
            for (float v : eyeAcc_[b]) if (v > mx) mx = v;
        for (int b = 0; b < kEyeBands; ++b) {
            float bmxNow = 0.0f;
            for (float v : eyeAcc_[b]) if (v > bmxNow) bmxNow = v;
            /* ★ The brightness reference is SMOOTHED (~1 s at 6 Hz), not the instant
             *   maximum: normalising each frame to its own peak made the stereo band
             *   pulse with its own loudness — "almost breathes, fades in and out"
             *   (Stuart, 2026-09-14). Rises are taken faster than falls so a band that
             *   appears is not clipped while the reference catches up. */
            float& bmx = eyeBmxSm_[b];
            bmx += ((bmxNow > bmx) ? 0.35f : 0.15f) * (bmxNow - bmx);
            /* ★ STRENGTH IN THE BRIGHTNESS: the band's true peak against the strongest
             *   band's, square-rooted so a component at a tenth still draws at a third,
             *   floored at 0.3 so nothing readable disappears. Vertical size no longer
             *   carries strength; this does. */
            /* Against its own NOMINAL, not the loudest band: judged against 29 kHz of
             *  stereo a 4.4 kHz pilot looked weak when it was healthy. Nominals in
             *  composite kHz: pilot 6.75, stereo 25 (typical peak L−R), RDS 2.5. RDS is
             *  capped at 0.6 so it stays a texture behind the two waves. */
            // The 5.5.5 brightness rule the reference shot was drawn with: each band on
            // its own maximum, lifted at most 4x above the shared one.
            float mxAll = 1e-6f;
            for (int k = 0; k < kEyeBands; ++k) mxAll = std::max(mxAll, eyeBmxSm_[k]);
            const float strength = std::min(1.0f, 4.0f * bmx / mxAll);
            // ★ bmx^0.75 · mx^0.25: the geometric mean left Heart's stereo at 36/63 —
            //   persistent on the wire, but on a retina Safari faint enough that
            //   Stuart saw it only when a chorus pushed it to full ("flashes for a
            //   split second, no persistence"). Three-quarters own scale keeps the
            //   tone-vs-spread ordering while a band 3x below the leader draws at ~48.
            const float es = (bmx > 1e-6f) ? (255.0f * strength / bmx) : 0.0f;
            for (size_t j = 0; j < eyeAcc_[b].size(); ++j) {
                const int v = (int)(eyeAcc_[b][j] * es);
                eyeOut_[b][j] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
        /* ★★★ PERSISTENCE ON THE SAME CLOCK AS THE REST OF THE PANEL: 0.889 at 6 Hz
         *   is ~1.5 s, matching pilotDev, rdsDev, coherence and drift
         *   ([[panel_readouts_need_one_clock]]). Fade AFTER publishing. */
        for (int b = 0; b < kEyeBands; ++b)
            for (float& v : eyeAcc_[b]) v *= 0.889f;
    }
}

void MpxMeasure::mpxSpectrum_(const float* x384, int n) {
    const double fsS_ = rc_;
    /* ★ AT MOST ~12 SPECTRA A SECOND. The panel is sent at 6 Hz and each message carries the LAST
     *  spectrum — every FFT between two sends was computed and overwritten. So the accumulator fills
     *  with 1024 contiguous samples, transforms, then rests for 1/12 s. */
    if (mpxSkip_ > 0) {
        const int s = std::min(n, mpxSkip_);
        mpxSkip_ -= s; x384 += s; n -= s;
        if (n <= 0) return;
    }
    if (!mpxFft_) {
        mpxFft_ = std::make_unique<RealFFT>(kMpxFft);
        mpxWin_.resize(kMpxFft);
        nuttallWindow(mpxWin_.data(), kMpxFft);
        mpxIn_.resize(kMpxFft);
        mpxDb_.resize(kMpxFft / 2 + 1);
        mpxOut_.assign(kMpxBins, -120.0f);
        mpxAcc_.assign(kMpxFft, 0.0f);
        mpxAccN_ = 0;
    }
    // ★ ACCUMULATE ACROSS BLOCKS. The first version required a single block of at
    // least kMpxFft samples — and the DSP block is smaller than that, so the FFT
    // never ran and the panel drew an empty box with labels on it (Stuart,
    // 2026-07-26). ★ Never gate work on a block size you do not control.
    for (int i = 0; i < n && mpxAccN_ < kMpxFft; ++i)
        mpxAcc_[mpxAccN_++] = x384[i];
    if (mpxAccN_ >= kMpxFft) {
    mpxAccN_ = 0;
    for (int i = 0; i < kMpxFft; ++i) mpxIn_[i] = mpxAcc_[i] * mpxWin_[i];
    mpxFft_->powerDb(mpxIn_.data(), mpxDb_.data(), 2.0f / kMpxFft);
    // ★ The discriminator's sinc, undone per bin — the same correction the meters get.
    for (size_t k = 0; k < mpxDb_.size() && k < sincDbCorr_.size(); ++k) mpxDb_[k] += sincDbCorr_[k];
    mpxSkip_ = std::max(0, (int)(fsS_ / 12.0) - kMpxFft);
    // Map DC..kMpxSpanHz onto kMpxBins, taking the PEAK of each group: the pilot
    // and RDS are narrow, and averaging would flatten the very features this
    // display exists to show.
    const double binHz = fsS_ / kMpxFft;
    const int hi = (int)std::min<double>(kMpxFft / 2.0, kMpxSpanHz / binHz);
    for (int i = 0; i < kMpxBins; ++i) {
        const int a = (int)((double)i / kMpxBins * hi);
        const int b = std::max(a + 1, (int)((double)(i + 1) / kMpxBins * hi));
        float m = -200.0f;
        for (int k = a; k < b && k < (int)mpxDb_.size(); ++k)
            if (mpxDb_[k] > m) m = mpxDb_[k];
        mpxOut_[i] = m;
    }
    }
}

void MpxMeasure::panelAverage_(double dt) {
    /* ★★★ ONE OBSERVATION WINDOW FOR THE WHOLE PANEL. These fields are assembled in
     *     the same instant, but each arrived carrying a COMPLETELY DIFFERENT
     *     averaging interval: the RDS deviation smoothed over seconds, the pilot
     *     deviation essentially instantaneous, the block error rate over the
     *     decoder's own window, the constellation over its last N symbols. A frame
     *     was therefore a mosaic of a millisecond, a second and several seconds
     *     presented as one moment — so no two numbers on it were commensurable, and
     *     any contradiction between them was expected rather than diagnostic.
     *  ★ Stuart, after an evening of comparing four radios on six stations:
     *    "those numbers change so rapidly sub 1 second", "I think the issue is the
     *    lack of smoothing those numbers show the data the millisecond it arrives but
     *    ends up out of sync". He is right, and it invalidated most of a night's
     *    conclusions — mine especially: I read single frames of jittering quantities
     *    as measurements and built three theories on them, two of which were wrong.
     *  ★★ A PANEL IS AN INSTRUMENT, AND AN INSTRUMENT NEEDS A STATED INTEGRATION TIME.
     *    1.5 s, applied to every scalar here, so a frame describes one interval. The
     *    coefficient is derived from the real block duration rather than assumed, so
     *    the window is 1.5 seconds at any sample rate or block size.
     *  ★★★ Pilot deviation and RDS deviation are TRANSMITTER CONSTANTS — they should
     *      not move at all, and watching them jitter was the clue that none of this
     *      was measuring what it claimed to. */
    const float  a   = (dt > 0.0) ? (float)(1.0 - std::exp(-dt / 1.5)) : 0.1f;
        const float  pd  = pll_.pilotDeviationKHz();
        const float  rd  = rds_.rdsDeviationKHz();
        /* ★★★ THE CONTROL ARM RIDES THE SAME SMOOTHER. Taking the raw figure live beside
         *  a 1.5 s average would put the smoothing INTO the difference the reading is
         *  meant to isolate, and the deficit being chased is only 16 % — the same order
         *  as the wobble a 1.5 s filter removes. Compare like with like or the experiment
         *  measures the filter. ★ Both share one sentinel: `agg_.groupTotal <= 0` is the
         *  ONLY -1 path in either, so `rr >= 0` exactly when `rd >= 0` and the branches
         *  below can carry both. */
        const float  rr  = rds_.rdsDeviationRawKHz();
        const float  coh = rds_.pilotPhaseCoherence();
        const float  drf = rds_.pilotPhaseDriftDegPerSec();
        /* ★★ START THE WINDOW FROM A MEASUREMENT, NOT FROM A LOOP THAT HAS NOT LOCKED. The first
         *  chunk after a restart has lockAmp ~ 0, and seeding a 1.5 s average with it took ~7 s to
         *  climb to within 1 % of a pilot that had been measurable after half a second (on the
         *  listener's path too — it read 6.53 for a true 6.75 six seconds after tuning). So the
         *  window opens once the PLL is tracking and its own 110 ms average has had 0.5 s. Until
         *  then the panel shows nothing measured, which is the truth. */
        if (!extAvgInit_ && (!pll_.trackable() || extSettle_ < 0.5)) { extSettle_ += dt; return; }
        if (!extAvgInit_) {
            extAvgInit_ = true;
            extPilotDev_ = pd; extRdsDev_ = rd; extCoh_ = coh; extDrift_ = drf;
            extRdsDevRaw_ = rr;
            extRdsBad_ = 0;
        } else {
            extPilotDev_ += a * (pd  - extPilotDev_);
            extCoh_      += a * (coh - extCoh_);
            extDrift_    += a * (drf - extDrift_);
            /* ★★★ THE "CANNOT MEASURE" SENTINEL MUST EARN ITS PLACE. The deviation
             *     carries a negative sentinel, and averaging a sentinel with a value
             *     produces a number that is neither — so it cannot simply be mixed in.
             *     But adopting it the instant it appears is just as wrong: a single
             *     bad tick then blanks a figure that is otherwise perfectly healthy,
             *     which is the whole fault being fixed here.
             *  ★ Stuart on tgcfabian's FelineFM — 0 % block errors, carrier locked, full
             *    radiotext, and the deviation showing "no subcarrier": "tgcfabian's
             *    FelineFM has flashed 3.4KHz typical". The subcarrier was there all
             *    along at a healthy 3.4 kHz; the panel was catching the dips.
             *  ★★ So a real reading is adopted at once (it proves measurability), and
             *    "cannot measure" only after it has held for about a second. */
            if (rd >= 0.0f) {
                extRdsBad_ = 0;
                if (extRdsDev_ < 0.0f) { extRdsDev_ = rd; extRdsDevRaw_ = rr; }  // first good
                else { extRdsDev_    += a * (rd - extRdsDev_);
                       extRdsDevRaw_ += a * (rr - extRdsDevRaw_); }
            } else if (++extRdsBad_ * dt > 1.0) {
                extRdsDev_ = rd; extRdsDevRaw_ = rr;              // genuinely gone
            }
        }
}

void MpxMeasure::publish_(bool grids) {
    std::lock_guard<std::mutex> lk(outM_);
    out_.valid = extAvgInit_;
    out_.pilotKHz = extPilotDev_;
    out_.rdsAvgKHz = extRdsDev_;
    /* ★ The measured peak travels LIVE rather than through the 1.5 s smoother: it carries its own
     *  3 s dwell, and smoothing a peak is how the averaged figure came to be mislabelled. */
    out_.rdsPeakKHz = rds_.rdsDeviationPeakKHz();
    out_.rdsRawKHz = extRdsDevRaw_;
    out_.phaseDeg = rds_.pilotPhaseDeg();
    out_.phaseSignedDeg = rds_.pilotPhaseSignedDeg();
    out_.coherence = extCoh_;
    out_.driftDegPerSec = extDrift_;
    // ★ Full scale is the pilot's peak over 0.75 — the shared axis the plot is drawn on.
    out_.eyeDevKHz = (eyeBandPk_[0] / 0.75f) / eyeHpGain_[0] * 75.0f;
    for (int b = 0; b < kEyeBands; ++b) out_.eyeBandKHz[b] = eyeBandPk_[b] / eyeHpGain_[b] * 75.0f;
    out_.mpxDevKHz = mpxDevOut_ * 75.0f;
    out_.mpxDevAvgKHz = mpxDevAvgOut_ * 75.0f;
    out_.mpxDevNoiseKHz = mpxDevNoise_ * 75.0f;
    out_.mpxDevHoldKHz = mpxDevHold_ * 75.0f;
    out_.mpxPowerDb = mpxPowerDb_; out_.mpxPowerSecs = mpxPowerSecs_;
    out_.snrDb = snrDb_; out_.snrOk = snrOk_;
    out_.multipath = multipathCorr_; out_.multipathOk = multipathOk_;
    out_.multipathRaw = multipath_.depth();
    if (grids) {
        for (int b = 0; b < kEyeBands; ++b) pubEye_[b] = eyeOut_[b];
        pubMpx_ = mpxOut_;
        ++out_.gridSeq;
    }
}

void MpxMeasure::snapshot(Out& o, std::vector<unsigned char>* eye3, int& eyeW, std::vector<float>* mpx,
                          unsigned& haveSeq) {
    std::lock_guard<std::mutex> lk(outM_);
    o = out_;
    if (out_.gridSeq != haveSeq) {
        haveSeq = out_.gridSeq;
        if (eye3) for (int b = 0; b < kEyeBands; ++b) eye3[b] = pubEye_[b];
        eyeW = pubEye_[0].empty() ? 0 : kEyeWMax;   // ★ the constant, not eyeW_ — the worker owns that
        if (mpx) *mpx = pubMpx_;
    }
}

}  // namespace vibedsp
