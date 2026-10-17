// Binaural tuning — the spatialiser, the controller and its SSB worker. See binaural.h for the why.
#include "binaural.h"
#include "ssb_offset.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace vibedsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

float binauralPanFor(float offsetHz, float deadHz, float fullHz) {
    if (!std::isfinite(offsetHz)) return 0.0f;
    const float dead = std::max(0.0f, deadHz);
    const float full = std::max(dead + 1.0f, fullHz);
    const float x = std::fabs(offsetHz);
    if (x <= dead) return 0.0f;
    float u = (x - dead) / (full - dead);
    if (u > 1.0f) u = 1.0f;
    const float s = u * u * (3.0f - 2.0f * u);     // smoothstep: eased into and out of the ramp
    return offsetHz > 0 ? s : -s;
}

// ── The effect ───────────────────────────────────────────────────────────────────────────────────

void BinauralSpatialiser::configure_(double rate) {
    fs_ = rate;
    // ★ The ring holds the longest delay plus the interpolation tap, at the highest rate we meet (96 kHz → 58
    //   samples); 256 leaves room and keeps the index a mask.
    int need = (int)std::ceil(kMaxItdSec * rate) + 4, sz = 64;
    while (sz < need) sz <<= 1;
    mask_ = sz - 1; w_ = 0;
    l_.ring.assign(sz, 0.0f); r_.ring.assign(sz, 0.0f);
    lpB_ = (float)(1.0 - std::exp(-2.0 * kPi * kShelfHz / rate));
    l_.lp = r_.lp = 0;
    l_.g = r_.g = 1; l_.shelf = r_.shelf = 1; l_.delay = r_.delay = 0;
}

void BinauralSpatialiser::reset() {
    pan_ = 0;
    if (fs_ > 0) configure_(fs_);
}

void BinauralSpatialiser::params_(float far, double fs, float& g, float& shelf, float& delay) {
    // far: 0 (this ear faces the signal, or on tune) … 1 (full lean away from this ear)
    g = std::pow(10.0f, -kMaxDropDb * far / 20.0f);
    shelf = 1.0f - kShelfCut * far;
    delay = (float)(far * kMaxItdSec * fs);
}

void BinauralSpatialiser::ear_(Ear& e, float* x, int n, float g1, float sh1, float d1) {
    // Parameters ramp linearly from where the last sub-block left them — no step for the ear to hear.
    const float inv = 1.0f / (float)n;
    const float dg = (g1 - e.g) * inv, ds = (sh1 - e.shelf) * inv, dd = (d1 - e.delay) * inv;
    float g = e.g, sh = e.shelf, d = e.delay, lp = e.lp;
    float* ring = e.ring.data();
    int w = w_;
    for (int i = 0; i < n; ++i) {
        g += dg; sh += ds; d += dd;
        ring[w] = x[i];
        // Fractional delay, linear interpolation. d = 0 reads the sample just written.
        const int di = (int)d;
        const float fr = d - (float)di;
        const float a = ring[(w - di) & mask_], b = ring[(w - di - 1) & mask_];
        const float y = a + fr * (b - a);
        // Head shadow: a high shelf — everything above kShelfHz scaled by `sh`, the lows untouched.
        lp += lpB_ * (y - lp);
        x[i] = g * (lp + sh * (y - lp));
        w = (w + 1) & mask_;
    }
    e.g = g1; e.shelf = sh1; e.delay = d1; e.lp = lp;
}

void BinauralSpatialiser::process(float* L, float* R, int n, double rate, float panTarget) {
    if (n <= 0 || rate <= 0) return;
    if (rate != fs_) configure_(rate);
    if (!std::isfinite(panTarget)) panTarget = 0;
    panTarget = std::max(-1.0f, std::min(1.0f, panTarget));
    for (int off = 0; off < n; off += kSub) {
        const int m = std::min(kSub, n - off);
        // The one-pole pan smoother, advanced by the whole sub-block at once (exact for a held target).
        const float a = (float)(1.0 - std::exp(-(double)m / (kPanTauSec * fs_)));
        pan_ += a * (panTarget - pan_);
        if (std::fabs(pan_) < 1e-6f && panTarget == 0) pan_ = 0;   // settle exactly: no denormal tail
        // + pan = lean RIGHT, so the LEFT ear is the far one.
        float gL, sL, dL, gR, sR, dR;
        params_(std::max(0.0f, pan_), fs_, gL, sL, dL);
        params_(std::max(0.0f, -pan_), fs_, gR, sR, dR);
        const int w0 = w_;
        ear_(l_, L + off, m, gL, sL, dL);
        w_ = w0;                        // both ears write the same ring positions
        ear_(r_, R + off, m, gR, sR, dR);
        w_ = (w0 + m) & mask_;
    }
}

// ── SPREAD: the filter bank ──────────────────────────────────────────────────────────────────────

BinauralSpread::BinauralSpread() = default;
BinauralSpread::~BinauralSpread() = default;

