// VibeSDR V5 — RDS data-link layer (block sync + group parsing).
// Clean-room implementation of EN 50067 / IEC 62106. Original VibeSDR code.
#include "vibedsp.h"
#include <cstring>
#include <string>
#include <cmath>

namespace vibedsp {

// ── ★★ THE RDS CHARACTER SET (G0, IEC 62106 Annex E) ────────────────────────
// RDS does NOT use ASCII above 0x7F — it has its own repertoire, and the bytes were being
// copied straight through as if they were ASCII. Fine for the UK, visibly broken for every
// continental station, which is exactly who FM-DXers spend their time listening to: a
// Norwegian "NRK P3" is fine but "Sørlandet" arrives as mojibake, and an accented artist
// name in RadioText is worse because there is no obvious clue it is wrong.
// Converted to UTF-8 on the way out, so every consumer downstream stays plain UTF-8.
static const char* const kRdsG0[256] = {
    " ", " ", " ", " ", " ", " ", " ", " ",
    " ", " ", " ", " ", " ", " ", " ", " ",
    " ", " ", " ", " ", " ", " ", " ", " ",
    " ", " ", " ", " ", " ", " ", " ", " ",
    " ", "!", "\"", "#", "$", "%", "&", "'",
    "(", ")", "*", "+", ",", "-", ".", "/",
    "0", "1", "2", "3", "4", "5", "6", "7",
    "8", "9", ":", ";", "<", "=", ">", "?",
    "@", "A", "B", "C", "D", "E", "F", "G",
    "H", "I", "J", "K", "L", "M", "N", "O",
    "P", "Q", "R", "S", "T", "U", "V", "W",
    "X", "Y", "Z", "[", "\\", "]", "^", "_",
    "`", "a", "b", "c", "d", "e", "f", "g",
    "h", "i", "j", "k", "l", "m", "n", "o",
    "p", "q", "r", "s", "t", "u", "v", "w",
    "x", "y", "z", "{", "|", "}", "~", "",
    "á", "à", "é", "è", "í", "ì", "ó", "ò",
    "ú", "ù", "Ñ", "Ç", "Ş", "ß", "¡", "Ĳ",
    "â", "ä", "ê", "ë", "î", "ï", "ô", "ö",
    "û", "ü", "ñ", "ç", "ş", "ğ", "ı", "ĳ",
    "ª", "α", "©", "‰", "Ğ", "ě", "ň", "ő",
    "π", "€", "£", "$", "←", "↑", "→", "↓",
    "º", "¹", "²", "³", "±", "İ", "ń", "ű",
    "µ", "¿", "÷", "°", "¼", "½", "¾", "§",
    "Á", "À", "É", "È", "Í", "Ì", "Ó", "Ò",
    "Ú", "Ù", "Ř", "Č", "Š", "Ž", "Ð", "Ŀ",
    "Â", "Ä", "Ê", "Ë", "Î", "Ï", "Ô", "Ö",
    "Û", "Ü", "ř", "č", "š", "ž", "đ", "ŀ",
    "Ã", "Å", "Æ", "Œ", "ŷ", "Ý", "Õ", "Ø",
    "Þ", "Ŋ", "Ŕ", "Ć", "Ś", "Ź", "Ŧ", "ð",
    "ã", "å", "æ", "œ", "ŵ", "ý", "õ", "ø",
    "þ", "ŋ", "ŕ", "ć", "ś", "ź", "ŧ", " "
};

/** Convert an RDS G0 string to UTF-8, stopping at the first NUL. */
static std::string rdsToUtf8(const char* s, int maxLen) {
    std::string out;
    for (int i = 0; i < maxLen && s[i]; ++i)
        out += kRdsG0[(unsigned char)s[i]];
    // Trim the trailing spaces RDS pads everything with.
    while (!out.empty() && (out.back() == ' ' || out.back() == '\r')) out.pop_back();
    return out;
}


// ── RDS DSP front-end ─────────────────────────────────────────────────────
static constexpr double kRdsBit = 1187.5;     // bits/sec

// Which hypothesis is currently trustworthy: synced, and with the most good blocks
// behind it. Ties go to the lowest index, which is stable rather than flapping.
int RdsDemod::bestIdx() const {
    int best = -1, bestScore = 0;
    for (int p = 0; p < NPH; ++p) {
        if (!dec_[p].synced()) continue;
        const int score = dec_[p].recentGood();
        if (score > bestScore) { bestScore = score; best = p; }
    }
    return best;
}

// Pull anything the winner now knows into the sticky aggregate. ★ KNOWN values only: a
// field that reads -1 or "" means "not received yet", never "no longer true", so letting it
// overwrite would be forgetting on the strength of an absence.
void RdsDemod::updateAggregate() {
    const RdsDecoder* d = best();
    if (!d) return;
    auto keepInt = [](int& dst, int src, int unknown) { if (src != unknown) dst = src; };
    keepInt(agg_.pty, d->pty(), -1);
    keepInt(agg_.tp,  d->tp(),  -1);
    keepInt(agg_.ta,  d->ta(),  -1);
    keepInt(agg_.ms,  d->ms(),  -1);
    keepInt(agg_.di,  d->di(),  -1);
    keepInt(agg_.ctMinutes, d->ctMinutes(), -1);
    if (d->ctMinutes() >= 0) agg_.ctOffsetHalfHours = d->ctOffsetHalfHours();
    keepInt(agg_.language, d->languageCode(), 0);
    if (d->pinHour() >= 0) {
        agg_.pinDay = d->pinDay(); agg_.pinHour = d->pinHour(); agg_.pinMinute = d->pinMinute();
    }
    auto keepStr = [](char* dst, size_t cap, const char* src) {
        if (src && *src) { std::strncpy(dst, src, cap - 1); dst[cap - 1] = '\0'; }
    };
    keepStr(agg_.ptyn, sizeof agg_.ptyn, d->ptyn());
    keepStr(agg_.rtpTitle, sizeof agg_.rtpTitle, d->rtPlusTitle());
    keepStr(agg_.rtpArtist, sizeof agg_.rtpArtist, d->rtPlusArtist());
    keepStr(agg_.longPs, sizeof agg_.longPs, d->longPs());
    // Highest count wins per bucket: a hypothesis that has been in sync longer knows more,
    // and taking the maximum can only ever move forwards.
    int tot = 0;
    for (int i = 0; i < 32; ++i) {
        if (d->groupCount(i) > agg_.groupCounts[i]) agg_.groupCounts[i] = d->groupCount(i);
        tot += agg_.groupCounts[i];
    }
    agg_.groupTotal = tot;
    // EON and ODA merge by key, so a partially-heard sister station fills in over time
    // rather than being replaced wholesale by a less complete copy.
    for (int i = 0; i < d->eonCount(); ++i) {
        const RdsDecoder::Eon& e = d->eon(i);
        RdsDecoder::Eon* slot = nullptr;
        for (int k = 0; k < aggEonN_; ++k) if (aggEon_[k].pi == e.pi) { slot = &aggEon_[k]; break; }
        if (!slot && aggEonN_ < RdsDecoder::kMaxEon) slot = &aggEon_[aggEonN_++];
        if (!slot) continue;
        slot->pi = e.pi;
        for (int c = 0; c < 8; ++c) if (e.ps[c]) slot->ps[c] = e.ps[c];
        if (e.afKhz) slot->afKhz = e.afKhz;
        if (e.ta >= 0) slot->ta = e.ta;
    }
    for (int i = 0; i < d->odaCount(); ++i) {
        const RdsDecoder::Oda& o = d->oda(i);
        bool seen = false;
        for (int k = 0; k < aggOdaN_; ++k) if (aggOda_[k].aid == o.aid) { seen = true; break; }
        if (!seen && aggOdaN_ < RdsDecoder::kMaxOda) aggOda_[aggOdaN_++] = o;
    }
}

int RdsDemod::mergedEon(RdsDecoder::Eon* out, int maxOut) {
    const int n = std::min(maxOut, aggEonN_);
    for (int i = 0; i < n; ++i) out[i] = aggEon_[i];
    return n;
}

int RdsDemod::mergedOda(RdsDecoder::Oda* out, int maxOut) {
    const int n = std::min(maxOut, aggOdaN_);
    for (int i = 0; i < n; ++i) out[i] = aggOda_[i];
    return n;
}

int RdsDemod::mergedAf(int* khzOut, int maxOut, int* seenOut) {
    // A station change invalidates the whole list — AF belongs to a PI, not to a dial spot.
    const RdsDecoder* b = best();
    const uint16_t pi = b ? b->confirmedPi() : 0;
    if (pi && pi != mergedAfPi_) {
        // ★ A new PI is a DIFFERENT STATION, and that is the only thing that invalidates any
        // of this. Sync loss does not — the station is still there and still saying the same
        // things, so forgetting on a fade would be exactly the flicker this exists to stop.
        mergedAfN_ = 0; mergedAfPi_ = pi;
        agg_ = Agg{}; aggEonN_ = 0; aggOdaN_ = 0;
    }
    updateAggregate();
    for (int p = 0; p < NPH; ++p) {
        const int n = dec_[p].afCount();
        for (int i = 0; i < n; ++i) {
            if (dec_[p].afHits(i) < RdsDecoder::kAfConfirm) continue;   // unconfirmed
            const int khz = dec_[p].afKhz(i);
            bool seen = false;
            for (int k = 0; k < mergedAfN_; ++k) if (mergedAf_[k] == khz) { seen = true; break; }
            if (!seen && mergedAfN_ < RdsDecoder::kMaxAf) mergedAf_[mergedAfN_++] = khz;
        }
    }
    // ★ How many DISTINCT frequencies have been glimpsed at all, confirmed or not — the
    // denominator of an "AF score", as the FM-DX Webserver shows it (confirmed / seen).
    // A score below 100% says entries are arriving damaged, which is a link statement.
    if (seenOut) {
        int seen = 0;
        for (int p = 0; p < NPH; ++p) seen = std::max(seen, dec_[p].afCount());
        *seenOut = std::max(seen, mergedAfN_);
    }
    const int out = std::min(maxOut, mergedAfN_);
    for (int i = 0; i < out; ++i) khzOut[i] = mergedAf_[i];
    return out;
}

const RdsDecoder* RdsDemod::best() const {
    const int b = bestIdx();
    return (b >= 0 && b < NPH) ? &dec_[b] : nullptr;
}

int RdsDemod::constellation(float* xy, int maxPts) const {
    const int n = std::min(maxPts, kConstPts);
    // Oldest first, so the plot ages consistently rather than jumping at the wrap.
    for (int i = 0; i < n; ++i) {
        const int k = (constHead_ + kConstPts - n + i) % kConstPts;
        xy[i * 2]     = constXY_[k * 2];
        xy[i * 2 + 1] = constXY_[k * 2 + 1];
    }
    return n;
}

float RdsDemod::rdsDeviationKHz() const {
    // ★★★ NO SUBCARRIER, NO NUMBER. See the guardPow_ note in vibedsp.h: this band always holds
    // something, so without a floor to subtract, noise becomes a "deviation". Observed on a
    // dead carrier as "12.9 kHz - generous" beside no lock and 0.0 groups/s (Stuart,
    // 2026-07-27) — a reading that is not merely wrong but IMPOSSIBLE, the spec ceiling being
    // 7.5% = 5.6 kHz.
    // ★★ SCOPED TO THE STATION, NOT TO THIS INSTANT. The first version gated on best() —
    // "is a hypothesis synced RIGHT NOW" — which on a marginal signal flickers many times a
    // second, so the reading blinked between a number and a dash (Stuart, 2026-07-27).
    // ★ That contradicted the rule the sticky aggregate already follows: a new PI is a
    // DIFFERENT STATION and is the only thing that invalidates any of this. Sync loss is not —
    // the station is still there, and the estimate behind this is heavily smoothed, so it
    // remains just as true through a fade as it was a second earlier.
    // ★ groupTotal is exactly the right scope: it is zero until this station has produced
    // groups and is cleared when the PI changes, so a dead carrier still reads "—" (which is
    // the bug this gate exists for) while a fading one holds its last honest value.
    if (agg_.groupTotal <= 0) return -1.0f;

    // ★ SUBTRACT THE FLOOR IN POWER. The guard band sees the same noise through the same
    // filter but no subcarrier, so what survives the subtraction is signal alone. If the
    // difference is not positive there is nothing above the noise, which is the honest answer
    // on a station this weak — better than a confident number built out of hiss.
    if (guardOn_) {
        // ★★ SUBTRACT SLOWLY. rdsPow_ and guardPow_ are each smoothed with a 0.0005 coefficient
        // at ~40 kHz — a 50 ms time constant, far faster than "smoothed" suggests. On a weak
        // signal their DIFFERENCE therefore crosses zero many times a second, and every negative
        // crossing returned -1, so the reading flashed between a value and a dash several times
        // a second (Stuart, 2026-07-27).
        // ★ A deviation is a property of the transmitter and should not flicker at 20 Hz. The
        // difference is re-smoothed over seconds, and -1 is reserved for a subcarrier that is
        // genuinely and persistently at or below the noise — not for one that dipped for 50 ms.
        const float sigPow = sigPowSlow_;
        if (sigPow <= 0.0f) return -1.0f;
        // 1.381 = peak / RMS of a spec-shaped biphase envelope through our own +/-2.4 kHz
        // filter (tools/rdsdev_cal, stable to +/-0.2% from 192 to 320 kHz). NOT the 1.520 used
        // below: that one is peak / mean-envelope, and only an RMS can have noise power taken
        // off it.
        return std::sqrt(sigPow) * 1.381f * 75.0f;
    }

    // ★ Uncorrected fallback when the operator has not enabled the guard band. Same crest
    // factor reasoning, applied to the mean envelope: 1.520, replacing a hard-coded sqrt(2)
    // that was a SINUSOID's RMS->peak factor applied to a quantity that is neither an RMS nor
    // a sinusoid, and under-read by ~7%. This path still reads high on a weak signal — the
    // client flags anything past the spec ceiling as suspect.
    return rdsRms_ * 1.520f * 75.0f;
}

// ★ Enabling costs a second decimating filter pair on the RDS front end (the rotation itself is
// a complex multiply per input sample). Configured lazily here rather than in configure(),
// because the operator can turn it on while a radio is already running.
void RdsDemod::setNoiseCorrection(bool on) {
    guardOn_ = on;
    if (!on) { guardPow_ = 0.0f; return; }
    guardPow_ = 0.0f;
}

// ★★ ROTATION IS A RATE, SO MEASURE A RATE. Sampled a few times a second and differenced
// against the previous reading, unwrapped so a pass through +/-90 does not read as a jump.
// ★ The RAW angle is used, not the folded display one: folding reflects at 90 degrees, which
// would turn a steady rotation into a triangle wave and its rate into nonsense at every turn.
void RdsDemod::measurePhaseDrift() {
    const float m = std::sqrt(phCos2_ * phCos2_ + phSin2_ * phSin2_);
    // Nothing to measure without a usable estimate — and saying "not rotating" because the
    // signal is absent would be the same confident-wrong-answer this panel exists to avoid.
    if (m < 0.05f) { phDriftDeg_ = 0.0f; phLastDeg_ = -1.0f; return; }

    // Half the doubled angle, in [0,180) — the raw estimate before the display fold.
    float deg = 0.5f * std::atan2(phSin2_, phCos2_) * 180.0f / (float)M_PI;
    while (deg < 0.0f) deg += 180.0f;
    while (deg >= 180.0f) deg -= 180.0f;

    const double dt = phClock_ - phLastAt_;
    if (dt < 0.25) return;                    // a few times a second is plenty for a drift
    if (phLastDeg_ >= 0.0f) {
        // Unwrap across the 180-degree boundary the BPSK ambiguity imposes: a step of more
        // than half a turn is really a smaller step the other way.
        float d = deg - phLastDeg_;
        while (d >  90.0f) d -= 180.0f;
        while (d < -90.0f) d += 180.0f;
        const float rate = std::fabs(d) / (float)dt;
        // ★ Smoothed hard. This is a transmitter characteristic; it should not flicker, and a
        // single noisy sample must not be able to accuse a broadcaster of anything.
        phDriftDeg_ += 0.15f * (rate - phDriftDeg_);
    }
    phLastDeg_ = deg;
    phLastAt_  = phClock_;
}

float RdsDemod::pilotPhaseCoherence() const {
    // The accumulator holds an average of UNIT vectors, so its length is the agreement
    // between them: 1 = every symbol reports the same angle, 0 = uniformly distributed.
    const float m = std::sqrt(phCos2_ * phCos2_ + phSin2_ * phSin2_);
    return m > 1.0f ? 1.0f : m;
}

float RdsDemod::pilotPhaseDeg() const {
    const float m = std::sqrt(phCos2_ * phCos2_ + phSin2_ * phSin2_);
    if (m < 1e-9f) return -1.0f;
    float deg = 0.5f * std::atan2(phSin2_, phCos2_) * 180.0f / (float)M_PI;
    // Modulo 180 into [0,180): the ambiguity is inherent to BPSK.
    while (deg < 0.0f)    deg += 180.0f;
    while (deg >= 180.0f) deg -= 180.0f;
    // ★★ THEN FOLD TO [0,90] — the quantity is an unsigned ANGULAR DISTANCE, and a
    // subcarrier at -8 degrees is 8 degrees out, not 172. Reporting the raw [0,180) value
    // made every station read as its own reflection: 8->172, 45->131, 2->174.
    // ★ Why it survived: the two cases anyone checks against are the two the bug cannot
    // touch. At 0 the reflection is 180 (still "near 0 mod 180") and at 90 the two
    // branches COINCIDE — so a correctly-phased station and a quadrature station both
    // read right, and only the faults in between were wrong. Confirmed against
    // HansVanEijsden's Pira analyser over six stations (2026-07-27): our 89 vs its 90 on
    // quadrature Oost is precisely the case that proves nothing, and was what made this
    // look correct for a month.
    if (deg > 90.0f) deg = 180.0f - deg;
    return deg;
}

float RdsDemod::subcarrierRelDb() const {
    if (rdsRms_ <= 1e-9f || pilotRef_ <= 1e-9f) return -99.0f;
    return 20.0f * std::log10(rdsRms_ / pilotRef_);
}

int RdsDemod::blockErrorPercent() const {
    const int b = bestIdx();
    return (b >= 0 && b < NPH) ? dec_[b].blockErrorPercent() : -1;
}

void RdsDemod::configure(double mpxRate, const RdsDecoder::Callbacks& cb) {
    // Gentle low-pass to isolate the RDS baseband (±~2.4 kHz) after the coherent
    // 57 kHz downconvert. After downconversion the nearest MPX content (stereo,
    // pilot, L+R) lands well above 2.4 kHz, so a wide transition is fine.
    // ★ Tried and REVERTED 2026-07-26: widening this to cutoff 3200 / transition 1600, on
    // the theory that a 2400 Hz transition was clipping the biphase spectrum (which peaks
    // near 1187 Hz) and causing ISI. The probe said otherwise — no improvement anywhere and
    // a clear loss at moderate noise — and it then measured 0% block errors at zero noise,
    // which rules out ISI in this filter as the source of any error floor at all.
    mpxRate_ = mpxRate;
    const double cut = 2400.0 / mpxRate;
    std::vector<float> taps = designLowpass(cut, cut);
    // ★ DECIMATE. The RDS baseband is +/-2.4 kHz, but everything here ran at the channel
    // rate — a ~412-tap FIR and 16 parallel timing hypotheses, all at ~300 kHz, to recover
    // 1187.5 bits per second. That is why RDS cost more than the whole rest of the WFM
    // chain put together (pi-bench on a 32-bit Pi: 101% of a core -> 190% with RDS on).
    //
    // A decimating FIR only evaluates the outputs it keeps, so this divides BOTH the filter
    // and the biphase loop by decim_ for exactly the same filter shape.
    //
    // The floor is set by symbol timing, not by bandwidth: the early/late gates sit an
    // eighth of a bit apart (842 us / 8 = 105 us) and need several samples each to mean
    // anything. Targeting 40 kHz leaves ~34 samples per bit, so ~4 per gate step.
    decim_ = std::max(1, (int)std::floor(mpxRate / 40000.0));
    lpfI_ = std::make_unique<RealFir>(taps, decim_);
    lpfQ_ = std::make_unique<RealFir>(taps, decim_);
    // ★ THE GUARD BAND: the same filter, offset +6 kHz, so it sees the same noise through the
    // same shape but no subcarrier. 6 kHz is chosen to sit in genuinely empty MPX — the RDS
    // sidebands end near 59.4 kHz and the next thing anyone transmits is an SCA around 67 kHz,
    // so 63 +/- 2.4 kHz (60.6-65.4) is clear of both. Too close and it would measure RDS and
    // subtract the signal from itself; too far and it would measure something else's noise.
    lpfGI_ = std::make_unique<RealFir>(taps, decim_);
    lpfGQ_ = std::make_unique<RealFir>(taps, decim_);
    guardStep_ = 2.0 * M_PI * 6000.0 / mpxRate;
    const double groupDelay = (taps.size() - 1) / 2.0;     // samples
    const double bitStep = 2.0 * M_PI * kRdsBit / mpxRate; // bit-phase per sample
    groupDelayPhase_ = std::fmod(groupDelay * bitStep, 2.0 * M_PI);
    // Route every hypothesis through a trampoline that knows which one it is, so only
    // the winner's PS/RadioText/ECC ever reaches the caller.
    user_ = cb;
    RdsDecoder::Callbacks route;
    for (int p = 0; p < NPH; ++p) {
        slots_[p] = { this, p };
        route.ctx = &slots_[p];
        route.ps = [](void* c, uint16_t pi, const char* ps) {
            auto* sl = (Slot*)c;
            if (sl->self->bestIdx() == sl->idx && sl->self->user_.ps)
                sl->self->user_.ps(sl->self->user_.ctx, pi, ps);
        };
        route.radiotext = [](void* c, const char* rt) {
            auto* sl = (Slot*)c;
            if (sl->self->bestIdx() == sl->idx && sl->self->user_.radiotext)
                sl->self->user_.radiotext(sl->self->user_.ctx, rt);
        };
        route.ecc = [](void* c, uint16_t pi, uint8_t e) {
            auto* sl = (Slot*)c;
            if (sl->self->bestIdx() == sl->idx && sl->self->user_.ecc)
                sl->self->user_.ecc(sl->self->user_.ctx, pi, e);
        };
        dec_[p].setCallbacks(route);
    }
    reset();
}

void RdsDemod::reset() {
    if (lpfI_) lpfI_->reset();
    if (lpfQ_) lpfQ_->reset();
    if (lpfGI_) lpfGI_->reset();
    if (lpfGQ_) lpfGQ_->reset();
    rdsPow_ = guardPow_ = sigPowSlow_ = 0.0f; guardPhase_ = 0.0;
    bphase_ = decim_;                  // must match RealFir's own starting phase
    started_ = false;
    mergedAfN_ = 0; mergedAfPi_ = 0; phCos2_ = phSin2_ = 0.0f;
    phDriftDeg_ = 0.0f; phLastDeg_ = -1.0f; phLastAt_ = 0.0; phClock_ = 0.0;
    agg_ = Agg{}; aggEonN_ = 0; aggOdaN_ = 0;
    for (int k = 0; k < NPH; ++k) {
        accI_[k] = accQ_[k] = 0.0f; prevPh_[k] = 0.0f;
        prevAI_[k] = prevAQ_[k] = 0.0f; havePrev_[k] = false;
        dec_[k].reset();
    }
}

void RdsDemod::process(const float* mpx, const float* ref57, const float* ref57q,
                       const float* bitClk, int n) {
    if (!lpfI_) return;
    // Coherent downconvert to a COMPLEX baseband. Mixing with only the in-phase 57 kHz
    // reference throws away everything in quadrature, so any phase error against the
    // station's actual subcarrier scaled the data by cos(theta) — zero at 90 degrees.
    // Keeping both parts makes the detector indifferent to that angle.
    xI_.resize(n); xQ_.resize(n);
    for (int i = 0; i < n; ++i) {
        xI_[i] = mpx[i] * ref57[i]  * 2.0f;
        xQ_[i] = mpx[i] * ref57q[i] * 2.0f;
    }
    sI_.resize(lpfI_->maxOut(n));
    sQ_.resize(lpfQ_->maxOut(n));
    const int ns = std::min(lpfI_->process(xI_.data(), n, sI_.data()),
                            lpfQ_->process(xQ_.data(), n, sQ_.data()));

    // Subsample the bit clock at EXACTLY the inputs the decimator kept, by mirroring
    // RealFir's own phase counter — same starting value, stepped over the same samples —
    // so it stays aligned with the filtered stream across block boundaries.
    bclk_.clear();
    for (int i = 0; i < n; ++i)
        if (--bphase_ == 0) { bphase_ = decim_; bclk_.push_back(bitClk[i]); }
    const int nb = std::min(ns, (int)bclk_.size());

    const float twoPi = 2.0f * (float)M_PI;
    const float kPi   = (float)M_PI;
    const float phaseStep = twoPi / NPH;

    // ★★ A HYPOTHESIS SWITCH INVALIDATES THE PLOT. Each hypothesis has its own carrier
    // phase and timing alignment, so points captured either side of a switch are two
    // DIFFERENT receivers' outputs sharing one ring — which draws as FOUR lobes, and makes
    // the de-rotation average across both and land between them. Observed on air by Stuart
    // (2026-07-26): "2 blobs, now 4 blobs".
    // ★ It is also worth noticing rather than smoothing over: frequent switching means the
    // arbitration is FLAPPING, which is the same instability that once produced two-character
    // station names. Clearing keeps the plot honest — a sparse plot on a flapping signal is
    // the truth, where four lobes is an artefact that looks like a signal property.
    {
        const int nb2 = bestIdx();
        if (nb2 != constBest_) {
            for (int i = 0; i < kConstPts * 2; ++i) constXY_[i] = 0.0f;
            constHead_ = 0;
        }
        constBest_ = nb2;
    }
    // ★ A CLOCK FROM THE SAMPLES, not from the wall. The DSP layer has no business calling
    // the system clock — it runs on a callback thread whose timing is not the signal's — and
    // sample count IS the elapsed time as far as the radio is concerned.
    phClock_ += (double)n / mpxRate_;
    measurePhaseDrift();

    // Smoothed RMS of the complex RDS baseband — the level the detector actually sees.
    // ★ Mean-square is tracked alongside the mean envelope: the envelope feeds the legacy
    // uncorrected deviation and subcarrierRelDb, the POWER feeds the noise subtraction, which
    // can only be done on a power. Same smoothing on both so they stay comparable.
    for (int i = 0; i < nb; ++i) {
        const float mag2 = sI_[i] * sI_[i] + sQ_[i] * sQ_[i];
        rdsRms_ += 0.0005f * (std::sqrt(mag2) - rdsRms_);
        rdsPow_ += 0.0005f * (mag2 - rdsPow_);
    }
    // ★ The noise-subtracted power, smoothed over SECONDS rather than milliseconds — see
    // rdsDeviationKHz(). Clamped at zero first so a momentary negative excursion pulls the
    // average down rather than latching the whole reading to "nothing".
    if (guardOn_ && nb > 0) {
        const float inst = std::max(0.0f, rdsPow_ - guardPow_);
        sigPowSlow_ += 0.02f * (inst - sigPowSlow_);
    }

    // ★★ THE GUARD BAND — only when the operator has paid for it. Rotating the ALREADY
    // downconverted complex baseband by the offset and reusing the same taps costs one complex
    // multiply per input sample plus a second decimating filter pair; building a whole second
    // downconvert from the MPX would have cost the mixer twice over for the same answer.
    if (guardOn_ && lpfGI_) {
        xGI_.resize(n); xGQ_.resize(n);
        double ph = guardPhase_;
        for (int i = 0; i < n; ++i) {
            const float c = (float)std::cos(ph), sn = (float)std::sin(ph);
            xGI_[i] =  xI_[i] * c + xQ_[i] * sn;      // rotate by -ph: selects 57 kHz + offset
            xGQ_[i] = -xI_[i] * sn + xQ_[i] * c;
            ph += guardStep_;
            if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;    // bounded, or the float cos/sin degrades
        }
        guardPhase_ = ph;
        sGI_.resize(lpfGI_->maxOut(n));
        sGQ_.resize(lpfGQ_->maxOut(n));
        const int ng = std::min(lpfGI_->process(xGI_.data(), n, sGI_.data()),
                                lpfGQ_->process(xGQ_.data(), n, sGQ_.data()));
        for (int i = 0; i < ng; ++i) {
            const float m2 = sGI_[i] * sGI_[i] + sGQ_[i] * sGQ_[i];
            guardPow_ += 0.0005f * (m2 - guardPow_);
        }
    }

    for (int i = 0; i < nb; ++i) {
        const float sI = sI_[i], sQ = sQ_[i];
        float base = bclk_[i] - (float)groupDelayPhase_;
        // Wrap ONCE per sample, not once per hypothesis: bitClk arrives in [0, 2*pi) by
        // construction, so after this `base` is in range and subtracting p*phaseStep
        // (itself < 2*pi) can only push it below zero once. No libm in the inner loop.
        base = std::fmod(base, twoPi);
        if (base < 0.0f) base += twoPi;

        for (int p = 0; p < NPH; ++p) {
            float ph = base - p * phaseStep;
            if (ph < 0.0f) ph += twoPi;
            // Biphase matched integration: +1 over the first half-bit, -1 over the second.
            const float sign = (ph < kPi) ? 1.0f : -1.0f;
            accI_[p] += sI * sign;
            accQ_[p] += sQ * sign;
            if (started_ && ph < prevPh_[p]) {          // bit-clock wrap = symbol boundary
                const float aI = accI_[p], aQ = accQ_[p];
                // Differential (DBPSK) decision. The carrier phase cancels in the product
                // with the previous symbol, so no phase estimate is needed and multipath
                // rotation cannot invert or null us. RDS is differentially encoded already,
                // so this hands the decoder the data bit directly.
                if (havePrev_[p]) {
                    const float dot = aI * prevAI_[p] + aQ * prevAQ_[p];
                    dec_[p].pushBit(dot < 0.0f ? 1 : 0);
                }
                if (p == constBest_) {
                    // Accumulate the doubled angle, magnitude-weighted so strong symbols
                    // define the estimate and noise near the origin barely counts.
                    const float mag2 = aI * aI + aQ * aQ;
                    if (mag2 > 1e-12f) {
                        const float c = (aI * aI - aQ * aQ) / mag2;   // cos(2*theta)
                        const float s = (2.0f * aI * aQ) / mag2;      // sin(2*theta)
                        phCos2_ += 0.002f * (c - phCos2_);
                        phSin2_ += 0.002f * (s - phSin2_);
                    }
                    // Normalised by the running baseband RMS so the plot's SCALE is stable
                    // and only its SHAPE varies — which is the part that carries meaning.
                    // ★ Divide generously. The symbol accumulator sums ~34 samples, so its
                    // magnitude is many times the per-sample RMS — and the wire format is a
                    // SIGNED BYTE, so anything over 127 clips and the shape is destroyed
                    // rather than merely rescaled. A strong station overflowed and the plot
                    // emptied (Stuart, 2026-07-26). The client rescales to fit, so headroom
                    // here costs nothing and clipping cannot be undone.
                    const float k = (rdsRms_ > 1e-9f) ? 1.0f / (rdsRms_ * 96.0f) : 0.0f;
                    constXY_[constHead_ * 2]     = aI * k;
                    constXY_[constHead_ * 2 + 1] = aQ * k;
                    constHead_ = (constHead_ + 1) % kConstPts;
                }
                prevAI_[p] = aI; prevAQ_[p] = aQ; havePrev_[p] = true;
                accI_[p] = 0.0f; accQ_[p] = 0.0f;
            }
            prevPh_[p] = ph;
        }

        started_ = true;
    }
}

// Generator g(x) = x^10+x^8+x^7+x^5+x^4+x^3+1 = 0x5B9. Offset words A,B,C,C',D.
// For a valid block the syndrome (block mod g) equals the offset word value.
static constexpr uint32_t kGen = 0x5B9;
const uint16_t RdsDecoder::OFFSET[5] = { 0x0FC, 0x198, 0x168, 0x350, 0x1B4 };

uint16_t RdsDecoder::syndrome(uint32_t b) {
    uint32_t r = b & 0x3FFFFFF;                 // 26-bit codeword
    for (int i = 25; i >= 10; --i)
        if ((r >> i) & 1) r ^= (kGen << (i - 10));
    return (uint16_t)(r & 0x3FF);
}

uint16_t RdsDecoder::checkword(uint16_t data) {
    uint32_t r = (uint32_t)data << 10;
    for (int i = 25; i >= 10; --i)
        if ((r >> i) & 1) r ^= (kGen << (i - 10));
    return (uint16_t)(r & 0x3FF);
}

// ── Burst-error correction table ─────────────────────────────────────────────
// The checkword is a shortened cyclic code, and syndromes are LINEAR:
// syndrome(codeword ^ error) == syndrome(codeword) ^ syndrome(error). So the
// syndrome of a corrupted block, XORed with the offset word it should have carried,
// IS the syndrome of the error alone — and if we have tabulated that syndrome we know
// exactly which bits flipped and can put them back.
//
// ★★ NOW TABULATED TO 5 BITS, which is what the code can actually correct (EN 50067 /
// IEC 62106). It was capped at 2 — the conservative choice, and the file said so — on the
// reasoning that every added pattern makes a FALSE correction on noise more likely, and a
// wrongly "repaired" block is worse than a discarded one because it reaches the parser
// looking valid.
//
// ★ That reasoning was sound and the conclusion no longer follows, because the thing it
// protected has changed: the parser now receives the REPAIR WIDTH, not a bare "ok", and
// weighs a block by how much rebuilding it took. A wide repair no longer arrives
// indistinguishable from a clean read, so admitting wide repairs costs confidence rather
// than correctness — and confidence is recoverable by repetition, which RDS supplies for
// free ~11 times a second. The cap is also a runtime lever now (setMaxBurstBits), so a DX
// user can trade the other way. Measure with the probe, not by eye: the whole point of
// widening is a threshold shift, and only the probe can see one.
//
// A burst of length L is any pattern whose first and last bits are set, so patterns are
// enumerated by length and narrower ones always win a syndrome collision.
static int g_maxBurstBits = RdsDecoder::kDefaultBurst;

void RdsDecoder::setMaxBurstBits(int bits) {
    if (bits < 1) bits = 1;
    if (bits > 5) bits = 5;
    g_maxBurstBits = bits;
}

const uint32_t* RdsDecoder::errorTable() {
    static uint32_t table[1024] = {0};
    static int builtFor = -1;
    if (builtFor != g_maxBurstBits) {
        for (int i = 0; i < 1024; ++i) table[i] = 0;
        // Narrow before wide: prefer the SMALLEST error that explains a syndrome, so a
        // 1-bit slip is never "explained" by an invented 5-bit burst.
        for (int len = 1; len <= g_maxBurstBits; ++len) {
            // Interior bits are free; the ends are forced, which is what makes it a
            // burst of exactly this length.
            const uint32_t ends = (len == 1) ? 1u : (1u | (1u << (len - 1)));
            const int interior = (len <= 2) ? 0 : (len - 2);
            for (uint32_t mid = 0; mid < (1u << interior); ++mid) {
                const uint32_t pattern = ends | (mid << 1);
                for (int shift = 0; shift + len <= 26; ++shift) {
                    const uint32_t err = (pattern << shift) & 0x3FFFFFF;
                    if (!err) continue;
                    const uint16_t syn = syndrome(err);
                    if (syn && !table[syn]) table[syn] = err;
                }
            }
        }
        builtFor = g_maxBurstBits;
    }
    return table;
}

// Where each offset word sits in the group's cyclic order: A, B, C or C', D.
int RdsDecoder::seqOfOffset(uint16_t offsetIdx) {
    switch (offsetIdx) {
        case 0:  return 0;    // A
        case 1:  return 1;    // B
        case 2:  return 2;    // C
        case 3:  return 2;    // C' (same slot, different offset word)
        default: return 3;    // D
    }
}

// Verify `block26` against the offset word it should carry, repairing a correctable
// burst in place. Returns whether the block is trustworthy afterwards.
bool RdsDecoder::tryCorrect(uint16_t offsetIdx, uint32_t& block26, int& repairBits) const {
    repairBits = 0;
    const uint16_t errSyn = syndrome(block26) ^ OFFSET[offsetIdx];
    if (errSyn == 0) return true;                       // clean already
    const uint32_t err = errorTable()[errSyn];
    if (!err) return false;                             // not a pattern we can undo
    block26 ^= err;
    for (uint32_t e = err; e; e >>= 1) repairBits += (int)(e & 1);
    return true;
}

// Record a candidate block boundary and decide whether enough of them agree.
// A genuine stream puts boundaries exactly 26 bits apart, with the offset words
// advancing one step round A,B,C,D per boundary — so a pulse only supports another if
// their bit-distance is a whole number of blocks AND the offsets differ by that same
// number of steps. Noise throws syndrome matches at random positions, which almost
// never line up on the grid twice.
void RdsDecoder::notePulse(int seq) {
    const SyncPulse now{ (uint8_t)seq, bitPos_ };
    int agree = 0;
    for (int i = 0; i < pulseCount_ && i < kSyncPulses; ++i) {
        const uint64_t delta = now.bitPos - pulses_[i].bitPos;
        if (delta == 0 || delta % 26 != 0) continue;
        const uint64_t steps = delta / 26;
        if (((pulses_[i].seq + steps) & 3) == (uint64_t)now.seq) ++agree;
    }
    // Ring, oldest overwritten.
    pulses_[pulseCount_ % kSyncPulses] = now;
    ++pulseCount_;

    if (agree + 1 < kSyncNeeded) return;

    // Locked. This pulse IS a block boundary, so the next block starts now.
    synced_ = true;
    badHist_ = 0; blocksSeen_ = 0;
    for (int i = 0; i < 4; ++i) blkOk_[i] = false;   // nothing from this group yet
    nextBlk_ = (seq + 1) & 3;
    bitsLeft_ = 26;
}

int RdsDecoder::recentGood() const {
    if (blocksSeen_ <= 0) return 0;
    const int n = (blocksSeen_ < kRateWindow) ? blocksSeen_ : kRateWindow;
    int bad = 0;
    uint64_t h = badHist_ & ((1ull << kRateWindow) - 1);
    for (int i = 0; i < n && h; ++i, h >>= 1) bad += (int)(h & 1);
    return n - bad;
}

int RdsDecoder::blockErrorPercent() const {
    if (errSeen_ < kBerBlocks) return -1;      // not yet a full window — say so, don't guess
    int bad = 0;
    for (uint64_t h = errHist_ & ((1ull << kBerBlocks) - 1); h; h >>= 1)
        bad += (int)(h & 1);
    return (bad * 100) / kBerBlocks;
}

void RdsDecoder::reset() {
    reg_ = 0; synced_ = false; bitsLeft_ = 0; nextBlk_ = 0;
    pulseCount_ = 0; bitPos_ = 0; badHist_ = 0; blocksSeen_ = 0;
    for (int i = 0; i < 4; ++i) { blk_[i] = 0; blkOk_[i] = false; }
    std::memset(ps_, 0, sizeof ps_);
    std::memset(rt_, 0, sizeof rt_);
    ecc_ = 0;
    piLast_ = 0; piSeen_ = false; grpRepairBits_ = 0; piConfirmedVal_ = 0;
    pty_ = tp_ = ta_ = ms_ = -1; afN_ = 0;
    ptyCand_ = tpCand_ = -1; ptySeen_ = false; trustedB_ = false;
    for (int i = 0; i < kMaxAf; ++i) afHits_[i] = 0;
    di_ = 0; diSeen_ = 0; ctMin_ = -1; ctOff_ = 0;
    ctOffCand_ = 0; ctOffSeen_ = false;
    taCand_ = msCand_ = -1;
    diCandBit_[0] = diCandBit_[1] = diCandBit_[2] = diCandBit_[3] = -1;
    rtpGroup_ = -1; lpsSeen_ = 0; rtAb_ = 0; rtAbSeen_ = false;
    ptynSeen_ = 0; lang_ = 0; pinHour_ = -1; eonN_ = 0; odaN_ = 0;
    for (int i = 0; i < kMaxOda; ++i) oda_[i] = Oda{};
    std::memset(ptyn_, 0, sizeof ptyn_);
    for (int i = 0; i < kMaxEon; ++i) eon_[i] = Eon{};
    std::memset(rtpTitle_, 0, sizeof rtpTitle_);
    std::memset(rtpArtist_, 0, sizeof rtpArtist_);
    std::memset(longPs_, 0, sizeof longPs_);
    for (int i = 0; i < 32; ++i) grpCount_[i] = 0;
    grpTotal_ = 0;
    for (int i = 0; i < 4; ++i) blkRepair_[i] = 0;
    errHist_ = 0; errSeen_ = 0;
    eccCand_ = 0; eccSeen_ = false;
    for (int i = 0; i < 4; ++i)  { psCand_[i] = 0; psSeen_[i] = false; }
    for (int i = 0; i < 16; ++i) { rtCand_[i] = 0; rtSeen_[i] = false; }
}

void RdsDecoder::pushBit(int bit) {
    reg_ = ((reg_ << 1) | (bit & 1)) & 0x3FFFFFF;
    ++bitPos_;

    if (!synced_) {
        // ANY offset word is a candidate boundary, not just A — four times as many
        // chances to acquire per group. notePulse() decides whether enough of them
        // agree on the 26-bit grid to be believed.
        const uint16_t syn = syndrome(reg_);
        for (int k = 0; k < 5; ++k)
            if (syn == OFFSET[k]) { notePulse(seqOfOffset((uint16_t)k)); break; }
        return;
    }

    if (--bitsLeft_ > 0) return;
    bitsLeft_ = 26;

    // Verify (and where possible repair) against the offset word this slot should
    // carry. Slot C is allowed to be either C or C', so try both and keep the one
    // that verifies.
    static const uint16_t kSlotOffset[4] = { 0, 1, 2, 4 };
    if (nextBlk_ == 0) grpRepairBits_ = 0;        // a fresh group starts trusted
    uint32_t block = reg_;
    bool ok = false;
    int repaired = 0;
    // ★ BER is measured BEFORE any repair, so it describes the LINK and not our effort.
    // A decoder that quietly fixes everything would otherwise report a perfect link right
    // up to the moment it falls over.
    const bool preErr = (syndrome(reg_) != OFFSET[kSlotOffset[nextBlk_]])
                     && (nextBlk_ != 2 || syndrome(reg_) != OFFSET[3]);
    if (nextBlk_ == 2) {
        // Slot C may legitimately carry either C or C'. Prefer whichever verifies, and
        // prefer a CLEAN match over a repaired one.
        uint32_t c = reg_, cp = reg_;
        int rc = 0, rcp = 0;
        const bool okC  = tryCorrect(2, c,  rc);
        const bool okCp = tryCorrect(3, cp, rcp);
        if (okC && !rc)        { block = c;  ok = true; repaired = 0; }
        else if (okCp && !rcp) { block = cp; ok = true; repaired = 0; }
        // Both repairable: believe the NARROWER repair. Preferring C by position would
        // pick an invented 5-bit burst over a 1-bit slip in C'.
        else if (okC && okCp)  { if (rc <= rcp) { block = c;  ok = true; repaired = rc;  }
                                 else           { block = cp; ok = true; repaired = rcp; } }
        else if (okC)          { block = c;  ok = true; repaired = rc;  }
        else if (okCp)         { block = cp; ok = true; repaired = rcp; }
    } else {
        uint32_t b = reg_;
        if (tryCorrect(kSlotOffset[nextBlk_], b, repaired)) { block = b; ok = true; }
    }
    // An unrecoverable block counts as maximally damaged, so a group containing one is
    // weighed down rather than merely flagged.
    grpRepairBits_ += ok ? repaired : 8;
    blk_[nextBlk_] = (uint16_t)((block >> 10) & 0xFFFF);
    blkOk_[nextBlk_] = ok;
    blkRepair_[nextBlk_] = ok ? repaired : 8;
    errHist_ = (errHist_ << 1) | (preErr ? 1u : 0u);
    if (errSeen_ < kBerBlocks) ++errSeen_;

    // Drop sync on a sustained error RATE, not a short run of bad blocks. Four
    // consecutive failures used to cost the lock, which a single deep fade can cause —
    // and re-acquiring discards everything in flight, so the station name vanishes from
    // the screen for a signal that never really went away. Riding the fade out is
    // strictly better as long as acquisition itself is hard to fool, which the rhythm
    // check above now makes it.
    badHist_ = (badHist_ << 1) | (ok ? 0u : 1u);
    if (blocksSeen_ < kRateWindow) ++blocksSeen_;
    if (blocksSeen_ >= kRateWindow) {
        int bad = 0;
        for (uint64_t h = badHist_ & ((1ull << kRateWindow) - 1); h; h >>= 1)
            bad += (int)(h & 1);
        if (bad >= kRateDrop) {
            synced_ = false; nextBlk_ = 0; pulseCount_ = 0;
            return;
        }
    }

    if (++nextBlk_ == 4) { parseGroup(); nextBlk_ = 0; }
}

// RT+ content types. 1 = title, 4 = artist; the rest are album, track, station info and
// so on, which nothing here needs yet.
// ★ 0x0D ends a RadioText message. A station with something short to say sends it followed
// by a carriage return rather than padding to 64 characters, so honouring it is the other
// half of not showing stale text — the A/B flag handles a REPLACEMENT, this handles a
// message that was simply shorter than the buffer.
void RdsDecoder::endRadioTextAtCr() {
    for (int i = 0; i < 64; ++i) {
        if (rt_[i] == '\r') { std::memset(rt_ + i, 0, (size_t)(65 - i)); return; }
    }
}

// EON keeps one record per other-network PI. A station cross-references a handful of
// sisters, so a small fixed table is right — and a fixed table cannot be made to grow
// without bound by a mis-corrected PI, which an unbounded map could.
RdsDecoder::Eon* RdsDecoder::eonFor(uint16_t pi) {
    for (int i = 0; i < eonN_; ++i) if (eon_[i].pi == pi) return &eon_[i];
    if (eonN_ >= kMaxEon) return nullptr;
    Eon& e = eon_[eonN_++];
    e = Eon{}; e.pi = pi; e.afKhz = 0; e.ta = -1;
    return &e;
}

void RdsDecoder::applyRtPlus(int type, int start, int len) {
    if (type != 1 && type != 4) return;                 // only title and artist
    if (start < 0 || len <= 0 || start + len > 64) return;
    // The pointers index the RadioText, so a tag arriving before its text would copy
    // whatever happened to be there — refuse rather than publish a fragment.
    for (int i = 0; i < len; ++i) if (rt_[start + i] == '\0') return;
    char* dst = (type == 1) ? rtpTitle_ : rtpArtist_;
    int n = len; if (n > 64) n = 64;
    std::memcpy(dst, rt_ + start, (size_t)n);
    dst[n] = '\0';
    // Trim the padding RadioText is full of.
    for (int i = n - 1; i >= 0 && (dst[i] == ' ' || dst[i] == '\r'); --i) dst[i] = '\0';
}

// ★★ TA / MS / DI — the rest of block B, held to the SAME standard as PTY and TP. A clean
// block B is trusted outright; a repaired one must agree with the previous reception before it
// is allowed to change anything.
//
// ★ DI is the one that really needed it. It is FOUR SINGLE BITS, one per group, each indexed by
// the segment address — so a single mis-corrected block flips one flag, and the sticky
// aggregate then keeps it for the whole session with nothing to contradict it.
// ★★ THE PROMPT WAS A STATION AT 87% BLOCK ERRORS READING "Artificial head · Compressed"
// (2026-07-27) — but do NOT record that as a confirmed false positive. Stuart points out it is
// a local station with genuinely poor audio (ground hum has been heard on air), so
// "Compressed" may well be true and the flags may be perfectly correct. What was wrong was the
// STANDARD OF EVIDENCE: a single sighting, unconfirmable, indistinguishable from corruption.
// The fix is justified by that, not by the reading having been proven wrong.
// Candidates are tracked PER ADDRESS, because
// consecutive groups carry DIFFERENT DI bits and comparing a bit against the previous group's
// would compare two unrelated flags and confirm nothing.
void RdsDecoder::acceptBlockBFlags(int addr) {
    const int ta  = (blk_[1] >> 4) & 1;
    const int ms  = (blk_[1] >> 3) & 1;
    const int dib = (blk_[1] >> 2) & 1;
    taRaw_ = ta; msRaw_ = ms;                          // what arrived, confirmed or not
    if (trustedB_ || (taCand_ >= 0 && ta == taCand_)) ta_ = ta;
    if (trustedB_ || (msCand_ >= 0 && ms == msCand_)) ms_ = ms;
    taCand_ = ta; msCand_ = ms;

    // ★★ REVERSED. The four DI bits are numbered d3..d0 and are transmitted in SEGMENT ADDRESS
    // order 0,1,2,3 — so address 0 carries d3, not d0. Indexing them straight by address
    // mirrors every flag: Heart, plainly in stereo, reported Mono because we were reading its
    // dynamic-PTY bit and calling it stereo (Stuart, on air 2026-07-26).
    const int b = 3 - addr;
    if (addr >= 0 && addr < 4) {
        if (dib) diRaw_ |= (1 << b); else diRaw_ &= ~(1 << b);
        diSeenRaw_ |= (1 << b);
        if (trustedB_ || (diCandBit_[addr] >= 0 && dib == diCandBit_[addr])) {
            if (dib) di_ |= (1 << b); else di_ &= ~(1 << b);
            diSeen_ |= (1 << b);   // ★ only once ACCEPTED, so a rejected bit stays "not seen"
        }
        diCandBit_[addr] = dib;
    }
}

void RdsDecoder::parseGroup() {
    // ★★ BLOCK B IS THE ONLY HARD REQUIREMENT. It carries the group type and the segment
    // ADDRESS, so without it there is nowhere to put anything — but block A carries only
    // PI, and once a PI has been CONFIRMED it is already known. Requiring a clean A threw
    // away entire groups whose payload was perfectly good, for want of a field we could
    // already supply from memory. On a weak signal that is a large fraction of everything
    // received, because errors are spread across blocks at random: demanding two specific
    // blocks be clean is roughly squaring the per-block failure rate (2026-07-26).
    if (!blkOk_[1]) return;
    const bool haveA = blkOk_[0];
    if (!haveA && !piConfirmedVal_) return;    // no identity, from the air or from memory
    const uint16_t pi = haveA ? blk_[0] : piConfirmedVal_;
    const int gtype = (blk_[1] >> 12) & 0xF;
    const int ver   = (blk_[1] >> 11) & 1;
    // ★★ CONFIRMED BY REPETITION, like everything else here. PTY and TP ride in block B of
    // EVERY group, and were accepted unconditionally — so on a weak signal ONE mis-corrected
    // block B set a wrong programme type and it STUCK, because the sticky aggregate cannot
    // tell a good reading from a bad one (Stuart, on a station at 85% block errors,
    // 2026-07-27: "the programme type is wrong but otherwise its all there").
    // ★ They arrive ~11 times a second, so demanding two agreeing readings costs under
    // 200 ms and removes the whole class of one-bad-group poisoning. A clean group is
    // trusted at once; only a repaired one has to prove itself.
    {
        const int pty = (blk_[1] >> 5) & 0x1F;
        const int tp  = (blk_[1] >> 10) & 1;
        ptyRaw_ = pty; tpRaw_ = tp;
        if (trustedB_ || (ptySeen_ && pty == ptyCand_)) pty_ = pty;
        if (trustedB_ || (ptySeen_ && tp  == tpCand_))  tp_  = tp;
        ptyCand_ = pty; tpCand_ = tp; ptySeen_ = true;
    }
    const int gidx = gtype * 2 + ver;
    if (gidx >= 0 && gidx < 32) { ++grpCount_[gidx]; ++grpTotal_; }

    // PI is carried by EVERY group, so a PI that disagrees with the previous one is
    // either a genuine station change or a mis-correction. Either way it is not yet
    // trustworthy — wait for it to repeat before acting on anything in this group.
    // A group with no repaired blocks is trusted outright; one that needed correction
    // must agree with the previous reception before it is allowed to change anything.
    const bool trusted = (grpRepairBits_ == 0);
    // Block B specifically — PTY and TP live there, so their trust depends on B alone
    // rather than on whether some other block in the group needed repair.
    trustedB_ = (blkRepair_[1] == 0);
    const bool piConfirmed = (piSeen_ && pi == piLast_) || (!haveA && pi == piConfirmedVal_);
    if (haveA) { piLast_ = pi; piSeen_ = true; }
    if (!trusted && !piConfirmed) return;

    // ★ PI FIRST — reported the moment it is confirmed, without waiting for a name.
    // Two agreeing receptions is a strong test for a 16-bit field that repeats ~11 times
    // a second, and it is the identity everything else (database lookup, learned
    // stations, the FM-DX dial) is keyed on. A DXer who can see C06F has identified the
    // station even if the name never assembles.
    if (piConfirmed && pi != piConfirmedVal_) {
        piConfirmedVal_ = pi;
        if (cb_.pi) cb_.pi(cb_.ctx, pi);
    }

    if (gtype == 0) {                                  // 0A/0B — programme service name
        const int addr = blk_[1] & 0x3;
        acceptBlockBFlags(addr);                       // TA / MS / DI, confirmed like PTY
        // ★ AF — 0A only, block C, two codes per group. 1..204 map to 87.5 + n/10 MHz;
        // 224+ are counts and filler, not frequencies. De-duplicated, because the list
        // repeats endlessly and a DXer wants the SET, not the stream.
        if (ver == 0 && blkOk_[2]) {
            const int codes[2] = { (blk_[2] >> 8) & 0xFF, blk_[2] & 0xFF };
            for (int ci = 0; ci < 2; ++ci) {
                const int c = codes[ci];
                if (c < 1 || c > 204) continue;
                const int khz = 87500 + c * 100;
                bool seen = false;
                for (int k = 0; k < afN_; ++k)
                    if (afKhz_[k] == khz) { ++afHits_[k]; seen = true; break; }
                if (!seen && afN_ < kMaxAf) { afKhz_[afN_] = khz; afHits_[afN_] = 1; ++afN_; }
            }
        }
        if (blkOk_[3]) {
            if (trusted || (psSeen_[addr] && psCand_[addr] == blk_[3])) {
                ps_[addr * 2]     = (char)((blk_[3] >> 8) & 0xFF);
                ps_[addr * 2 + 1] = (char)(blk_[3] & 0xFF);
                if (cb_.ps) { psU8_ = rdsToUtf8(ps_, 8); cb_.ps(cb_.ctx, pi, psU8_.c_str()); }
            } else {
                psCand_[addr] = blk_[3]; psSeen_[addr] = true;
            }
        }
    } else if (gtype == 3 && ver == 0) {               // 3A — ODA announcement
        // ★ RT+ does not have a fixed group of its own: 3A declares WHICH group carries it
        // (block D is the Application ID, 0x4BD7 for RT+; block B's low 5 bits are the group
        // type and version that will carry the tags). So RT+ is unparseable until this
        // arrives, which is why it can appear a while after RadioText does.
        if (blkOk_[3]) {
            const uint16_t aid = blk_[3];
            const uint8_t grp = (uint8_t)(blk_[1] & 0x1F);
            if (aid == 0x4BD7) rtpGroup_ = grp;       // RT+
            bool seen = false;
            for (int i = 0; i < odaN_; ++i)
                if (oda_[i].aid == aid) { oda_[i].group = grp; seen = true; break; }
            if (!seen && odaN_ < kMaxOda) { oda_[odaN_].aid = aid; oda_[odaN_].group = grp; ++odaN_; }
        }
    } else if (rtpGroup_ >= 0 && (gtype * 2 + ver) == rtpGroup_) {
        // RT+ tags: two (content type, start, length) triplets pointing INTO the RadioText
        // we have already assembled. ★ Length is stored as length-1, and a tag is only
        // meaningful once the text it points at has actually been received — so both are
        // range-checked against rt_ rather than trusted.
        if (blkOk_[2] && blkOk_[3]) {
            const int t1  = ((blk_[1] & 0x7) << 3) | ((blk_[2] >> 13) & 0x7);
            const int s1  = (blk_[2] >> 7) & 0x3F;
            const int l1  = ((blk_[2] >> 1) & 0x3F) + 1;
            const int t2  = ((blk_[2] & 0x1) << 5) | ((blk_[3] >> 11) & 0x1F);
            const int s2  = (blk_[3] >> 5) & 0x3F;
            const int l2  = (blk_[3] & 0x1F) + 1;
            applyRtPlus(t1, s1, l1);
            applyRtPlus(t2, s2, l2);
        }
    } else if (gtype == 10 && ver == 0) {              // 10A — PTYN
        // The station's own name for its programme type, 8 chars in two 4-char halves.
        const int seg = blk_[1] & 0x1;
        if (blkOk_[2] && blkOk_[3]) {
            ptyn_[seg * 4 + 0] = (char)((blk_[2] >> 8) & 0xFF);
            ptyn_[seg * 4 + 1] = (char)(blk_[2] & 0xFF);
            ptyn_[seg * 4 + 2] = (char)((blk_[3] >> 8) & 0xFF);
            ptyn_[seg * 4 + 3] = (char)(blk_[3] & 0xFF);
            ptynSeen_ |= (1 << seg);
        }
    } else if (gtype == 14) {                          // 14A/14B — EON, other networks
        // ★ Block D is ALWAYS the other network's PI — that is what makes the record
        // identifiable — and block C's meaning depends on the variant in block B.
        if (blkOk_[3]) {
            Eon* e = eonFor(blk_[3]);
            if (e) {
                e->ta = (blk_[1] >> 4) & 1;            // TA flag for THAT network
                const int variant = blk_[1] & 0xF;
                if (ver == 0 && blkOk_[2]) {
                    if (variant <= 3) {                 // PS name, 2 chars per variant
                        e->ps[variant * 2 + 0] = (char)((blk_[2] >> 8) & 0xFF);
                        e->ps[variant * 2 + 1] = (char)(blk_[2] & 0xFF);
                    } else if (variant == 4) {          // AF for the other network
                        const int c = (blk_[2] >> 8) & 0xFF;
                        if (c >= 1 && c <= 204) e->afKhz = 87500 + c * 100;
                    }
                }
            }
        }
    } else if (gtype == 15 && ver == 1) {              // 15B — fast basic tuning
        // Carries the same TA/MS/DI and PS segment as 0B, for quicker acquisition. Feeding
        // it through the same path means a station that leans on 15B is not slower for us.
        const int addr = blk_[1] & 0x3;
        acceptBlockBFlags(addr);
        if (blkOk_[3]) {
            ps_[addr * 2]     = (char)((blk_[3] >> 8) & 0xFF);
            ps_[addr * 2 + 1] = (char)(blk_[3] & 0xFF);
            if (cb_.ps) { psU8_ = rdsToUtf8(ps_, 8); cb_.ps(cb_.ctx, pi, psU8_.c_str()); }
        }
    } else if (gtype == 15 && ver == 0) {              // 15A — Long PS (32 UTF-8 bytes)
        const int seg = blk_[1] & 0x7;
        if (blkOk_[2] && blkOk_[3]) {
            longPs_[seg * 4 + 0] = (char)((blk_[2] >> 8) & 0xFF);
            longPs_[seg * 4 + 1] = (char)(blk_[2] & 0xFF);
            longPs_[seg * 4 + 2] = (char)((blk_[3] >> 8) & 0xFF);
            longPs_[seg * 4 + 3] = (char)(blk_[3] & 0xFF);
            lpsSeen_ |= (1 << seg);
        }
    } else if (gtype == 4 && ver == 0) {               // 4A — clock time and date
        if (blkOk_[2] && blkOk_[3]) {
            const int hour = ((blk_[2] & 0x1) << 4) | ((blk_[3] >> 12) & 0xF);
            const int min  = (blk_[3] >> 6) & 0x3F;
            const int sign = (blk_[3] >> 5) & 0x1;
            const int off  = blk_[3] & 0x1F;
            if (hour < 24 && min < 60) {
                // The TIME is single-shot by nature: every CT is different, so there is
                // nothing to compare it against and no repetition can confirm it.
                ctMin_ = hour * 60 + min;
                // ★★ BUT THE OFFSET IS A CONSTANT, so it CAN be confirmed — and it was being
                // taken on one sighting, which is how a station reported UTC+3 and then
                // UTC-4.5 minutes apart on a signal at 41% block errors (Stuart, 2026-07-27).
                // ★ Same rule as PTY/TP/TA/MS/DI: a clean block is trusted at once, a repaired
                // one must agree with the previous reception before it is allowed to change
                // anything. It rides in block D, so trust follows block D alone.
                // ★ Note this does NOT rescue the time — a wrong offset and a wrong time come
                // from the same corrupt block, and only one of them is checkable. The panel
                // should keep saying "damaged" when the group did not arrive clean.
                const int offVal = sign ? -off : off;
                const bool trustedD = (blkRepair_[3] == 0);
                if (trustedD || (ctOffSeen_ && offVal == ctOffCand_)) ctOff_ = offVal;
                ctOffCand_ = offVal; ctOffSeen_ = true;
            }
        }
    } else if (gtype == 2) {                            // 2A/2B — RadioText
        const int addr = blk_[1] & 0xF;
        // ★★ THE TEXT A/B FLAG. RadioText is assembled into a 64-character buffer, so a new
        // message SHORTER than the last leaves the old tail in place — "Now on Heart: Spin
        // Doctors with Two Princes" followed by "La La La Long)" from the song before
        // (Stuart, on air 2026-07-26). The standard's answer is this flag: when it toggles,
        // the receiver MUST clear the buffer before assembling what follows.
        // ★ The pending candidates go too. A segment held from the previous message would
        // otherwise be "confirmed" against the new one and write a fragment of the old text
        // into the new — stale data laundered into looking verified.
        const int ab = (blk_[1] >> 4) & 1;
        if (rtAbSeen_ && ab != rtAb_) {
            std::memset(rt_, 0, sizeof rt_);
            for (int i = 0; i < 16; ++i) { rtCand_[i] = 0; rtSeen_[i] = false; }
            std::memset(rtpTitle_, 0, sizeof rtpTitle_);     // RT+ points INTO the old text
            std::memset(rtpArtist_, 0, sizeof rtpArtist_);
        }
        rtAb_ = ab; rtAbSeen_ = true;
        if (ver == 0 && blkOk_[2] && blkOk_[3]) {       // 2A: 4 chars (C,D)
            const uint32_t seg = ((uint32_t)blk_[2] << 16) | blk_[3];
            if (trusted || (rtSeen_[addr] && rtCand_[addr] == seg)) {
                rt_[addr * 4 + 0] = (char)((blk_[2] >> 8) & 0xFF);
                rt_[addr * 4 + 1] = (char)(blk_[2] & 0xFF);
                rt_[addr * 4 + 2] = (char)((blk_[3] >> 8) & 0xFF);
                rt_[addr * 4 + 3] = (char)(blk_[3] & 0xFF);
                endRadioTextAtCr();
                if (cb_.radiotext) { rtU8_ = rdsToUtf8(rt_, 64); cb_.radiotext(cb_.ctx, rtU8_.c_str()); }
            } else {
                rtCand_[addr] = seg; rtSeen_[addr] = true;
            }
        } else if (ver == 1 && blkOk_[3]) {             // 2B: 2 chars (D)
            const uint32_t seg = blk_[3];
            if (trusted || (rtSeen_[addr] && rtCand_[addr] == seg)) {
                rt_[addr * 2 + 0] = (char)((blk_[3] >> 8) & 0xFF);
                rt_[addr * 2 + 1] = (char)(blk_[3] & 0xFF);
                endRadioTextAtCr();
                if (cb_.radiotext) { rtU8_ = rdsToUtf8(rt_, 64); cb_.radiotext(cb_.ctx, rtU8_.c_str()); }
            } else {
                rtCand_[addr] = seg; rtSeen_[addr] = true;
            }
        }
    } else if (gtype == 1 && ver == 0) {                // 1A — slow labelling → ECC
        // Block C variant 0 (bits 14-12 == 0) carries the Extended Country Code
        // in its low byte. Combined with the PI country nibble it identifies the
        // station's country (RDS/IEC 62106).
        // ★ PIN — programme item number, in block D of every 1A: the scheduled start time
        // of the current programme, which is how a receiver recognises a programme rather
        // than a station.
        if (blkOk_[3]) {
            const int d = (blk_[3] >> 11) & 0x1F, h = (blk_[3] >> 6) & 0x1F, m = blk_[3] & 0x3F;
            if (d >= 1 && d <= 31 && h < 24 && m < 60) { pinDay_ = d; pinHour_ = h; pinMin_ = m; }
        }
        // Slow labelling variant 3 carries the LANGUAGE code — which station is in which
        // language is a first-order question for a DXer chasing foreign catches.
        if (blkOk_[2] && ((blk_[2] >> 12) & 0x7) == 3) lang_ = blk_[2] & 0xFF;
        if (blkOk_[2] && ((blk_[2] >> 12) & 0x7) == 0) {
            const uint8_t e = (uint8_t)(blk_[2] & 0xFF);
            if (trusted || (eccSeen_ && eccCand_ == e)) {
                ecc_ = e;
                if (ecc_ && cb_.ecc) cb_.ecc(cb_.ctx, pi, ecc_);
            } else {
                eccCand_ = e; eccSeen_ = true;
            }
        }
    }
}

} // namespace vibedsp
