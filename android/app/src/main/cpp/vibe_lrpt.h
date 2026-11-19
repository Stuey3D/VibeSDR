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
//    (a second sync searcher costs a few % of a core); the first to pass Reed–Solomon wins and the other stops. QPSK vs
//    OQPSK changes the demodulator, so AUTO alternates them every kModeSwapSec while neither has produced a frame.
//    Every one of those can be pinned by the owner (the manual fallback).
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
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
        process();
    }
    Stats& stats() { return st_; }
    bool locked() const { return locked_; }
    /** ★ Reed–Solomon passed within the last 8 CADUs — the only proof the carrier is really locked (an ASM can be "found"
     *  with the carrier 400 Hz out at Es/N0 3 dB; holding the acquisition on that kept it out for the whole run). */
    bool decoding() const { return sinceGood_ < 8; }
    /** ★ Test seam: each CADU's ASM score, ambiguity, Viterbi metric and the four RS words' results. */
    std::function<void(int, int, int, const int*)> caduDiag;
    void reset() { buf_.clear(); locked_ = false; scan_ = 0; misses_ = 0; }

private:
    static constexpr int kLead = 128, kTail = 128;               // soft bits each side (64 Viterbi steps)
    static constexpr int kFind = 46, kKeep = 38;                 // of 52 fixed coded ASM bits
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
                    buf_.erase(buf_.begin(), buf_.end() - long(keep));
                    scan_ = keep >= 64 ? keep - 64 + 1 : 0;
                    if (scan_ > buf_.size()) scan_ = 0;
                    return;
                }
                pos_ = p; locked_ = true; misses_ = 0;
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
            if (best >= kKeep) { pos_ = bp; x_ = bx; misses_ = 0; }
            else if (++misses_ > 3) { locked_ = false; st_.syncLost++; pos_ = want; }
            else pos_ = want;                                   // flywheel: RS judges it
            // trim: keep kLead bits before the next position
            const size_t cut = pos_ > size_t(kLead) ? pos_ - size_t(kLead) : 0;
            buf_.erase(buf_.begin(), buf_.begin() + long(cut));
            pos_ -= cut;
            scan_ = locked_ ? 0 : pos_;
        }
    }
    void decodeAt(size_t p) {
        const int nSoft = kLead + kCaduSoft + kTail;
        soft_.assign(size_t(nSoft), 0);
        const long start = long(p) - kLead;
        for (int i = 0; i + 1 < nSoft; i += 2) {
            const long a = start + i;
            if (a < 0 || size_t(a + 1) >= buf_.size()) continue;
            int oa, ob; applyX(x_, buf_[size_t(a)], buf_[size_t(a + 1)], oa, ob);
            soft_[size_t(i)] = int8_t(std::max(-127, std::min(127, oa)));
            soft_[size_t(i + 1)] = int8_t(std::max(-127, std::min(127, ob)));
        }
        bits_.resize(kCaduBits);
        st_.lastVit = vit_.decode(soft_.data(), nSoft / 2, kLead / 2, kCaduBits, bits_.data());
        uint8_t cadu[kCaduBytes];
        for (int i = 0; i < kCaduBytes; i++) {
            uint8_t b = 0;
            for (int j = 0; j < 8; j++) b = uint8_t((b << 1) | bits_[size_t(i * 8 + j)]);
            cadu[i] = b;
        }
        st_.cadus++;
        uint8_t* f = cadu + kAsmBytes;
        derandomise(f);
        int pw[4];
        const int e = rsFrame(f, pw);
        if (caduDiag) caduDiag(syncScore(windowAt(p), x_), x_, st_.lastVit, pw);
        if (e < 0) { st_.rsFail++; sinceGood_++; return; }
        sinceGood_ = 0;
        st_.rsOk++; st_.rsFixed += uint64_t(e);
        fn_(f, e);
    }

    VcduFn fn_;
    Viterbi vit_;
    std::vector<int8_t> buf_, soft_;
    std::vector<uint8_t> bits_;
    size_t scan_ = 0, pos_ = 0;
    int x_ = 0, misses_ = 0;
    uint64_t sinceGood_ = 1000;
    bool locked_ = false;
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
    bool forceScalar = false;                 ///< ★ tests: the A/B against the SIMD paths
};

