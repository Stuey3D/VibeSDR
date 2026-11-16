// vibe_dab_block_quality.h — ★★ V12 HOW WELL EACH DAB BLOCK DECODES HERE: working / marginal / not working.
//
// ★★★ STUART (2026-11-16, the Almere B210, all of Band III on one screen): 6C and 6D look the same strength on the
//     spectrum, 6C plays and 6D "locked, but nothing decodes" (MER 7 dB, every FIB failing, null depth 3 dB — two
//     multiplexes on one block). A line on the spectrum where decoding "may start to fail" would be wrong for exactly
//     that reason, so each block's label says what this receiver MEASURED there instead: "label working, marginal and
//     non working ones".
//
//  ★ A MEMORY, LIKE THE ENSEMBLE NAMES (local_sdr_shim.cpp g_dabEnsMem): one small record per Band III block — the grade,
//    the MER and FIB rate it was judged on, and WHEN (the clients fade a grade older than a week). Learnt from the live
//    decode of the block a listener is on (BlockGrader, once it has settled) and from the band and DX scans' per-block
//    results. Bounded: at most kBandIIICount (41) records, a few dozen bytes each, in memory and on disk.
//  ★ WRITTEN RARELY: only when a block's grade CHANGES, or to refresh the time of an unchanged grade at most every
//    kRefreshS — never per frame.
//
//  ★★ THE THRESHOLDS — from how this decoder behaves, not from a level:
//    • NO GRADE without a lock: an empty block (or one we never synchronised to) is not "not working", it is nothing.
//    • NOT WORKING: locked, but fewer than half the FIBs pass their CRC (kBadFib). The FIC is the best-protected part of a
//      multiplex (rate 1/3 — every audio sub-channel is protected less), so when half of it fails no service list can be
//      read and no audio survives either; the receiver's own no-FIB watchdog (vibe_dab_service.h, fibRate < 0.05) and
//      the scan's false-lock rule treat the bottom of that range as "no ensemble". Almere 6D: 0 % (12 of 12 failing).
//    • WORKING: at least 90 % of FIBs good (kGoodFib — the same bar the gain memory and the ensemble-name memory already
//      use: "a label half-decoded out of a marginal signal is worse than no label") AND the audio clean: at most 1 % of
//      audio units lost (kGoodAudioLoss — one concealed unit in a hundred is not heard; more is heard as chirps and gaps).
//      With no audio measured (no service chosen yet, a scan's visit) the MER stands in: at least kGoodMerDb (8 dB —
//      where the bench's DAB+ EEP-3A services stop losing super frames; 6C at Almere 16.4 dB, 7D 16 dB).
//    • MARGINAL: everything between — the FIBs mostly read but audio units are lost, or the FIBs pass 50–90 %, or the
//      MER is under 8 dB with no audio to judge by.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace vibedab {

enum class BlockGrade : uint8_t { None = 0, Good, Marginal, Bad };

inline const char* blockGradeName(BlockGrade g) {
    switch (g) { case BlockGrade::Good: return "good"; case BlockGrade::Marginal: return "marginal";
                 case BlockGrade::Bad: return "bad"; default: return ""; }
}
inline BlockGrade blockGradeFrom(const char* s) {
    if (!s) return BlockGrade::None;
    if (!std::strcmp(s, "good")) return BlockGrade::Good;
    if (!std::strcmp(s, "marginal")) return BlockGrade::Marginal;
    if (!std::strcmp(s, "bad")) return BlockGrade::Bad;
    return BlockGrade::None;
}

constexpr float  kBadFib = 0.5f;          // under this, locked = NOT WORKING
constexpr float  kGoodFib = 0.9f;         // at or over this (+ clean audio / MER) = WORKING
constexpr double kGoodAudioLoss = 0.01;   // audio units lost, at most, for WORKING
constexpr float  kGoodMerDb = 8.0f;       // the stand-in when no audio was measured
constexpr uint32_t kMinAudioUnits = 50;   // fewer units than this is no audio measurement (≈ 1 s of DAB+ at 48 kHz)

/** ★ One grade from one measurement — the rule above. `locked` = the receiver held (or reached) a lock in the span. */
inline BlockGrade gradeBlock(bool locked, float fibRate, float merDb, uint32_t audioIn, uint32_t audioBad) {
    if (!locked) return BlockGrade::None;
    if (fibRate < kBadFib) return BlockGrade::Bad;
    if (fibRate < kGoodFib) return BlockGrade::Marginal;
    if (audioIn >= kMinAudioUnits)
        return double(audioBad) / double(audioIn) <= kGoodAudioLoss ? BlockGrade::Good : BlockGrade::Marginal;
    return merDb >= kGoodMerDb ? BlockGrade::Good : BlockGrade::Marginal;
}

struct BlockQ { BlockGrade g = BlockGrade::None; float mer = 0; float fib = 0; int64_t t = 0; };

/** ★★ THE LIVE GRADER: fed the decoder's quality snapshot (vibe_dab_service.h Quality: its block, lock, FIB rate, MER, the
 *  audio counters since the block was entered) as often as the caller likes. It says nothing until the block has SETTLED
 *  (kSettleS after it was entered — the AGC's climb and the first FIC cycles), then judges each kWindowS span on its own
 *  audio (the counters' growth over the span) and the FIB rate / MER at its end, and returns the block and grade when a
 *  span completes. A block change restarts it. */