float BinauralSpread::panAt(float f, int kind, float centre, float half) {
    if (kind == Off || !(half > 0) || !std::isfinite(f)) return 0.0f;
    // ★ Position: −1 at the scale's low edge, +1 at its high edge, linear in Hz. LSB and lower-CW audio runs
    //   the other way in RF, so the sign flips — higher RF is to the right in every sideband.
    const float sense = (kind == Lsb || kind == CwLower) ? -1.0f : 1.0f;
    float u = sense * (f - centre) / half;
    u = std::max(-1.0f, std::min(1.0f, u));
    const float a = std::pow(std::fabs(u), kShape);
    return u >= 0 ? a : -a;
}

void BinauralSpread::configure_(double rate) {
    // ★ Allocates — once, the first time the bank engages at a rate (as the spatialiser's delay ring does on a rate
    //   change). Re-engaging at the same rate reuses everything; steady running never allocates.
    fs_ = rate;
    int n = 128;
    while (n < 4096 && rate / n > kBinHz) n <<= 1;   // 48 kHz → 512 (93.75 Hz), 12 kHz → 128, 96 kHz → 1024
    n_ = n; h_ = n / 2;
    fwd_ = std::make_unique<RealFFT>(n_);
    inv_ = std::make_unique<RealFFT>(n_, true);
    win_.resize(n_); win2_.resize(n_);
    for (int i = 0; i < n_; ++i) {
        win_[i] = (float)std::sin(kPi * (i + 0.5) / n_);   // sqrt of a periodic Hann
        win2_[i] = win_[i] * win_[i];                      // analysis × synthesis: the identity frame (no FFT)
    }
    in_.assign(n_, 0.0f); frame_buf_.assign(n_, 0.0f); outL_.assign(n_, 0.0f); outR_.assign(n_, 0.0f);
    tmp_.assign(n_, 0.0f);
    dryL_.assign(n_, 0.0f); dryR_.assign(n_, 0.0f);     // the on-tune path: exactly the bank's latency
    const int b = n_ / 2 + 1;
    spec_.assign(b, cf32(0, 0)); specL_.assign(b, cf32(0, 0)); specR_.assign(b, cf32(0, 0));
    wL_.assign(b, cf32(0, 0)); wR_.assign(b, cf32(0, 0));
    psm_.assign(b, -200.0f); floor_.assign(b, -200.0f);
    pos_ = 0; warm_ = 0; dpos_ = 0; wAmt_ = -1; cwHops_ = 0;
}

void BinauralSpread::cwForget_() {
    cwMeasured_ = false; cwCentre_ = kCwDefaultHz;
    std::fill(psm_.begin(), psm_.end(), -200.0f); std::fill(floor_.begin(), floor_.end(), -200.0f);
}

void BinauralSpread::weights_() {
    // ★ Rebuilt only when the amount, the map or the CW centre moves — steady, it is never touched.
    const int b = n_ / 2 + 1;
    const bool cw = kind_ == CwUpper || kind_ == CwLower;
    const float half = 0.5f * std::fabs(hi_ - lo_);
    const float centre = cw ? cwCentre_ : 0.5f * (lo_ + hi_);
    const float norm = 1.0f / (float)n_;               // KissFFT's round trip scales by N
    for (int k = 0; k < b; ++k) {
        const float f = (float)(k * fs_ / n_);
        const float p = eff_ * panAt(f, kind_, centre, half);
        const float farL = std::max(0.0f, p), farR = std::max(0.0f, -p);   // + = sits right: the LEFT ear is far
        auto w = [&](float far) {
            const float g = std::pow(10.0f, -BinauralSpatialiser::kMaxDropDb * far / 20.0f);
            const float sh = 1.0f - BinauralSpatialiser::kShelfCut * far;
            const float r = f / BinauralSpatialiser::kShelfHz;
            const float mag = std::sqrt((1.0f + sh * sh * r * r) / (1.0f + r * r));   // the head-shadow shelf at f
            const float ph = (float)(-2.0 * kPi * f * far * BinauralSpatialiser::kMaxItdSec);   // the delay, as phase
            return cf32(g * mag * norm * std::cos(ph), g * mag * norm * std::sin(ph));
        };
        wL_[k] = w(farL); wR_[k] = w(farR);
    }
    wAmt_ = eff_;
}

void BinauralSpread::cwMeasure_() {
    /* ★★ THE CW NOTE = THE MIDDLE OF THE NOISE. Each bin's power is smoothed (~0.1 s), and a floor follows it
     *    DOWN at once and UP slowly (0.5 dB/s) — so a keyed CW signal, which leaves gaps, barely lifts it, and what
     *    is left is the shape of the receiver's filter on its own noise. The passband is where that floor stands
     *    within 6 dB of its top; its middle is the note. Too narrow (< 2 bins), too wide (> 3 kHz) or silent
     *    (squelch, a −120 dB floor) → keep the last answer. */
    const int b = n_ / 2 + 1;
    const double binHz = fs_ / n_;
    const int k0 = std::max(1, (int)std::ceil(100.0 / binHz)), k1 = std::min(b - 2, (int)(3200.0 / binHz));
    if (k1 - k0 < 3) return;
    int pk = k0;
    for (int k = k0; k <= k1; ++k) if (floor_[k] > floor_[pk]) pk = k;
    if (floor_[pk] < -120.0f) return;
    const float thr = floor_[pk] - 6.0f;
    int a = pk, z = pk;
    while (a > k0 && floor_[a - 1] >= thr) --a;
    while (z < k1 && floor_[z + 1] >= thr) ++z;
    const double lo = (a - 0.5) * binHz, hi = (z + 0.5) * binHz;
    if (z - a + 1 < 2 || hi - lo > 3000.0) return;
    const float c = (float)(0.5 * (lo + hi));
    if (!cwMeasured_) { cwCentre_ = c; cwMeasured_ = true; wAmt_ = -1; return; }
    // Moves of under a bin are measurement noise; larger ones glide (half-way per look, ~4 looks a second).
    if (std::fabs(c - cwCentre_) > binHz) { cwCentre_ += 0.5f * (c - cwCentre_); wAmt_ = -1; }
}

