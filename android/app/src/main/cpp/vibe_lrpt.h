// vibe_lrpt.h — METEOR-M LRPT receiver (V12, 2026-11-19): soft symbols → CADUs → VCDUs → packets → MSU-MR pictures.
//
//   Demod (vibe_lrpt_demod.h) → [80k: marker strip + 36-branch deinterleave] → [differential decode]
//      → ASM search (eight ambiguities, every bit offset) → soft Viterbi per CADU → derandomise → RS(255,223)×4
//      → VCDU → M_PDU packets → 14-MCU JPEG-like segments → per-APID channels → greyscale + RGB composite PNGs
//
// ★★★ NOTHING IS DRAWN UNLESS REED–SOLOMON PASSED ALL FOUR WORDS OF ITS FRAME. That is the "0 false images on noise"
//     rule: the ASM search can fire on noise (it is meant to be eager), the Viterbi always outputs something, but a
//     random 1020 bytes passing four RS(255,223) checks is ~10⁻⁴⁰. Packets are then checked again (version, length,
//     APID range, the MCU number a multiple of 14 under 196, the quality factor 1…100).
//
// ★★ AUTO: the symbol rate (72k / 80k) comes from the satellite's entry (sats.json), never guessed. Within it, the
//    two variants that differ by satellite — plain or differentially coded — are BOTH searched on the same soft stream
//    (a second sync searcher costs a few % of a core); the first to pass Reed–Solomon wins and the other stops. QPSK and
//    OQPSK run as two demodulators side by side until one has the most good frames (Receiver::kContestFrames); with no
//    rate named, 72k and 80k are walked while nothing decodes. Every one of those can be pinned (the manual fallback).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>     // std::swap (rotate180)
#include <string>
#include <vector>

#include "vibe_lrpt_demod.h"
#include "vibe_lrpt_fec.h"
#include "vibe_lrpt_image.h"

namespace vibelrpt {

struct Stats {
    uint64_t cadus = 0, rsOk = 0, rsFail = 0, rsFixed = 0, packets = 0, badPackets = 0, blocks = 0, syncLost = 0;
    int lastVit = 0;        ///< Viterbi path metric per step ×100 of the last CADU (lower is cleaner)
    int scid = -1;          ///< spacecraft id from the last good VCDU
    uint32_t vcduCounter = 0;
};

/** ── Sync + Viterbi + RS: one soft stream in, verified 892-byte VCDUs out. ─────────────────────────────────────── */
class FrameSync {
public:
    using VcduFn = std::function<void(const uint8_t* vcdu892, int rsFixed)>;
    explicit FrameSync(VcduFn fn) : fn_(std::move(fn)) {}
    void setScalar(bool s) { vit_.forceScalar = s; }

