// vibe_dab_tii.h — Transmitter Identification Information: WHICH transmitter(s) we are hearing.
//
// ★★★ WHAT THIS IS FOR. EN 300 401 V2.2.1 clause 14.8. In an SFN every transmitter radiates the
//     same ensemble, so nothing in the FIC says which one reached the aerial. The null symbol
//     does: each transmitter switches on a sparse set of carrier PAIRS during the null of every
//     frame whose CIF count is 0..3 modulo 8, and the set encodes its Main Id (pattern p, 0..69)
//     and Sub Id (comb c, 0..23). Stuart's brief: "TII, which tells a DX-er WHICH transmitter
//     they have. Nobody in the list above surfaces this well and it is exactly our audience."
//
// The layout (14.8.1, mode I): four sections of 384 carriers; within each, comb c and bit b of
// the pattern select carrier k = base + 2c + 48b (base = -768, -384, 1, 385) and its neighbour
// k+1. Both carriers of a pair carry the PRS phase of the FIRST (z_k = A(k)e^{j phi_k} +
// A(k-1)e^{j phi_(k-1)}), so the product null[k] x conj(null[k+1]) is a real positive number for
// a transmitted pair and noise-like otherwise — coherent across the four sections, and coherent
// across frames, so it can be averaged for weak transmitters. That product is also invariant to
// the linear phase ramp any window position inside the null puts across the carriers, which is
// why the null need not be timed exactly.
//
// Table 26 (the patterns) is the seventy 8-bit values with exactly four ones, in ascending order
// — generated here and pinned against the printed table by test-dab-tii.
//
// ★ The approach — pair products, section sum, per-comb ranking, pattern lookup — is the one every
//   open decoder converges on (welle.io, dab-cmdline); this is our own implementation of it.
//
// ★★★ WHY 11D AND BEDS/BUCKS/HERTS (10D) IDENTIFIED NOTHING — measured 2026-11-04 on the Pi 500's
//     V4L, captures in ~/VibeSDR-test-audio/dab/tii. The TII was there all along; two things hid it:
//   1. THE SECOND CARRIER'S PHASE. The product above assumes both carriers of a pair carry phi_k.
//      Daventry (11D, 23/2), Bow Brickhill and Sandy Heath (10D, 22/9 and 22/3) radiate each carrier
//      of a pair with ITS OWN reference phase, phi_k and phi_(k+1): with each carrier's PRS phase taken
//      out, the four sections add up to within six degrees; without, the product lands on +1, -1 or
//      +-j of that depending on the pair, so a transmitter 7800 sigma clear summed NEGATIVE on two of
//      its four blocks and its comb was rejected. 12D carries both conventions (one site each way), so
//      every block is measured BOTH ways — the second is the same products, each turned by
//      conj(z_k) z_(k+1), a multiple of a quarter turn: no extra transform — and the larger is kept.
//   2. THE NOISE REFERENCE. "Odd carriers" are not TII-free: every carrier belongs to some pair slot
//      (24 combs x 8 blocks x 2 = 384, a whole section), and the odd ones include the FIRST carrier of
//      every pair in sections three and four. On a strong multiplex the reference was the TII itself
//      — 5.3 against a true noise of 0.09 on 11D, 18 dB high — burying every weaker site. The new path
//      takes the noise as the lower quartile of the null's carrier powers (an exponential's 25th
//      percentile is 0.288 of its mean): a TII would have to light three quarters of the band to move it.
//   ★ The original test (plain product, odd-carrier reference) still runs FIRST and unchanged: every
//     comb it identified it still identifies, with the same figure. The new path sees only its rejects.
// ★★ COLLISIONS (QIRX: qirx.softsyst.com/QIRXTIICollisions). A dense SFN reuses a Sub Id under
//    different Main Ids — around Northamptonshire 20 of 11D's 23 nearby Sub Ids are shared — so a comb
//    can light 5..8 blocks and no single 4-of-8 pattern fits. The detector does not guess: it hands such
//    a comb out as a TiiShared (which blocks are lit, and how strongly), and tiiResolveShared() keeps
//    only the patterns made entirely of lit blocks. With a transmitter directory for the ensemble, only
//    codes that really exist there survive (kTiiResolved); without one each fitting pattern is reported
//    as a CANDIDATE (kTiiCandidate) — "one of", never one confident wrong name. The directory stays out
//    of this header: the caller passes `known(main, sub)`.
// ★★ LEAKAGE. A very strong transmitter spills into the same blocks of the neighbouring combs (two
//    carriers away): ~1 % of Daventry's 7800 sigma at the Pi 500 — 66 to 96 sigma, far above the noise.
//    On the new path a block counts as lit only if it also stands above kTiiLeak of the same block one or
//    two combs either side. That costs a site more than ~18 dB under its neighbour comb's same block; it
//    is what stops Daventry's spill being named as a transmitter.
// ★ CPU: the second measure re-weights the same 768 products by a precomputed unit factor, and the
//   noise quartile is one nth_element over 1536 floats — per TII frame, about five a second. Nothing here
//   touches the audio or the FIC: it is a side output of a null the receiver has already transformed.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "vibe_dab_prs.h"