void BinauralSpread::frame_() {
    const int n = n_, h = h_, b = n_ / 2 + 1;
    const bool cw = kind_ == CwUpper || kind_ == CwLower;
    // ★★ ON TUNE (no spread) THE BANK IS THE IDENTITY, so on SSB it skips the FFTs and overlap-adds the windowed
    //    input straight back (analysis × synthesis window = the Hann, which overlap-adds to 1). Nobody hears it —
    //    the output on tune is the delay line — but it keeps the accumulators exactly as the FFT path would have
    //    left them, so the first frame off tune joins without a seam. CW still needs the spectrum for the note.
    const bool identity = eff_ <= 0.0f;
    if (!identity || cw) {
        for (int i = 0; i < n; ++i) frame_buf_[i] = in_[i] * win_[i];
        fwd_->forward(frame_buf_.data(), spec_.data());
    }
    if (cw) {
        const float rise = 0.5f * (float)h / (float)fs_;   // dB per hop: 0.5 dB/s
        // ★ …but 12 dB/s while the floor sits more than 12 dB under the power: after a squelch closes and opens,
        //   the floor has followed the silence down, and at 0.5 dB/s it would take minutes to find the noise again.
        //   A keyed mark (≈ 0.1 s) lifts it barely a dB before the next space drops it back.
        const float riseFast = 12.0f * (float)h / (float)fs_;
        for (int k = 0; k < b; ++k) {
            const float p = 10.0f * std::log10(std::norm(spec_[k]) + 1e-20f);
            // ★ The first frame seeds both (an unset floor of −200 dB would take hours to rise at 0.5 dB/s).
            if (psm_[k] <= -199.0f) { psm_[k] = p; floor_[k] = p; continue; }
            psm_[k] += 0.1f * (p - psm_[k]);
            floor_[k] = psm_[k] < floor_[k] ? psm_[k] : floor_[k] + (psm_[k] - floor_[k] > 12.0f ? riseFast : rise);
        }
        if (++cwHops_ * h >= (int)(0.25 * fs_)) { cwHops_ = 0; cwMeasure_(); }
    }
    // Slide the overlap-add accumulators by a hop, then add this frame (synthesis window applied).
    std::memmove(outL_.data(), outL_.data() + h, sizeof(float) * (n - h));
    std::memmove(outR_.data(), outR_.data() + h, sizeof(float) * (n - h));
    std::fill(outL_.begin() + (n - h), outL_.end(), 0.0f);
    std::fill(outR_.begin() + (n - h), outR_.end(), 0.0f);
    if (identity) {
        for (int i = 0; i < n; ++i) { const float y = in_[i] * win2_[i]; outL_[i] += y; outR_[i] += y; }
    } else {
        // ★ Rebuilt as the amount moves — but not for every whisker of the smoother's tail (a few thousand
        //   transcendentals per rebuild); 0.002 of the full spread is far below anything the ear can place.
        if (wAmt_ < 0 || std::fabs(eff_ - wAmt_) > 0.002f || (eff_ != wAmt_ && eff_ >= 1.0f)) weights_();
        for (int k = 0; k < b; ++k) { specL_[k] = spec_[k] * wL_[k]; specR_[k] = spec_[k] * wR_[k]; }
        inv_->inverse(specL_.data(), tmp_.data());
        for (int i = 0; i < n; ++i) outL_[i] += tmp_[i] * win_[i];
        inv_->inverse(specR_.data(), tmp_.data());
        for (int i = 0; i < n; ++i) outR_[i] += tmp_[i] * win_[i];
    }
    std::memmove(in_.data(), in_.data() + h, sizeof(float) * (n - h));
}

