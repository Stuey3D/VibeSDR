// decoders/digi_bands.h — the BANDS Band mode steps through and the published DIAL frequency of every digital mode on
// them (V12, 2026-10-26). Pure data and pure functions: the host, the shim's hwinfo, the benchmark and the tests read it.
//
// ★★★ WHERE THE NUMBERS COME FROM (typed in here, never fetched):
//   • HF and 630/2200 m dials: the frequencies WSJT-X and JS8Call ship as their defaults, which is where the activity
//     actually is. CHECKED 2026-10-26 against a day of real decodes on Stuart's UberSDR (STUEY3D, 2026-10-25): every
//     FT8, FT4, WSPR and JS8 dial below matched the lowest spot frequency heard on it.
//   • VHF/UHF: checked against the IARU Region 1 VHF band plan (Novi Sad, effective December 2020) and its UHF plan —
//     every FT8/FT4 dial here falls inside a segment that permits MGM (50.300–50.400 "narrow band modes, MGM";
//     70.100–70.250 "SSB, telegraphy, MGM"; 144.150–144.400 "SSB, telegraphy, MGM"; 432.100–432.400 "telegraphy, SSB,
//     MGM"). WSPR: 70.091 is the plan's own "personal WSPR beacons" frequency; on 2 m the plan sets 144.491–144.493 aside
//     for personal weak-signal MGM beacons, so the dial is 144.4905 (WSPR audio 1400–1600 Hz → 144.4919–144.4921 MHz,
//     the RSGB's 144.492). WSPR on 6 m and 70 cm is LEFT OUT: the usual WSJT-X dials (50.293, 432.300) sit outside the
//     Region 1 plan's MGM/beacon segments, and a region-aware table is a later job.
//   • FT2: no published band plan. Its dials are the ones FT2's own software (IU8LMC's Decodium) uses and Stuart's
//     UberSDR decodes on: 160 1.843 · 80 3.578 · 60 5.357 · 40 7.052 · 30 10.144 · 20 14.084 · 17 18.108 · 15 21.144 ·
//     12 24.923 · 10 28.184 MHz (20 m and 17 m read in Decodium's source; checked 2026-10-26, docs/v12/JS8-FT2.md).
//   • JTTY (V12, 2026-10-27): the calling frequencies announced with WSJT-X 3.2 ("may change"), kept only where PSKReporter
//     shows JTTY being used or the dial is clear of every other mode's window. CONFIRMED by PSKReporter's JTTY reports
//     (2026-10-26/27, ~/VibeSDR-test-audio/jtty and bandall): 7.090, 14.090 (the busiest — 88 of 128 reports in a day),
//     21.090 — every report 1.0–1.9 kHz above those dials. Announced, clear, not yet seen: 28.090, 50.160, 144.160.
//     LEFT OUT: 1.838, 3.575, 10.140, 18.100 and 24.920 — each sits inside an FT8 or FT4 window (3.575 and 10.140 ARE
//     FT4's dials, 18.100 is FT8's), nobody was heard on them, and a JTTY engine there would be gated open by FT8/FT4
//     energy all day: the costliest engine we have, decoding nothing.
//   • PSK31/63 (V12, 2026-10-27): the long-standing PSK31 calling frequencies (USB dial, PSK traffic 0.5–3 kHz above):
//     3.580 · 7.040 · 10.142 · 14.070 · 18.100 · 21.070 · 24.920 · 28.120. 14.070 CONFIRMED (PSKReporter's PSK31 reports
//     on 2026-10-26: every one 14.0700–14.0718 MHz; Stuart's UberSDR at 16:05 UTC: EA5JTI, OE5BAY, IW2NWK at 14.071).
//     The others are published plan, not seen on air this week (Monday's 07:30 sweep of every PSK segment on the
//     UberSDR heard no PSK at all). 21.070, not 21.080: the PSK31 segment's own frequency. 18.100 is shared with FT8 —
//     both run on one tuner there (the PSK skimmer reads nothing from FT8: docs/v12/PSK.md).
//   • Q65 (V12, 2026-10-31): only where the IARU Region 1 plan allows MGM AND PSKReporter shows Q65 in use. PSKReporter's
//     Q65 reports for the 24 h to 2026-10-31 01:57 UTC (all receivers): 1296.056–1296.121, 432.058–432.092, 144.181 MHz
//     and nothing on HF, 6 m or 4 m. The Q65 window is 200–4800 Hz above the dial (EME stations spread across their
//     segment), so: 2 m 144.115 and 144.120 (the plan's 144.110–144.160 "EME MGM"; random digital EME centres on
//     144.125) and 144.180 (the reports; 144.150–144.400 is SSB/telegraphy/MGM); 70 cm 432.055, 432.065 and 432.087
//     (the plan's 432.025–432.100 telegraphy/MGM, which holds every report). LEFT OUT: 6 m (50.310–50.320 is the plan's
//     EME centre of activity, but it is FT8's 50.313 and nobody was heard on Q65 there) and 23 cm (where most of the
//     reports are: the Band list stops at 70 cm — Stuart to decide whether 23 cm joins it).
//   • 60 m WSPR is 5.3647 (inside the WRC-15 allocation 5351.5–5366.5 kHz that every region shares), not the older
//     national 5.2872, which would stretch the 60 m passband to 73 kHz for one beacon frequency.
//
// ★★ A BAND'S PASSBAND IS WORKED OUT FROM THE REGISTERED MODES ONLY (spanOf). With FT8 + FT4 registered, 20 m is
//    14.0738–14.0838 MHz (10 kHz); when WSPR registers it grows to 14.0973 (23.5 kHz) by itself. The passband never covers
//    a mode nobody can decode — the CPU and the drawn VFO say what is actually being read.
#pragma once
#include "digi_mode.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace vibe {
namespace digi {

/** An amateur band Band mode can sit on: its id ("20m"), the readout's name ("20 m"), and its edges (the widest of the
 *  three IARU regions — the server's own band plan decides what may be TUNED; this only names the band). */
struct Band { const char* id; const char* name; double loHz, hiHz; };

/** In STEP ORDER: 2200 m … 10 m, then 6 m, 4 m, 2 m, 70 cm (Stuart's order). */
inline const std::vector<Band>& bands() {
    static const std::vector<Band> b = {
        {"2200m", "2200 m",   135700,    137800},
        {"630m",  "630 m",    472000,    479000},
        {"160m",  "160 m",   1800000,   2000000},
        {"80m",   "80 m",    3500000,   4000000},
        {"60m",   "60 m",    5250000,   5450000},
        {"40m",   "40 m",    7000000,   7300000},
        {"30m",   "30 m",   10100000,  10150000},
        {"20m",   "20 m",   14000000,  14350000},
        {"17m",   "17 m",   18068000,  18168000},
        {"15m",   "15 m",   21000000,  21450000},
        {"12m",   "12 m",   24890000,  24990000},
        {"10m",   "10 m",   28000000,  29700000},
        {"6m",    "6 m",    50000000,  54000000},
        {"4m",    "4 m",    70000000,  70500000},
        {"2m",    "2 m",   144000000, 148000000},
        {"70cm",  "70 cm", 420000000, 450000000},
    };
    return b;
}

/** One published dial: mode name, band id, dial frequency (USB, Hz). */
struct Dial { const char* mode; const char* band; double hz; };

/** ★ The published dials, typed in (see the file note for each source). Modes not yet registered are listed too, so
 *  the engine that registers them needs no dials of its own. */
inline const std::vector<Dial>& publishedDials() {
    static const std::vector<Dial> d = {
        // FT8 (WSJT-X defaults; HF checked against STUEY3D's decodes, VHF/UHF against the IARU R1 plans)
        {"FT8", "160m",  1840000}, {"FT8", "80m",  3573000}, {"FT8", "60m",  5357000}, {"FT8", "40m",  7074000},
        {"FT8", "30m",  10136000}, {"FT8", "20m", 14074000}, {"FT8", "17m", 18100000}, {"FT8", "15m", 21074000},
        {"FT8", "12m",  24915000}, {"FT8", "10m", 28074000}, {"FT8", "6m",  50313000}, {"FT8", "4m",  70154000},
        {"FT8", "2m",  144174000}, {"FT8", "70cm", 432174000},
        // FT4 (the FT4 protocol's published dials, as WSJT-X ships them)
        {"FT4", "80m",   3575000}, {"FT4", "40m",  7047500}, {"FT4", "30m", 10140000}, {"FT4", "20m", 14080000},
        {"FT4", "17m",  18104000}, {"FT4", "15m", 21140000}, {"FT4", "12m", 24919000}, {"FT4", "10m", 28180000},
        {"FT4", "6m",   50318000}, {"FT4", "2m", 144170000},
        // WSPR (WSJT-X's WSPR dials; 60 m and 2 m — see the file note; none on 6 m / 70 cm yet)
        {"WSPR", "2200m",  136000}, {"WSPR", "630m",  474200}, {"WSPR", "160m", 1836600}, {"WSPR", "80m", 3568600},
        {"WSPR", "60m",   5364700}, {"WSPR", "40m",  7038600}, {"WSPR", "30m", 10138700}, {"WSPR", "20m", 14095600},
        {"WSPR", "17m",  18104600}, {"WSPR", "15m", 21094600}, {"WSPR", "12m", 24924600}, {"WSPR", "10m", 28124600},
        {"WSPR", "4m",   70091000}, {"WSPR", "2m", 144490500},
        // FST4W shares WSPR's sub-bands (the same dials)
        {"FST4W", "2200m", 136000}, {"FST4W", "630m", 474200}, {"FST4W", "160m", 1836600}, {"FST4W", "80m", 3568600},
        {"FST4W", "40m",  7038600}, {"FST4W", "30m", 10138700}, {"FST4W", "20m", 14095600},
        // JS8 (JS8Call's defaults)
        {"JS8", "160m",  1842000}, {"JS8", "80m",  3578000}, {"JS8", "40m",  7078000}, {"JS8", "30m", 10130000},
        {"JS8", "20m",  14078000}, {"JS8", "17m", 18104000}, {"JS8", "15m", 21078000}, {"JS8", "12m", 24922000},
        {"JS8", "10m",  28078000}, {"JS8", "6m",  50318000},
        // FT2 (Decodium's dials, as Stuart's UberSDR decodes them — see the file note)
        {"FT2", "160m",  1843000}, {"FT2", "80m",  3578000}, {"FT2", "60m",  5357000}, {"FT2", "40m",  7052000},
        {"FT2", "30m",  10144000}, {"FT2", "20m", 14084000}, {"FT2", "17m", 18108000}, {"FT2", "15m", 21144000},
        {"FT2", "12m",  24923000}, {"FT2", "10m", 28184000},
        // JTTY (V12 — see the file note: confirmed 40/20/15 m; announced and clear 10/6/2 m)
        {"JTTY", "40m",  7090000}, {"JTTY", "20m", 14090000}, {"JTTY", "15m", 21090000}, {"JTTY", "10m", 28090000},
        {"JTTY", "6m",  50160000}, {"JTTY", "2m", 144160000},
        // Q65 (V12 — EME/scatter; see the file note)
        {"Q65", "2m", 144115000}, {"Q65", "2m", 144120000}, {"Q65", "2m", 144180000},
        {"Q65", "70cm", 432055000}, {"Q65", "70cm", 432065000}, {"Q65", "70cm", 432087000},
        // PSK31/63 (V12 — the PSK31 calling frequencies; 14.070 confirmed)
        {"PSK", "80m",   3580000}, {"PSK", "40m",  7040000}, {"PSK", "30m", 10142000}, {"PSK", "20m", 14070000},
        {"PSK", "17m",  18100000}, {"PSK", "15m", 21070000}, {"PSK", "12m", 24920000}, {"PSK", "10m", 28120000},
    };
    return d;
}

inline bool sameName(const std::string& a, const char* b) {
    if (a.size() != std::strlen(b)) return false;
    for (size_t i = 0; i < a.size(); ++i) if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i])) return false;
    return true;
}

