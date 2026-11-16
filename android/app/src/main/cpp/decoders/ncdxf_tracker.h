// ncdxf_tracker.h — V12 2026-11-16 THE NCDXF / IARU BEACON TRACKER: which of the 18 International Beacon Project beacons
// a listener hears, on which band, and how far down their four power steps. docs/v12/NCDXF-BEACONS.md.
//
// ★★ THE PUBLISHED SCHEDULE (NCDXF / IARU International Beacon Project): 18 beacons share 14.100, 18.110, 21.150, 24.930
//    and 28.200 MHz. Each sends for 10 s: its callsign in CW at 22 WPM, then four one-second dashes, the callsign and the
//    first dash at 100 W, the others at 10 W, 1 W and 0.1 W; then it moves up to the next band. The cycle is 3 minutes,
//    locked to UTC: 4U1UN is on 14.100 at hh:00:00, hh:03:00 … and on 18.110 ten seconds later. So beacon (slot − band)
//    mod 18 is on band `band` in slot (UTC seconds mod 180) / 10 — no decoding is needed to know WHO should be there,
//    only to know whether they ARE (heard, and down to which step) and that it is them (the callsign's keying).
// ★★ A LISTENER'S DECODER ON THEIR AUDIO (vibe_decoder_host.h, ext "ncdxf"): tuned to one of the five frequencies in CW
//    or USB, the carrier is a tone at a known audio pitch (the host's audio→RF map, inverted). Off the audio thread (the
//    host's decode queue), with the block's CAPTURE UTC — the slot is cut by when the audio was heard.
// The chain (scalar — a mixer and three one-poles at 48 kHz, then per 10 s slot a 17-bin DFT on 20 ms frames: ~10 M
//   multiply-adds per slot, under 0.1 % of a core; not a hot loop):
//   1. mix the expected tone to 0 Hz, low-pass (±300 Hz), decimate to 2 kHz complex; keep the last 16 s (bounded: 32 k
//      complex floats, 256 KB);
//   2. 2 s after a slot ends, cut its 20 ms frames, the power in 17 bins −200…+200 Hz: the signal's bin is the strongest
//      within ±75 Hz (a receiver a little off, a beacon's own offset);
//   3. every beacon's keying (callsign at 22 WPM, four dashes) is slid ±1.5 s against that bin (normalised correlation);
//      the scheduled one's best lag is where it is (and says how far the server's clock is from the beacons' GPS time);
//   4. the noise is that bin's own key-up frames (median / ln 2); each dash's SNR is its middle 800 ms against it, and a
//      step is HEARD at ≥ kHeardDb;
//   5. the callsign: "ID ✓" when the scheduled beacon's keying fits best, "another beacon" when one of the others
//      clearly does (a clock out by a slot, a mistuned band) — and then nothing is credited.
// ★ Never claims a step from noise: the threshold is ~4 σ of a 1 s mean in the noise, and a step is counted only from the
//   100 W dash downward without a gap (a 1 W dash "heard" while the 10 W one is not is noise or QSB, and is not shown).
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace vibe {
namespace ncdxf {

struct Beacon { const char* call; const char* where; double lat, lon; };
/** ★ The published order (ncdxf.org/beacon): slot 0 on 14.100 at hh:00:00 is 4U1UN. Places approximate (the grid's
 *  distance column only). */
inline const Beacon kBeacons[18] = {
    { "4U1UN",  "United Nations, New York", 40.75, -73.97 },
    { "VE8AT",  "Eureka, Nunavut",          79.99, -85.93 },
    { "W6WX",   "Mt Umunhum, California",   37.16, -121.90 },
    { "KH6RS",  "Maui, Hawaii",             20.75, -156.43 },
    { "ZL6B",   "Masterton, New Zealand",  -41.07, 175.65 },
    { "VK6RBP", "Rolystone, Australia",    -32.10, 116.05 },
    { "JA2IGY", "Mt Asama, Japan",          34.45, 136.78 },
    { "RR9O",   "Novosibirsk, Russia",      54.98, 82.90 },
    { "VR2B",   "Hong Kong",                22.27, 114.15 },
    { "4S7B",   "Colombo, Sri Lanka",        6.90, 79.87 },
    { "ZS6DN",  "Pretoria, South Africa",  -25.90, 28.27 },
    { "5Z4B",   "Kikuyu, Kenya",            -1.25, 36.70 },
    { "4X6TU",  "Tel Aviv, Israel",         32.05, 34.78 },
    { "OH2B",   "Lohja, Finland",           60.30, 24.00 },
    { "CS3B",   "Madeira",                  32.80, -17.00 },
    { "LU4AA",  "Buenos Aires, Argentina", -34.62, -58.37 },
    { "OA4B",   "Lima, Peru",              -12.07, -76.95 },
    { "YV5B",   "Caracas, Venezuela",        9.10, -67.80 },
};
inline const double kBandHz[5] = { 14.100e6, 18.110e6, 21.150e6, 24.930e6, 28.200e6 };
inline const char* const kBandName[5] = { "20 m", "17 m", "15 m", "12 m", "10 m" };
inline const char* const kStepName[4] = { "100 W", "10 W", "1 W", "0.1 W" };

/** The band an RF frequency is (±1.5 kHz of a beacon frequency), or −1. */
inline int bandOf(double rfHz) {
    for (int b = 0; b < 5; ++b) if (std::fabs(rfHz - kBandHz[b]) <= 1500.0) return b;
    return -1;
}
/** ★ Who is on `band` in the 10 s slot holding UTC second `utc`. */
inline int beaconAt(int band, long long utc) {
    const int s = int(((utc % 180) + 180) % 180 / 10);
    return ((s - band) % 18 + 18) % 18;
}

// ── the keying ─────────────────────────────────────────────────────────────────────────────────────────────────────
inline const char* morse(char c) {
    switch (c) {
        case 'A': return ".-";   case 'B': return "-..."; case 'C': return "-.-."; case 'D': return "-..";
        case 'E': return ".";    case 'F': return "..-."; case 'G': return "--.";  case 'H': return "....";
        case 'I': return "..";   case 'J': return ".---"; case 'K': return "-.-";  case 'L': return ".-..";
        case 'M': return "--";   case 'N': return "-.";   case 'O': return "---";  case 'P': return ".--.";
        case 'Q': return "--.-"; case 'R': return ".-.";  case 'S': return "...";  case 'T': return "-";
        case 'U': return "..-";  case 'V': return "...-"; case 'W': return ".--";  case 'X': return "-..-";
        case 'Y': return "-.--"; case 'Z': return "--..";
        case '0': return "-----"; case '1': return ".----"; case '2': return "..---"; case '3': return "...--";
        case '4': return "....-"; case '5': return "....."; case '6': return "-...."; case '7': return "--...";
        case '8': return "---.."; case '9': return "----.";
        default: return "";
    }
}
/** ★ The timing, seconds from the slot's start. The schedule says "callsign at 22 WPM, then four one-second dashes";
 *  MEASURED ON AIR (2026-11-16, 14.100 MHz from Stuart's UberSDR, OH2B and CS3B at 18–20 dB): the callsign starts at
 *  the slot's start (±20 ms), the first dash a word space (7 units) after its last element, and the dashes repeat every
 *  ~1.02 s, each ~0.80 s key-down — so "one second" is the dash's PERIOD, not its length. */
struct Timing {
    double wpm = 22;          ///< the callsign's speed (PARIS: a dot is 1.2 / wpm s)
    double idStart = 0.0;     ///< the callsign's first element
    double dashStart = 0.0;   ///< 0 = straight after the callsign and a word space
    double dashLen = 0.80, dashGap = 0.22;   ///< key-down and key-up of each dash (0 gap = a character space)
};
/** The key-down intervals of a beacon's 10 s, from `t` (seconds from the slot start). */
inline void keying(const char* call, const Timing& T, std::vector<std::pair<double, double>>& on, double* dashAt = nullptr) {
    on.clear();
    const double u = 1.2 / T.wpm;
    double t = T.idStart;
    for (const char* p = call; *p; ++p) {
        const char* m = morse(*p);
        for (const char* e = m; *e; ++e) {
            const double len = *e == '-' ? 3 * u : u;
            on.push_back({ t, t + len });
            t += len + u;
        }
        t += 2 * u;   // ★ a character space is three units (one already added)
    }
    t += 4 * u;       // ★ and a word space (seven) before the dashes
    double d = T.dashStart > 0 ? T.dashStart : t;
    if (dashAt) *dashAt = d;
    const double gap = T.dashGap > 0 ? T.dashGap : 3 * u;
    for (int k = 0; k < 4; ++k) { on.push_back({ d, d + T.dashLen }); d += T.dashLen + gap; }
}

/** One beacon's 10 s, as heard. */
struct SlotResult {
    long long utc = 0;          ///< the slot's start (UTC seconds, a multiple of 10)
    int band = -1, beacon = -1;
    float snr[4] = { -99, -99, -99, -99 };   ///< each dash, dB over the noise in ~50 Hz
    int steps = 0;              ///< dashes heard from 100 W down, without a gap (0–4)
    int id = 0;                 ///< 1 the callsign fits · 0 not judged / unclear · −1 another beacon's fits better
    int other = -1;             ///< for id −1: whose
    float lagS = 0;             ///< where the keying was found against the schedule (+ = late: the server's clock is fast)
    float offHz = 0;            ///< the carrier against the expected pitch
    float fit = 0;              ///< how well the scheduled beacon's whole keying fits (normalised correlation, −1…1)
    float idFit = 0;            ///< the scheduled callsign's own fit (its callsign part alone)
    float fitOther = 0;         ///< the best callsign fit of the other 17 (and `other` is whose)
};

/** One cell of the propagation grid. */
struct Cell { long long heard = 0, tried = 0; int steps = 0; float snr = -99; int id = 0; long long bestAt = 0; int best = 0; };

class Tracker {
public:
    static constexpr double kHeardDb = 2.0;       ///< a 1 s dash at this SNR (per-bin) is ~4 σ of the noise's 1 s mean
    static constexpr double kLagS = 1.5;          ///< the keying is searched this far either side of the schedule
    static constexpr double kFitMin = 0.25;       ///< the keying's fit a slot needs before any step is counted

    explicit Tracker(int rate = 48000) : rate_(rate > 0 ? rate : 48000) {
        dec_ = std::max(1, rate_ / 2000);
        fsD_ = double(rate_) / dec_;
        lpA_ = 1.0 - std::exp(-2.0 * M_PI * 300.0 / rate_);
        ring_.assign(size_t(16.0 * fsD_), std::complex<float>(0, 0));
        const int fl = int(std::lround(0.020 * fsD_));
        tw_.resize(size_t(17 * fl));
        for (int k = 0; k < 17; ++k) for (int j = 0; j < fl; ++j) {
            const double w = -2 * M_PI * (-200.0 + 25.0 * k) / fsD_ * j;
            tw_[size_t(k * fl + j)] = std::complex<float>(float(std::cos(w)), float(std::sin(w)));
        }
    }

    Timing timing;   ///< the keying's timing (tests change it to check the search)

    /** The expected audio pitch of the beacon carrier, and which band the dial is on (−1 = not a beacon frequency). */
    void setTone(double audioHz, int band) {
        if (band != band_ || std::fabs(audioHz - toneHz_) > 1) { ringN_ = 0; lastJudged_ = 0; }
        toneHz_ = audioHz; band_ = band;
        w_ = 2 * M_PI * audioHz / rate_;
    }
    int band() const { return band_; }

    /** Audio (48 kHz mono float), `utcLast` = the capture UTC of its last sample. */
    void feed(const float* x, int n, double utcLast) {
        if (n <= 0 || band_ < 0) return;
        const double t0 = utcLast - double(n - 1) / rate_;
        for (int i = 0; i < n; ++i) {
            const double c = std::cos(ph_), s = std::sin(ph_);
            ph_ += w_; if (ph_ > M_PI) ph_ -= 2 * M_PI;
            double re = x[i] * c, im = -x[i] * s;
            for (int k = 0; k < 3; ++k) { li_[k] += lpA_ * (re - li_[k]); re = li_[k]; lq_[k] += lpA_ * (im - lq_[k]); im = lq_[k]; }
            if (++dc_ < dec_) continue;
            dc_ = 0;
            ring_[ringW_] = std::complex<float>(float(re), float(im));
            ringW_ = (ringW_ + 1) % ring_.size();
            ++ringN_;
            lastUtc_ = t0 + double(i) / rate_;
        }
        // ★ judge every slot whose end + margin has passed and whose audio is all in the ring
        const long long slot = (long long)std::floor((lastUtc_ - kLagS - 0.5 - 10.0) / 10.0) * 10;
        if (slot > lastJudged_) { judge(slot); lastJudged_ = slot; }   // ★ judge() skips a slot the ring does not wholly hold
    }

    std::function<void(const SlotResult&)> onSlot;
    const Cell& cell(int beacon, int band) const { return grid_[beacon][band]; }

    /** {"beacons":[[call,where,lat,lon]…],"bands":[Hz…],"cells":[[beacon,band,heardAgoS,steps,snr,id,triedAgoS]…]} — only
     *  the cells tried; times as seconds before `now`. */
    std::string gridJson(double now) const {
        std::string o = "\"beacons\":[";
        char b[200];
        for (int i = 0; i < 18; ++i) {
            std::snprintf(b, sizeof b, "%s[\"%s\",\"%s\",%.2f,%.2f]", i ? "," : "", kBeacons[i].call, kBeacons[i].where, kBeacons[i].lat, kBeacons[i].lon);
            o += b;
        }
        o += "],\"bands\":[14100000,18110000,21150000,24930000,28200000],\"cells\":[";
        bool first = true;
        for (int i = 0; i < 18; ++i) for (int k = 0; k < 5; ++k) {
            const Cell& c = grid_[i][k];
            if (!c.tried) continue;
            std::snprintf(b, sizeof b, "%s[%d,%d,%.0f,%d,%.1f,%d,%.0f,%d,%.0f]", first ? "" : ",", i, k,
                          c.heard ? now - double(c.heard) : -1.0, c.steps, c.snr, c.id, now - double(c.tried),
                          c.best, c.bestAt ? now - double(c.bestAt) : -1.0);
            o += b; first = false;
        }
        return o + "]";
    }

    /** ★ For tests: judge the slot starting at `utc` now. */
    void judgeForTest(long long utc) { judge(utc); }

private:
    void judge(long long slot) {
        // frames of 20 ms over [slot − kLagS − 0.5, slot + 10 + kLagS + 0.5]
        const double from = double(slot) - kLagS - 0.5, to = double(slot) + 10.0 + kLagS + 0.5;
        const double ringStart = lastUtc_ - double(std::min(ringN_, ring_.size()) - 1) / fsD_;
        if (from < ringStart || to > lastUtc_ + 1e-6) return;
        const int fl = int(std::lround(0.020 * fsD_));
        const int nf = int((to - from) / 0.020);
        static constexpr int kBins = 17;
        frames_.assign(size_t(nf) * kBins, 0.0f);
        const size_t n = ring_.size();
        const long long iFrom = (long long)std::llround((from - ringStart) * fsD_);
        const size_t base = (ringW_ + n - std::min(ringN_, n)) % n;
        for (int f = 0; f < nf; ++f) {
            for (int k = 0; k < kBins; ++k) {
                std::complex<float> acc(0, 0);
                const std::complex<float>* w = &tw_[size_t(k * fl)];
                for (int j = 0; j < fl; ++j) acc += ring_[(base + size_t(iFrom + (long long)f * fl + j)) % n] * w[j];
                frames_[size_t(f) * kBins + size_t(k)] = std::norm(acc);
            }
        }
        // ★ the signal's bin: the strongest mean within ±75 Hz over the slot (in the bin's own noise, from its key-up
        //   frames below — never from neighbouring bins, which a narrow CW filter or the low-pass shapes)
        const int f0 = int(std::lround((kLagS + 0.5) / 0.020)), f1 = f0 + 500;
        double mean[kBins] = {};
        for (int f = f0; f < f1 && f < nf; ++f) for (int k = 0; k < kBins; ++k) mean[k] += frames_[size_t(f) * kBins + size_t(k)];
        int best = 8;
        for (int k = 5; k <= 11; ++k) if (mean[k] > mean[best]) best = k;
        e_.assign(size_t(nf), 0.0f);
        for (int f = 0; f < nf; ++f) e_[size_t(f)] = frames_[size_t(f) * kBins + size_t(best)];
        SlotResult r;
        r.utc = slot; r.band = band_; r.beacon = beaconAt(band_, slot); r.offHz = float(-200.0 + 25.0 * best);
        // ★ every beacon's whole 10 s keying, slid ±kLagS: the normalised correlation and where it is best
        const int lagMax = int(std::lround(kLagS / 0.020));
        double full[18]; int lagOf[18]; double dashOf[18];
        std::vector<std::pair<double, double>> on;
        std::vector<float> tpl;
        for (int b = 0; b < 18; ++b) {
            keying(kBeacons[b].call, timing, on, &dashOf[b]);
            templ(on, 0, 10.0, tpl);
            double bs = -2; int bl = 0;
            for (int L = -lagMax; L <= lagMax; ++L) { const double sc = ncorr(tpl, f0 + L); if (sc > bs) { bs = sc; bl = L; } }
            full[b] = bs; lagOf[b] = bl;
        }
        const int me = r.beacon;
        const int bestLag = lagOf[me];
        r.lagS = float(bestLag * 0.020);
        r.fit = float(full[me]);
        // ★ the noise in the signal's bin: its key-up frames (40 ms clear of any element) inside the slot, from their
        //   median (a frame's power in noise is exponential: mean = median / ln 2) — a QSB dip or a burst moves it little
        keying(kBeacons[me].call, timing, on, nullptr);
        templ(on, 0, 10.0, tpl);
        std::vector<float> up;
        for (int k = 2; k + 2 < (int)tpl.size(); ++k) {
            if (tpl[size_t(k - 2)] || tpl[size_t(k - 1)] || tpl[size_t(k)] || tpl[size_t(k + 1)] || tpl[size_t(k + 2)]) continue;
            const int f = f0 + bestLag + k;
            if (f >= 0 && f < nf) up.push_back(e_[size_t(f)]);
        }
        if (up.size() < 20) { if (onSlot) onSlot(r); return; }
        std::nth_element(up.begin(), up.begin() + up.size() / 2, up.end());
        const double noise = std::max(1e-30, double(up[up.size() / 2]) / std::log(2.0));
        // ★ each dash's middle (its length less 100 ms at each end)
        const double period = timing.dashLen + (timing.dashGap > 0 ? timing.dashGap : 3.6 / timing.wpm);
        for (int d = 0; d < 4; ++d) {
            const double a = dashOf[me] + d * period + 0.1;
            const int fa = f0 + bestLag + int(std::lround(a / 0.020)), fb = fa + int(std::lround((timing.dashLen - 0.2) / 0.020));
            double sum = 0; int m = 0;
            for (int f = std::max(0, fa); f < std::min(nf, fb); ++f) { sum += e_[size_t(f)]; ++m; }
            r.snr[d] = m ? float(10 * std::log10(std::max(1e-6, sum / m / noise))) : -99.f;
        }
        // ★ and only where the beacon's keying is THERE: in noise the best fit over every lag reads ≤ 0.16 and the best
        //   "dash" ≤ 2.1 dB (test-ncdxf, 140 slots); a beacon 8 dB under the noise in 2.5 kHz still fits ≥ 0.43
        if (r.fit >= kFitMin) for (int d = 0; d < 4 && r.snr[d] >= kHeardDb; ++d) r.steps = d + 1;
        // ★ the callsign: each beacon's CALLSIGN part alone, where its whole keying put it (±60 ms) — the four dashes
        //   are alike for every beacon, so they find the place and the callsign decides whose it is (on air the whole
        //   pattern told OH2B from VR2B by only 0.04; the callsign part alone tells them apart clearly)
        if (r.steps >= 1) {
            double idSc[18];
            for (int b = 0; b < 18; ++b) {
                keying(kBeacons[b].call, timing, on, nullptr);
                templ(on, 0, dashOf[b] - 0.10, tpl);
                double bs = -2;
                for (int L = lagOf[b] - 3; L <= lagOf[b] + 3; ++L) bs = std::max(bs, ncorr(tpl, f0 + L));
                idSc[b] = bs;
            }
            double other = -2; int who = -1;
            for (int b = 0; b < 18; ++b) if (b != me && idSc[b] > other) { other = idSc[b]; who = b; }
            r.idFit = float(idSc[me]); r.fitOther = float(other); r.other = who;
            if (idSc[me] >= 0.35 && idSc[me] >= other + 0.05) r.id = 1;   // ★ on air: OH2B 0.66 against VR2B 0.60 (both end "2B")
            else if (other >= 0.35 && other >= idSc[me] + 0.15) r.id = -1;
        }
        // ★ the grid: a slot that is someone else's is not credited to the scheduled beacon
        Cell& c = grid_[r.beacon][band_];
        c.tried = slot + 10;
        if (r.steps > 0 && r.id >= 0) {
            c.heard = slot + 10; c.steps = r.steps; c.snr = r.snr[0]; c.id = r.id;
            if (r.steps >= c.best || slot + 10 - c.bestAt > 3600) { c.best = r.steps; c.bestAt = slot + 10; }
        }
        if (onSlot) onSlot(r);
    }
    /** The keying as frames (1 key down / 0 up) over [a, b) seconds of the slot. */
    void templ(const std::vector<std::pair<double, double>>& on, double a, double b, std::vector<float>& t) const {
        const int n = int(std::lround((b - a) / 0.020));
        t.assign(size_t(std::max(0, n)), 0.0f);
        for (const auto& iv : on) {
            const int fa = int(std::lround((iv.first - a) / 0.020)), fb = int(std::lround((iv.second - a) / 0.020));
            for (int f = std::max(0, fa); f < std::min(n, fb); ++f) t[size_t(f)] = 1.0f;
        }
    }
    /** The normalised correlation (−1…1) of the template's zero-mean shape with the signal from `at`. */
    double ncorr(const std::vector<float>& t, int at) const {
        if (t.empty()) return 0;
        double mt = 0, me = 0; int m = 0;
        for (size_t k = 0; k < t.size(); ++k) { const long long f = at + (long long)k; if (f < 0 || f >= (long long)e_.size()) continue; mt += t[k]; me += e_[size_t(f)]; ++m; }
        if (m < 10) return 0;
        mt /= m; me /= m;
        double sxy = 0, sxx = 0, syy = 0;
        for (size_t k = 0; k < t.size(); ++k) {
            const long long f = at + (long long)k; if (f < 0 || f >= (long long)e_.size()) continue;
            const double x = t[k] - mt, y = e_[size_t(f)] - me;
            sxy += x * y; sxx += x * x; syy += y * y;
        }
        return sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : 0;
    }

    const int rate_;
    int dec_ = 24, dc_ = 0;
    double fsD_ = 2000;
    double lpA_ = 0, li_[3] = {0, 0, 0}, lq_[3] = {0, 0, 0};
    double ph_ = 0, w_ = 0, toneHz_ = 0;
    int band_ = -1;
    std::vector<std::complex<float>> ring_;
    size_t ringW_ = 0, ringN_ = 0;
    double lastUtc_ = 0;
    long long lastJudged_ = 0;
    std::vector<float> frames_, e_;
    std::vector<std::complex<float>> tw_;   ///< the 17 bins' twiddles, one 20 ms frame each
    Cell grid_[18][5];
};

}  // namespace ncdxf
}  // namespace vibe