namespace vibedab {

using TiiC32 = std::complex<float>;

/** Table 26, generated: pattern p -> 8-bit a_p(b) with b = 0 the MSB, exactly four bits set. */
inline uint8_t tiiPattern(int p) {
    static uint8_t table[70] = {0};
    static bool built = false;
    if (!built) {
        int n = 0;
        for (int v = 0; v < 256 && n < 70; ++v)
            if (__builtin_popcount(unsigned(v)) == 4) table[n++] = uint8_t(v);
        built = true;
    }
    return (p >= 0 && p < 70) ? table[p] : 0;
}
/** The inverse: an 8-bit selection with four ones -> pattern number, or -1. */
inline int tiiPatternIndex(uint8_t bits) {
    if (__builtin_popcount(unsigned(bits)) != 4) return -1;
    for (int p = 0; p < 70; ++p) if (tiiPattern(p) == bits) return p;
    return -1;
}

/** Carrier index k (-768..768, 0 unused) -> position in our K = 1536 carrier array. */
inline int tiiCarrierPos(int k) { return k < 0 ? k + 768 : k + 767; }

/** The first carrier of pair (section s, comb c, bit b). */
inline int tiiPairCarrier(int section, int c, int b) {
    static const int base[4] = { -768, -384, 1, 385 };
    return base[section & 3] + 2 * c + 48 * b;
}

/** How a hit was reached — `"c"` in the report (absent for kTiiClean). */
enum : uint8_t {
    kTiiClean     = 0,   ///< a clean 4-of-8 comb: a measurement on its own
    kTiiResolved  = 1,   ///< from a shared comb, and the directory has a site with this code here
    kTiiCandidate = 2,   ///< from a shared comb, no directory for the ensemble: ONE OF its candidates
};

struct TiiHit {
    int     mainId   = -1;     ///< pattern p
    int     subId    = -1;     ///< comb c
    float   strength = 0.0f;   ///< the pair energy of this transmitter over the null's noise, in dB
    uint8_t from     = kTiiClean;
};

/** ★ A comb with MORE than four blocks lit — several transmitters on one Sub Id (a collision).
 *  `v` is each block's pair energy and `noise` the null's carrier noise power, in the same units. */
struct TiiShared {
    int     subId  = -1;
    uint8_t active = 0;          ///< the lit blocks, bit 0x80 >> b as in tiiPattern
    float   v[8]   = {0};
    float   noise  = 0;
    float   sigma  = 0;          ///< the noise of one averaged block, same units (the lit bar's sigma)
    bool    alongside = false;   ///< this comb ALSO gave a measured (clean) hit
};

/** ★★★ WHY "≈ SCALLOWAY, 541 mi" (Stuart, 2026-11-13: "unrealistic transmitter IDs on the other
 *  multiplexes"). The first resolver named EVERY directory code whose four blocks were all lit. Beside a
 *  clean site that is almost any code on the same Sub Id: Daventry 23/2 lights four blocks, one more block
 *  lit by anything at all makes C(5,4) = 5 patterns fit, two more make 15 — and the UK reuses Sub Id 2 on
 *  dozens of sites, so 12B (BBC National) named Bilsdale, Felixstowe, Kirkconnel, Jedburgh and Scalloway
 *  at 1.0–1.9 dB, and 11A seven more. Three in the same handful of blocks cannot all be on the air; at
 *  most one of them explains the extra block. So a shared comb is now resolved as an EXPLANATION:
 *   1. the blocks the comb's clean hit already accounts for are spoken for; only the REST need a site;
 *   2. a block counts as evidence of a second site only if it stands kTiiSharedSigma clear (twice the
 *      lit bar — a block barely over the noise is not a transmitter, and it was all the 1 dB names had);
 *   3. the fewest directory codes that cover the rest, each adding a block of its own — one extra block,
 *      ONE site; and where several sets cover it equally, the nearest (cost = distance) is the one named;
 *   4. a genuine tie (no position to choose by, or no directory) is reported as candidates, never resolved.
 *  The collision it was built for — Wellingborough 23/6 + Aylesbury 22/6, no clean hit, five blocks — is the
 *  same answer as before: two codes, the only cover of the five. */
static constexpr float kTiiSharedSigma = 8.0f;
/** ★★ AND CLEAR OF ITS OWN SITE'S SPILL (2026-11-13, 11A on the Pi 500's V4L): Daventry 23/2 at ~34 dB lights its own
 *  four blocks of Sub 2 and leaves a steady +2..+4 dB in two of the other four, every verdict — ~30 dB down, not noise.
 *  Over the 8-sigma bar (+3 dB) about half the time, it named Bilsdale 59/2 (blocks 0,1,4,6: one spill block and three of
 *  Daventry's) in 15 of 31 verdicts. The guard the detector already applies across neighbouring combs (kTiiLeak, −18 dB)
 *  now applies inside a comb with a measured site too, at −24 dB: a genuine second site on the same Sub Id up to 24 dB
 *  under the first still counts. */
static constexpr float kTiiSelfLeak = 0.004f;

/** ★★ Resolve shared combs into hits (see above). `known(main, sub)`: does the transmitter directory hold a
 *  site with this code on this ensemble; `haveDirectory`: does it hold ANY TII code for the ensemble (when
 *  false, every fitting pattern is weighed, as candidates); `cost(main, sub)`: how unlikely that site is —
 *  its distance in km, 0 when unknown — used only to choose between equally small explanations.
 *  Appends to `hits`, then orders the lot — measured and resolved first, strongest first, candidates
 *  after — and caps it at `cap`. The strength of a collision hit is its WEAKEST block. */
template <class Known, class Cost>
inline void tiiResolveShared(const std::vector<TiiShared>& shared, bool haveDirectory, Known known, Cost cost,
                             std::vector<TiiHit>& hits, size_t cap = 8) {
    const size_t measured = hits.size();
    for (const auto& sc : shared) {
        if (sc.alongside && !haveDirectory) continue;   // a measured site already speaks for this Sub Id
        // 1. what the clean hit(s) on this Sub Id already explain
        uint8_t spoken = 0;
        for (size_t i = 0; i < measured; ++i)
            if (hits[i].subId == sc.subId && hits[i].from == kTiiClean) spoken |= tiiPattern(hits[i].mainId);
        // 2. the rest: lit, not spoken for, and clear enough to be a transmitter — of the noise, and of the comb's own
        //    measured site's spill (kTiiSelfLeak)
        float spokenMax = 0.0f;
        for (int b = 0; b < 8; ++b) if (spoken & (0x80 >> b)) spokenMax = std::max(spokenMax, sc.v[b]);
        const float bar = std::max(kTiiSharedSigma * sc.sigma, kTiiSelfLeak * spokenMax);
        uint8_t rest = 0;
        for (int b = 0; b < 8; ++b) {
            const uint8_t m = uint8_t(0x80 >> b);
            if ((sc.active & m) && !(spoken & m) && sc.v[b] > bar) rest |= m;
        }
        if (!rest) continue;
        const uint8_t usable = uint8_t(spoken | rest);
        // the codes that could be the second site: wholly in usable blocks, adding at least one of the rest
        int cand[70]; float cc[70]; int nc = 0;
        for (int p = 0; p < 70; ++p) {
            const uint8_t bits = tiiPattern(p);
            if ((bits & usable) != bits || !(bits & rest)) continue;
            if (haveDirectory && !known(p, sc.subId)) continue;
            bool dup = false;
            for (size_t i = 0; i < measured; ++i) if (hits[i].mainId == p && hits[i].subId == sc.subId) dup = true;
            if (dup) continue;
            cand[nc] = p; cc[nc] = float(cost(p, sc.subId)); ++nc;
        }
        if (!nc) continue;
        // 3. the smallest covers of `rest` (up to three codes), the cheapest of them
        float best = 1e30f; uint64_t pick[2] = {0, 0}; bool tie = false; bool found = false;
        auto mark = [](uint64_t* set, int i) { set[i >> 6] |= uint64_t(1) << (i & 63); };
        for (int k = 1; k <= 3 && !found; ++k) {
            int ix[3] = {0, 1, 2};
            if (k > nc) break;
            for (;;) {
                uint8_t u = 0; float c = 0;
                for (int j = 0; j < k; ++j) { u |= tiiPattern(cand[ix[j]]); c += cc[ix[j]]; }
                if ((u & rest) == rest) {
                    // every code must add a block of the rest no other code in the set covers
                    bool minimal = true;
                    for (int j = 0; j < k && minimal; ++j) {
                        uint8_t others = 0;
                        for (int q = 0; q < k; ++q) if (q != j) others |= tiiPattern(cand[ix[q]]);
                        if (!(tiiPattern(cand[ix[j]]) & rest & uint8_t(~others))) minimal = false;
                    }
                    if (minimal) {
                        found = true;
                        if (c < best - 1e-3f) { best = c; pick[0] = pick[1] = 0; tie = false; for (int j = 0; j < k; ++j) mark(pick, ix[j]); }
                        else if (c <= best + 1e-3f) { tie = true; for (int j = 0; j < k; ++j) mark(pick, ix[j]); }
                    }
                }
                int j = k - 1;
                while (j >= 0 && ix[j] == nc - k + j) --j;
                if (j < 0) break;
                ++ix[j];
                for (int q = j + 1; q < k; ++q) ix[q] = ix[q - 1] + 1;
            }
        }
        if (!found) continue;
        // 4. a single cheapest explanation is resolved; a tie, or no directory, stays a candidate
        const uint8_t from = (haveDirectory && !tie) ? kTiiResolved : kTiiCandidate;
        for (int i = 0; i < nc; ++i) {
            if (!(pick[i >> 6] & (uint64_t(1) << (i & 63)))) continue;
            const uint8_t bits = tiiPattern(cand[i]);
            float weakest = 1e30f;
            for (int b = 0; b < 8; ++b) if (bits & (0x80 >> b)) weakest = std::min(weakest, sc.v[b]);
            TiiHit h; h.mainId = cand[i]; h.subId = sc.subId; h.from = from;
            h.strength = 10.0f * std::log10(std::max(weakest, 1e-20f) / (sc.noise + 1e-20f));
            hits.push_back(h);
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const TiiHit& a, const TiiHit& b) {
        const bool ca = a.from == kTiiCandidate, cb = b.from == kTiiCandidate;
        if (ca != cb) return cb;
        return a.strength > b.strength;
    });
    if (hits.size() > cap) hits.resize(cap);
}
/** Without a way to weigh sites: every equally small explanation is a tie. */
template <class Known>
inline void tiiResolveShared(const std::vector<TiiShared>& shared, bool haveDirectory, Known known,
                             std::vector<TiiHit>& hits, size_t cap = 8) {
    tiiResolveShared(shared, haveDirectory, known, [](int, int) { return 0.0; }, hits, cap);
}

/** ★★★ A TRANSMITTER IS ONE THAT COMES BACK — AND ONE THAT GOES AWAY (Stuart, 2026-11-13, 7D NNDAB: "that list is
 *  constantly growing"; 2026-11-15, the Almere B210 on 7D MTVNL: ~40 lines, nearly all "signal lost").
 *  One verdict is ~0.8 s of null symbols. The 11-13 rule listed a code once it was in 2 of the last 3 verdicts, and the
 *  panels then remembered every code they were ever shown for the life of the multiplex. In a big SFN that let through
 *  codes that held for a couple of seconds — 0D/06, then 2B/06, then 02/06 (hex, as the panels write them), one at a
 *  time at 13–15 dB, each "signal
 *  lost" for ever after. Three things now, all here on the server so every client gets the same list:
 *
 *   1. CONFIRMED BEFORE LISTED. A code is listed once it is in kTiiTrackConfirm of the last kTiiTrackWindow verdicts
 *      (10 of 32: the window is ~25 s). From the captures (dab/tii, 2026-11-13: 11A, 12B, 7D, 11D, 10D, 32 verdicts each):
 *      every real site but the weakest is in 30–32 of 32 — Daventry 23/2, Northampton 23/15, Bow Brickhill 22/9, Sandy
 *      Heath 22/3, 1/5 at NNDAB — and Sutton Coldfield 19/18 (real, 1–3 dB) in 16; the only stray (23/5, one verdict of
 *      12B, on a frame where every figure jumped 4 dB) is in 1. So a steady site is listed ~8 s after the block is
 *      tuned (it was ~1.6 s — the trade, stated), a site in half the verdicts in ~16 s, and a code must hold for 10
 *      verdicts of 32 before a panel ever shows it: a few-second flicker never appears. Bow Brickhill on 12B (2–6 dB,
 *      8 of 31) and Zouches Farm 22/14 on 10D (3 of 32) stay unlisted on 25 s; they are at the edge.
 *   2. SAME-COMB MISREADINGS. Table 26's patterns are the 70 bytes with four ones; a weak site whose comb loses one of
 *      its four blocks under a noise peak is read as the pattern with that block swapped — another Main Id sharing
 *      THREE blocks with it. The 15th's three are exactly that: 2B (1001 1010) and 02 (0001 1011) each share three of
 *      0D's four (0011 1010) on Sub Id 06. So a code that is not confirmed, on the Sub Id of a confirmed code with
 *      kTiiGhostOverlap blocks in common, not more than kTiiGhostMarginDb above it, and in a verdict that did NOT also
 *      read the confirmed code, is that code's misreading: it is never listed, and it counts as a sighting of the code
 *      it misread (so the real site does not flicker to "signal lost" when it is misread). A misreading cannot be
 *      stronger than its site (its mean holds a noise block), hence the margin; and two codes read in the SAME verdict
 *      are two sites (a collision the directory resolved), so neither is touched. Readings of such a code from BEFORE
 *      its site was listed are dropped too, except those beside it. The site must itself have been heard in the last
 *      8 verdicts: a code that turns up after its "site" has gone quiet is not that site's misreading.
 *   3. LOST, THEN GONE. A listed code not in one of the last kTiiLiveWindow verdicts — or every code, when no verdict has
 *      come for kTiiStallMs (no signal) — is reported "signal lost" (`tiiHeld`), at its last reading, for kTiiExpireMs
 *      (5 minutes) after it was last seen, then removed. At most kTiiListCap lines: past that, the longest-lost goes.
 *   ★ A CANDIDATE (one of several on a shared Sub Id, no directory) is confirmed the same way but never held: a
 *     "signal lost" line for a guess would be clutter (the panels' rule since 2026-11-04).
 *  The list is in the order codes were confirmed, so lines do not jump. A block change starts it afresh; a re-acquire
 *  on the same block (the verdict count starting again) does not. Times are the caller's milliseconds. */
static constexpr int     kTiiTrackWindow   = 32;      ///< verdicts (~25 s)
static constexpr int     kTiiTrackConfirm  = 10;      ///< own sightings in the window to be listed
static constexpr int     kTiiLiveWindow    = 3;       ///< listed and seen in one of the last 3 verdicts = on the air
static constexpr int64_t kTiiStallMs       = 5000;    ///< no verdict for this long: every line is "signal lost"
static constexpr int64_t kTiiExpireMs      = 5 * 60 * 1000;   ///< unseen this long: the line is removed
static constexpr size_t  kTiiListCap       = 24;      ///< lines listed, live and lost together
static constexpr size_t  kTiiPendingCap    = 64;      ///< codes being weighed (not yet listed)
static constexpr int     kTiiGhostOverlap  = 3;       ///< blocks in common with a confirmed code on its Sub Id
static constexpr float   kTiiGhostMarginDb = 3.0f;    ///< …and not more than this above it
static constexpr uint32_t kTiiGhostRecent  = 0xFFu;   ///< …which was itself heard in the last 8 verdicts (~6 s)

/** One line of the list: the code at its newest reading, its place, and whether it is on the air now. */
struct TiiLine {
    TiiHit   hit;
    uint32_t order = 0;
    bool     live  = false;
};

class TiiTrack {
public:
    /** One verdict's whole list (measured + resolved + candidates) at `nowMs`. */
    void verdict(const std::vector<TiiHit>& hits, int64_t nowMs) {
        for (auto& e : e_) { e.mask <<= 1; e.own <<= 1; e.beside <<= 1; }
        auto inVerdict = [&](int m, int s) {
            for (const auto& h : hits) if (h.mainId == m && h.subId == s) return true;
            return false;
        };
        for (size_t i = 0; i < hits.size(); ++i) {
            const TiiHit& h = hits[i];
            if (h.mainId < 0 || h.mainId >= 70 || h.subId < 0 || h.subId >= 24) continue;
            bool dup = false;
            for (size_t j = 0; j < i; ++j) if (hits[j].mainId == h.mainId && hits[j].subId == h.subId) dup = true;
            if (dup) continue;
            Entry* e = find_(h.mainId, h.subId);
            if (!e || !e->confirmed) {
                if (Entry* c = misreadOf_(h, [&](const Entry& x) { return (x.mask & kTiiGhostRecent) && !inVerdict(x.hit.mainId, x.hit.subId); })) {
                    c->mask |= 1u; c->lastSeen = nowMs;   // ★ rule 2: a sighting of the site it misread
                    continue;
                }
            }
            if (!e) {
                if (pending_() >= kTiiPendingCap) dropWeakestPending_();
                e_.push_back(Entry{}); e = &e_.back();
            }
            e->hit = h; e->mask |= 1u; e->own |= 1u; e->lastSeen = nowMs;
            for (const auto& x : hits)   // read beside an overlapping code: two sites, not a misreading
                if (x.subId == h.subId && x.mainId != h.mainId && overlap_(x.mainId, h.mainId)) { e->beside |= 1u; break; }
        }
        // rule 1: confirm — the most-seen first, so of two overlapping codes the better-read one is the site
        std::vector<size_t> ready;
        for (size_t i = 0; i < e_.size(); ++i)
            if (!e_[i].confirmed && seen_(e_[i].own) >= kTiiTrackConfirm) ready.push_back(i);
        std::stable_sort(ready.begin(), ready.end(), [&](size_t a, size_t b) {
            if (seen_(e_[a].own) != seen_(e_[b].own)) return seen_(e_[a].own) > seen_(e_[b].own);
            return e_[a].hit.strength > e_[b].hit.strength;
        });
        for (size_t i : ready) {
            Entry& e = e_[i];
            if (seen_(e.own) < kTiiTrackConfirm) continue;   // its lone readings went to a code confirmed just now
            e.confirmed = true; e.order = nextOrder_++;
            /* ★ What an overlapping code on this Sub Id read on its own, BEFORE this one was listed, was this one
             *  misread (rule 2) — only its readings beside this one still count towards its own line. */
            for (auto& u : e_)
                if (!u.confirmed && u.hit.subId == e.hit.subId && overlap_(u.hit.mainId, e.hit.mainId)
                    && u.hit.strength <= e.hit.strength + kTiiGhostMarginDb) { u.own &= u.beside; u.mask &= u.beside; }
        }
        // forget what has left the window unlisted
        e_.erase(std::remove_if(e_.begin(), e_.end(), [](const Entry& e) { return !e.confirmed && e.mask == 0; }), e_.end());
        lastVerdict_ = nowMs; haveVerdict_ = true;
        trim_(nowMs);
    }
    /** The list at `nowMs`, in the order the codes were confirmed: expired lines removed, lost candidates dropped. */
    std::vector<TiiLine> lines(int64_t nowMs) {
        trim_(nowMs);
        std::vector<TiiLine> out;
        for (const auto& e : e_) {
            if (!e.confirmed) continue;
            const bool live = live_(e, nowMs);
            if (!live && e.hit.from == kTiiCandidate) continue;
            TiiLine l; l.hit = e.hit; l.order = e.order; l.live = live;
            out.push_back(l);
        }
        std::sort(out.begin(), out.end(), [](const TiiLine& a, const TiiLine& b) { return a.order < b.order; });
        return out;
    }
    /** ★ The strongest code read in two of the last three verdicts, listed or not — for a DX scan's short visit,
     *  which logs the transmitter it heard and cannot wait the ~8 s a line takes (local_sdr_shim's DX log). */
    bool strongestRecent(TiiHit& out) const {
        bool any = false;
        for (const auto& e : e_) {
            if (e.hit.from == kTiiCandidate || __builtin_popcount(e.own & 7u) < 2) continue;
            if (!any || e.hit.strength > out.strength) { out = e.hit; any = true; }
        }
        return any;
    }
    void reset() { e_.clear(); nextOrder_ = 0; haveVerdict_ = false; lastVerdict_ = 0; }

private:
    struct Entry {
        TiiHit   hit;
        uint32_t mask = 0;        ///< bit 0 = the newest verdict: read, or misread (rule 2)
        uint32_t own  = 0;        ///< …read as itself
        uint32_t beside = 0;      ///< …read in a verdict that also read an overlapping code on its Sub Id
        bool     confirmed = false;
        uint32_t order = 0;
        int64_t  lastSeen = 0;
    };
    static_assert(kTiiTrackWindow <= 32, "the window is a 32-bit mask");
    static int seen_(uint32_t m) {
        const uint32_t w = kTiiTrackWindow >= 32 ? 0xFFFFFFFFu : ((1u << kTiiTrackWindow) - 1u);
        return __builtin_popcount(m & w);
    }
    bool live_(const Entry& e, int64_t nowMs) const {
        return (e.mask & ((1u << kTiiLiveWindow) - 1u)) && haveVerdict_ && nowMs - lastVerdict_ <= kTiiStallMs;
    }
    static bool overlap_(int a, int b) {
        return __builtin_popcount(unsigned(tiiPattern(a) & tiiPattern(b))) >= kTiiGhostOverlap;
    }
    Entry* find_(int m, int s) {
        for (auto& e : e_) if (e.hit.mainId == m && e.hit.subId == s) return &e;
        return nullptr;
    }
    /** Rule 2: the confirmed code `h` would be a misreading of, or null. `eligible` narrows the confirmed codes. */
    template <class Eligible>
    Entry* misreadOf_(const TiiHit& h, Eligible eligible) {
        Entry* best = nullptr;
        for (auto& c : e_) {
            if (!c.confirmed || c.hit.subId != h.subId || c.hit.mainId == h.mainId) continue;
            if (!overlap_(c.hit.mainId, h.mainId)) continue;
            if (h.strength > c.hit.strength + kTiiGhostMarginDb) continue;
            if (!eligible(c)) continue;
            if (!best || c.hit.strength > best->hit.strength) best = &c;
        }
        return best;
    }
    size_t pending_() const { size_t n = 0; for (const auto& e : e_) if (!e.confirmed) ++n; return n; }
    void dropWeakestPending_() {
        size_t w = e_.size();
        for (size_t i = 0; i < e_.size(); ++i) {
            if (e_[i].confirmed) continue;
            if (w == e_.size() || seen_(e_[i].own) < seen_(e_[w].own)) w = i;
        }
        if (w < e_.size()) e_.erase(e_.begin() + long(w));
    }
    /** Rule 3: expire, drop candidates no longer heard, and hold the list to its cap (the longest-lost first). */
    void trim_(int64_t nowMs) {
        e_.erase(std::remove_if(e_.begin(), e_.end(), [&](const Entry& e) {
            return e.confirmed && (nowMs - e.lastSeen > kTiiExpireMs || (e.hit.from == kTiiCandidate && !live_(e, nowMs)));
        }), e_.end());
        for (;;) {
            size_t n = 0, out = e_.size();
            for (size_t i = 0; i < e_.size(); ++i) {
                if (!e_[i].confirmed) continue;
                ++n;
                const Entry& e = e_[i];
                if (out == e_.size()) { out = i; continue; }
                const Entry& o = e_[out];
                const bool el = live_(e, nowMs), ol = live_(o, nowMs);
                if (el != ol ? !el : (el ? e.hit.strength < o.hit.strength : e.lastSeen < o.lastSeen)) out = i;
            }
            if (n <= kTiiListCap || out == e_.size()) break;
            e_.erase(e_.begin() + long(out));
        }
    }

