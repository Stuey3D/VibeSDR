// vibe_lora_discover.h — V12 ISM mode: LoRa channels FOUND ON AIR (2026-11-12). docs/v12/LORA-MESH.md §Discovery.
//
// Stuart: in ISM mode "we decoded the entire band that is visible on screen" — but LoRa ran only on the fixed plan
// channels, so a network on a frequency no plan names (Kiko's 915 band in Brazil: 81 bursts, LoRa 0) was never tried.
//
//   1. Shaper  — a small FFT over the WHOLE capture (one 256-point frame in every four, ~2 300 a second at 2.4 MS/s) finds
//                bursts and keeps only LoRa-shaped ones: 62.5 / 125 / 250 / 500 kHz wide (±40 %), a FLAT top over that
//                width, the per-frame peak SWEEPING across it (a chirp visits every part of its band; an FSK burst sits on
//                two tones, a carrier and its sidebands on one), and long enough for a preamble (12.25 symbols at SF7).
//   2. FoundBook — what to do about them: a burst a STANDARD channel explains (any band plan channel within ±BW/4 or
//                ±50 ppm, whichever is wider — an RTL's crystal) brings that channel in; two LoRa-shaped bursts within
//                five minutes at a centre nothing covers start a "found on air" channel there. Spreading factors are
//                tried THREE at a time — the band's mesh presets at that width first, then SF7 upward, never past what
//                the burst's length allows — and a set that hears four bursts and decodes none is retired for the next;
//                a channel that decodes keeps the SFs that decoded. All tried and nothing: retired for an hour.
//                Silent for a week: forgotten. At most eight per band.
//   3. FoundStore — every band's found channels for ONE radio, in one text file beside its bookmarks, written atomically
//                (a temporary file, then rename), read back at start: a restart starts on them.
//
// ★★ THE BUDGET STILL RULES (vibe_lora_service.h choose): found channels run after the default set and before tiers 1
//    and 2, only while the benchmark's budget allows — never on a Pi 2 with no benchmark (the default tier only).
#pragma once
#include "vibe_ism.h"      // cf, DetFft
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace vibelora {

/** One LoRa-shaped burst. `hz` absolute; `sfMax` the largest SF whose preamble alone fits its length. */
struct Shape { double tSec = 0, hz = 0, bw = 0, durSec = 0, snrDb = 0; int sfMax = 12; };

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  1. THE SHAPER
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Shaper {
public:
    static constexpr int N = 256;          ///< ~9.4 kHz bins at 2.4 MS/s (a 62.5 kHz channel is ~7 bins)
    static constexpr int kEvery = 4;       ///< one frame in every four: ~0.43 ms apart at 2.4 MS/s
    struct Counts { uint64_t shapes = 0, narrow = 0, wide = 0, notFlat = 0, noSweep = 0, tooShort = 0, tooLong = 0, weak = 0; };

    void reset(double rateHz, double hwCentreHz) {
        rate_ = rateHz; hc_ = hwCentreHz; binHz_ = rateHz / N; hopSec_ = double(N * kEvery) / rateHz;
        fft_.reset(new vibeism::DetFft(N));
        win_.resize(N); for (int i = 0; i < N; ++i) win_[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / N));
        buf_.assign(N, cf(0, 0)); P_.assign(N, 0.f); S_.assign(N, 0.f); F_.assign(N, 0.f);
        pos_ = 0; frame_ = 0; ev_.clear(); out_.clear(); c_ = Counts{};
    }
    /** Capture samples in (the LoRa thread, or inline). Cheap: a copy, and one FFT per 4·N samples. */
    void feed(const cf* x, size_t n) {
        if (!fft_) return;
        const size_t period = size_t(N) * kEvery;
        for (size_t i = 0; i < n; ++i) {
            const size_t p = pos_ % period;
            if (p < size_t(N)) { buf_[p] = x[i] * win_[p]; if (p == size_t(N) - 1) frame(); }
            ++pos_;
        }
    }
    std::vector<Shape> take() { std::vector<Shape> v; v.swap(out_); return v; }
    const Counts& counts() const { return c_; }

private:
    struct Ev { int lo, hi, first, last; std::vector<float> sum; std::vector<int> peaks; float peakSnr = 0; };
    void frame() {
        fft_->forward(buf_.data());
        // natural order → frequency order: bin k is (k − N/2)·binHz from the radio's centre
        for (int k = 0; k < N; ++k) { const cf v = buf_[size_t((k + N / 2) % N)]; P_[size_t(k)] = std::norm(v); }
        const int edge = int(0.45 * N);
        const int kLo = N / 2 - edge, kHi = N / 2 + edge;
        /* ★★ A DECAYING MAX-HOLD, not the frame's spectrum. One frame is ~0.1 ms: inside it a chirp is one moving TONE
         *  (an SF9 symbol sweeps 125 kHz in 4 ms), so frame by frame LoRa is never wide. Held with a ~70 ms half-life, the
         *  sweep paints its whole band (an SF12 symbol at 62.5 kHz is 65 ms) while a tone or two FSK tones stay narrow.
         *  The floor is tracked on the HELD spectrum, so noise's own max-hold (~7 dB over its mean) is the reference. */
        static const float kHold = float(std::pow(0.5, 1.0 / 160.0));   // −6 dB in 160 frames (~70 ms at 2.4 MS/s)
        for (int k = 0; k < N; ++k) S_[size_t(k)] = std::max(P_[size_t(k)], S_[size_t(k)] * kHold);
        if (frame_ < 240) {   // the first ~100 ms: the hold fills, the floor starts from it
            for (int k = 0; k < N; ++k) F_[size_t(k)] = frame_ >= 200 ? std::min(F_[size_t(k)] ? F_[size_t(k)] : S_[size_t(k)], S_[size_t(k)]) : 0.f;
            ++frame_; return;
        }
        std::vector<char> act(size_t(N), 0);
        for (int k = kLo; k <= kHi; ++k) {
            float& f = F_[size_t(k)]; const float s = S_[size_t(k)];
            const bool on = s > 4.f * f && std::abs(k - N / 2) > 1;          // 6 dB over the held floor; the DC bins never
            act[size_t(k)] = on;
            // ★ the floor: down fast, up slowly — and almost not at all under a burst, so a long packet does not lift it
            if (s < f) f = 0.95f * f + 0.05f * s;
            else f = on ? f * 0.99998f + s * 0.00002f : f * 0.998f + s * 0.002f;
            if (!(f > 1e-20f)) f = 1e-20f;
        }
        // runs of active bins (a gap of one bin, or the DC pair, bridged)
        std::vector<std::pair<int, int>> runs;
        for (int k = kLo; k <= kHi;) {
            if (!act[size_t(k)]) { ++k; continue; }
            int e = k;
            while (e + 1 <= kHi && (act[size_t(e + 1)] || (e + 2 <= kHi && act[size_t(e + 2)]) || (e + 3 <= kHi && std::abs(e + 1 - N / 2) <= 1 && act[size_t(e + 3)]))) ++e;
            if ((e - k + 1) * binHz_ >= 40e3) runs.push_back({k, e});
            k = e + 1;
        }
        for (const auto& r : runs) {
            Ev* m = nullptr;
            for (auto& e : ev_) if (r.first <= e.hi + 2 && r.second >= e.lo - 2) { m = &e; break; }
            if (!m) { ev_.push_back(Ev{r.first, r.second, frame_, frame_, std::vector<float>(size_t(N), 0.f), {}, 0.f}); m = &ev_.back(); }
            m->lo = std::min(m->lo, r.first); m->hi = std::max(m->hi, r.second); m->last = frame_;
            int am = r.first; float best = 0, fl = 0;
            for (int k = r.first; k <= r.second; ++k) { if (P_[size_t(k)] > best) { best = P_[size_t(k)]; am = k; } fl += F_[size_t(k)]; }
            if (m->peaks.size() < 8192) m->peaks.push_back(am);
            m->peakSnr = std::max(m->peakSnr, best / std::max(1e-20f, fl / float(r.second - r.first + 1)));
            (void)0;
            for (int k = std::max(0, r.first - 3); k <= std::min(N - 1, r.second + 3); ++k) m->sum[size_t(k)] += S_[size_t(k)];
        }
        for (size_t i = 0; i < ev_.size();) {
            if (frame_ - ev_[i].last > 6 || frame_ - ev_[i].first > int(8.0 / hopSec_)) { close(ev_[i]); ev_.erase(ev_.begin() + long(i)); }
            else ++i;
        }
        ++frame_;
    }
    void close(const Ev& e) {
        const int nf = e.last - e.first + 1;
        const double dur = nf * hopSec_;
        if (nf < 3) { ++c_.tooShort; return; }
        if (dur > 7.5) { ++c_.tooLong; return; }
        const int lo = std::max(0, e.lo - 3), hi = std::min(N - 1, e.hi + 3);
        float mx = 0; int km = lo;
        for (int k = lo; k <= hi; ++k) if (e.sum[size_t(k)] > mx) { mx = e.sum[size_t(k)]; km = k; }
        if (!(mx > 0)) { ++c_.weak; return; }
        // the occupied span: contiguous bins round the strongest within 6 dB of it
        int a = km, b = km;
        while (a - 1 >= lo && e.sum[size_t(a - 1)] >= mx / 4) --a;
        while (b + 1 <= hi && e.sum[size_t(b + 1)] >= mx / 4) ++b;
        const double width = (b - a + 1) * binHz_;
        double fl = 0; for (int k = a; k <= b; ++k) fl += F_[size_t(k)];
        fl = fl / (b - a + 1) * nf;
        const double snr = 10 * std::log10(std::max(1e-12, double(mx) / std::max(1e-30, fl)));
        if (snr < 8) { ++c_.weak; return; }
        static const double kBw[4] = {62.5e3, 125e3, 250e3, 500e3};
        double bw = 0, bestR = 9;
        for (double c : kBw) { const double r = std::fabs(std::log(width / c)); if (r < bestR) { bestR = r; bw = c; } }
        if (width < 0.6 * 62.5e3) { ++c_.narrow; return; }
        if (bestR > std::log(1.45)) { ++(width > 500e3 ? c_.wide : c_.narrow); return; }
        // ★ flat: the middle 60 % of the span within 6 dB of its strongest (a carrier with sidebands is one tall spike)
        const int m0 = a + int(0.2 * (b - a + 1)), m1 = b - int(0.2 * (b - a + 1));
        float mn = mx; for (int k = m0; k <= m1; ++k) mn = std::min(mn, e.sum[size_t(k)]);
        if (mn < mx / 4) { ++c_.notFlat; return; }
        // ★ a sweep: the frame-by-frame peak visits most of the span (eighths), not two tones or one
        if (b - a + 1 >= 4) {
            int hits[8] = {0};
            for (int p : e.peaks) if (p >= a && p <= b) ++hits[std::min(7, (p - a) * 8 / (b - a + 1))];
            int visited = 0; const int need = std::max(1, int(e.peaks.size()) / 40);
            for (int h : hits) visited += h >= need;
            if (visited < 5) { ++c_.noSweep; return; }
        }
        // ★ long enough for a preamble at SF7 (12.25 symbols) — and the largest SF whose preamble alone fits
        const double minDur = 12.25 * 128.0 / bw;
        if (dur < 0.7 * minDur) { ++c_.tooShort; return; }
        int sfMax = 7; while (sfMax < 12 && 12.25 * double(1 << (sfMax + 1)) / bw <= dur * 1.25) ++sfMax;
        double sw = 0, sf = 0;
        for (int k = a; k <= b; ++k) { sw += e.sum[size_t(k)]; sf += e.sum[size_t(k)] * (k - N / 2) * binHz_; }
        Shape s; s.hz = hc_ + sf / sw; s.bw = bw; s.durSec = dur; s.snrDb = snr; s.sfMax = sfMax; s.tSec = (e.last + 1) * hopSec_;
        out_.push_back(s); ++c_.shapes;
    }
    double rate_ = 0, hc_ = 0, binHz_ = 1, hopSec_ = 1;
    std::unique_ptr<vibeism::DetFft> fft_;
    std::vector<float> win_, P_, S_, F_;
    std::vector<cf> buf_;
    uint64_t pos_ = 0;
    int frame_ = 0;
    std::vector<Ev> ev_;
    std::vector<Shape> out_;
    Counts c_;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  2. THE FOUND CHANNELS OF ONE BAND
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct FoundCh {
    double hz = 0, bw = 0;
    std::string std;                 ///< the plan channel it was snapped to ("" = none: a frequency of its own)
    std::vector<int> order;          ///< every SF to try, in order (presets first)
    std::vector<int> sfs;            ///< the SFs running now
    std::vector<int> good;           ///< the SFs that decoded
    std::vector<int> tried;          ///< guesses retired
    double first = 0, last = 0;      ///< unix seconds: found, last burst or frame
    double retiredUntil = 0;         ///< > now: tried everything, nothing decoded — resting
    int bursts = 0;                  ///< LoRa-shaped bursts on it since its SFs last changed
    uint64_t frames = 0;             ///< frames decoded on it, ever
    std::string name() const {
        char b[96];
        if (!std.empty()) std::snprintf(b, sizeof b, "%s (found on air)", std.c_str());
        else std::snprintf(b, sizeof b, "Found on air %.3f MHz BW%g", hz / 1e6, bw / 1e3);
        return b;
    }
    bool active(double now) const { return retiredUntil <= now && !sfs.empty(); }
};

/** The plan channels a found channel is checked against — name, centre, width, SFs (decoupled from PlanCh). */
struct StdCh { std::string name; double hz, bw; std::vector<int> sfs; bool running; };

class FoundBook {
public:
    static constexpr int kTry = 3;                  ///< SFs on trial at once
    static constexpr int kRetireBursts = 4;         ///< bursts heard, nothing decoded → the next SFs
    static constexpr double kPairSec = 300;         ///< two bursts within five minutes start a channel
    static constexpr double kForgetSec = 7 * 86400.0;
    static constexpr double kRestSec = 3600;
    static constexpr size_t kMax = 8;
    std::vector<FoundCh> list;

    /** A LoRa-shaped burst. True when the set of channels to run changed. `running`: the engine's channel names now — a
     *  found channel's burst counts against its SF guesses only while it is actually listening (not while it waits for
     *  the restart that starts it, nor while the budget holds it back). */
    bool offer(const Shape& s, double now, const std::vector<StdCh>& plan, const std::vector<std::string>& running) {
        const double ppmTol = 50e-6 * s.hz;
        // already listened for — a running plan channel, or a found one
        for (const auto& p : plan) if (p.running && p.bw == s.bw && std::fabs(p.hz - s.hz) <= std::max(p.bw / 4, ppmTol)) return false;
        for (auto& f : list) if (f.bw == s.bw && std::fabs(f.hz - s.hz) <= std::max(f.bw / 4, ppmTol)) {
            f.last = now;
            if (f.retiredUntil > now) return false;
            if (std::find(running.begin(), running.end(), f.name()) != running.end()) ++f.bursts;
            return false;
        }
        // ★ a STANDARD channel that is not running explains it (other regions' channels, tiers past the budget)
        for (const auto& p : plan) if (!p.running && p.bw == s.bw && std::fabs(p.hz - s.hz) <= std::max(p.bw / 4, ppmTol)) {
            FoundCh f; f.hz = p.hz; f.bw = p.bw; f.std = p.name; f.order = p.sfs; f.first = f.last = now;
            for (int sf = 7; sf <= 12; ++sf) if (std::find(f.order.begin(), f.order.end(), sf) == f.order.end() && sf <= s.sfMax) f.order.push_back(sf);
            next(f);
            return add(std::move(f), now);
        }
        // a frequency of its own: wait for a second burst there
        pend_.erase(std::remove_if(pend_.begin(), pend_.end(), [&](const Shape& q) { return now - q.tSec > kPairSec; }), pend_.end());
        Shape here = s; here.tSec = now;
        std::vector<Shape> near;
        for (const auto& q : pend_) if (q.bw == s.bw && std::fabs(q.hz - s.hz) <= s.bw / 4) near.push_back(q);
        if (near.empty()) { if (pend_.size() < 64) pend_.push_back(here); return false; }
        near.push_back(here);
        double hz = 0; int sfMax = 7;
        for (const auto& q : near) { hz += q.hz; sfMax = std::max(sfMax, q.sfMax); }
        hz = std::round(hz / double(near.size()) / 1e3) * 1e3;
        pend_.erase(std::remove_if(pend_.begin(), pend_.end(), [&](const Shape& q) { return q.bw == s.bw && std::fabs(q.hz - s.hz) <= s.bw / 4; }), pend_.end());
        FoundCh f; f.hz = hz; f.bw = s.bw; f.first = f.last = now;
        // the band's mesh / LoRaWAN presets at this width first, then SF7 upward — never past what the bursts allow
        for (const auto& p : plan) if (p.bw == s.bw) for (int sf : p.sfs)
            if (sf <= sfMax && std::find(f.order.begin(), f.order.end(), sf) == f.order.end()) f.order.push_back(sf);
        for (int sf = 7; sf <= sfMax; ++sf) if (std::find(f.order.begin(), f.order.end(), sf) == f.order.end()) f.order.push_back(sf);
        next(f);
        return add(std::move(f), now);
    }
    /** A frame decoded on a found channel (by its name). True when its SFs changed (narrowed to what decodes). */
    bool decoded(const std::string& chanName, int sf, double now) {
        for (auto& f : list) if (f.name() == chanName) {
            ++f.frames; f.last = now;
            if (std::find(f.good.begin(), f.good.end(), sf) == f.good.end()) f.good.push_back(sf);
            std::vector<int> keep = f.good; std::sort(keep.begin(), keep.end());
            if (keep != f.sfs) { f.sfs = keep; f.bursts = 0; return true; }
            return false;
        }
        return false;
    }
    /** Retire guesses that hear bursts and decode nothing; rest a channel that has tried everything; forget the silent. */
    bool tick(double now) {
        bool changed = false;
        for (auto& f : list) {
            if (f.retiredUntil > 0 && f.retiredUntil <= now && f.sfs.empty()) { f.tried.clear(); next(f); f.retiredUntil = 0; changed = true; }
            if (!f.good.empty() || f.retiredUntil > now || f.bursts < kRetireBursts) continue;
            for (int sf : f.sfs) f.tried.push_back(sf);
            f.sfs.clear(); f.bursts = 0;
            next(f);
            if (f.sfs.empty()) f.retiredUntil = now + kRestSec;
            changed = true;
        }
        const size_t n = list.size();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const FoundCh& f) { return now - f.last > kForgetSec; }), list.end());
        return changed || list.size() != n;
    }
    size_t activeCount(double now) const { size_t n = 0; for (const auto& f : list) n += f.active(now); return n; }