/** A mode's dials on one band: its own (ModeDef::dials) if it lists any, else the published table by name. */
inline std::vector<double> dialsOf(const ModeDef& m, const std::string& band) {
    std::vector<double> out;
    if (!m.dials.empty()) { for (auto& d : m.dials) if (d.first == band) out.push_back(d.second); return out; }
    for (const Dial& d : publishedDials()) if (sameName(m.name, d.mode) && band == d.band) out.push_back(d.hz);
    return out;
}

inline const Band* bandById(const std::string& id) {
    for (const Band& b : bands()) if (id == b.id) return &b;
    return nullptr;
}
/** The band an RF frequency is inside, or null. */
inline const Band* bandAt(double hz) {
    for (const Band& b : bands()) if (hz >= b.loHz && hz <= b.hiHz) return &b;
    return nullptr;
}

/** ★ Guard each side of the passband: what the slice's filter rolls off in, so no mode's edge sits on it. */
constexpr double kSpanGuardHz = 250.0;

/** ★★ THE WIDEST A BAND'S PASSBAND MAY BE: 48 kHz — 40 m's whole cluster (WSPR 7.0386 → JS8 7.078, ≈ 42 kHz) fits, and
 *  it is what a 96 kHz-class slice carries comfortably on every radio we drive. Where a band's dials are spread wider
 *  (15 m: FT8 21.074 but FT4 21.140; 10 m: FT8 28.074 but FT4 28.180; 2 m: FT8 144.174 but WSPR 144.4905) Band mode
 *  takes the CLUSTER around the FT8 dial — the busiest — and names the modes left OUTSIDE it (Span::outside), rather
 *  than a 100–300 kHz slice that is mostly empty and costs the DSP thread a bigger extract. */