    std::vector<Entry> e_;
    uint32_t nextOrder_ = 0;
    int64_t  lastVerdict_ = 0;
    bool     haveVerdict_ = false;
};

/** ★ Accumulates pair products over TII frames and reports the transmitters it can see. */
class TiiDetector {
public:
    /** Feed the K = 1536 carriers (our array order) of one TII-bearing null symbol. */
    void feed(const TiiC32* carriers, int K) {
        if (K != 1536) return;
        if (acc_.size() != 24u * 8u) { acc_.assign(24 * 8, TiiC32{}); accOwn_.assign(24 * 8, TiiC32{}); }
        if (ownTurn_.empty()) {
            /* ★ The own-phase measure (see the top): x_k conj(z_k) times conj(x_(k+1) conj(z_(k+1))) is
             *  the plain product turned by conj(z_k) z_(k+1) — a multiple of a quarter turn. */
            ownTurn_.resize(24 * 8 * 4);
            for (int c = 0; c < 24; ++c) for (int b = 0; b < 8; ++b) for (int s = 0; s < 4; ++s) {
                const int k = tiiPairCarrier(s, c, b);
                ownTurn_[size_t((c * 8 + b) * 4 + s)] = std::conj(prsCarrier(k)) * prsCarrier(k + 1);
            }
        }
        for (int c = 0; c < 24; ++c)
            for (int b = 0; b < 8; ++b) {
                TiiC32 sum{}, own{};
                for (int s = 0; s < 4; ++s) {
                    const int k  = tiiPairCarrier(s, c, b);
                    const int p0 = tiiCarrierPos(k), p1 = tiiCarrierPos(k + 1);
                    if (p0 < 0 || p1 >= K) continue;
                    const TiiC32 prod = carriers[p0] * std::conj(carriers[p1]);
                    sum += prod;
                    own += prod * ownTurn_[size_t((c * 8 + b) * 4 + s)];
                }
                acc_[size_t(c * 8 + b)] += sum;
                accOwn_[size_t(c * 8 + b)] += own;
            }
        // ★ The null's own noise power, from carriers no pattern uses (odd carriers, which are
        //   never the FIRST of a pair): the reference the hits are measured against.
        //   ★ Not true in sections three and four — see the top. Kept for the original test only.
        double noise = 0.0; int n = 0;
        for (int k = -767; k < 768; k += 2) {
            if (k == 0 || k == 1) continue;
            const int p = tiiCarrierPos(k);
            if (p < 0 || p >= K) continue;
            noise += std::norm(carriers[p]); ++n;
        }
        if (n) noiseAcc_ += noise / n;
        // ★ The new path's noise: the lower quartile of the carrier powers, as a mean (see the top).
        pw_.resize(size_t(K));
        for (int i = 0; i < K; ++i) pw_[size_t(i)] = std::norm(carriers[i]);
        std::nth_element(pw_.begin(), pw_.begin() + K / 4, pw_.end());
        noiseQ_ += double(pw_[size_t(K / 4)]) / 0.2877;
        if (++frames_ >= kFramesPerVerdict) {
            decide(); frames_ = 0; noiseAcc_ = 0.0; noiseQ_ = 0.0;
            std::fill(acc_.begin(), acc_.end(), TiiC32{}); std::fill(accOwn_.begin(), accOwn_.end(), TiiC32{});
        }
    }
    /** The combs that identified a transmitter on their own (kTiiClean), strongest first. */
    const std::vector<TiiHit>& hits() const { return hits_; }
    /** ★ The combs with more than four blocks lit — for tiiResolveShared, with the directory. */
    const std::vector<TiiShared>& shared() const { return shared_; }
    /** ★ Why a null symbol did NOT yield a hit: the best comb's four-strongest against the noise
     *  and against the fifth. For the pane and for tuning the test on a multiplex that identifies
     *  nothing (11D, Stuart, 2026-09-07: "the original commercial DAB multiplex of the UK").
     *  ★ Measured on the new path (both phase conventions, the quartile noise) since 2026-11-04 — the
     *    old figure called 11D "none transmitted" while Daventry stood 7800 sigma clear. `active` is how
     *    many blocks of that comb are lit (more than 4 = a shared Sub Id); `verdicts` counts decisions. */
    struct Diag { int comb = -1; float fourthOverSigma = 0, fourthOverFifth = 0; int frames = 0; int active = 0; unsigned verdicts = 0; };
    const Diag& diag() const { return diag_; }
    void reset() { acc_.clear(); accOwn_.clear(); frames_ = 0; noiseAcc_ = 0.0; noiseQ_ = 0.0; hits_.clear(); shared_.clear(); diag_ = Diag{}; }