void BinauralSpread::process(float* L, float* R, int n, double rate, int kind, float amount, float edgeLo, float edgeHi) {
    if (n <= 0 || rate <= 0 || !L || !R) return;
    if (kind < Off || kind > CwLower) kind = Off;
    if (!running_) {
        if (kind == Off) return;                        // bypassed: not a single multiply
        if (rate != fs_ || !fwd_) configure_(rate);
        else {
            std::fill(in_.begin(), in_.end(), 0.0f); std::fill(outL_.begin(), outL_.end(), 0.0f);
            std::fill(outR_.begin(), outR_.end(), 0.0f); std::fill(dryL_.begin(), dryL_.end(), 0.0f);
            std::fill(dryR_.begin(), dryR_.end(), 0.0f); pos_ = 0; warm_ = 0; dpos_ = 0; cwHops_ = 0;
        }
        running_ = true; amt_ = 0; eff_ = 0; map_ = 1; wet_ = 0; wAmt_ = -1; kind_ = kind;
        if (kind_ == CwUpper || kind_ == CwLower) cwForget_();
    } else if (rate != fs_) {
        // A rate change mid-run: start the bank again from a dry crossfade (rare — a reconnect).
        configure_(rate); amt_ = 0; eff_ = 0; map_ = 1; wet_ = 0; kind_ = kind == Off ? kind_ : kind;
    }
    if (std::isfinite(edgeLo) && std::isfinite(edgeHi) && std::fabs(edgeHi - edgeLo) >= 100.0f
        && (edgeLo != lo_ || edgeHi != hi_)) { lo_ = std::min(edgeLo, edgeHi); hi_ = std::max(edgeLo, edgeHi); wAmt_ = -1; }

    const bool want = kind != Off;
    float tgt = want && std::isfinite(amount) ? std::max(0.0f, std::min(1.0f, amount)) : 0.0f;
    // ★ The lean's own smoothing (a one-pole at kPanTauSec), so spread and lean, fed the same w, move together.
    const float aAmt = (float)(1.0 - std::exp(-1.0 / (BinauralSpatialiser::kPanTauSec * fs_)));
    const float dSwap = (float)(1.0 / (kSwapSec * fs_)), dWet = (float)(1.0 / (kWetSec * fs_));
    const float mixPer = 1.0f / kMixFullAt;
    const int h = h_;
    for (int i = 0; i < n; ++i) {
        // ★ The order of a glide: IN — warm a frame (the delay line and the bank fill), then the undelayed input
        //   crossfades to the delayed path. The spread follows w throughout. OUT — the spread down first, then wet
        //   down. A NEW MAP — the spread down, swap, back up; the delay never moves.
        if (want && kind != kind_) {
            if (map_ > 0) map_ = std::max(0.0f, map_ - dSwap);
            else {
                if ((kind_ == CwUpper || kind_ == CwLower) != (kind == CwUpper || kind == CwLower)) cwForget_();
                kind_ = kind; wAmt_ = -1;
            }
        } else if (map_ < 1) {
            map_ = std::min(1.0f, map_ + dSwap);
        }
        amt_ += aAmt * (tgt - amt_);
        if (std::fabs(tgt - amt_) < 1e-4f) amt_ = tgt;   // settle exactly: on tune means 0, not 1e-30
        eff_ = amt_ * map_;
        if (want) {
            if (warm_ < n_) ++warm_;
            else if (wet_ < 1) wet_ = std::min(1.0f, wet_ + dWet);
        } else if (amt_ <= 0 && wet_ > 0) {
            wet_ = std::max(0.0f, wet_ - dWet);
        }
        const float dl = L[i], dr = R[i];
        in_[n_ - h + pos_] = 0.5f * (dl + dr);
        // The delay line: exactly the bank's latency (N), each ear on its own — on tune, the input as it was.
        const float ddl = dryL_[dpos_], ddr = dryR_[dpos_];
        dryL_[dpos_] = dl; dryR_[dpos_] = dr;
        if (++dpos_ == n_) dpos_ = 0;
        float pl = ddl, pr = ddr;
        const float mx = std::min(1.0f, eff_ * mixPer);
        if (mx > 0) { pl = ddl + mx * (outL_[pos_] - ddl); pr = ddr + mx * (outR_[pos_] - ddr); }
        if (wet_ >= 1) { L[i] = pl; R[i] = pr; }        // ★ exact: on tune, wet 1, mx 0 → the delayed input bit for bit
        else { L[i] = dl + wet_ * (pl - dl); R[i] = dr + wet_ * (pr - dr); }
        if (++pos_ == h) { pos_ = 0; frame_(); }
        if (!want && amt_ <= 0 && wet_ <= 0) {
            // Fully out: the rest of this block (from the next sample) is dry, and the bank stops.
            running_ = false; amt_ = 0; eff_ = 0; wet_ = 0;
            return;
        }
    }
}

// ── The controller ───────────────────────────────────────────────────────────────────────────────

BinauralTuning::BinauralTuning(bool startWorker) : ring_(kRing, 0.0f), startWorker_(startWorker) {
    hist_.reserve(kMedianOf + 1);
    chunk_.resize(4096);
    sL_.reserve(4096); sR_.reserve(4096);
}

BinauralTuning::~BinauralTuning() {
    quit_.store(true);
    if (worker_.joinable()) worker_.join();
}

void BinauralTuning::setEnabled(bool on) {
    // The audio thread clears the delay lines on its next block — unless it is still gliding out from a moment ago,
    // when carrying on from where it is is the seamless thing (see processPlanar).
    if (on && !enabled_.load()) resetFx_.store(true);
    enabled_.store(on);
    if (on) { const int s = source_.load(); if (s == SsbUsb || s == SsbLsb) ensureWorker_(); }
}

void BinauralTuning::setSource(int source, float deadHz, float fullHz) {
    if (source < None || source > SsbLsb) source = None;
    if (std::isfinite(deadHz)) dead_.store(std::max(0.0f, deadHz));
    if (std::isfinite(fullHz)) full_.store(std::max(1.0f, fullHz));
    if (source_.exchange(source) != source) {
        gen_.fetch_add(1);
        ssbValid_.store(false);
        givenValid_.store(false);
        hintValid_.store(false);    // a waterfall cue belongs to the sideband it was read for
    }
    if ((source == SsbUsb || source == SsbLsb) && enabled_.load()) ensureWorker_();
}

