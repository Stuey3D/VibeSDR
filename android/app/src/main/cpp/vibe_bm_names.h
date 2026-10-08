#pragma once
// ★★★ NAMES FOR A STATION WHOSE OWN TEXT NEVER SETTLES (2026-10-08). Stuart: "the US stations also rotate their names
// too, but on these servers we only bookmark the pi code ... I noticed it on Kiko's and now on Chicopee and the
// Ukrainian one too". Read off the servers' own lists: Chicopee 7 of 8 learned bookmarks stuck at "PI: XXXX", Kiko's TV
// box 3 of 4. The server names a station when its 8-character PS SETTLES, and a marquee never does; the only fallback
// was RadioDNS, which needs an ECC (US stations send none, the Ukrainian ones send Morocco's) and a network transport (a
// phone has none — Kiko's Lite). The rank kNameGuess ("unverified") existed for exactly this and nothing produced it.
// Both rules here are pure, so test-bm-names.cpp drives them; local_sdr_shim.cpp bmLearn calls them.
#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace vibe {

/** ★★ THE CALL LETTERS A US PI ENCODES (NRSC-4-B Annex D). 0x1000–0x54A7 = K + 3 letters, 0x54A8–0x994F = W + 3
 *  letters, base 26; a PI 0xAxyz is the compressed form of 0xx0yz. Checked by hand against Chicopee's list:
 *  87A6 → WTIC (96.5 Hartford), 672C → WHAI (98.3 Greenfield), 5660 → WAQY (102.1 Springfield).
 *  "" when the PI is not a call-letter code (three-letter calls, nationally linked and the rest).
 *  ★ US ONLY — elsewhere a PI's first digit is a COUNTRY and the rest a station number; the caller decides. */
inline std::string rbdsCallsign(int pi) {
    if (pi <= 0 || pi > 0xFFFF) return "";
    if ((pi & 0xF000) == 0xA000) pi = ((pi & 0x0F00) << 4) | (pi & 0x00FF);   // 0xAxyz → 0xx0yz
    int base; char first;
    if (pi >= 0x1000 && pi <= 0x54A7)      { base = 0x1000; first = 'K'; }
    else if (pi >= 0x54A8 && pi <= 0x994F) { base = 0x54A8; first = 'W'; }   // 21672 + 26³ − 1
    else return "";
    const int n = pi - base;
    std::string s(1, first);
    s += (char)('A' + n / 676); s += (char)('A' + (n % 676) / 26); s += (char)('A' + n % 26);
    return s;
}

/** ★★ A ROTATING NAME, LEARNED FROM THE ROTATION. The station's name keeps coming back; a song's artist and title
 *  recur only for as long as the song. A segment heard across at least SPAN_SECS, in BUCKETS separate BUCKET_SECS
 *  windows and at least MIN_COUNT times, is the station's own — "UMUARAMA" and "MASSA" on Kiko's 94.5 qualify,
 *  "JUSTIN" and "BIEBER" in one four-minute song do not. Of two that differ in ONE character (a garble on a weak
 *  signal) only the more frequent counts. Up to three, in the order first heard; "" until something qualifies. */
struct RotationNamer {
    static constexpr long long SPAN_SECS = 6 * 60, BUCKET_SECS = 120;
    static constexpr int BUCKETS = 3, MIN_COUNT = 8, MAX_SEGS = 64, MAX_PARTS = 4;
    struct Seg { long long first = 0, last = 0, lastBucket = -1; int buckets = 0, count = 0; };
    std::map<std::string, Seg> segs;

    std::string feed(const std::string& seg, long long now) {
        if (!seg.empty() && seg.size() <= 8) {
            auto& s = segs[seg];
            if (!s.first) s.first = now;
            s.last = now; s.count++;
            const long long b = now / BUCKET_SECS;
            if (b != s.lastBucket) { s.lastBucket = b; s.buckets++; }
            if ((int)segs.size() > MAX_SEGS) {                   // song text churns: drop the stalest
                auto oldest = segs.begin();
                for (auto i = segs.begin(); i != segs.end(); ++i) if (i->second.last < oldest->second.last) oldest = i;
                segs.erase(oldest);
            }
        }
        return name();
    }

    std::string name() const {
        auto oneOff = [](const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            int d = 0; for (size_t i = 0; i < a.size(); ++i) d += a[i] != b[i];
            return d == 1;
        };
        std::vector<std::pair<long long, std::string>> keep;
        for (auto& [k, s] : segs) {
            if (s.last - s.first < SPAN_SECS || s.buckets < BUCKETS || s.count < MIN_COUNT) continue;
            bool beaten = false;
            for (auto& [k2, s2] : segs)
                if (k2 != k && oneOff(k, k2) && (s2.count > s.count || (s2.count == s.count && k2 < k))) { beaten = true; break; }
            if (!beaten) keep.push_back({s.first, k});
        }
        if (keep.empty()) return "";
        // ★ A CHARACTER SCROLLER ("RADIO MA", "ADIO MAS", …) makes every 8-character window recur, so dozens qualify. A
        //   real rotation is a handful of words; more than MAX_PARTS qualifying means a scroller — no guess at all (the
        //   app's PsStabiliser reassembles scrollers for display; a bookmark name of window fragments helps nobody).
        if ((int)keep.size() > MAX_PARTS) return "";
        std::sort(keep.begin(), keep.end());
        std::string out;
        for (size_t i = 0; i < keep.size() && i < 3; ++i) { if (!out.empty()) out += ' '; out += keep[i].second; }
        return out;
    }
};

}  // namespace vibe