    void push(const int8_t* s, size_t n) {
        buf_.insert(buf_.end(), s, s + n);
        softSinceGood_ += n;
        process();
    }
    /** ★★ THE DIFFERENTIAL PATH: `s` is the differentially decoded stream (the ASM search runs on it, as before) and `raw`
     *  the same soft values BEFORE that decoding — the CADU is Viterbi-decoded from `raw` (see decodeAt). Both are pairs
     *  in the demodulator's order, the same length. */
    void push(const int8_t* s, const int8_t* raw, size_t n) {
        buf_.insert(buf_.end(), s, s + n);
        raw_.insert(raw_.end(), raw, raw + n);
        softSinceGood_ += n;
        joint_ = true;
        process();
    }
    Stats& stats() { return st_; }
    /** ★ Locked AND confirmed — the marker seen again one CADU on, or Reed–Solomon passed (2026-11-24: with kFind 44 a
     *  noise window matches now and then; that unconfirmed find is not reported as "sync" to the status or the summary). */
    bool locked() const { return locked_ && confirmed_; }
    /** ★ Reed–Solomon passed within the last 8 CADUs — the only proof the carrier is really locked (an ASM can be "found"
     *  with the carrier 400 Hz out at Es/N0 3 dB; holding the acquisition on that kept it out for the whole run). */
    /* ★★★ COUNTED IN SOFT VALUES ARRIVING, NOT IN CADUs TRIED (2026-11-22, the live pass's "never relocked"). It counted
     *  failed decodes — and once the ASM was lost there were none, so it stayed "decoding" for ever after the last good
     *  frame: the demodulator held its acquisition off (setHold) for the rest of the pass and the loop wandered 2–3 kHz
     *  away in the fade. Now: within 8 CADUs' worth of soft values of the last good frame, however they were spent. */
    bool decoding() const { return softSinceGood_ < uint64_t(8) * uint64_t(kCaduSoft); }
    /** ★ Test seam: each CADU's ASM score, ambiguity, Viterbi metric and the four RS words' results. */
    std::function<void(int, int, int, const int*)> caduDiag;
    void reset() { base_ += buf_.size(); buf_.clear(); raw_.clear(); locked_ = false; scan_ = 0; misses_ = 0; }

private:
    static constexpr int kLead = 128, kTail = 128;               // soft bits each side (64 Viterbi steps)
    /* ★★ kFind 46 → 44 (2026-11-24, docs/v12/LRPT-SENSITIVITY.md): on the differential path the ASM is searched on the
     *  differentially decoded stream, whose hard-bit error rate is about twice the channel's — at Es/N0 2 dB about 9–10 of
     *  the 52 fixed bits are wrong, and ≥ 46 matched one CADU in eight. 44 finds the frame sooner after a fade; noise
     *  passes 44 a few times a second more, each costing one Viterbi that Reed–Solomon then refuses (0 false pictures:
     *  test-lrpt's noise runs). Real passes: 400 → 415 frames (the weak 24 Nov 09:20 pass 44 → 56). 42 lost frames
     *  (385): false locks hold the searcher off the real marker. */
    static constexpr int kFind = 44, kKeep = 38;                 // of 52 fixed coded ASM bits
    uint64_t windowAt(size_t p) const {                         // 64 hard bits starting at p
        uint64_t w = 0;
        for (int i = 0; i < 64; i++) w = (w << 1) | uint64_t(buf_[p + size_t(i)] > 0);
        return w;
    }
    void process() {
        for (;;) {
            if (!locked_) {
                // ── SEARCH: every bit offset, eight ambiguities ──
                if (buf_.size() < 64) return;
                const size_t end = buf_.size() - 64;
                size_t p = scan_;
                uint64_t w = p < end ? windowAt(p) : 0;
                bool found = false;
                for (; p < end; p++) {
                    if (p != scan_) w = (w << 1) | uint64_t(buf_[p + 63] > 0);
                    for (int x = 0; x < 8; x++) if (syncScore(w, x) >= kFind) { found = true; x_ = x; break; }
                    if (found) break;
                }
                if (!found) {
                    // keep the last kLead+64 bits for the next call
                    const size_t keep = std::min(buf_.size(), size_t(kLead + 64));
                    drop(buf_.size() - keep);
                    scan_ = keep >= 64 ? keep - 64 + 1 : 0;
                    if (scan_ > buf_.size()) scan_ = 0;
                    return;
                }
                pos_ = p; locked_ = true; misses_ = 0; confirmed_ = false;
            }
            // ── LOCKED: a whole CADU plus the tail must be here ──
            if (buf_.size() < pos_ + size_t(kCaduSoft + kTail + 64)) return;
            decodeAt(pos_);
            // the next ASM: exactly one CADU on, ± 2 bits (a timing slip), any ambiguity (a phase slip)
            const size_t want = pos_ + size_t(kCaduSoft);
            int best = -1; size_t bp = want; int bx = x_;
            for (int d = -2; d <= 2; d++) {
                const size_t q = size_t(long(want) + d);
                const uint64_t w = windowAt(q);
                for (int x = 0; x < 8; x++) { const int sc = syncScore(w, x); if (sc > best) { best = sc; bp = q; bx = x; } }
            }
            // ★ confirmed: the marker again, on the same ambiguity, at ≥ 42 of 52 (noise: ≈ 10⁻⁵ a CADU)
            if (best >= 42 && bx == x_) confirmed_ = true;
            if (best >= kKeep) { pos_ = bp; x_ = bx; misses_ = 0; }
            else if (++misses_ > 3) { locked_ = false; st_.syncLost++; pos_ = want; }
            else pos_ = want;                                   // flywheel: RS judges it
            // trim: keep kLead bits before the next position
            const size_t cut = pos_ > size_t(kLead) ? pos_ - size_t(kLead) : 0;
            drop(cut);
            pos_ -= cut;
            scan_ = locked_ ? 0 : pos_;
        }
    }
    /** Drop the oldest `k` soft values (both streams), keeping the absolute index of buf_[0]. */
    void drop(size_t k) {
        if (k == 0) return;
        buf_.erase(buf_.begin(), buf_.begin() + long(k));
        if (joint_) raw_.erase(raw_.begin(), raw_.begin() + long(std::min(k, raw_.size())));
        base_ += k;
    }
    /** The plain path: the transformed soft stream around p. */
    void softPlain(size_t p, int nSoft) {
        soft_.assign(size_t(nSoft), 0);
        const long start = long(p) - kLead;
        for (int i = 0; i + 1 < nSoft; i += 2) {
            const long a = start + i;
            if (a < 0 || size_t(a + 1) >= buf_.size()) continue;
            int oa, ob; applyX(x_, buf_[size_t(a)], buf_[size_t(a + 1)], oa, ob);
            soft_[size_t(i)] = int8_t(std::max(-127, std::min(127, oa)));
            soft_[size_t(i + 1)] = int8_t(std::max(-127, std::min(127, ob)));
        }
    }
    /* ★★★ THE DIFFERENTIAL CODE DECODED WITH THE CONVOLUTIONAL CODE, NOT BEFORE IT (2026-11-22, the live Meteor-M2-4 pass).
     *  Decoding the differential code on the soft values first (the product of each value and the last) multiplies two
     *  noisy numbers: at the Es/N0 a pass is decoded at (4–5 dB) that costs the Viterbi about 1.5 dB, and a synthetic
     *  OQPSK + differential signal needed Es/N0 5 dB where plain QPSK decodes at 3. The way round it is plain algebra:
     *  the differential encoder is an accumulator on each coded stream (a_n = a_{n−1} ⊕ c_n, ⊕ 1 on the first), and
     *  an accumulator COMMUTES with the convolutional code (both linear): g ∗ (u / (1+D)) = (g ∗ u) / (1+D). So the
     *  transmitted streams ARE the same K=7 code of the accumulated data u′ = u / (1+D) — up to sign patterns — and the
     *  ordinary 64-state Viterbi decodes u′ from the RAW soft values with nothing multiplied; u = u′ ⊕ u′ delayed by one
     *  (a hard XOR of the decoded bits: an error burst stays one burst). Same cost as the plain path.
     *  ★ The signs, in ±1 terms (bit 1 = +): the diff-decoded value the ASM search used is D = R_n·R_{n−1} on the first
     *  stream and −R_n·R_{n−1} on the second, and the sync's transform x_ makes each Viterbi input G_e = σ_e·D_{src(e)}.
     *  With τ = σ_e (first stream) or −σ_e (second), G_n = τ·R_n·R_{n−1}, and the accumulated value P with
     *  P_n·P_{n−1} = −G_n (the XOR of the accumulator) is P_n = κ·R_n when τ = −1 and κ·(−1)ⁿ·R_n when τ = +1 (n = the
     *  value's place along its stream). κ is a constant per stream: flipping BOTH is u′ → ¬u′, which the final XOR
     *  removes, so only their relative sign matters — kRel_, tried both ways when Reed–Solomon fails and kept when it
     *  passes (on a real pass it is found on the first frame and never changes). */
    void softJoint(size_t p, int nSoft, int rel) {
        soft_.assign(size_t(nSoft), 0);
        int src[2], sg[2];
        { int oa, ob; applyX(x_, 1, 2, oa, ob); src[0] = std::abs(oa) - 1; sg[0] = oa > 0 ? 1 : -1; src[1] = std::abs(ob) - 1; sg[1] = ob > 0 ? 1 : -1; }
        const long start = long(p) - kLead;
        for (int i = 0; i + 1 < nSoft; i += 2) {
            const long a = start + i;
            if (a < 0 || size_t(a + 1) >= raw_.size()) continue;
            for (int e = 0; e < 2; e++) {
                const long li = a + src[e];
                const uint64_t abs = base_ + uint64_t(li);
                const int tau = (abs & 1) ? -sg[e] : sg[e];
                int v = raw_[size_t(li)];
                if (tau > 0 && ((abs >> 1) & 1)) v = -v;
                if (e == 1 && rel < 0) v = -v;
                soft_[size_t(i + e)] = int8_t(std::max(-127, std::min(127, v)));
            }
        }
    }
    int tryDecode(uint8_t* cadu, int* pw) {
        const int nSoft = kLead + kCaduSoft + kTail;
        if (!joint_) {
            bits_.resize(kCaduBits);
            st_.lastVit = vit_.decode(soft_.data(), nSoft / 2, kLead / 2, kCaduBits, bits_.data());
        } else {
            // one bit before the CADU too: u = u′ ⊕ u′ delayed
            acc_.resize(kCaduBits + 1); bits_.resize(kCaduBits);
            st_.lastVit = vit_.decode(soft_.data(), nSoft / 2, kLead / 2 - 1, kCaduBits + 1, acc_.data());
            for (int i = 0; i < kCaduBits; i++) bits_[size_t(i)] = uint8_t(acc_[size_t(i + 1)] ^ acc_[size_t(i)]);
        }
        for (int i = 0; i < kCaduBytes; i++) {
            uint8_t b = 0;
            for (int j = 0; j < 8; j++) b = uint8_t((b << 1) | bits_[size_t(i * 8 + j)]);
            cadu[i] = b;
        }
        uint8_t* f = cadu + kAsmBytes;
        derandomise(f);
        return rsFrame(f, pw);
    }
    void decodeAt(size_t p) {
        const int nSoft = kLead + kCaduSoft + kTail;
        uint8_t cadu[kCaduBytes];
        int pw[4];
        int e;
        if (!joint_) { softPlain(p, nSoft); e = tryDecode(cadu, pw); }
        else {
            softJoint(p, nSoft, rel_);
            e = tryDecode(cadu, pw);
            if (e < 0) {   // ★ the other relative sign (once per lock in practice)
                uint8_t c2[kCaduBytes]; int pw2[4];
                const int vit = st_.lastVit;
                softJoint(p, nSoft, -rel_);
                const int e2 = tryDecode(c2, pw2);
                if (e2 >= 0) { rel_ = -rel_; e = e2; std::memcpy(cadu, c2, sizeof cadu); std::memcpy(pw, pw2, sizeof pw); }
                else st_.lastVit = vit;
            }
        }
        st_.cadus++;
        uint8_t* f = cadu + kAsmBytes;
        if (caduDiag) caduDiag(syncScore(windowAt(p), x_), x_, st_.lastVit, pw);
        if (e < 0) { st_.rsFail++; return; }
        softSinceGood_ = 0; confirmed_ = true;
        st_.rsOk++; st_.rsFixed += uint64_t(e);
        fn_(f, e);
    }