void BinauralTuning::setOffset(float hz, bool valid) {
    given_.store(std::isfinite(hz) ? hz : 0.0f);
    givenValid_.store(valid && std::isfinite(hz));
}

void BinauralTuning::setStereoHint(int hint) {
    stereoHint_.store(hint == Locked ? Locked : hint == Unknown ? Unknown : Mono);
}

void BinauralTuning::setDial(double hz) {
    if (std::isfinite(hz)) dial_.store(hz);
}

void BinauralTuning::setSsbHint(double carrierHz, bool valid) {
    if (std::isfinite(carrierHz)) hintCarrier_.store(carrierHz);
    hintValid_.store(valid && std::isfinite(carrierHz));
}

void BinauralTuning::setSpread(int kind, float edgeLoHz, float edgeHiHz) {
    if (kind < BinauralSpread::Off || kind > BinauralSpread::CwLower) kind = BinauralSpread::Off;
    if (std::isfinite(edgeLoHz)) spreadLo_.store(edgeLoHz);
    if (std::isfinite(edgeHiHz)) spreadHi_.store(edgeHiHz);
    spreadKind_.store(kind);
}

bool BinauralTuning::ssbOffset(float& hz) const {
    hz = ssbHz_.load();
    return ssbValid_.load();
}

float BinauralTuning::targetPan_(int n, int rate, const float* L, const float* R) {
    const int src = source_.load(std::memory_order_relaxed);
    float off = 0; bool ok = false;
    if (src == Given) { off = given_.load(); ok = givenValid_.load(); }
    else if (src == SsbUsb || src == SsbLsb) { off = ssbHz_.load(); ok = ssbValid_.load(); }
    float p = ok ? binauralPanFor(off, dead_.load(), full_.load()) : 0.0f;

    /* ★★ THE WFM HAND-OFF. Off tune the pilot is not locked and the aid pans you in; close enough to lock,
     *    the stereo image takes over. ✗ NO HARD SWITCH: near lock the pilot flickers, and a switch would make
     *    the image jump between panned-mono and stereo several times a second. Two guards:
     *    1. HYSTERESIS IN TIME — the reading must disagree with the current state for kStereoOnHoldSec (going
     *       stereo) or kStereoOffHoldSec (going mono) before the state flips; a flicker shorter than that is
     *       ignored outright.
     *    2. A SLOW CROSSFADE — the aid's weight then ramps over kStereoFadeSec, so even a real hand-off is a
     *       glide.
     *    ★ No server sends its stereo decoder's BLEND amount to every client (VibeServer's hiCutLmr reaches only
     *      the Advanced RDS panel; OWRX, Kiwi and UberSDR send none), so the reading is the stereo INDICATOR
     *      the client already shows — or, where the backend reports none (Stereo::Unknown), the audio itself:
     *      the L−R power against L+R, which is the blend as it actually arrives, with its own hysteresis. */
    const double secs = rate > 0 ? (double)n / rate : 0.0;
    const int hint = stereoHint_.load(std::memory_order_relaxed);
    bool raw;
    if (hint == Unknown && R && L) {
        double es = 0, em = 0;
        for (int i = 0; i < n; ++i) { const float s = L[i] - R[i], m = L[i] + R[i]; es += s * s; em += m * m; }
        const float r = em > 1e-12 ? (float)(es / em) : 0.0f;
        const float a = (float)(1.0 - std::exp(-secs / 0.5));
        sideRatio_ += a * (r - sideRatio_);
        if (!sideStereo_ && sideRatio_ > kSideOnRatio) sideStereo_ = true;
        else if (sideStereo_ && sideRatio_ < kSideOffRatio) sideStereo_ = false;
        raw = sideStereo_;
    } else {
        raw = hint == Locked;
    }
    if (raw != stereoState_) {
        stereoHeld_ += secs;
        if (stereoHeld_ >= (raw ? kStereoOnHoldSec : kStereoOffHoldSec)) { stereoState_ = raw; stereoHeld_ = 0; }
    } else {
        stereoHeld_ = 0;
    }
    const float want = stereoState_ ? 0.0f : 1.0f;
    const float step = (float)(secs / kStereoFadeSec);
    if (stereoW_ < want) stereoW_ = std::min(want, stereoW_ + step);
    else if (stereoW_ > want) stereoW_ = std::max(want, stereoW_ - step);
    p *= stereoW_;
    stereoWPub_.store(stereoW_, std::memory_order_relaxed);
    return p;
}

void BinauralTuning::ssbFeed_(const float* L, const float* R, int n, int rate) {
    // ★ Never waits: if the worker has fallen behind and the ring is full, this block is dropped.
    ringRate_.store(rate, std::memory_order_relaxed);
    const uint32_t w = wr_.load(std::memory_order_relaxed), r = rd_.load(std::memory_order_acquire);
    if ((uint32_t)(w - r) + (uint32_t)n > kRing) { drops_.fetch_add(1, std::memory_order_relaxed); return; }
    float* ring = ring_.data();
    for (int i = 0; i < n; ++i) ring[(w + i) & (kRing - 1)] = 0.5f * (L[i] + R[i]);
    wr_.store(w + (uint32_t)n, std::memory_order_release);
}

