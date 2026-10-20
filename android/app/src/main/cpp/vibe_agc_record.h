#pragma once
/**
 * vibe_agc_record.h — the VibeAGC record: what the gain loop did and what it had in hand, kept in memory so a
 * flap can be diagnosed AFTER the fact on a receiver nobody can log into (Stuart's Portable, 2026-10-20: "not
 * sure how much of a tune will trigger VibeAGC" — and no SSH, no journal to read).
 *
 * ★★★ BOUNDED AND NEVER ON DISK. A fixed ring of kAgcRecordCap records of fixed size (~200 bytes), allocated on
 *     the FIRST record — a receiver whose AGC never moves pays nothing — and never more than ~400 KB. The
 *     oldest is overwritten. Nothing is written to storage: a Portable's SD card and a 1 GB phone both matter.
 * ★★ WHAT GOES IN: every gain write (who wrote it and why), every tune (how far, what it forgot, whether the
 *    tuner moved), every forget (its cause), and a one-second snapshot of the loop's inputs — but snapshots
 *    ONLY while the gain is moving and for kAgcSnapAfterSec after any event, so a settled receiver adds
 *    nothing and the ring keeps the interesting minutes.
 * ★★ FLAP COUNTERS: loop moves and direction REVERSALS in the last 60 s. A reversal is the shape of a flap
 *    (up-down-up); a run of moves one way is a loop converging. Published in /vibeserver.json.
 * ★ Pure (no shim types) so vibeserver/test-agc-record.cpp can hold it down.
 */
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vibeagc {

constexpr size_t kAgcRecordCap    = 2000;
constexpr double kAgcSnapAfterSec = 30.0;
constexpr double kAgcFlapWindowSec = 60.0;

enum class RecKind : uint8_t { Gain = 1, Tune = 2, Forget = 3, Snap = 4 };
enum class GainSrc : uint8_t { Loop = 0, Owner, BandSeed, DabSeed, IdlePark, Other };
inline const char* gainSrcName(GainSrc s) {
    switch (s) {
        case GainSrc::Loop: return "vibeagc";   case GainSrc::Owner: return "owner";
        case GainSrc::BandSeed: return "band";  case GainSrc::DabSeed: return "dab";
        case GainSrc::IdlePark: return "idle";  default: return "other";
    }
}

/** The loop's inputs at one moment. Plain numbers; NaN = not known. */
struct AgcSnap {
    float gainDb = NAN, peakDbfs = NAN, peakHeldDbfs = NAN, clipPct = NAN;
    float sepDb = NAN, chanDb = NAN, shoulderDb = NAN, contrastDb = NAN;
    float ifKhz = NAN, fftRate = NAN, badGainDb = NAN;
    int16_t steps = -1, hotRun = -1, cleanRun = -1, badHold = -1, strikes = -1;
    uint8_t settled = 0, hurry = 0, provisional = 0, headroomOnly = 0, sepFromShoulders = 0;
    char profile[14] = {0};
};

struct AgcRec {
    double  t = 0;          // wall clock, epoch seconds
    RecKind kind = RecKind::Snap;
    GainSrc src = GainSrc::Other;
    uint8_t forget = 0;     // tune: 0 none, 1 channel, 2 full
    uint8_t tunerMoved = 0; // tune: the tuner itself had to move
    int16_t fromT = 0, toT = 0;   // gain: tenths of a dB
    double  fromHz = 0, toHz = 0; // tune
    AgcSnap s;              // snap
    char    why[72] = {0};  // gain reason / forget cause / tune mode
};

inline void agcCopyWhy(char* dst, size_t cap, const char* src) {
    if (!src) { dst[0] = 0; return; }
    std::strncpy(dst, src, cap - 1); dst[cap - 1] = 0;
}

class AgcRecord {
public:
    void gain(double t, GainSrc src, int fromT, int toT, const char* why) {
        AgcRec r; r.t = t; r.kind = RecKind::Gain; r.src = src;
        r.fromT = (int16_t)fromT; r.toT = (int16_t)toT; agcCopyWhy(r.why, sizeof r.why, why);
        std::lock_guard<std::mutex> lk(mtx_);
        push(r); lastEventAt_ = t;
        if (src == GainSrc::Loop && toT != fromT) noteMove(t, toT > fromT ? +1 : -1);
    }
    void tune(double t, double fromHz, double toHz, int forget, bool tunerMoved, const char* mode) {
        AgcRec r; r.t = t; r.kind = RecKind::Tune; r.fromHz = fromHz; r.toHz = toHz;
        r.forget = (uint8_t)forget; r.tunerMoved = tunerMoved ? 1 : 0; agcCopyWhy(r.why, sizeof r.why, mode);
        std::lock_guard<std::mutex> lk(mtx_);
        push(r); lastEventAt_ = t;
    }
    void forget(double t, const char* cause) {
        AgcRec r; r.t = t; r.kind = RecKind::Forget; agcCopyWhy(r.why, sizeof r.why, cause);
        std::lock_guard<std::mutex> lk(mtx_);
        push(r); lastEventAt_ = t;
    }
    /** True while snapshots are wanted: within kAgcSnapAfterSec of the last event. */
    bool wantSnap(double t) const {
        std::lock_guard<std::mutex> lk(mtx_);
        return lastEventAt_ > 0 && t - lastEventAt_ <= kAgcSnapAfterSec;
    }
    void snap(double t, const AgcSnap& s) {
        AgcRec r; r.t = t; r.kind = RecKind::Snap; r.s = s;
        std::lock_guard<std::mutex> lk(mtx_);
        push(r);
    }
    /** Loop moves, and direction reversals, in the last kAgcFlapWindowSec. */
    int moves(double t) const {
        std::lock_guard<std::mutex> lk(mtx_);
        int n = 0; for (const auto& m : mv_) if (m.t > 0 && t - m.t <= kAgcFlapWindowSec) ++n; return n;
    }
    int reversals(double t) const {
        std::lock_guard<std::mutex> lk(mtx_);
        int n = 0; for (const auto& m : mv_) if (m.t > 0 && m.reversal && t - m.t <= kAgcFlapWindowSec) ++n; return n;
    }
    size_t size() const { std::lock_guard<std::mutex> lk(mtx_); return n_; }
    size_t bytesReserved() const { std::lock_guard<std::mutex> lk(mtx_); return ring_.capacity() * sizeof(AgcRec); }
    void clear() { std::lock_guard<std::mutex> lk(mtx_); n_ = head_ = 0; lastEventAt_ = 0; mv_ = {}; mvHead_ = 0; lastDir_ = 0; }

    /** The whole ring, oldest first, one JSON object per line (JSONL). The first line describes the dump. */
    std::string jsonl(double now) const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::string out;
        out.reserve(n_ * 160 + 256);
        char b[640];
        std::snprintf(b, sizeof b, "{\"type\":\"agcRecord\",\"now\":%.3f,\"records\":%zu,\"cap\":%zu,"
                      "\"moves60s\":%d,\"reversals60s\":%d}\n", now, n_, kAgcRecordCap,
                      countMovesLocked(now, false), countMovesLocked(now, true));
        out += b;
        const size_t start = (n_ < kAgcRecordCap) ? 0 : head_;
        for (size_t k = 0; k < n_; ++k) {
            const AgcRec& r = ring_[(start + k) % kAgcRecordCap];
            const std::string why = esc(r.why);
            switch (r.kind) {
            case RecKind::Gain:
                std::snprintf(b, sizeof b, "{\"t\":%.3f,\"k\":\"gain\",\"src\":\"%s\",\"from\":%.1f,\"to\":%.1f,\"why\":\"%s\"}\n",
                              r.t, gainSrcName(r.src), r.fromT / 10.0, r.toT / 10.0, why.c_str());
                break;
            case RecKind::Tune:
                std::snprintf(b, sizeof b, "{\"t\":%.3f,\"k\":\"tune\",\"fromHz\":%.0f,\"toHz\":%.0f,\"dHz\":%.0f,"
                              "\"forget\":\"%s\",\"tunerMoved\":%s,\"mode\":\"%s\"}\n",
                              r.t, r.fromHz, r.toHz, r.toHz - r.fromHz,
                              r.forget == 2 ? "full" : r.forget == 1 ? "channel" : "none",
                              r.tunerMoved ? "true" : "false", why.c_str());
                break;
            case RecKind::Forget:
                std::snprintf(b, sizeof b, "{\"t\":%.3f,\"k\":\"forget\",\"why\":\"%s\"}\n", r.t, why.c_str());
                break;
            case RecKind::Snap: {
                const AgcSnap& s = r.s;
                std::snprintf(b, sizeof b,
                    "{\"t\":%.3f,\"k\":\"snap\",\"gain\":%s,\"steps\":%d,\"peak\":%s,\"peakHeld\":%s,\"clip\":%s,"
                    "\"hot\":%d,\"clean\":%d,\"sep\":%s,\"sepRuler\":\"%s\",\"chan\":%s,\"shoulder\":%s,\"contrast\":%s,"
                    "\"ifKhz\":%s,\"fps\":%s,\"settled\":%d,\"hurry\":%d,\"headroomOnly\":%d,\"badGain\":%s,"
                    "\"badHold\":%d,\"provisional\":%d,\"strikes\":%d,\"profile\":\"%s\"}\n",
                    r.t, num(s.gainDb).c_str(), s.steps, num(s.peakDbfs).c_str(), num(s.peakHeldDbfs).c_str(),
                    num(s.clipPct, 4).c_str(), s.hotRun, s.cleanRun, num(s.sepDb).c_str(),
                    s.sepFromShoulders ? "shoulders" : "floor", num(s.chanDb).c_str(), num(s.shoulderDb).c_str(),
                    num(s.contrastDb).c_str(), num(s.ifKhz, 0).c_str(), num(s.fftRate).c_str(), s.settled, s.hurry,
                    s.headroomOnly, num(s.badGainDb).c_str(), s.badHold, s.provisional, s.strikes,
                    esc(s.profile).c_str());
                break; }
            }
            out += b;
        }
        return out;
    }

private:
    struct Move { double t = 0; int8_t dir = 0; bool reversal = false; };
    static constexpr size_t kMoveRing = 64;

    void push(const AgcRec& r) {                       // ★ caller holds mtx_
        if (ring_.empty()) ring_.resize(kAgcRecordCap); // ★ allocated on the first record, never grown
        ring_[head_] = r;
        head_ = (head_ + 1) % kAgcRecordCap;
        if (n_ < kAgcRecordCap) ++n_;
    }
    void noteMove(double t, int dir) {                 // ★ caller holds mtx_
        Move m; m.t = t; m.dir = (int8_t)dir;
        m.reversal = (lastDir_ != 0 && dir != lastDir_ && t - lastMoveAt_ <= kAgcFlapWindowSec);
        mv_[mvHead_] = m; mvHead_ = (mvHead_ + 1) % kMoveRing;
        lastDir_ = dir; lastMoveAt_ = t;
    }
    int countMovesLocked(double t, bool rev) const {
        int n = 0; for (const auto& m : mv_) if (m.t > 0 && (!rev || m.reversal) && t - m.t <= kAgcFlapWindowSec) ++n; return n;
    }
    static std::string num(float v, int dp = 1) {
        if (!std::isfinite(v)) return "null";
        char b[32]; std::snprintf(b, sizeof b, "%.*f", dp, (double)v); return b;
    }
    static std::string esc(const char* s) {
        std::string o;
        for (const char* p = s; *p; ++p) {
            const unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
            else if (c < 0x20) o += ' ';
            else o += (char)c;
        }
        return o;
    }

    mutable std::mutex mtx_;
    std::vector<AgcRec> ring_;
    size_t head_ = 0, n_ = 0;
    double lastEventAt_ = 0;
    std::array<Move, kMoveRing> mv_{};
    size_t mvHead_ = 0;
    int lastDir_ = 0;
    double lastMoveAt_ = 0;
};

}  // namespace vibeagc