    VcduFn fn_;
    Viterbi vit_;
    std::vector<int8_t> buf_, raw_, soft_;
    std::vector<uint8_t> bits_, acc_;
    uint64_t base_ = 0;          ///< absolute index of buf_[0] (the stream a value belongs to and its place along it)
    bool joint_ = false;         ///< the differential path: decode from raw_ (softJoint)
    int rel_ = 1;                ///< the two accumulated streams' relative sign
    size_t scan_ = 0, pos_ = 0;
    int x_ = 0, misses_ = 0;
    uint64_t softSinceGood_ = uint64_t(1) << 40;
    bool locked_ = false, confirmed_ = false;
    Stats st_;
};

/** ── 80k mode front: find the 0x27 marker every 80 bits, derotate, strip it, deinterleave. ─────────────────────── */
class InterleaveFront {
public:
    void push(const int8_t* s, size_t n, std::vector<int8_t>& out) {
        in_.insert(in_.end(), s, s + n);
        for (;;) {
            if (!have_) {
                if (in_.size() < size_t(kInterStride * kLook + 8)) return;
                // ★ score every bit offset and ambiguity over kLook marker periods
                int best = -1;
                for (int o = 0; o < kInterStride; o++)
                    for (int x = 0; x < 8; x++) {
                        int sc = 0;
                        for (int k = 0; k < kLook; k++) sc += markerScore(size_t(o + k * kInterStride), x);
                        if (sc > best) { best = sc; off_ = o; x_ = x; }
                    }
                if (best < kLook * 7) { in_.erase(in_.begin(), in_.begin() + kInterStride * (kLook / 2)); return; }
                in_.erase(in_.begin(), in_.begin() + off_);
                have_ = true; bad_ = 0;
            }
            if (in_.size() < size_t(kInterStride * 2)) return;
            // re-check this period's marker (an ambiguity change re-searches)
            if (markerScore(0, x_) < 6) { if (++bad_ > 8) { have_ = false; continue; } } else bad_ = 0;
            for (int i = 8; i < kInterStride; i += 2) {
                int a, b; applyX(x_, in_[size_t(i)], in_[size_t(i + 1)], a, b);
                out.push_back(dl_.push(int8_t(std::max(-127, std::min(127, a)))));
                out.push_back(dl_.push(int8_t(std::max(-127, std::min(127, b)))));
            }
            in_.erase(in_.begin(), in_.begin() + kInterStride);
        }
    }
    bool synced() const { return have_; }
private:
    static constexpr int kLook = 24;
    int markerScore(size_t p, int x) const {
        int sc = 0;
        for (int i = 0; i < 8; i += 2) {
            int a, b; applyX(x, in_[p + size_t(i)], in_[p + size_t(i + 1)], a, b);
            sc += ((a > 0) == bool((kInterMarker >> (7 - i)) & 1)) + ((b > 0) == bool((kInterMarker >> (6 - i)) & 1));
        }
        return sc;
    }
    std::vector<int8_t> in_;
    Deinterleaver dl_;
    int off_ = 0, x_ = 0, bad_ = 0; bool have_ = false;
};

/** ── VCDU → space packets (CCSDS 132 M_PDU), one assembler per virtual channel. ─────────────────────────────────── */
class PacketAssembler {
public:
    using PacketFn = std::function<void(const uint8_t* pkt, size_t len)>;   // primary header included
    explicit PacketAssembler(PacketFn fn) : fn_(std::move(fn)) {}
    void vcdu(const uint8_t* v) {
        const uint32_t ctr = (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 8) | v[4];
        if (haveCtr_ && ctr != ((lastCtr_ + 1) & 0xFFFFFF)) pk_.clear(), need_ = 0;     // a gap: the partial is lost
        haveCtr_ = true; lastCtr_ = ctr;
        const uint8_t* d = v + 10;
        const int fhp = ((v[8] & 7) << 8) | v[9];
        const int n = 882;
        int at = 0;
        if (!pk_.empty()) {
            const int upto = fhp == 0x7FF ? n : std::min(fhp, n);
            take(d, at, upto);
            if (!pk_.empty() && fhp != 0x7FF) { pk_.clear(); need_ = 0; }   // ★ the header pointer disagrees: resync
        }
        if (pk_.empty()) {
            if (fhp == 0x7FF) return;
            if (fhp >= n) return;
            at = std::max(at, fhp);
        }
        while (at < n) take(d, at, n);
    }
    uint64_t drops() const { return drops_; }
private:
    void take(const uint8_t* d, int& at, int upto) {
        while (at < upto) {
            if (pk_.size() < 6) { pk_.push_back(d[at++]); if (pk_.size() == 6) need_ = 6 + ((size_t(pk_[4]) << 8) | pk_[5]) + 1; continue; }
            if (need_ > 8192) { pk_.clear(); need_ = 0; drops_++; at = upto; return; }     // nonsense length
            const size_t want = std::min(need_ - pk_.size(), size_t(upto - at));
            pk_.insert(pk_.end(), d + at, d + at + long(want));
            at += int(want);
            if (pk_.size() == need_) { fn_(pk_.data(), pk_.size()); pk_.clear(); need_ = 0; }
        }
    }
    PacketFn fn_;
    std::vector<uint8_t> pk_;
    size_t need_ = 0;
    bool haveCtr_ = false; uint32_t lastCtr_ = 0;
    uint64_t drops_ = 0;
};

struct ReceiverConfig {
    double sampleRate = 0;
    double symbolRate = 72000;
    enum Mod { Auto, Qpsk, Oqpsk } mod = Auto;
    enum Diff { DiffAuto, DiffOff, DiffOn } diff = DiffAuto;
    bool interleaved = false;                 ///< the 80k mode (sats.json)
    /** ★ The symbol rate is NOT known (no satellite entry named it): AUTO also tries 80k OQPSK interleaved, with a longer
     *  dwell (its deinterleaver alone takes 16 s to fill). `symbolRate` / `interleaved` are then ignored. */
    bool rateAuto = false;
    bool forceScalar = false;                 ///< ★ tests: the A/B against the SIMD paths
};

/** ── The whole receiver. Feed IQ; pictures grow as the pass goes. ─────────────────────────────────────────────── */
class Receiver {
public:
    static constexpr double kModeSwapSec = 8.0;
    static constexpr double kMarkerSec = 4.0;        ///< the 80k group without a marker this long moves on
    static constexpr double kPeriodMs = 1232.0;      ///< MSU-MR: eight lines every 1.232 s
    /** ★ AUTO's contest: both modulations run until one has this many good frames; the one with more stays. */
    static constexpr uint64_t kContestFrames = 12;
    explicit Receiver(const ReceiverConfig& c) : cfg_(c) {
        /* ★★ WHAT AUTO TRIES. A GROUP is a symbol rate (+ the 80k interleave); within it, with the modulation not pinned,
         *  QPSK and OQPSK demodulators run SIDE BY SIDE on the same IQ and the one with more good frames wins
         *  (kContestFrames). Not "the first to decode": an OQPSK demodulator on the 2018 QPSK recording still passes
         *  Reed–Solomon on 55 % of the frames, and taking the first success kept it — half the picture lost.
         *  Groups are walked while nothing decodes. Order: the satellites in orbit (METEOR-M N2-3 and N2-4, 2026) send 72k
         *  OQPSK most of the time and 80k OQPSK at times; 72k QPSK was METEOR-M N2's (silent since 2022). */
        auto mods = [&](bool only80) {
            std::vector<bool> m;   // oqpsk?
            if (c.mod != ReceiverConfig::Qpsk) m.push_back(true);
            if (c.mod != ReceiverConfig::Oqpsk && !(only80 && c.mod == ReceiverConfig::Auto)) m.push_back(false);   // ★ 80k has only ever been OQPSK
            return m;
        };
        if (c.rateAuto) { groups_.push_back({ 72000, false, mods(false) }); groups_.push_back({ 80000, true, mods(true) }); }
        else groups_.push_back({ c.symbolRate, c.interleaved, mods(c.symbolRate >= 76000) });
        for (auto& g : groups_) g.dwellSec = kModeSwapSec + (g.interleaved ? double(Deinterleaver::latency()) / (2.0 * g.sym) : 0.0);
        asm_ = std::make_unique<PacketAssembler>([this](const uint8_t* p, size_t n) { onPacket(p, n); });
        applyGroup(0);
    }
    /** ★ Test seam: every image packet's APID, sequence count, MCU number, time (ms of day) and quality factor. */
    std::function<void(int, int, int, uint32_t, int)> diag;
    void setCaduDiag(std::function<void(int, int, int, const int*)> f) { caduDiag_ = f; for (auto& ch : chains_) for (auto& p : ch->paths) if (p.sync) p.sync->caduDiag = f; }
    double carrierHz() const { return lead().demod->carrierHz(); }
    void setDopplerHz(double hz) { dop_ = hz; for (auto& ch : chains_) ch->demod->setDopplerHz(hz); }