void BinauralTuning::processPlanar(float* L, float* R, int n, int rate) {
    if (!L || !R || n <= 0 || rate <= 0) return;
    const bool on = enabled_.load(std::memory_order_relaxed);
    // ★ Switched back on while still gliding out (iOS keeps calling until active() drops): keep the state — a reset
    //   would cut the delay line mid-glide. Otherwise start clean, never from a stale frame.
    if (resetFx_.exchange(false) && !glidingOut_) {
        fx_.reset();
        spread_.reset();
        stereoState_ = false; stereoHeld_ = 0; stereoW_ = 1; sideRatio_ = 0; sideStereo_ = false;
    }
    if (!on && !spread_.running() && fx_.pan() == 0.0f) {   // fully out: the untouched input
        glidingOut_ = false; activePub_.store(false, std::memory_order_relaxed);
        return;
    }
    const int src = source_.load(std::memory_order_relaxed);
    const int sk = on ? spreadKind_.load(std::memory_order_relaxed) : (int)BinauralSpread::Off;
    // ★★ The SSB estimator is fed WHENEVER binaural is on in SSB — the spread included: on SSB it is the offset
    //    source that sets w, and so the spread itself. (Until 476 it was starved under the SPREAD style.)
    if (on && (src == SsbUsb || src == SsbLsb)) ssbFeed_(L, R, n, rate);   // before the effect
    const float p = on ? targetPan_(n, rate, L, R) : 0.0f;
    // ★★ ONE WEIGHT FOR BOTH: the spread's amount IS the lean's size, w = |p| — 0 in the dead zone (untouched),
    //    1 at full scale. The bank smooths it with the lean's own time constant, so they grow together.
    spread_.process(L, R, n, rate, sk, std::fabs(p), spreadLo_.load(std::memory_order_relaxed), spreadHi_.load(std::memory_order_relaxed));
    fx_.process(L, R, n, rate, p);
    panPub_.store(fx_.pan(), std::memory_order_relaxed);
    spreadPub_.store(spread_.running() ? spread_.amount() : 0.0f, std::memory_order_relaxed);
    cwCentrePub_.store(spread_.cwCentreHz(), std::memory_order_relaxed);
    const bool still = spread_.running() || fx_.pan() != 0.0f;
    glidingOut_ = !on && still;
    activePub_.store(on || still, std::memory_order_relaxed);
}

int BinauralTuning::processInt16(const int16_t* in, int inCh, int frames, int rate, int16_t* out) {
    if (!in || !out || frames <= 0 || (inCh != 1 && inCh != 2)) return 0;
    if ((int)sL_.size() < frames) { sL_.resize(frames); sR_.resize(frames); }   // grows once, then never again
    float* L = sL_.data(); float* R = sR_.data();
    constexpr float k = 1.0f / 32768.0f;
    if (inCh == 1) for (int i = 0; i < frames; ++i) L[i] = R[i] = in[i] * k;   // mono → both ears first
    else for (int i = 0; i < frames; ++i) { L[i] = in[2 * i] * k; R[i] = in[2 * i + 1] * k; }
    processPlanar(L, R, frames, rate);
    for (int i = 0; i < frames; ++i) {
        const float a = std::max(-1.0f, std::min(1.0f, L[i])), b = std::max(-1.0f, std::min(1.0f, R[i]));
        out[2 * i] = (int16_t)std::lrint(a * 32767.0f);
        out[2 * i + 1] = (int16_t)std::lrint(b * 32767.0f);
    }
    return frames;
}

// ── The SSB worker ───────────────────────────────────────────────────────────────────────────────

void BinauralTuning::ensureWorker_() {
    if (!startWorker_ || workerUp_.exchange(true)) return;
    worker_ = std::thread([this] { workerLoop_(); });
}

void BinauralTuning::workerLoop_() {
    // ★ A decoder, so it takes what is left: the audio thread never waits for it (the ring drops instead).
    while (!quit_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        ssbStep_();
    }
}

void BinauralTuning::pumpSsbForTest() { ssbStep_(); }

void BinauralTuning::ssbAccept_(float signalHz) {
    hist_.push_back(signalHz);
    // ★ reckoned_ stays set: an answer that agreed with the knob refines it, it does not hand the estimator back
    //   the authority to drag it to centre (kReckonTrustHz). Only a dial move or a reset changes who is in charge.
    if ((int)hist_.size() > kMedianOf) hist_.erase(hist_.begin());
    lastAccept_ = nowSec();
    accepted_.fetch_add(1);
    if ((int)hist_.size() < kMedianMin) return;
    std::vector<float> s(hist_);
    std::sort(s.begin(), s.end());
    const size_t n = s.size();
    const float med = n % 2 ? s[n / 2] : 0.5f * (s[n / 2 - 1] + s[n / 2]);
    int agree = 0;
    for (float v : hist_) if (std::fabs(v - med) <= kAgreeHz) ++agree;
    // ★ A median of answers that do not agree (two stations in a QSO, a pitch alias) is not an answer:
    //   hold the last one rather than lean toward a number nobody measured.
    if (agree < kMedianMin || agree * 2 <= (int)n) return;
    ssbHz_.store(med);
    ssbValid_.store(true);
    // ★ The estimator has spoken (two agreeing answers): a coarse-held answer hands over to it here.
    if (coarseHeld_) { coarseHeld_ = false; coarsePub_.store(false); }
}