    static constexpr int kFramesPerVerdict = 4;   ///< ~0.8 s of TII frames (every other frame)
    /** On the new path a block is lit above this many sigma (the original test's own floor)… */
    static constexpr float kTiiActiveSigma = 4.0f;
    /** …and above this fraction of the same block one or two combs either side (leakage; see the top). */
    static constexpr float kTiiLeak = 0.015f;

private:
    void decide() {
        hits_.clear(); shared_.clear();
        if (acc_.empty() || frames_ == 0) return;
        bool taken[24] = {false};
        /* A transmitted pair sums coherently (real, positive): use the real part, which also
         * rejects half the noise. Per pair-slot power over `frames_` frames and 4 sections. */
        const double norm = 1.0 / (double(frames_) * 4.0);
        const double noisePair = (noiseAcc_ / frames_) * 1.0;    // E|n_k n_k+1*| ~ noise power
        { const unsigned nv = diag_.verdicts + 1; diag_ = Diag{}; diag_.frames = frames_; diag_.verdicts = nv; }
        for (int c = 0; c < 24; ++c) {
            float v[8]; int idx[8];
            for (int b = 0; b < 8; ++b) { v[b] = float(acc_[size_t(c * 8 + b)].real() * norm); idx[b] = b; }
            std::sort(idx, idx + 8, [&](int a, int b2) { return v[a] > v[b2]; });
            // the four strongest must all stand well above the noise, and above the other four
            const float fourth = v[idx[3]], fifth = v[idx[4]];
            /* ★ The noise in an AVERAGED pair product is the carrier noise power over the square
             *  root of the number of products summed (4 sections x frames); a transmitter must
             *  clear four of those on all four of its pairs, and the fifth-strongest slot must
             *  look like noise, or this is not a 4-of-8 pattern but a strong neighbour's leakage. */
            const float sigma = float(noisePair / std::sqrt(4.0 * frames_));
            if (fourth <= 4.0f * sigma) continue;
            if (fourth < fifth * 2.0f + 2.0f * sigma) continue;
            uint8_t bits = 0;
            for (int i = 0; i < 4; ++i) bits |= uint8_t(0x80 >> idx[i]);
            const int p = tiiPatternIndex(bits);
            if (p < 0) continue;
            TiiHit h;
            h.mainId = p; h.subId = c;
            const float mean4 = (v[idx[0]] + v[idx[1]] + v[idx[2]] + v[idx[3]]) / 4.0f;
            /* ★★ THE FIGURE IS OVER THE TRUE NOISE (Stuart, 2026-11-04: "if our SNR figures are wrong
             *  because we measure them wrong please fix them"). The DECISION above still uses the
             *  odd-carrier reference, untouched — what it accepts is exactly what it accepted — but that
             *  reference holds TII carriers (the top), so as a figure it read Daventry 15.8 dB on 12B
             *  against 33 on 11D. Now every hit, either path, is its mean over the quartile noise. */
            h.strength = 10.0f * std::log10(mean4 / float(noiseQ_ / frames_ + 1e-20));
            hits_.push_back(h);
            taken[c] = true;
        }
        decideOwn_(taken, norm);
        std::sort(hits_.begin(), hits_.end(), [](const TiiHit& a, const TiiHit& b) { return a.strength > b.strength; });
        if (hits_.size() > 8) hits_.resize(8);
    }