    /** Feed interleaved float IQ. */
    void feedIq(const float* iq, size_t n) {
        for (auto& ch : chains_) {
            soft_.clear();
            ch->demod->setHold(ch->decoding());
            ch->demod->feed(iq, n, soft_);
            chainSoft(*ch, soft_.data(), soft_.size());
        }
        samples_ += n;
        // ── the contest: one modulation stays once it has proved itself ──
        if (chains_.size() > 1) {
            size_t best = 0;
            for (size_t i = 1; i < chains_.size(); i++) if (chains_[i]->rsOk() > chains_[best]->rsOk()) best = i;
            if (chains_[best]->rsOk() >= kContestFrames) {
                std::unique_ptr<Chain> keep = std::move(chains_[best]);
                chains_.clear(); chains_.push_back(std::move(keep));
            }
        }
        // ── AUTO: the next group while nothing has decoded ──
        /* ★★ THE 80k GROUP LEAVES EARLY WHEN ITS MARKERS NEVER APPEAR (2026-11-22). Its dwell is 24 s because the
         *  deinterleaver takes 16 s to fill — but the 0x27 marker every 80 bits is found BEFORE the deinterleaver, within
         *  a second of the carrier. A 72k signal never shows one, and on the live Meteor-M2-4 pass the 80k group's 24 s
         *  sat on the strongest 13 s of it (470–483 s: the replay with AUTO got 8 frames, 72k pinned 20). Without a marker
         *  in kMarkerSec it moves on; with one it stays the full dwell. */
        if (groups_.size() > 1 && !anyFrame_) {
            const Group& g = groups_[gi_];
            bool marker = false;
            for (auto& ch : chains_) if (ch->front && ch->front->synced()) marker = true;
            const double dwell = g.interleaved && !marker && !markerSeen_ ? kMarkerSec : g.dwellSec;
            if (marker) markerSeen_ = true;
            if (samples_ - swapAt_ > uint64_t(dwell * cfg_.sampleRate)) {
                swapAt_ = samples_; markerSeen_ = false;
                applyGroup((gi_ + 1) % groups_.size());
            }
        }
    }
    /** ★★ The satellite engine knows the satellite (its entry names 72k or 80k): AUTO's walk stops on that rate. Ignored
     *  once a frame has decoded (the signal has spoken) or when no group has that rate. */
    void preferRate(double sym) {
        if (groups_.size() < 2 || anyFrame_ || !(sym > 0)) return;
        for (size_t i = 0; i < groups_.size(); i++) if (std::fabs(groups_[i].sym - sym) < 1000) {
            Group g = groups_[i];
            groups_.assign(1, g);
            if (i != gi_) applyGroup(0); else gi_ = 0;
            return;
        }
    }
    /** ★ The packet time (ms of day, as the satellite stamped it) of strip `s`'s scan period; −1 when no packet of that
     *  strip was heard. The map overlay's only clock (vibe_lrpt_geo.h). */
    int32_t stripTimeMs(int s) const { return s >= 0 && size_t(s) < stripMs_.size() ? stripMs_[size_t(s)] : -1; }
    /** ★ The newest period stamp heard (ms of day), −1 = none yet — paired with OUR clock by Run for the overlay's clock
     *  check (vibe_lrpt_overlay.h checkClock). */
    int32_t lastStampMs() const { return haveT0_ ? int32_t(lastMs_) : -1; }
    /** How many rates AUTO is still walking (1 = settled or pinned). */
    size_t rateGroups() const { return groups_.size(); }
    /** Feed soft symbols directly (an archived .s file, or the client-assisted stage): the first chain's path. */
    void feedSoft(const int8_t* s, size_t n) { if (!chains_.empty()) chainSoft(*chains_.front(), s, n); }

