// vibe_dab_wide.h — V12 DAB PHASE 2: A LISTENER'S OWN MULTIPLEX IN A LOCKED WIDE RANGE (docs/v12/DAB-OWN-STATION.md §11).
//
// Stuart, 2026-11-17: "carry on with the multimultiplex stuff please for the wideband locked range multiple users scenario
// for DAB". A radio the owner has LOCKED to a wide range (the RSP1A at 8 MS/s round 215.14 MHz holds 10C, 10D and 11A)
// already gives every listener their own VFO. With this, every listener may also be in DAB on ANY block inside that
// capture — each block that has a listener gets its own multiplex decoder (DabService: its DDC cuts the block out of the
// shared capture, then OFDM / FIC / TII), lazily, and the listener's station hangs off THEIR multiplex (Phase 1's station
// mode). The radio never moves.
//
// ★★★ HARD RULE §11.0: this is ONLY the wide locked range — a capture of 4 MS/s or more holding at least two Band III
//     blocks. Below that, and in the ordinary single-multiplex DAB mode, nothing here runs and nothing of the AGC, the gain
//     path or the decode changes (scripts/test-dab-standard-path-guard.mjs).
//
// The pure rules live here, unit-tested (vibeserver/test-dab-wide-lr.cpp); the shim carries them out.
#pragma once
#include "vibe_dab_channels.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace vibedab {
namespace wide {

/** A capture narrower than this is never a wide range (§11.0). */
static constexpr double kMinRateHz = 4000000.0;
/** Half a block, with the guard: an ensemble is 1.536 MHz; ±0.80 MHz must sit inside the flat part. */
static constexpr double kHalfBlockHz = 800000.0;
/** The flat part of a capture, as a fraction of the rate either side of the centre — the same figure the wide whole-radio
 *  plan uses (Impl::dabPlanFor), inside every radio's own anti-alias roll-off. */
static constexpr double kFlat = 0.42;

/** The Band III blocks wholly inside a capture centred on `centreHz` (the PHYSICAL centre) at `rateHz`, lowest first. */
inline std::vector<int> blocksIn(double centreHz, double rateHz) {
    std::vector<int> v;
    if (!(rateHz > 0) || !(centreHz > 0)) return v;
    for (int i = 0; i < int(kBandIIICount); ++i)
        if (std::fabs(double(kBandIII[i].centreHz) - centreHz) + kHalfBlockHz <= kFlat * rateHz) v.push_back(i);
    /* ★ The "N" raster (10N, 11N, 12N) overlaps the ordinary blocks beside it — 11N is 160 kHz from 11A. Where an ordinary
     *  block overlaps one in the same capture, the N block is not offered: the block keys would otherwise step 11A → 11N,
     *  the same air 160 kHz along. A capture holding an N block alone still offers it. */
    auto isN = [](int b) { const char* n = kBandIII[b].name; size_t k = 0; while (n[k]) ++k; return k > 0 && n[k - 1] == 'N'; };
    std::vector<int> out;
    for (int b : v) {
        bool overlapped = false;
        if (isN(b)) for (int o : v) if (o != b && !isN(o) && std::fabs(double(kBandIII[o].centreHz) - double(kBandIII[b].centreHz)) < 1536000.0) overlapped = true;
        if (!overlapped) out.push_back(b);
    }
    return out;
}

/** ★ Does this capture qualify? 4 MS/s or more, and at least TWO blocks in it — one block is the ordinary DAB mode's job. */
inline bool eligible(double centreHz, double rateHz) {
    return rateHz >= kMinRateHz - 1000.0 && blocksIn(centreHz, rateHz).size() >= 2;
}

/** ★ Which block a listener's request lands on. `want` = the block asked for (-1 = none named); `current` = the block the
 *  listener is on now (-1 = entering). `in` = blocksIn().
 *   · `want` inside the capture → `want`.
 *   · Already in DAB, `want` outside → the next block inside in the SAME DIRECTION (a client's block keys step one block at a
 *     time and know nothing of the capture); none that way → stay where they are.
 *   · Entering, `want` outside or none → the block inside nearest in frequency to `want` (none named → the middle one).
 *  -1 only when the capture holds no block at all. */
inline int choose(int want, int current, const std::vector<int>& in) {
    if (in.empty()) return -1;
    if (want >= 0 && std::find(in.begin(), in.end(), want) != in.end()) return want;
    if (current >= 0 && want >= 0) {
        if (want > current) { for (int b : in) if (b > current) return b; return current; }
        if (want < current) { for (auto it = in.rbegin(); it != in.rend(); ++it) if (*it < current) return *it; return current; }
        return current;
    }
    if (current >= 0) return current;
    if (want < 0 || want >= int(kBandIIICount)) return in[in.size() / 2];
    int best = in.front();
    for (int b : in)
        if (std::fabs(double(kBandIII[b].centreHz) - double(kBandIII[want].centreHz)) < std::fabs(double(kBandIII[best].centreHz) - double(kBandIII[want].centreHz))) best = b;
    return best;
}

/** ★ How many stations may be open on the radio at once (across every multiplex), besides each multiplex's own.
 *  The owner's switch (`setting`: -1 the rule, 0 off, 1 on), the benchmark's N (0 = not measured), the user limit:
 *   · the rule (N ≥ limit) on → N − 1, as Phase 1 (vibe_dab_own.h stationCap);
 *   · forced ON → limit − 1: in a locked range every listener may be on a different service and a station per listener is
 *     the most there can ever be. Not capped at 16 here (Phase 1's figure for one shared multiplex): the shim's memory
 *     floor and the per-multiplex hard cap (DabService::kMaxStations) still hold, and the health chip shows the strain.
 *   · off → 0: a pick on a multiplex switches that multiplex's own station for everyone ON IT. Other multiplexes are other
 *     listeners' and never move — a listener's own block is the point of the locked range. */
/** ★★ V12 §11.1 THE BANK'S MEASURED COSTS (the benchmark's dec_dab_bank row, % of a core at real time): its forward FFT and
 *  one block's slice, at the rates measured (8, 16, 32, 56 MS/s on a desktop). `at` interpolates linearly in the rate and
 *  scales by the rate beyond the ends. Empty = not measured: the per-mux DDCs are the only front end then. */
struct BankCost {
    std::vector<double> rates, fwd, slice;
    bool ok() const { return !rates.empty() && rates.size() == fwd.size() && rates.size() == slice.size(); }
    static double interp(const std::vector<double>& x, const std::vector<double>& y, double r) {
        if (r <= x.front()) return y.front() * r / x.front();
        if (r >= x.back()) return y.back() * r / x.back();
        for (size_t i = 1; i < x.size(); ++i) if (r <= x[i]) { const double t = (r - x[i - 1]) / (x[i] - x[i - 1]); return y[i - 1] + t * (y[i] - y[i - 1]); }
        return y.back();
    }
    double fwdAt(double r) const { return interp(rates, fwd, r); }
    double sliceAt(double r) const { return interp(rates, slice, r); }
};
/** ★★ The cheaper way to cut `muxes` blocks out of a capture at `rateHz`: N per-mux DDCs, or the bank (its forward FFT once
 *  plus a slice per block). `useBank` says which. One block at its own rate needs neither (the ordinary DAB path). */
inline double cutCostPct(int muxes, double rateHz, double ddc8Pct, const BankCost* bank, bool* useBank = nullptr) {
    if (useBank) *useBank = false;
    if (muxes < 2) return 0.0;
    const double ddc = std::max(0.0, ddc8Pct) * rateHz / 8e6 * double(muxes);
    if (bank && bank->ok()) {
        const double b = bank->fwdAt(rateHz) + bank->sliceAt(rateHz) * double(muxes);
        if (b < ddc || !(ddc8Pct > 0)) { if (useBank) *useBank = true; return b; }
    }
    return ddc8Pct > 0 ? ddc : -1.0;
}

/** ★★ HOW MANY DAB LISTENERS a locked range carries (Stuart, 2026-11-17: "in locked range a user with a b210 would lock
 *  it to the DAB band and the users are based on DAB decodes"), each on their own station with every block in the range
 *  decoding — any can be in use. From this machine's benchmark (the dabStations block):
 *   · perStationPct — one station: its decode, FAAD2 and Opus encode (dec_dab_station);
 *   · muxFrontPct   — one multiplex's OFDM / FIC front end (the dab row less its own station's decode);
 *   · the cutting — per-mux DDCs (ddc8Pct, dec_dab_mux, scaled by the rate) or the filter bank (`bank`, dec_dab_bank),
 *     whichever is cheaper for this many blocks at this rate (cutCostPct); none for a single block at its own rate.
 *  Within the green budget: 70 % of every core, less one core for network, spectrum and the audio clock. 0 = not measured.
 *  Never above what the user limit allows; the shim's memory floor holds on top. */
inline int dabListeners(double perStationPct, double muxFrontPct, double ddc8Pct, double rateHz, int muxes, int cores,
                        const BankCost* bank = nullptr) {
    if (!(perStationPct > 0) || !(muxFrontPct > 0) || cores < 1 || muxes < 1) return 0;
    const double cut = cutCostPct(muxes, rateHz, ddc8Pct, bank);
    if (cut < 0) return 0;
    const double spare = 70.0 * cores - 70.0 - double(muxes) * muxFrontPct - cut;
    return spare > 0 ? int(std::floor(spare / perStationPct)) : 0;
}

/** ★ Stations that may be open at once in a locked range, besides each multiplex's own: the measured DAB listeners
 *  (dabListeners — the hardware's figure, never a fixed number) up to the user limit, less the multiplexes' own streams.
 *  The owner's switch: 0 off; 1 forced on (still the measured figure); -1 the rule — on when the box carries the limit. */
inline int stationCap(int setting, int listeners, int limit, int muxes) {
    if (setting == 0 || listeners <= 0) return 0;
    if (setting == -1 && listeners < std::max(1, limit)) return 0;
    return std::max(0, std::min(listeners, std::max(1, limit)) - std::max(1, muxes));
}

/** One multiplex's clock reading, for the radio's ONE PPM controller (§11.2). */
struct Reading { double ppm = 0; double merDb = 0; };
/** ★ The MER-weighted median of the multiplexes' offsets (ppm) — every one measures the same crystal. Readings further
 *  than `outlierPpm` from the median are dropped (a weak multiplex, a transmitter off frequency) and counted in `dropped`.
 *  Returns false with no usable reading. */
inline bool weightedMedianPpm(std::vector<Reading> r, double outlierPpm, double& out, int& dropped) {
    dropped = 0;
    r.erase(std::remove_if(r.begin(), r.end(), [](const Reading& x) { return !(x.merDb > 0) || !std::isfinite(x.ppm); }), r.end());
    if (r.empty()) return false;
    auto wmed = [](std::vector<Reading> v) {
        std::sort(v.begin(), v.end(), [](const Reading& a, const Reading& b) { return a.ppm < b.ppm; });
        double total = 0; for (const auto& x : v) total += x.merDb;
        double acc = 0;
        for (const auto& x : v) { acc += x.merDb; if (acc >= total / 2) return x.ppm; }
        return v.back().ppm;
    };
    const double m0 = wmed(r);
    std::vector<Reading> keep;
    for (const auto& x : r) { if (std::fabs(x.ppm - m0) <= outlierPpm) keep.push_back(x); else ++dropped; }
    if (keep.empty()) return false;
    out = wmed(keep);
    return true;
}

}  // namespace wide
}  // namespace vibedab