private:
    static void next(FoundCh& f) {
        f.sfs.clear();
        for (int sf : f.order) {
            if (int(f.sfs.size()) >= kTry) break;
            if (std::find(f.tried.begin(), f.tried.end(), sf) == f.tried.end()) f.sfs.push_back(sf);
        }
    }
    bool add(FoundCh&& f, double now) {
        if (list.size() >= kMax) {
            // the quietest channel that never decoded makes room; none such: the new one waits
            auto it = std::min_element(list.begin(), list.end(), [](const FoundCh& a, const FoundCh& b) {
                return (a.frames != 0) != (b.frames != 0) ? a.frames == 0 : a.last < b.last; });
            if (it == list.end() || it->frames) return false;
            list.erase(it);
        }
        (void)now;
        list.push_back(std::move(f));
        return true;
    }
    std::vector<Shape> pend_;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  3. THE STORE — one radio's found channels, every band, one file
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class FoundStore {
public:
    /** One line per channel: band hz bw first last retiredUntil bursts frames |order|sfs|good|tried|std */
    static std::string toText(const std::map<std::string, FoundBook>& books) {
        std::ostringstream o;
        o << "# VibeServer LoRa channels found on air (vibe_lora_discover.h) — one per line\n";
        auto ints = [](const std::vector<int>& v) { std::string s; for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + std::to_string(v[i]); return s; };
        for (const auto& kv : books) for (const auto& f : kv.second.list) {
            char b[200];
            std::snprintf(b, sizeof b, "%s %.0f %.0f %.0f %.0f %.0f %d %llu |", kv.first.c_str(), f.hz, f.bw, f.first, f.last, f.retiredUntil,
                          f.bursts, (unsigned long long)f.frames);
            std::string st = f.std; for (char& c : st) if (c == '\n' || c == '|') c = ' ';
            o << b << ints(f.order) << "|" << ints(f.sfs) << "|" << ints(f.good) << "|" << ints(f.tried) << "|" << st << "\n";
        }
        return o.str();
    }
    static void fromText(const std::string& text, std::map<std::string, FoundBook>& books) {
        std::istringstream in(text);
        std::string line;
        auto ints = [](const std::string& s) { std::vector<int> v; std::istringstream q(s); std::string t;
            while (std::getline(q, t, ',')) { const int x = std::atoi(t.c_str()); if (x >= 7 && x <= 12) v.push_back(x); } return v; };
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            const size_t bar = line.find('|');
            if (bar == std::string::npos) continue;
            char band[16] = {0}; double hz = 0, bw = 0, first = 0, last = 0, ret = 0; int bursts = 0; unsigned long long frames = 0;
            if (std::sscanf(line.substr(0, bar).c_str(), "%15s %lf %lf %lf %lf %lf %d %llu", band, &hz, &bw, &first, &last, &ret, &bursts, &frames) != 8) continue;
            if (!(hz > 1e6) || !(bw == 62.5e3 || bw == 125e3 || bw == 250e3 || bw == 500e3)) continue;
            std::vector<std::string> parts; std::string rest = line.substr(bar + 1);
            for (int i = 0; i < 4; ++i) { const size_t p = rest.find('|'); if (p == std::string::npos) break; parts.push_back(rest.substr(0, p)); rest = rest.substr(p + 1); }
            if (parts.size() != 4) continue;
            FoundCh f; f.hz = hz; f.bw = bw; f.first = first; f.last = last; f.retiredUntil = ret; f.bursts = bursts; f.frames = frames;
            f.order = ints(parts[0]); f.sfs = ints(parts[1]); f.good = ints(parts[2]); f.tried = ints(parts[3]); f.std = rest;
            auto& bk = books[band];
            if (bk.list.size() < FoundBook::kMax) bk.list.push_back(std::move(f));
        }
    }
    void setPath(const std::string& p) {
        std::lock_guard<std::mutex> lk(m_);
        path_ = p; books_.clear();
        if (p.empty()) return;
        FILE* fp = std::fopen(p.c_str(), "rb");
        if (!fp) return;
        std::string t; char buf[4096]; size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0 && t.size() < 256 * 1024) t.append(buf, n);
        std::fclose(fp);
        fromText(t, books_);
    }
    /** ★ Atomic: a temporary file beside it, then rename — a power cut leaves the old file or the new, never half. */
    bool save() {
        std::lock_guard<std::mutex> lk(m_);
        if (path_.empty()) return false;
        const std::string t = toText(books_), tmp = path_ + ".tmp";
        FILE* fp = std::fopen(tmp.c_str(), "wb");
        if (!fp) return false;
        const bool ok = std::fwrite(t.data(), 1, t.size(), fp) == t.size();
        if (std::fclose(fp) != 0 || !ok) { std::remove(tmp.c_str()); return false; }
        return std::rename(tmp.c_str(), path_.c_str()) == 0;
    }
    /** Under the caller's own discipline: the ISM service's housekeeping is the only writer. */
    FoundBook& book(const std::string& band) { std::lock_guard<std::mutex> lk(m_); return books_[band]; }
    const std::string& path() const { return path_; }
private:
    std::mutex m_;
    std::string path_;
    std::map<std::string, FoundBook> books_;
};

} // namespace vibelora