    // ── Results ──
    const std::map<int, Channel>& channels() const { return ch_; }
    Stats stats() const {
        Stats s;
        uint64_t rsOk = 0, rsFail = 0, cadus = 0, fixed = 0, lost = 0; int vit = 0;
        for (auto& ch : chains_) for (auto& p : ch->paths) if (p.sync) {
            const Stats& t = p.sync->stats();
            cadus = std::max(cadus, t.cadus); rsFail = std::max(rsFail, t.rsFail); lost += t.syncLost; if (t.lastVit) vit = t.lastVit;
        }
        rsOk = rsOkTotal_; fixed = fixedTotal_;
        s.cadus = cadusBase_ + cadus; s.rsOk = rsOk; s.rsFail = rsFailBase_ + rsFail; s.rsFixed = fixed; s.syncLost = lost; s.lastVit = vit;
        s.packets = packets_; s.badPackets = badPackets_; s.blocks = blocks_; s.scid = scid_; s.vcduCounter = ctr_;
        return s;
    }
    bool demodLocked() const { return lead().demod->locked(); }
    double lockMetric() const { return lead().demod->lockMetric(); }
    /** Es/N0 (linear) the lead demodulator measures — signed, ≈ 0 on noise (average it). */
    double esn0() const { return lead().demod->esn0(); }
    /** ★ diagnostics: the lead demodulator itself. */
    const Demod& leadDemod() const { return *lead().demod; }
    /** ★ diagnostics: how often the lead demodulator's x⁴ acquisition has set its NCO. */
    uint64_t acquisitions() const { return lead().demod->acquisitions(); }
    bool syncLocked() const { for (auto& ch : chains_) for (auto& p : ch->paths) if (p.sync && p.sync->locked()) return true; return false; }
    bool decoding() const { for (auto& ch : chains_) if (ch->decoding()) return true; return false; }
    const char* modeName() const { return lead().oqpsk ? "OQPSK" : "QPSK"; }
    double symbolRate() const { return lead().sym; }
    bool interleaved() const { return lead().front != nullptr; }
    int diffWinner() const { const Chain& c = lead(); return c.winner < 0 ? -1 : c.paths[c.winner].diff ? 1 : 0; }
    /** ★ How many demodulators are running (2 while AUTO's contest is on) — the CPU figure's context. */
    size_t chains() const { return chains_.size(); }
    /** ★ Bumped whenever a strip lands — a service re-renders its preview when this moves. */
    uint64_t version() const { return version_; }
    /** The strip range that holds any picture. */
    void stripRange(int& s0, int& s1) const {
        s0 = 1 << 30; s1 = 0;
        for (auto& kv : ch_) { if (kv.second.goodBlocks() == 0) continue; s1 = std::max(s1, kv.second.strips());
            for (int s = 0; s < kv.second.strips(); s++) { bool any = false; for (int c = 0; c < kMcuPerLine && !any; c++) any = kv.second.has(s, c); if (any) { s0 = std::min(s0, s); break; } } }
        if (s0 > s1) s0 = s1 = 0;
    }
    /** ★ Only channels that carry real picture: ≥ 5 % of the blocks of the richest (a channel the satellite sends a
     *  few stray packets of is not offered as an image). */
    std::vector<int> liveApids() const {
        uint64_t mx = 0; for (auto& kv : ch_) mx = std::max(mx, kv.second.goodBlocks());
        std::vector<int> a; for (auto& kv : ch_) if (mx && kv.second.goodBlocks() * 20 >= mx && kv.second.goodBlocks() >= 196) a.push_back(kv.first);
        return a;
    }

private:
    struct Path { bool diff = false; DiffDecoder dd; std::unique_ptr<FrameSync> sync; };
    struct Group { double sym = 72000; bool interleaved = false; std::vector<bool> oqpsk; double dwellSec = kModeSwapSec; };
    /** ★ One demodulator and everything after it up to the VCDU: its own 80k front, its plain and differential paths. */
    struct Chain {
        double sym = 72000; bool oqpsk = false;
        std::unique_ptr<Demod> demod;
        std::unique_ptr<InterleaveFront> front;   // ★ the 2.6 MB deinterleaver exists only in the 80k group
        Path paths[2];
        int winner = -1;
        uint64_t rsOk() const { uint64_t n = 0; for (auto& p : paths) if (p.sync) n += p.sync->stats().rsOk; return n; }
        bool decoding() const { for (auto& p : paths) if (p.sync && p.sync->decoding()) return true; return false; }
    };
    const Chain& lead() const {
        size_t best = 0;
        for (size_t i = 1; i < chains_.size(); i++) if (chains_[i]->rsOk() > chains_[best]->rsOk()) best = i;
        return *chains_[best];
    }
    void applyGroup(size_t i) {
        // ★ the counts of the chains going away are kept (a stats() that went backwards would read as a fault)
        for (auto& ch : chains_) for (auto& p : ch->paths) if (p.sync) { cadusBase_ += p.sync->stats().cadus; rsFailBase_ += p.sync->stats().rsFail; }
        gi_ = i;
        chains_.clear();
        const Group& g = groups_[i];
        for (bool oq : g.oqpsk) {
            auto ch = std::make_unique<Chain>();
            ch->sym = g.sym; ch->oqpsk = oq;
            DemodConfig d; d.sampleRate = cfg_.sampleRate; d.symbolRate = g.sym; d.oqpsk = oq;
            ch->demod = std::make_unique<Demod>(d);
            ch->demod->fir().forceScalar = cfg_.forceScalar;
            ch->demod->setDopplerHz(dop_);
            if (g.interleaved) ch->front = std::make_unique<InterleaveFront>();
            Chain* cp = ch.get();
            for (int k = 0; k < 2; k++) {
                const bool diffOn = k == 1;
                if (cfg_.diff == ReceiverConfig::DiffOff && diffOn) continue;
                if (cfg_.diff == ReceiverConfig::DiffOn && !diffOn) continue;
                ch->paths[k].diff = diffOn;
                ch->paths[k].sync = std::make_unique<FrameSync>([this, cp, k](const uint8_t* v, int e) { onVcdu(*cp, k, v, e); });
                ch->paths[k].sync->setScalar(cfg_.forceScalar);
                if (caduDiag_) ch->paths[k].sync->caduDiag = caduDiag_;
            }
            chains_.push_back(std::move(ch));
        }
    }
    void chainSoft(Chain& ch, const int8_t* s, size_t n) {
        const int8_t* src = s; size_t len = n;
        if (ch.front) { deint_.clear(); ch.front->push(s, n, deint_); src = deint_.data(); len = deint_.size(); }
        for (int k = 0; k < 2; k++) {
            Path& p = ch.paths[k];
            if (!p.sync || (ch.winner >= 0 && ch.winner != k)) continue;
            if (p.diff) { tmp_.assign(src, src + len); p.dd.run(tmp_.data(), tmp_.size()); p.sync->push(tmp_.data(), src, tmp_.size()); }
            else p.sync->push(src, len);
        }
    }
    void onVcdu(Chain& ch, int k, const uint8_t* v, int fixed) {
        if ((v[0] >> 6) != 1) return;                        // transfer frame version 2 ("01")
        const int vcid = v[1] & 0x3F;
        if (vcid == 63) return;                              // fill
        if (ch.winner < 0) ch.winner = k;
        const uint32_t ctr = (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 8) | v[4];
        /* ★★ ONCE EACH, whichever demodulator got it first: during AUTO's contest both chains can decode the same frame.
         *  A counter not ahead of the last one delivered is a duplicate — unless it is far behind (a new pass, a reset). */
        if (haveCtr_) {
            const uint32_t ahead = (ctr - lastCtr_) & 0xFFFFFF;
            if (ahead == 0 || (ahead > 0x800000 && ((lastCtr_ - ctr) & 0xFFFFFF) < 4096)) return;
        }
        haveCtr_ = true; lastCtr_ = ctr;
        rsOkTotal_++; fixedTotal_ += uint64_t(fixed > 0 ? fixed : 0);
        anyFrame_ = true;
        scid_ = ((v[0] & 0x3F) << 2) | (v[1] >> 6);
        ctr_ = ctr;
        if (vcid != 5) return;                               // ★ MSU-MR imagery is virtual channel 5
        asm_->vcdu(v);
    }
    void onPacket(const uint8_t* p, size_t n) {
        packets_++;
        if (n < 6 + 8 + 6 + 1 || (p[0] >> 5) != 0) { badPackets_++; return; }
        const int apid = ((p[0] & 7) << 8) | p[1];
        const int seq = ((p[2] & 0x3F) << 8) | p[3];
        (void)seq;
        if (apid < 64 || apid > 69) return;                  // 70 = telemetry, 2047 = idle
        const uint8_t* d = p + 6 + 8;                        // after the primary header and the time
        const int mcu = d[0], q = d[5];
        if (diag) diag(apid, seq, mcu, (uint32_t(p[8]) << 24) | (uint32_t(p[9]) << 16) | (uint32_t(p[10]) << 8) | p[11], q);
        if (mcu % kMcuPerPacket != 0 || mcu >= kMcuPerLine || q < 1 || q > 100) { badPackets_++; return; }
        // ── the strip: from the packet's TIME, which every channel's packets of one scan period share ──
        /* ★★★ NOT FROM THE SEQUENCE COUNT ALONE. The count runs across all APIDs (14 a channel + 1 telemetry = 43 a
         *  period), so a channel's place in the period is unknown until a period boundary has been seen — and numbering
         *  each channel from its own first packet put channel 2 one strip below channel 1 on the first real pass (the
         *  first packet heard was channel 2's MCU 168, the end of the previous period). The time (ms of day, the same for
         *  64 / 65 / 68 of one period on the 2018 recording) aligns every channel at once, and a gap of any length lands
         *  in the right place. The period is MSU-MR's 1.232 s for eight lines, measured on that recording (1232 ms). */
        const uint32_t ms = (uint32_t(p[8]) << 24) | (uint32_t(p[9]) << 16) | (uint32_t(p[10]) << 8) | p[11];
        if (ms >= 86400000u) { badPackets_++; return; }
        /* ★★ COUNTED FROM THE LAST PERIOD SEEN, NOT FROM THE FIRST: the stamps step 1232 ms mostly and 1228 sometimes
         *  (84 steps on the 2018 recording: 66 × 1232, 18 × 1228 — a true period near 1231.1 ms in whole-ms stamps), so
         *  dividing the whole elapsed time by one figure drifts off the grid within a minute. Each new stamp is a whole
         *  number of periods after the last one (±10 ms); a gap of hundreds of periods still rounds right. */
        if (!haveT0_) { haveT0_ = true; lastMs_ = ms; lastStrip_ = 0; }
        long dt = long(ms) - long(lastMs_);
        if (dt < -43200000L) dt += 86400000L; else if (dt > 43200000L) dt -= 86400000L;   // ★ midnight
        const long k = std::lround(double(dt) / kPeriodMs);
        if (std::fabs(double(dt) - double(k) * kPeriodMs) > 10.0 + 1.0 * std::fabs(double(k))) { badPackets_++; return; }
        const long strip = lastStrip_ + k;
        if (k > 0) { lastMs_ = ms; lastStrip_ = strip; }
        /* ★★ THE STRIP'S OWN STAMP, kept for the map overlay (vibe_lrpt_geo.h): the ms of day the satellite put on this
         *  period's packets — never a stamp computed from the grid, so a wrong on-board clock shows as itself and the
         *  georeference can refuse it. The first stamp seen for a strip stays (every channel of one period shares it).
         *  kMaxStrips × 4 bytes = 3.2 KB. */
        if (strip >= 0 && strip < kMaxStrips) {
            if (stripMs_.size() <= size_t(strip)) stripMs_.resize(size_t(strip) + 1, -1);
            if (stripMs_[size_t(strip)] < 0) stripMs_[size_t(strip)] = int32_t(ms);
        }
        if (ch_.size() >= size_t(kMaxChannels) && !ch_.count(apid)) return;
        uint8_t blocks[kMcuPerPacket][64];
        const int good = decodeMcus(d + 6, n - 6 - 8 - 6, q, blocks);
        auto& ch = ch_.emplace(apid, Channel(apid)).first->second;
        if (ch.put(int(strip), mcu, blocks, good)) { blocks_ += uint64_t(good); version_++; }
    }

