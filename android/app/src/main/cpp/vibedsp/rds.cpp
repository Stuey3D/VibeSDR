// VibeSDR V5 — RDS data-link layer (block sync + group parsing).
// Clean-room implementation of EN 50067 / IEC 62106. Original VibeSDR code.
#include "vibedsp.h"
#include <cstring>
#include <cmath>

namespace vibedsp {

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

void RdsDemod::configure(double mpxRate, const RdsDecoder::Callbacks& cb) {
    // Gentle low-pass to isolate the RDS baseband (±~2.4 kHz) after the coherent
    // 57 kHz downconvert. After downconversion the nearest MPX content (stereo,
    // pilot, L+R) lands well above 2.4 kHz, so a wide transition is fine.
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
    bphase_ = decim_;                  // must match RealFir's own starting phase
    started_ = false;
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
// Only 1-bit and 2-bit adjacent bursts are tabulated. The code can technically correct
// bursts up to 5 bits, but every pattern added makes a FALSE correction on noise more
// likely — a wrong "corrected" block is worse than a discarded one, because it enters
// the group parser looking valid. Two bits is the same conservative choice redsea
// makes, for the same reason.
const uint32_t* RdsDecoder::errorTable() {
    static uint32_t table[1024] = {0};
    static bool built = false;
    if (!built) {
        // Later (wider) patterns must not overwrite earlier (narrower) ones: prefer the
        // smallest error that explains a syndrome.
        for (uint32_t pattern : { 0x1u, 0x3u }) {
            for (int shift = 0; shift < 26; ++shift) {
                const uint32_t err = (pattern << shift) & 0x3FFFFFF;
                if (!err) continue;
                const uint16_t syn = syndrome(err);
                if (syn && !table[syn]) table[syn] = err;
            }
        }
        built = true;
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
bool RdsDecoder::tryCorrect(uint16_t offsetIdx, uint32_t& block26, bool& repaired) const {
    repaired = false;
    const uint16_t errSyn = syndrome(block26) ^ OFFSET[offsetIdx];
    if (errSyn == 0) return true;                       // clean already
    const uint32_t err = errorTable()[errSyn];
    if (!err) return false;                             // not a pattern we can undo
    block26 ^= err;
    repaired = true;
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

void RdsDecoder::reset() {
    reg_ = 0; synced_ = false; bitsLeft_ = 0; nextBlk_ = 0;
    pulseCount_ = 0; bitPos_ = 0; badHist_ = 0; blocksSeen_ = 0;
    for (int i = 0; i < 4; ++i) { blk_[i] = 0; blkOk_[i] = false; }
    std::memset(ps_, 0, sizeof ps_);
    std::memset(rt_, 0, sizeof rt_);
    ecc_ = 0;
    piLast_ = 0; piSeen_ = false; grpRepaired_ = false;
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
    if (nextBlk_ == 0) grpRepaired_ = false;      // a fresh group starts trusted
    uint32_t block = reg_;
    bool ok = false, repaired = false;
    if (nextBlk_ == 2) {
        // Slot C may legitimately carry either C or C'. Prefer whichever verifies, and
        // prefer a CLEAN match over a repaired one.
        uint32_t c = reg_, cp = reg_;
        bool rc = false, rcp = false;
        const bool okC  = tryCorrect(2, c,  rc);
        const bool okCp = tryCorrect(3, cp, rcp);
        if (okC && !rc)        { block = c;  ok = true; repaired = false; }
        else if (okCp && !rcp) { block = cp; ok = true; repaired = false; }
        else if (okC)          { block = c;  ok = true; repaired = true;  }
        else if (okCp)         { block = cp; ok = true; repaired = true;  }
    } else {
        uint32_t b = reg_;
        if (tryCorrect(kSlotOffset[nextBlk_], b, repaired)) { block = b; ok = true; }
    }
    if (repaired || !ok) grpRepaired_ = true;
    blk_[nextBlk_] = (uint16_t)((block >> 10) & 0xFFFF);
    blkOk_[nextBlk_] = ok;

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

void RdsDecoder::parseGroup() {
    if (!(blkOk_[0] && blkOk_[1])) return;
    const uint16_t pi = blk_[0];
    const int gtype = (blk_[1] >> 12) & 0xF;
    const int ver   = (blk_[1] >> 11) & 1;

    // PI is carried by EVERY group, so a PI that disagrees with the previous one is
    // either a genuine station change or a mis-correction. Either way it is not yet
    // trustworthy — wait for it to repeat before acting on anything in this group.
    // A group with no repaired blocks is trusted outright; one that needed correction
    // must agree with the previous reception before it is allowed to change anything.
    const bool trusted = !grpRepaired_;
    const bool piConfirmed = (piSeen_ && pi == piLast_);
    piLast_ = pi; piSeen_ = true;
    if (!trusted && !piConfirmed) return;

    if (gtype == 0) {                                  // 0A/0B — programme service name
        const int addr = blk_[1] & 0x3;
        if (blkOk_[3]) {
            if (trusted || (psSeen_[addr] && psCand_[addr] == blk_[3])) {
                ps_[addr * 2]     = (char)((blk_[3] >> 8) & 0xFF);
                ps_[addr * 2 + 1] = (char)(blk_[3] & 0xFF);
                if (cb_.ps) cb_.ps(cb_.ctx, pi, ps_);
            } else {
                psCand_[addr] = blk_[3]; psSeen_[addr] = true;
            }
        }
    } else if (gtype == 2) {                            // 2A/2B — RadioText
        const int addr = blk_[1] & 0xF;
        if (ver == 0 && blkOk_[2] && blkOk_[3]) {       // 2A: 4 chars (C,D)
            const uint32_t seg = ((uint32_t)blk_[2] << 16) | blk_[3];
            if (trusted || (rtSeen_[addr] && rtCand_[addr] == seg)) {
                rt_[addr * 4 + 0] = (char)((blk_[2] >> 8) & 0xFF);
                rt_[addr * 4 + 1] = (char)(blk_[2] & 0xFF);
                rt_[addr * 4 + 2] = (char)((blk_[3] >> 8) & 0xFF);
                rt_[addr * 4 + 3] = (char)(blk_[3] & 0xFF);
                if (cb_.radiotext) cb_.radiotext(cb_.ctx, rt_);
            } else {
                rtCand_[addr] = seg; rtSeen_[addr] = true;
            }
        } else if (ver == 1 && blkOk_[3]) {             // 2B: 2 chars (D)
            const uint32_t seg = blk_[3];
            if (trusted || (rtSeen_[addr] && rtCand_[addr] == seg)) {
                rt_[addr * 2 + 0] = (char)((blk_[3] >> 8) & 0xFF);
                rt_[addr * 2 + 1] = (char)(blk_[3] & 0xFF);
                if (cb_.radiotext) cb_.radiotext(cb_.ctx, rt_);
            } else {
                rtCand_[addr] = seg; rtSeen_[addr] = true;
            }
        }
    } else if (gtype == 1 && ver == 0) {                // 1A — slow labelling → ECC
        // Block C variant 0 (bits 14-12 == 0) carries the Extended Country Code
        // in its low byte. Combined with the PI country nibble it identifies the
        // station's country (RDS/IEC 62106).
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