constexpr double kMaxSpanHz = 48000.0;

/** One band as Band mode uses it: the RF span the registered modes cover there (guard included), its centre — where
 *  the dial goes, and so where the IQ slice is cut — and which modes are on it (and which are on the band but too far
 *  from its cluster to share the slice). `ok` false = no registered mode has a dial on this band. */
/** ★★ V12 (2026-10-27, Stuart: "we need to get JTTY to the full band decoder mode"): a band's dials further from its
 *  cluster than kMaxSpanHz get a SLICE OF THEIR OWN — one more extract of the same forward FFT (no extra USB, no extra
 *  transform), its own tuner — instead of being left out. 40 m's JTTY (7.090), 6 m's JTTY (50.160, 153 kHz from FT8),
 *  15 m's and 10 m's FT4, 10/4/2 m's WSPR. Each extra slice is at least kMinSliceHz wide (the tuner's 12 kHz output wants
 *  a slice rate above it). */
constexpr double kMinSliceHz = 24000.0;
struct SubSpan {
    double loHz = 0, hiHz = 0, centreHz = 0;
    std::vector<std::string> modes;
    double widthHz() const { return hiHz - loHz; }
};
struct Span {
    bool ok = false;
    const Band* band = nullptr;
    double loHz = 0, hiHz = 0, centreHz = 0;
    std::vector<std::string> modes;        ///< on the MAIN slice (the cluster round FT8, where the dial goes)
    std::vector<std::string> outside;      ///< ★ on the band but on an extra slice (see `extras`) — never undecoded now
    std::vector<SubSpan> extras;           ///< the band's other clusters, each a slice of its own
    double widthHz() const { return hiHz - loHz; }
    /** Every mode decoded on this band: the main slice's, then the extras'. */
    std::vector<std::string> allModes() const {
        std::vector<std::string> o = modes;
        for (const SubSpan& e : extras) for (const std::string& m : e.modes) {
            bool have = false; for (const std::string& x : o) if (x == m) have = true;
            if (!have) o.push_back(m);
        }
        return o;
    }
};
inline Span spanOf(const Band& b, const std::vector<ModeDef>& ms) {
    Span s; s.band = &b;
    struct Iv { size_t mode; double lo, hi; };
    std::vector<Iv> iv;
    for (size_t k = 0; k < ms.size(); ++k)
        for (double d : dialsOf(ms[k], b.id)) iv.push_back({ k, d + ms[k].audioLoHz, d + ms[k].audioHiHz });
    if (iv.empty()) return s;
    // ★ The anchor: FT8's passband if FT8 is on this band (the busiest), else the first registered mode's.
    size_t a = 0;
    for (size_t i = 0; i < iv.size(); ++i) if (sameName(ms[iv[i].mode].name, "FT8")) { a = i; break; }
    const double ac = 0.5 * (iv[a].lo + iv[a].hi);
    std::vector<size_t> order(iv.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    /* ★★ V12 (2026-10-27): grown in PRIORITY order (ModeDef::priority — the governor's), nearest first within one
     *  priority. Before, nearest first: a new mode nearer FT8 could push an established one out of the 48 kHz cluster
     *  (JTTY at 7.090 would have pushed 40 m's WSPR out). Now a mode joins the span only where it still fits beside
     *  every mode ranked above it. */
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        if (x == a || y == a) return x == a && y != a;
        const int px = ms[iv[x].mode].priority, py = ms[iv[y].mode].priority;
        if (px != py) return px < py;
        return std::fabs(0.5 * (iv[x].lo + iv[x].hi) - ac) < std::fabs(0.5 * (iv[y].lo + iv[y].hi) - ac);
    });
    double lo = iv[a].lo, hi = iv[a].hi;
    std::vector<bool> in(ms.size(), false), on(ms.size(), false), used(iv.size(), false);
    for (size_t i : order) {
        on[iv[i].mode] = true;
        const double nlo = std::min(lo, iv[i].lo), nhi = std::max(hi, iv[i].hi);
        if (nhi - nlo + 2 * kSpanGuardHz > kMaxSpanHz) continue;   // ★ too far from the cluster: an extra slice
        lo = nlo; hi = nhi; in[iv[i].mode] = true; used[i] = true;
    }
    for (size_t k = 0; k < ms.size(); ++k) {
        if (in[k]) s.modes.push_back(ms[k].name);
        else if (on[k]) s.outside.push_back(ms[k].name);
    }
    /* ★★ THE EXTRA SLICES: what the cluster left, grouped the same way (in `order` — priority, then nearest FT8 — each
     *  group anchored on the first dial still unplaced, growing while it stays within kMaxSpanHz). */
    for (size_t g = 0; g < order.size(); ++g) {
        const size_t a2 = order[g];
        if (used[a2]) continue;
        double elo = iv[a2].lo, ehi = iv[a2].hi;
        used[a2] = true;
        std::vector<size_t> members{ a2 };
        for (size_t h = g + 1; h < order.size(); ++h) {
            const size_t i = order[h];
            if (used[i]) continue;
            const double nlo = std::min(elo, iv[i].lo), nhi = std::max(ehi, iv[i].hi);
            if (nhi - nlo + 2 * kSpanGuardHz > kMaxSpanHz) continue;
            elo = nlo; ehi = nhi; used[i] = true; members.push_back(i);
        }
        SubSpan e;
        e.centreHz = std::round((elo + ehi) / 2.0 / 100.0) * 100.0;
        const double half = std::max(kMinSliceHz / 2.0, std::max(e.centreHz - elo, ehi - e.centreHz) + kSpanGuardHz);
        e.loHz = e.centreHz - half; e.hiHz = e.centreHz + half;
        for (size_t k = 0; k < ms.size(); ++k)
            for (size_t i : members) if (iv[i].mode == k) { e.modes.push_back(ms[k].name); break; }
        s.extras.push_back(e);
    }
    s.ok = true;
    s.loHz = lo - kSpanGuardHz; s.hiHz = hi + kSpanGuardHz;
    s.centreHz = std::round((s.loHz + s.hiHz) / 2.0 / 100.0) * 100.0;   // ★ a round dial: the readout and the drawn VFO agree
    // ★ The rounding moved the centre by < 50 Hz: widen the side it moved away from, so the span stays symmetrical.
    const double half = std::max(s.centreHz - s.loHz, s.hiHz - s.centreHz);
    s.loHz = s.centreHz - half; s.hiHz = s.centreHz + half;
    return s;
}
/** Every band some registered mode is on, in step order, kept only where `reaches(lo, hi)` says the radio can tune the
 *  whole span (an HF+ never lists 70 cm; an RTL without direct sampling lists no HF). */