    ReceiverConfig cfg_;
    std::vector<Group> groups_;
    size_t gi_ = 0;
    std::vector<std::unique_ptr<Chain>> chains_;
    std::function<void(int, int, int, const int*)> caduDiag_;
    double dop_ = 0;
    std::unique_ptr<PacketAssembler> asm_;
    std::vector<int8_t> soft_, deint_, tmp_;
    uint64_t samples_ = 0, swapAt_ = 0;
    bool anyFrame_ = false, markerSeen_ = false;
    bool haveCtr_ = false; uint32_t lastCtr_ = 0;
    uint64_t rsOkTotal_ = 0, fixedTotal_ = 0, cadusBase_ = 0, rsFailBase_ = 0;
    std::map<int, Channel> ch_;
    bool haveT0_ = false; uint32_t lastMs_ = 0; long lastStrip_ = 0;
    std::vector<int32_t> stripMs_;   ///< ★ per strip: the period's packet time (ms of day), −1 = none heard
    uint64_t packets_ = 0, badPackets_ = 0, blocks_ = 0, version_ = 0;
    int scid_ = -1; uint32_t ctr_ = 0;
};

// ── The gallery's pictures ──────────────────────────────────────────────────────────────────────────────────────────
struct Picture {
    std::string name; int apid = 0; std::string png; int w = 0, h = 0;
    /** ★ V12 the MAP OUTLINE for this pass (vibe_lrpt_overlay.h): the vector JSON (same for every channel of the pass —
     *  they share the line grid), or empty with `geoWhy` saying why there is none. Both empty = not attempted. */
    std::string geoJson, geoWhy;
    /** ★★★ beta96 NORTH UP: 180 = the picture was turned round (a northbound pass comes off the scanner south-up, east on
     *  the left) — said in the PNG's own Comment, its name, and the outline's "rot". 0 = as scanned. Never turned twice:
     *  only renderPictures turns it, once, and everything after reads the turned picture. */
    int rot = 0;
};
/** ★★★ beta96 the note a turned picture carries (PNG tEXt Comment) — what says it was turned, so nothing turns it again. */
constexpr const char* kRotatedNote = "VibeSDR: rotated 180 degrees (northbound pass), north up";
/** Turn a picture (w × h × ch bytes) round 180° in place: the last pixel first. */
inline void rotate180(std::vector<uint8_t>& px, int w, int h, int ch) {
    const size_t n = size_t(w) * size_t(h);
    for (size_t i = 0, j = n - 1; i < j; i++, j--) for (int k = 0; k < ch; k++) std::swap(px[i * ch + k], px[j * ch + k]);
}
/** ★ Channel names per MSU-MR (1: 0.5–0.7 µm … 6: 11.5–12.5 µm); APID 64 + n − 1. Channels 4–6 are thermal and drawn
 *  inverted (cloud white). */
inline const char* channelName(int apid) {
    static const char* n[6] = {"1 visible", "2 near-IR", "3 SWIR", "4 MWIR", "5 thermal", "6 thermal"};
    return apid >= 64 && apid <= 69 ? n[apid - 64] : "?";
}
/** Greyscale per live channel, plus the RGB composite (R = ch2, G = ch2, B = ch1: the familiar false colour) when
 *  channels 1 and 2 are both live. Empty when nothing is worth showing. */
/** `rot180` (★ beta96): a northbound pass — every picture turned round so north is up (Picture::rot). */
inline std::vector<Picture> renderPictures(const Receiver& rx, bool rot180 = false) {
    std::vector<Picture> out;
    int s0, s1; rx.stripRange(s0, s1);
    if (s1 <= s0) return out;
    const auto live = rx.liveApids();
    std::map<int, std::vector<uint8_t>> grey;
    for (int a : live) {
        int w, h;
        auto g = channelImage(rx.channels().at(a), s0, s1, a >= 67, w, h);
        if (g.empty()) continue;
        Picture p; p.apid = a; p.w = w; p.h = h; p.name = std::string("channel ") + channelName(a);
        // ★ turned for its PNG and back (no second copy on a 256 MB board): the composite is built from the pictures as
        //   scanned, then turned once itself
        if (rot180) { rotate180(g, w, h, 1); p.rot = 180; p.name += " \xc2\xb7 north up"; }
        p.png = vibe::png::encode(w, h, 1, g.data(), rot180 ? kRotatedNote : "");
        if (rot180) rotate180(g, w, h, 1);
        out.push_back(std::move(p));
        grey[a] = std::move(g);
    }
    if (grey.count(64) && grey.count(65)) {
        const auto& b = grey[64]; const auto& r = grey[65];
        const int w = kLineWidth, h = int(r.size() / size_t(w));
        std::vector<uint8_t> rgb(size_t(w) * size_t(h) * 3);
        // ★ A block that only ONE channel has is black in the composite, not half-coloured: a lost packet of channel 1
        //   drew a bright yellow stripe across the 2018 pass before this (the picture invents nothing).
        const Channel& c1 = rx.channels().at(64); const Channel& c2 = rx.channels().at(65);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            const int s = s0 + y / 8, col = x / 8;
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            if (!c1.has(s, col) || !c2.has(s, col)) continue;
            rgb[i * 3] = r[i]; rgb[i * 3 + 1] = r[i]; rgb[i * 3 + 2] = b[i];
        }
        Picture p; p.apid = 0; p.w = w; p.h = h; p.name = "RGB 221 false colour";
        if (rot180) { rotate180(rgb, w, h, 3); p.rot = 180; p.name += " \xc2\xb7 north up"; }
        p.png = vibe::png::encode(w, h, 3, rgb.data(), rot180 ? kRotatedNote : "");
        out.insert(out.begin(), std::move(p));
    }
    return out;
}

} // namespace vibelrpt
