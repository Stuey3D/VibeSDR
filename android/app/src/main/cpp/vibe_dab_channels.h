// vibe_dab_channels.h — the DAB Band III channel plan, and who is allowed to offer DAB.
//
// ★★★ HEADER-ONLY AND DEPENDENCY-FREE ON PURPOSE. This is a TABLE and two predicates; it must be
//     testable without a radio, a socket or the rest of the core (the same rule test-utf8 follows).
//     Everything above it — OFDM, FIC, MSC — will be far harder to test, so the part that CAN be
//     pinned down exactly should be.
//
// ★★ WHY A TABLE AND NOT ARITHMETIC. The Band III raster looks regular and is not: the blocks are
//     1.712 MHz apart within a channel group (5A-D, 6A-D …) but the STEP BETWEEN GROUPS is larger,
//     and 13A-13F break the pattern again at the top. Generating these from a formula gets you
//     within a few hundred kHz — close enough to lock onto nothing at all, and close enough that
//     the mistake looks like a demodulator bug for a week. ETSI EN 300 401 lists them; so do we.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace vibedab {

struct Channel { const char* name; uint32_t centreHz; };

/** Band III, the whole plan. The UK uses 11A-12D for national and local multiplexes; the rest are
 *  here because a receiver that only knows its own country's subset is wrong the moment somebody
 *  runs it elsewhere, and the table costs nothing. */
inline constexpr Channel kBandIII[] = {
    {"5A", 174928000}, {"5B", 176640000}, {"5C", 178352000}, {"5D", 180064000},
    {"6A", 181936000}, {"6B", 183648000}, {"6C", 185360000}, {"6D", 187072000},
    {"7A", 188928000}, {"7B", 190640000}, {"7C", 192352000}, {"7D", 194064000},
    {"8A", 195936000}, {"8B", 197648000}, {"8C", 199360000}, {"8D", 201072000},
    {"9A", 202928000}, {"9B", 204640000}, {"9C", 206352000}, {"9D", 208064000},
    {"10A", 209936000}, {"10B", 211648000}, {"10C", 213360000}, {"10D", 215072000},
    {"11A", 216928000}, {"11B", 218640000}, {"11C", 220352000}, {"11D", 222064000},
    {"12A", 223936000}, {"12B", 225648000}, {"12C", 227360000}, {"12D", 229072000},
    {"13A", 230784000}, {"13B", 232496000}, {"13C", 234208000}, {"13D", 235776000},
    {"13E", 237488000}, {"13F", 239200000},
};

inline constexpr size_t kBandIIICount = sizeof(kBandIII) / sizeof(kBandIII[0]);

/** ★ The occupied bandwidth of one ensemble (1536 carriers x 1 kHz). Not the sample rate — see
 *  kMinCaptureHz, which is what a radio actually has to be able to DELIVER. */
inline constexpr uint32_t kEnsembleBandwidthHz = 1536000;

/** ★★ THE GATE IS BANDWIDTH, NEVER THE DRIVER NAME. 2.048 MSPS is the canonical DAB rate and
 *  leaves usable transition band either side of the 1.536 MHz ensemble. A radio that can reach
 *  Band III but only captures ~912 kHz (the Airspy HF+) can see a third of a multiplex, which
 *  demodulates to nothing — so it must not be offered DAB at all.
 *  ★ AGENTS.md: "a control that works on ONE radio only should be REMOVED, not left inert", and
 *    "ELSE MEANS DONGLE" — gate on the CAPABILITY so the next radio inherits the right answer. */
inline constexpr uint32_t kMinCaptureHz = 2048000;

/** Channel by name ("11D", case-insensitive). nullptr when there is no such channel. */
inline const Channel* channelByName(const char* name) {
    if (!name) return nullptr;
    for (size_t i = 0; i < kBandIIICount; ++i) {
        const char* a = kBandIII[i].name; const char* b = name;
        size_t k = 0;
        for (;; ++k) {
            char ca = a[k], cb = b[k];
            if (cb >= 'a' && cb <= 'z') cb = char(cb - 'a' + 'A');
            if (ca != cb) break;
            if (ca == '\0') return &kBandIII[i];
        }
    }
    return nullptr;
}