void BinauralTuning::release_() {
    ssbValid_.store(false); hist_.clear(); reckoned_ = false;
    coarseHeld_ = false; coarsePub_.store(false);
}

void BinauralTuning::coarseStep_() {
    // ★ The hand-off — see setSsbHint in binaural.h for the order of authority and why.
    if (!hintValid_.load()) { if (coarseHeld_) release_(); return; }
    const float c = (float)(hintCarrier_.load() - dial_.load());
    if (!std::isfinite(c) || std::fabs(c) > kSsbHoldHz) { if (coarseHeld_) release_(); return; }
    auto seed = [&] {
        hist_.clear(); reckoned_ = false;
        coarseHeld_ = true; coarsePub_.store(true);
        ssbHz_.store(c); ssbValid_.store(true);
    };
    if (coarseHeld_) {
        // Ours already: follow the cue — unless it now says "on tune or within its own error", which it cannot
        // tell apart, so let go and leave the fine centring to the estimator.
        if (std::fabs(c) < kCoarseMinHz) release_();
        else ssbHz_.store(c);
        return;
    }
    const bool valid = ssbValid_.load();
    const float h = ssbHz_.load();
    if (!valid) { if (std::fabs(c) >= kCoarseMinHz) seed(); return; }
    if (std::fabs(h) <= kSsbRangeHz) return;              // 1. a fine answer in reach: never fought
    if (std::fabs(c - h) <= kCoarseAgreeHz) return;       // 3. the same voice: the knob's exact figure stays
    if (std::fabs(c) >= kCoarseMinHz) seed();             //    a different voice fills the passband
    else release_();                                      //    a voice on tune: let the estimator look
}

void BinauralTuning::ssbStep_() {
    const int src = source_.load();
    const int gen = gen_.load();
    const bool ssb = (src == SsbUsb || src == SsbLsb) && enabled_.load();
    if (!ssb) {
        // Idle: nothing is fed; keep the ring empty so a later start begins with fresh audio.
        rd_.store(wr_.load(std::memory_order_acquire), std::memory_order_release);
        if (est_) { est_.reset(); estRate_ = 0; }
        hist_.clear(); ssbValid_.store(false); workerGen_ = gen; workerSrc_ = src; haveDial_ = false; reckoned_ = false;
        coarseHeld_ = false; coarsePub_.store(false);
        return;
    }
    if (gen != workerGen_ || src != workerSrc_) {
        workerGen_ = gen; workerSrc_ = src;
        hist_.clear(); ssbValid_.store(false); reckoned_ = false;
        coarseHeld_ = false; coarsePub_.store(false);
        if (est_) est_->reset();
        haveDial_ = false;
        rd_.store(wr_.load(std::memory_order_acquire), std::memory_order_release);
    }
    // ★★ THE DIAL MOVED: move what we hold by the same amount. offset = signal − dial, so dial +Δ → offset −Δ,
    //    in BOTH sidebands (the sideband was applied when each answer was accepted). The lean then follows
    //    the knob at once. The estimator's window straddles the move and means nothing — start it again, and
    //    drop the audio queued before the move.
    const double dial = dial_.load();
    if (!haveDial_) { lastDial_ = dial; haveDial_ = true; }
    else if (std::fabs(dial - lastDial_) >= kDialMoveHz) {
        const float d = (float)(dial - lastDial_);
        lastDial_ = dial;
        for (float& v : hist_) v -= d;
        // ★ A coarse-held answer moves too, but stays COARSE (not reckoned): its figure is still the cue's ±150 Hz,
        //   so the estimator may replace it, not merely refine it. coarseStep_ re-reads the cue below anyway.
        if (ssbValid_.load()) { ssbHz_.store(ssbHz_.load() - d); if (!coarseHeld_) reckoned_ = true; }
        if (est_) est_->reset();
        rd_.store(wr_.load(std::memory_order_acquire), std::memory_order_release);
        // A move beyond the estimator's reach leaves nothing it could confirm; let the answer go stale.
        if (std::fabs(ssbHz_.load()) > kSsbHoldHz) release_();
    }
    coarseStep_();
    const int rate = ringRate_.load();
    if (rate <= 0) return;
    if (!est_ || rate != estRate_) {
        est_ = std::make_unique<SsbOffsetEstimator>((double)rate, SsbOffsetEstimator::Profile::Fast);
        estRate_ = rate;
    }
    const int sb = src == SsbUsb ? 1 : -1;
    uint32_t r = rd_.load(std::memory_order_relaxed);
    const uint32_t w = wr_.load(std::memory_order_acquire);
    while (r != w) {
        const int n = (int)std::min<uint32_t>(w - r, (uint32_t)chunk_.size());
        for (int i = 0; i < n; ++i) chunk_[i] = ring_[(r + i) & (kRing - 1)];
        r += (uint32_t)n;
        rd_.store(r, std::memory_order_release);
        if (est_->push(chunk_.data(), n)) {
            estimates_.fetch_add(1);
            const SsbOffsetEstimate& e = est_->last();
            const bool gated = e.valid && e.frames >= e.minFrames
                && e.confidence >= kFastGateScale * SsbOffsetEstimator::gateFor(e.offsetHz)
                && (std::fabs(e.offsetHz) <= SsbOffsetEstimator::kLargeHz || e.snrDb >= SsbOffsetEstimator::kLargeMinSnrDb)
                && std::fabs(e.offsetHz) <= kSsbRangeHz;
            // ★ +d = the audio is d Hz HIGH. USB: the signal is above the dial (tune up, lean right) → +d.
            //   LSB inverts the audio: high audio means the signal is BELOW the dial → −d.
            // ★★ BEYOND ITS REACH, THE ESTIMATOR IS NOT ASKED. While the held answer is outside ±kSsbRangeHz the
            //    voice is somewhere it cannot see, so anything it reports is a pitch alias or another station —
            //    and a few of those would outvote the knob's exact figure in the median. It resumes refining the
            //    moment the dial brings the signal back within its range.
            const bool beyondReach = ssbValid_.load() && std::fabs(ssbHz_.load()) > kSsbRangeHz;
            const bool contradicts = reckoned_ && ssbValid_.load()
                                  && std::fabs(sb * e.offsetHz - ssbHz_.load()) > kReckonTrustHz;   // see kReckonTrustHz
            if (gated && !beyondReach && !contradicts && std::isfinite(e.offsetHz)) ssbAccept_(sb * e.offsetHz);
        }
    }
    // ★ Only the ESTIMATOR's own answers go stale — a reckoned one holds while the dial stays put (see kSsbHoldHz).
    //   Nor does a coarse-held one: the cue is live, and its own going-invalid releases it (coarseStep_).
    if (ssbValid_.load() && !reckoned_ && !coarseHeld_ && nowSec() - lastAccept_ > kSsbStaleSec) { ssbValid_.store(false); hist_.clear(); }
}