template <class Reaches>
inline std::vector<Span> reachableSpans(const std::vector<ModeDef>& ms, Reaches reaches) {
    std::vector<Span> out;
    for (const Band& b : bands()) {
        Span s = spanOf(b, ms);
        if (s.ok && reaches(s.loHz, s.hiHz)) out.push_back(s);
    }
    return out;
}

/** ★ BAND STEPPING (the arrows, the knob, the D-pad, the keys, a game controller): the next span `dir` (+1 / −1) from
 *  wherever the dial is. On a band: the one after it. Between bands: the nearest one in that direction. CLAMPED at the
 *  ends, never wrapped (DAB's and ISM's rule: 70 cm → 2200 m in one press is not what a band scan meant). Returns the
 *  index into `spans`, or −1 when `spans` is empty. */
inline int stepSpan(const std::vector<Span>& spans, double dialHz, int dir) {
    if (spans.empty()) return -1;
    const int n = (int)spans.size();
    for (int i = 0; i < n; ++i)
        if (dialHz >= spans[i].band->loHz && dialHz <= spans[i].band->hiHz)
            return std::max(0, std::min(n - 1, i + (dir < 0 ? -1 : 1)));
    if (dir >= 0) { for (int i = 0; i < n; ++i) if (spans[i].centreHz > dialHz) return i; return n - 1; }
    for (int i = n - 1; i >= 0; --i) if (spans[i].centreHz < dialHz) return i;
    return 0;
}

} // namespace digi
} // namespace vibe