/** Index of the channel whose centre is nearest `hz`, or -1 if the table is somehow empty.
 *  ★ Used to answer "which multiplex am I on?" for a frequency that arrived from a bookmark or a
 *    restored session, where nobody stored the channel name. */
inline int nearestChannel(uint32_t hz) {
    int best = -1; uint64_t bestD = UINT64_MAX;
    for (size_t i = 0; i < kBandIIICount; ++i) {
        uint64_t d = kBandIII[i].centreHz > hz ? uint64_t(kBandIII[i].centreHz) - hz
                                               : hz - uint64_t(kBandIII[i].centreHz);
        if (d < bestD) { bestD = d; best = int(i); }
    }
    return best;
}

/** Step to the next/previous multiplex — what the tuning buttons do in DAB mode.
 *  ★ CLAMPS rather than wraps: 13F -> 5A in one press is never what somebody scanning the band
 *    meant, and a silent wrap reads as the tuner having jumped at random. */
inline int stepChannel(int index, int delta) {
    int n = index + delta;
    if (n < 0) return 0;
    if (n >= int(kBandIIICount)) return int(kBandIIICount) - 1;
    return n;
}

struct Range { uint32_t loHz, hiHz; };

/** ★★★ FEED THIS THE EFFECTIVE LIMITS, NOT THE HARDWARE'S DATASHEET.
 *
 *  Stuart, 2026-09-04: *"an unlocked RTL-SDR v4 is fair game, but if i were to put restrictions on
 *  it such as the sample rate like on the Xcover or limit its frequency range also like the xcover
 *  then the button shouldnt even present itself."*
 *
 *  The operator's settings are part of the answer, not a detail applied afterwards. A locked
 *  sample rate below 2.048 MSPS, or an allowed-range list that excludes Band III, makes DAB
 *  impossible on hardware that could otherwise do it — and a button offered on that receiver is
 *  the exact fault AGENTS.md names: the user concludes the FEATURE is broken, not the server.
 *
 *  ★★ So `captureHz` is the rate this receiver will ACTUALLY run at (the locked rate if one is
 *     set, otherwise the widest it sustains), and `ranges` is the tunable set AFTER allow/block
 *     lists — the same `tunable` array the server already publishes in vibeserver.json, so there
 *     is ONE reader of that rule rather than a second copy that can drift from it.
 *
 *  On Stuart's own three radios today:
 *    · RTL-SDR V4 / V4L, unrestricted  — capable.
 *    · Airspy HF+   — reaches nothing above 31 MHz AND caps near 912 kHz: out on both counts.
 *    · SDRplay RSP1B — plenty of sample rate, but locked to 2.8-10.8 MHz: out on range alone.
 *      ★ That one is the reason this takes RANGES rather than a single lo/hi: the restriction is
 *        the operator's, the hardware is blameless, and only the effective set knows the difference.
 */
inline bool radioCanDab(const Range* ranges, size_t n, uint32_t captureHz) {
    if (!ranges || n == 0) return false;
    if (captureHz < kMinCaptureHz) return false;
    // A whole channel must be REACHABLE — touching the band edge is not enough, and a channel
    // straddling two disjoint allowed ranges is not reachable either.
    for (size_t i = 0; i < kBandIIICount; ++i)
        for (size_t r = 0; r < n; ++r)
            if (kBandIII[i].centreHz >= ranges[r].loHz && kBandIII[i].centreHz <= ranges[r].hiHz)
                return true;
    return false;
}

/** Single-range convenience — the common case, and it keeps the tests readable. */
inline bool radioCanDab(uint32_t loHz, uint32_t hiHz, uint32_t captureHz) {
    const Range r{loHz, hiHz};
    return radioCanDab(&r, 1, captureHz);
}

/** The readout Stuart specified: "222.064 (11D)" — the frequency AND the channel, because on a
 *  multiplex the number alone tells a DX-er nothing and the name alone tells a newcomer nothing. */
inline std::string displayLabel(int index) {
    if (index < 0 || index >= int(kBandIIICount)) return std::string();
    const Channel& c = kBandIII[index];
    char buf[32];
    // kHz to three decimals: 222064000 -> "222.064". Integer maths, so no float formatting drift.
    snprintf(buf, sizeof buf, "%u.%03u (%s)", c.centreHz / 1000000u,
             (c.centreHz % 1000000u) / 1000u, c.name);
    return std::string(buf);
}

}  // namespace vibedab
