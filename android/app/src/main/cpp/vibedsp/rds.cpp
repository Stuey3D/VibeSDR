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

    // PI is carried by EVERY group, so a PI that disagrees with the previous one is
    // either a genuine station change or a mis-correction. Either way it is not yet
    // trustworthy — wait for it to repeat before acting on anything in this group.
    // A group with no repaired blocks is trusted outright; one that needed correction
    // must agree with the previous reception before it is allowed to change anything.
    const bool trusted = (grpRepairBits_ == 0);
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