class BlockGrader {
public:
    static constexpr double kSettleS = 6.0, kWindowS = 10.0;
    struct Verdict { int channel = -1; BlockQ q; };
    /** One snapshot at time `now` (s). True with `out` filled when a span completes with a grade. */
    bool feed(int channel, bool locked, float fibRate, float merDb, uint32_t audioIn, uint32_t audioBad, double now, Verdict& out) {
        if (channel != ch_ || audioIn < in0_ || audioBad < bad0_) {   // a new block (or its counters were reset)
            ch_ = channel; enteredAt_ = now; winAt_ = -1; lockedN_ = n_ = 0; in0_ = audioIn; bad0_ = audioBad;
            return false;
        }
        if (channel < 0 || now - enteredAt_ < kSettleS) { in0_ = audioIn; bad0_ = audioBad; return false; }
        if (winAt_ < 0) { winAt_ = now; in0_ = audioIn; bad0_ = audioBad; lockedN_ = locked; n_ = 1; return false; }
        lockedN_ += locked; ++n_;
        if (now - winAt_ < kWindowS) return false;
        // ★ "locked" = locked for at least half the span: a lock that came and went once (noise can fake one for a moment
        //   before the no-FIB watchdog breaks it) is not a multiplex failing to decode
        const BlockGrade g = gradeBlock(2 * lockedN_ >= n_, fibRate, merDb, audioIn - in0_, audioBad - bad0_);
        winAt_ = now; in0_ = audioIn; bad0_ = audioBad; lockedN_ = locked; n_ = 1;
        if (g == BlockGrade::None) return false;
        out.channel = channel; out.q = BlockQ{ g, merDb, fibRate, 0 };
        return true;
    }
private:
    int ch_ = -2; double enteredAt_ = 0, winAt_ = -1; int lockedN_ = 0, n_ = 0; uint32_t in0_ = 0, bad0_ = 0;
};

/** ★★ THE MEMORY: block index → its last grade. `names[i]` = kBandIII[i].name. Text file, one "6D bad 7.0 0.000 1763300000"
 *  line per block (block, grade, MER dB, FIB rate, unix seconds). */
class BlockQualityMemory {
public:
    static constexpr int64_t kRefreshS = 1800;   // an unchanged grade's time refreshed at most every 30 min
    /** Note a grade at unix time `t`. True when the memory CHANGED (the caller saves it). */
    bool note(int blk, const BlockQ& in, int64_t t) {
        if (blk < 0 || in.g == BlockGrade::None) return false;
        auto it = m_.find(blk);
        if (it != m_.end() && it->second.g == in.g && t - it->second.t < kRefreshS) return false;
        BlockQ q = in; q.t = t;
        m_[blk] = q;
        return true;
    }
    const std::map<int, BlockQ>& all() const { return m_; }
    void clear() { m_.clear(); }
    /** `,"blockQ":{"6D":{"g":"bad","mer":7.0,"fib":0.00,"t":1763300000}}` — "" when empty. */
    template <class NameOf> std::string json(NameOf nameOf, size_t count) const {
        std::string j;
        for (const auto& kv : m_) {
            if (kv.first < 0 || size_t(kv.first) >= count || kv.second.g == BlockGrade::None) continue;
            char b[128];
            std::snprintf(b, sizeof b, "%s\"%s\":{\"g\":\"%s\",\"mer\":%.1f,\"fib\":%.2f,\"t\":%lld}", j.empty() ? "" : ",",
                          nameOf(kv.first), blockGradeName(kv.second.g), kv.second.mer, kv.second.fib, (long long)kv.second.t);
            j += b;
        }
        return j.empty() ? j : ",\"blockQ\":{" + j + "}";
    }
    template <class NameOf> void save(FILE* f, NameOf nameOf, size_t count) const {
        for (const auto& kv : m_)
            if (kv.first >= 0 && size_t(kv.first) < count && kv.second.g != BlockGrade::None)
                std::fprintf(f, "%s %s %.1f %.3f %lld\n", nameOf(kv.first), blockGradeName(kv.second.g),
                             kv.second.mer, kv.second.fib, (long long)kv.second.t);
    }
    /** Read the file back; a line that does not parse (or names no block) is skipped — a corrupt file is not a licence. */
    template <class IndexOf> void load(FILE* f, IndexOf indexOf) {
        char line[160];
        while (std::fgets(line, sizeof line, f)) {
            char name[8] = {}, grade[12] = {}; float mer = 0, fib = 0; long long t = 0;
            if (std::sscanf(line, "%7s %11s %f %f %lld", name, grade, &mer, &fib, &t) != 5) continue;
            const int blk = indexOf(name);
            const BlockGrade g = blockGradeFrom(grade);
            if (blk < 0 || g == BlockGrade::None || !(mer > -50 && mer < 80) || !(fib >= 0 && fib <= 1) || t < 0) continue;
            m_[blk] = BlockQ{ g, mer, fib, int64_t(t) };
            if (m_.size() > 64) break;   // ★ bounded whatever the file says
        }
    }
private:
    std::map<int, BlockQ> m_;
};

}  // namespace vibedab
