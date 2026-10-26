// vibe_dab_confirm.h — WHEN IS A MULTIPLEX'S SERVICE LIST TRUE ENOUGH TO DELETE BY? (V12, 2026-10-26)
//
// Stuart: "DAB multiplexes learn every time you tune to them so that a stale Radio X 90's cannot linger." In Northampton
// Radio X 90s was swapped for Radio X Classic Rock in the same slot; the learned bookmark for the old service then sat
// there pointing at nothing. So when the server is on an ensemble and HOLDS ITS WHOLE SERVICE LIST, the bookmarks for
// that block are reconciled: present services added or refreshed, learned/scanned ones that are gone removed.
//
// ★★★ DELETING ON A PARTIAL LIST IS THE FAULT TO FEAR, not the stale bookmark. A weak or flapping signal decodes some
//     FIGs and not others; the MCI can look complete for a moment with a service missing because its FIG 0/2 has not
//     come round yet. So a list is CONFIRMED only when, for the same block and the same ensemble id:
//       - every sample is MCI-complete (the receiver's own judgement, TS 103 176 6.3.3) with a non-zero EId,
//       - every sample is at the memories' quality bar — FIB rate above 0.9, as the gain and ensemble memories use,
//       - the SET of service ids is IDENTICAL in every sample,
//       - for at least kMinSamples samples spanning at least kMinSecs (several complete FIC repetition cycles — the
//         FIG 0/2 for every service repeats well inside a second on a real multiplex; 4 s is many cycles).
//     Anything else — a different set, a dip below the bar, a block or ensemble change, an incomplete MCI — starts the
//     count again. Header-only, no radio, so the rule is pinned down by scripts/test-dab-confirm.cpp.
#pragma once

#include <cstdint>
#include <set>
#include <vector>

namespace vibedab {

class ServiceListConfirm {
public:
    static constexpr int    kMinSamples = 4;
    static constexpr double kMinSecs    = 4.0;
    static constexpr float  kFibBar     = 0.9f;

    /** Feed one sample. @return true when the list is CONFIRMED (and stays true while every further sample agrees). */
    bool feed(int block, uint32_t eid, bool mciComplete, float fibRate, const std::set<uint32_t>& sids, double nowSecs) {
        const bool good = block >= 0 && eid != 0 && mciComplete && fibRate > kFibBar && !sids.empty();
        if (!good) { reset(); return false; }
        if (n_ == 0 || block != block_ || eid != eid_ || sids != sids_) {
            block_ = block; eid_ = eid; sids_ = sids; n_ = 1; since_ = last_ = nowSecs;
            return false;
        }
        ++n_;
        last_ = nowSecs;
        return confirmed();
    }
    bool confirmed() const { return n_ >= kMinSamples && last_ - since_ >= kMinSecs; }
    void reset() { n_ = 0; block_ = -1; eid_ = 0; sids_.clear(); }
    int block() const { return block_; }
    uint32_t eid() const { return eid_; }
    const std::set<uint32_t>& sids() const { return sids_; }

private:
    int block_ = -1;
    uint32_t eid_ = 0;
    std::set<uint32_t> sids_;
    int n_ = 0;
    double since_ = 0.0, last_ = 0.0;
};

/* ★★★ WHAT A CONFIRMED LIST MAY DO TO THE BOOKMARKS (Stuart, 2026-10-26: "a weak multiplex causing issues, especially if
 *  one day you see all stations then another day you see hardly any on the same multiplex"). Even a confirmed list is
 *  one visit's view, so a removal needs TWO:
 *    - a saved learned/scanned service absent from a confirmed list is MARKED (missedAt = now) — nothing is removed;
 *    - it is REMOVED only when a confirmed list, at least kRemoveAfterS after that mark, still lacks it;
 *    - any sighting in between clears the mark (the shim's bmLearnDab / bmScanDab do that — the service was heard);
 *    - a SUSPECT visit — more than half of the saved services of this ensemble missing at once — marks and removes
 *      nothing at all (the caller adds/refreshes what it saw and logs it): that is the weak day, not a re-plan;
 *    - a row saved by hand is never marked or removed.
 *  The list itself is the ensemble's ANNOUNCED service organisation (FIG 0/2 with its FIG 1/1 labels — the decoder's
 *  learnable()), never "the services whose audio decoded", which shrinks with the signal. Pure: test-dab-confirm.cpp. */
inline constexpr long long kRemoveAfterS = 6LL * 3600;
struct ReconRow { uint32_t sid; bool manual; long long missedAt; };
enum class ReconAct { Keep, ClearMissed, MarkMissed, Remove };
struct ReconPlan { bool suspect = false; int missing = 0; std::vector<ReconAct> acts; };
inline ReconPlan reconcilePlan(const std::vector<ReconRow>& rows, const std::set<uint32_t>& sids, long long now) {
    ReconPlan p;
    p.acts.assign(rows.size(), ReconAct::Keep);
    for (const auto& r : rows) if (!sids.count(r.sid)) ++p.missing;
    p.suspect = !rows.empty() && p.missing * 2 > int(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const ReconRow& r = rows[i];
        if (sids.count(r.sid)) { if (r.missedAt) p.acts[i] = ReconAct::ClearMissed; continue; }
        if (r.manual || p.suspect) continue;
        if (!r.missedAt) p.acts[i] = ReconAct::MarkMissed;
        else if (now - r.missedAt >= kRemoveAfterS) p.acts[i] = ReconAct::Remove;
    }
    return p;
}

}  // namespace vibedab