/** ── The whole receiver. Feed IQ; pictures grow as the pass goes. ─────────────────────────────────────────────── */
class Receiver {
public:
    static constexpr double kModeSwapSec = 8.0;
    static constexpr double kPeriodMs = 1232.0;      ///< MSU-MR: eight lines every 1.232 s
    explicit Receiver(const ReceiverConfig& c) : cfg_(c) {
        curMod_ = c.mod == ReceiverConfig::Oqpsk ? 1 : 0;
        if (c.mod == ReceiverConfig::Auto && c.symbolRate >= 76000) curMod_ = 1;   // ★ 80k has only ever been OQPSK
        makeDemod();
        for (int k = 0; k < 2; k++) {
            const bool diffOn = k == 1;
            if (c.diff == ReceiverConfig::DiffOff && diffOn) continue;
            if (c.diff == ReceiverConfig::DiffOn && !diffOn) continue;
            Path p;
            p.diff = diffOn;
            p.sync = std::make_unique<FrameSync>([this, k](const uint8_t* v, int e) { onVcdu(k, v, e); });
            p.sync->setScalar(c.forceScalar);
            paths_[k] = std::move(p);
        }
        asm_ = std::make_unique<PacketAssembler>([this](const uint8_t* p, size_t n) { onPacket(p, n); });
    }
    /** ★ Test seam: every image packet's APID, sequence count, MCU number, time (ms of day) and quality factor. */
    std::function<void(int, int, int, uint32_t, int)> diag;
    void setCaduDiag(std::function<void(int, int, int, const int*)> f) { for (auto& p : paths_) if (p.sync) p.sync->caduDiag = f; }
    double carrierHz() const { return demod_->carrierHz(); }
    void setDopplerHz(double hz) { if (demod_) demod_->setDopplerHz(hz); dop_ = hz; }

    /** Feed interleaved float IQ. */
    void feedIq(const float* iq, size_t n) {
        soft_.clear();
        demod_->setHold(decoding());
        demod_->feed(iq, n, soft_);
        samples_ += n;
        feedSoft(soft_.data(), soft_.size());
        // ── AUTO modulation: swap QPSK ↔ OQPSK while nothing has decoded ──
        if (cfg_.mod == ReceiverConfig::Auto && !anyFrame_ && samples_ - swapAt_ > uint64_t(kModeSwapSec * cfg_.sampleRate)) {
            swapAt_ = samples_; curMod_ ^= 1; makeDemod();
            for (auto& p : paths_) if (p.sync) p.sync->reset();
        }
    }
    /** Feed soft symbols directly (an archived .s file, or the client-assisted stage). */
    void feedSoft(const int8_t* s, size_t n) {
        const int8_t* src = s; size_t len = n;
        if (cfg_.interleaved) { deint_.clear(); front_.push(s, n, deint_); src = deint_.data(); len = deint_.size(); }
        for (int k = 0; k < 2; k++) {
            Path& p = paths_[k];
            if (!p.sync || (winner_ >= 0 && winner_ != k)) continue;
            if (p.diff) { tmp_.assign(src, src + len); p.dd.run(tmp_.data(), tmp_.size()); p.sync->push(tmp_.data(), tmp_.size()); }
            else p.sync->push(src, len);
        }
    }