int BinauralTuning::stats(float* o, int cap) const {
    if (!o || cap <= 0) return 0;
    const int src = source_.load();
    float off = 0; bool ok = false;
    if (src == Given) { off = given_.load(); ok = givenValid_.load(); }
    else if (src == SsbUsb || src == SsbLsb) { off = ssbHz_.load(); ok = ssbValid_.load(); }
    const float v[12] = { enabled_.load() ? 1.0f : 0.0f, (float)src, off, ok ? 1.0f : 0.0f, panPub_.load(), stereoWPub_.load(),
                          (float)estimates_.load(), (float)accepted_.load(), (float)drops_.load(),
                          spreadPub_.load(), coarsePub_.load() ? 1.0f : 0.0f, cwCentrePub_.load() };
    const int n = std::min(cap, 12);
    for (int i = 0; i < n; ++i) o[i] = v[i];
    return n;
}

}  // namespace vibedsp

// ── C API ─────────────────────────────────────────────────────────────────────────────────────────
using vibedsp::BinauralTuning;

extern "C" {
void* vibe_binaural_create(void) { return new BinauralTuning(); }
void  vibe_binaural_destroy(void* h) { delete static_cast<BinauralTuning*>(h); }
void  vibe_binaural_set_enabled(void* h, int on) { if (h) static_cast<BinauralTuning*>(h)->setEnabled(on != 0); }
int   vibe_binaural_enabled(void* h) { return h && static_cast<BinauralTuning*>(h)->active() ? 1 : 0; }
void  vibe_binaural_set_source(void* h, int source, float deadHz, float fullHz) {
    if (h) static_cast<BinauralTuning*>(h)->setSource(source, deadHz, fullHz);
}
void  vibe_binaural_set_offset(void* h, float hz, int valid) {
    if (h) static_cast<BinauralTuning*>(h)->setOffset(hz, valid != 0);
}
void  vibe_binaural_set_stereo(void* h, int hint) { if (h) static_cast<BinauralTuning*>(h)->setStereoHint(hint); }
void  vibe_binaural_set_dial(void* h, double hz) { if (h) static_cast<BinauralTuning*>(h)->setDial(hz); }
void  vibe_binaural_set_ssb_hint(void* h, double carrierHz, int valid) {
    if (h) static_cast<BinauralTuning*>(h)->setSsbHint(carrierHz, valid != 0);
}
void  vibe_binaural_set_spread(void* h, int kind, float edgeLoHz, float edgeHiHz) {
    if (h) static_cast<BinauralTuning*>(h)->setSpread(kind, edgeLoHz, edgeHiHz);
}
void  vibe_binaural_process_planar(void* h, float* L, float* R, int n, int rate) {
    if (h) static_cast<BinauralTuning*>(h)->processPlanar(L, R, n, rate);
}
int   vibe_binaural_process_i16(void* h, const int16_t* in, int inCh, int frames, int rate, int16_t* out) {
    return h ? static_cast<BinauralTuning*>(h)->processInt16(in, inCh, frames, rate, out) : 0;
}
int   vibe_binaural_stats(void* h, float* out, int cap) {
    return h ? static_cast<BinauralTuning*>(h)->stats(out, cap) : 0;
}
}