    /** ★ The new path, over the combs the test above rejected: each block the larger of the two
     *  phase measures, against the quartile noise; lit only clear of the noise AND of leakage. */
    void decideOwn_(bool* taken, double norm) {
        const double P = noiseQ_ / frames_;
        const float sigma = float(P / std::sqrt(4.0 * frames_));    // the same form as the test above
        float v[24][8];
        for (int c = 0; c < 24; ++c) for (int b = 0; b < 8; ++b)
            v[c][b] = float(std::max(acc_[size_t(c * 8 + b)].real(), accOwn_[size_t(c * 8 + b)].real()) * norm);
        float bestFourth = -1.0f;
        for (int c = 0; c < 24; ++c) {
            uint8_t lit = 0; float rest = 0.0f;   // rest: the strongest block neither lit nor leakage
            for (int b = 0; b < 8; ++b) {
                float nb = 0.0f;
                for (int d = -2; d <= 2; ++d) if (d && c + d >= 0 && c + d < 24) nb = std::max(nb, v[c + d][b]);
                const bool leak = v[c][b] < kTiiLeak * nb;
                if (v[c][b] > kTiiActiveSigma * sigma && !leak) lit |= uint8_t(0x80 >> b);
                else if (!leak) rest = std::max(rest, v[c][b]);
            }
            const int n = __builtin_popcount(unsigned(lit));
            float s8[8]; for (int b = 0; b < 8; ++b) s8[b] = v[c][b];
            std::sort(s8, s8 + 8, [](float a, float b2) { return a > b2; });
            if (s8[3] > bestFourth) {
                bestFourth = s8[3]; diag_.comb = c; diag_.active = n;
                diag_.fourthOverSigma = sigma > 0 ? s8[3] / sigma : 0;
                diag_.fourthOverFifth = s8[4] > 0 ? s8[3] / s8[4] : 0;
            }
            if (n < 4) continue;
            if (!taken[c]) {
                /* ★ The original test's rule on this measure: the four strongest lit blocks, the weakest
                 *  of them twice whatever is left (a fifth lit block included). A fifth block that only
                 *  just clears the noise is not a second transmitter — this keeps it from turning one
                 *  clean site into five "candidates". */
                int top[8], nt = 0;
                for (int b = 0; b < 8; ++b) if (lit & (0x80 >> b)) top[nt++] = b;
                std::sort(top, top + nt, [&](int a, int b2) { return v[c][a] > v[c][b2]; });
                float rest4 = rest, sum = 0.0f;
                for (int i = 4; i < nt; ++i) rest4 = std::max(rest4, v[c][top[i]]);
                uint8_t bits = 0;
                for (int i = 0; i < 4; ++i) { bits |= uint8_t(0x80 >> top[i]); sum += v[c][top[i]]; }
                if (v[c][top[3]] >= rest4 * 2.0f + 2.0f * sigma) {
                    const int p = tiiPatternIndex(bits);
                    if (p >= 0) {
                        TiiHit h; h.mainId = p; h.subId = c; h.from = kTiiClean;
                        h.strength = 10.0f * std::log10((sum / 4.0f) / float(P + 1e-20));
                        hits_.push_back(h);
                        taken[c] = true;
                    }
                }
            }
            /* ★ More than four lit: a shared Sub Id — even beside a clean hit, whose co-channel neighbour the
             *  directory may still name (tiiResolveShared skips the clean one, and offers no candidates
             *  on a Sub Id that already has a measured hit). */
            if (n > 4) {
                TiiShared sc; sc.subId = c; sc.active = lit; sc.noise = float(P); sc.sigma = sigma; sc.alongside = taken[c];
                for (int b = 0; b < 8; ++b) sc.v[b] = v[c][b];
                shared_.push_back(sc);
            }
        }
    }

    std::vector<TiiC32> acc_;
    std::vector<TiiC32> accOwn_;      ///< the same products, each carrier's own PRS phase taken out
    std::vector<TiiC32> ownTurn_;     ///< conj(z_k) z_(k+1) per (comb, block, section)
    std::vector<float>  pw_;          ///< scratch: one null's carrier powers
    std::vector<TiiHit> hits_;
    std::vector<TiiShared> shared_;
    Diag diag_;
    int    frames_   = 0;
    double noiseAcc_ = 0.0;
    double noiseQ_   = 0.0;           ///< the quartile noise power, summed over the verdict's frames
};

}  // namespace vibedab