    // ── Results ──
    const std::map<int, Channel>& channels() const { return ch_; }
    Stats stats() const {
        Stats s;
        for (auto& p : paths_) if (p.sync) { const Stats& t = p.sync->stats(); s.cadus = std::max(s.cadus, t.cadus); s.rsOk += t.rsOk; s.rsFail = std::max(s.rsFail, t.rsFail); s.rsFixed += t.rsFixed; s.syncLost += t.syncLost; s.lastVit = t.lastVit ? t.lastVit : s.lastVit; }
        s.packets = packets_; s.badPackets = badPackets_; s.blocks = blocks_; s.scid = scid_; s.vcduCounter = ctr_;
        return s;
    }
    bool demodLocked() const { return demod_->locked(); }
    double lockMetric() const { return demod_->lockMetric(); }
    bool syncLocked() const { for (auto& p : paths_) if (p.sync && p.sync->locked()) return true; return false; }
    bool decoding() const { for (auto& p : paths_) if (p.sync && p.sync->decoding()) return true; return false; }
    const char* modeName() const { return curMod_ ? "OQPSK" : "QPSK"; }
    int diffWinner() const { return winner_ < 0 ? -1 : paths_[winner_].diff ? 1 : 0; }
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
    void makeDemod() {
        DemodConfig d; d.sampleRate = cfg_.sampleRate; d.symbolRate = cfg_.symbolRate; d.oqpsk = curMod_ == 1;
        demod_ = std::make_unique<Demod>(d);
        demod_->fir().forceScalar = cfg_.forceScalar;
        demod_->setDopplerHz(dop_);
    }
    void onVcdu(int k, const uint8_t* v, int) {
        if ((v[0] >> 6) != 1) return;                        // transfer frame version 2 ("01")
        const int vcid = v[1] & 0x3F;
        if (vcid == 63) return;                              // fill
        if (winner_ < 0) winner_ = k;
        anyFrame_ = true;
        scid_ = ((v[0] & 0x3F) << 2) | (v[1] >> 6);
        ctr_ = (uint32_t(v[2]) << 16) | (uint32_t(v[3]) << 8) | v[4];
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
        if (ch_.size() >= size_t(kMaxChannels) && !ch_.count(apid)) return;
        uint8_t blocks[kMcuPerPacket][64];
        const int good = decodeMcus(d + 6, n - 6 - 8 - 6, q, blocks);
        auto& ch = ch_.emplace(apid, Channel(apid)).first->second;
        if (ch.put(int(strip), mcu, blocks, good)) { blocks_ += uint64_t(good); version_++; }
    }

    ReceiverConfig cfg_;
    int curMod_ = 0;
    std::unique_ptr<Demod> demod_;
    double dop_ = 0;
    Path paths_[2];
    InterleaveFront front_;
    std::unique_ptr<PacketAssembler> asm_;
    std::vector<int8_t> soft_, deint_, tmp_;
    uint64_t samples_ = 0, swapAt_ = 0;
    bool anyFrame_ = false;
    int winner_ = -1;
    std::map<int, Channel> ch_;
    bool haveT0_ = false; uint32_t lastMs_ = 0; long lastStrip_ = 0;
    uint64_t packets_ = 0, badPackets_ = 0, blocks_ = 0, version_ = 0;
    int scid_ = -1; uint32_t ctr_ = 0;
};

// ── The gallery's pictures ──────────────────────────────────────────────────────────────────────────────────────────
struct Picture { std::string name; int apid = 0; std::string png; int w = 0, h = 0; };
/** ★ Channel names per MSU-MR (1: 0.5–0.7 µm … 6: 11.5–12.5 µm); APID 64 + n − 1. Channels 4–6 are thermal and drawn
 *  inverted (cloud white). */
inline const char* channelName(int apid) {
    static const char* n[6] = {"1 visible", "2 near-IR", "3 SWIR", "4 MWIR", "5 thermal", "6 thermal"};
    return apid >= 64 && apid <= 69 ? n[apid - 64] : "?";
}
/** Greyscale per live channel, plus the RGB composite (R = ch2, G = ch2, B = ch1: the familiar false colour) when
 *  channels 1 and 2 are both live. Empty when nothing is worth showing. */
inline std::vector<Picture> renderPictures(const Receiver& rx) {
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
        p.png = vibe::png::encode(w, h, 1, g.data());
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
        p.png = vibe::png::encode(w, h, 3, rgb.data());
        out.insert(out.begin(), std::move(p));
    }
    return out;
}

} // namespace vibelrpt
