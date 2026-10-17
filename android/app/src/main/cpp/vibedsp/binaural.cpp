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
    if (on && !enabled_.load()) resetFx_.store(true);   // the audio thread clears the delay lines on its next block
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
    if (resetFx_.exchange(false)) {
        fx_.reset();
        stereoState_ = false; stereoHeld_ = 0; stereoW_ = 1; sideRatio_ = 0; sideStereo_ = false;
    }
    const int src = source_.load(std::memory_order_relaxed);
    if (src == SsbUsb || src == SsbLsb) ssbFeed_(L, R, n, rate);   // before the effect: the estimator hears the source
    const float p = targetPan_(n, rate, L, R);
    fx_.process(L, R, n, rate, p);
    panPub_.store(fx_.pan(), std::memory_order_relaxed);
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
        return;
    }
    if (gen != workerGen_ || src != workerSrc_) {
        workerGen_ = gen; workerSrc_ = src;
        hist_.clear(); ssbValid_.store(false); reckoned_ = false;
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
        if (ssbValid_.load()) { ssbHz_.store(ssbHz_.load() - d); reckoned_ = true; }
        if (est_) est_->reset();
        rd_.store(wr_.load(std::memory_order_acquire), std::memory_order_release);
        // A move beyond the estimator's reach leaves nothing it could confirm; let the answer go stale.
        if (std::fabs(ssbHz_.load()) > kSsbHoldHz) { ssbValid_.store(false); hist_.clear(); reckoned_ = false; }
    }
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
    if (ssbValid_.load() && !reckoned_ && nowSec() - lastAccept_ > kSsbStaleSec) { ssbValid_.store(false); hist_.clear(); }
}

int BinauralTuning::stats(float* o, int cap) const {
    if (!o || cap <= 0) return 0;
    const int src = source_.load();
    float off = 0; bool ok = false;
    if (src == Given) { off = given_.load(); ok = givenValid_.load(); }
    else if (src == SsbUsb || src == SsbLsb) { off = ssbHz_.load(); ok = ssbValid_.load(); }
    const float v[9] = { enabled_.load() ? 1.0f : 0.0f, (float)src, off, ok ? 1.0f : 0.0f, panPub_.load(), stereoWPub_.load(),
                         (float)estimates_.load(), (float)accepted_.load(), (float)drops_.load() };
    const int n = std::min(cap, 9);
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
int   vibe_binaural_enabled(void* h) { return h && static_cast<BinauralTuning*>(h)->enabled() ? 1 : 0; }
void  vibe_binaural_set_source(void* h, int source, float deadHz, float fullHz) {
    if (h) static_cast<BinauralTuning*>(h)->setSource(source, deadHz, fullHz);
}
void  vibe_binaural_set_offset(void* h, float hz, int valid) {
    if (h) static_cast<BinauralTuning*>(h)->setOffset(hz, valid != 0);
}
void  vibe_binaural_set_stereo(void* h, int hint) { if (h) static_cast<BinauralTuning*>(h)->setStereoHint(hint); }
void  vibe_binaural_set_dial(void* h, double hz) { if (h) static_cast<BinauralTuning*>(h)->setDial(hz); }
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
