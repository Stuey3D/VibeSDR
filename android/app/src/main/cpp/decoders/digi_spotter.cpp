// decoders/digi_spotter.cpp — see digi_spotter.h.
#include "digi_spotter.h"
#include "digi_bands.h"

#include <cmath>
#include <cstdio>
#include <exception>

namespace vibe {
namespace digi {

Spotter::Spotter(Scope scope, const std::vector<ModeDef>& ms, Emit emit, std::function<double()> dialHz,
                 std::function<bool()> loaded)
    : scope_(scope), modes_(ms), emit_(std::move(emit)), dialHz_(std::move(dialHz)), loaded_(std::move(loaded)) {
    if (scope_ == Scope::Single) planSingle(dialHz_ ? dialHz_() : NAN);
}

namespace {
// ★ Single mode decimates 48 kHz by four; a mode at another rate is Band-only until it says otherwise.
bool singleCapable(const ModeDef& m) { return m.single && m.sampleRate == 12000; }
}

std::vector<std::string> Spotter::singleModesAt(const std::vector<ModeDef>& ms, double dialHz) {
    std::vector<std::string> out;
    if (std::isfinite(dialHz)) {
        const double lo = dialHz + kSingleLoHz, hi = dialHz + kSingleHiHz, need = 0.5 * (kSingleHiHz - kSingleLoHz);
        for (const ModeDef& m : ms) {
            if (!singleCapable(m)) continue;
            bool at = false;
            for (const Band& b : bands()) {
                for (double d : dialsOf(m, b.id)) {
                    const double ov = std::min(hi, d + m.audioHiHz) - std::max(lo, d + m.audioLoHz);
                    if (ov >= need) { at = true; break; }
                }
                if (at) break;
            }
            if (at) out.push_back(m.name);
        }
    }
    if (out.empty())          // ★ off the table: what Single always ran
        for (const ModeDef& m : ms)
            if (singleCapable(m) && (m.name == "FT8" || m.name == "FT4")) out.push_back(m.name);
    return out;
}

void Spotter::planSingle(double dialHz) {
    singleDial_ = dialHz;
    const std::vector<std::string> want = singleModesAt(modes_, dialHz);
    auto wanted = [&](const std::string& n) { for (const auto& w : want) if (w == n) return true; return false; };
    std::lock_guard<std::mutex> lk(statMtx_);
    for (auto it = single_.begin(); it != single_.end();) {
        if (wanted(it->def->name)) { ++it; continue; }
        retiredSkips_ += it->eng->slotsSkipped(); retiredDecodes_ += it->eng->slotsDecoded();
        it = single_.erase(it);
    }
    for (const ModeDef& m : modes_) {
        if (!wanted(m.name)) continue;
        bool have = false;
        for (const Run& r : single_) if (r.def == &m) have = true;
        if (have) continue;
        Run r; r.def = &m; r.dial = 0;
        try { r.eng = m.make(); } catch (const std::exception&) { continue; }
        if (!r.eng) continue;
        wire(r);
        single_.push_back(std::move(r));
    }
}

Spotter::~Spotter() = default;

void Spotter::wire(Run& r) {
    ++made_;
    const std::string name = r.def->name;
    const double dial = r.dial;
    const bool single = scope_ == Scope::Single;
    r.eng->onSpot = [this, name, dial, single](const Spot& s) {
        if (!emit_) return;
        Spot o = s;
        if (o.mode.empty()) o.mode = name;
        // ★ Single: the listener's dial NOW (USB) + the audio offset, as ever. Band: this engine's own dial.
        const double base = single ? (dialHz_ ? dialHz_() : 0.0) : dial;
        emit_(o, base + o.audioHz);
    };
    r.eng->loaded = loaded_;
}

void Spotter::feedAudio48k(const float* x, int n, double captureUtc) {
    if (scope_ != Scope::Single || n <= 0) return;
    // ★ A retune re-chooses the modes (a dial unchanged to the hertz costs one compare)
    if (dialHz_) {
        const double d = dialHz_();
        if (std::isfinite(d) && !(std::fabs(d - singleDial_) < 1.0)) planSingle(d);
    }
    if (single_.empty()) return;
    buf_.clear(); buf_.reserve((size_t)n / 4 + 1);
    for (int i = 0; i < n; ++i) {
        acc_ += x[i];
        if (++decim_ >= 4) { buf_.push_back(acc_ * 0.25f); acc_ = 0; decim_ = 0; }
    }
    if (buf_.empty()) return;
    for (Run& r : single_) r.eng->process(buf_.data(), (int)buf_.size(), captureUtc);
}

void Spotter::plan(double rate, double centreHz) {
    planRate_ = rate; planCentre_ = centreHz;
    // What should run: every (mode, dial) of a band whose passband fits this slice.
    std::map<long long, std::vector<std::pair<const ModeDef*, double>>> want;
    for (const ModeDef& m : modes_) {
        if (m.sampleRate != 12000) continue;     // ★ the tuners make 12 kHz; another rate needs its own tuner group
        for (const Band& b : bands())
            for (double d : dialsOf(m, b.id))
                if (DigiTuner::fits(d, m.audioLoHz, m.audioHiHz, rate, centreHz))
                    want[std::llround(d)].push_back({ &m, d });
    }
    std::lock_guard<std::mutex> lk(statMtx_);
    // Drop channels whose dial left the slice (their engines' counts are kept).
    for (auto it = chans_.begin(); it != chans_.end();) {
        if (want.count(it->first)) { ++it; continue; }
        for (Run& r : it->second.runs) { retiredSkips_ += r.eng->slotsSkipped(); retiredDecodes_ += r.eng->slotsDecoded(); }
        it = chans_.erase(it);
    }
    // Make what is new.
    for (auto& kv : want) {
        Chan& c = chans_[kv.first];
        float lo = 1e9f, hi = -1e9f;
        for (auto& md : kv.second) { lo = std::min(lo, md.first->audioLoHz); hi = std::max(hi, md.first->audioHiHz); }
        if (!c.tuner || c.tuner->loHz() != lo || c.tuner->hiHz() != hi)
            c.tuner.reset(new DigiTuner((double)kv.first, lo, hi, 12000));
        for (auto& md : kv.second) {
            bool have = false;
            for (Run& r : c.runs) if (r.def == md.first) have = true;
            if (have) continue;
            Run r; r.def = md.first; r.dial = md.second;
            try { r.eng = md.first->make(); } catch (const std::exception&) { continue; }
            if (!r.eng) continue;
            wire(r);
            c.runs.push_back(std::move(r));
        }
    }
}

void Spotter::feedIq(const cf32* x, int n, double rate, double centreHz, double captureUtc) {
    if (scope_ != Scope::Band || n <= 0 || !(rate > 0)) return;
    if (std::fabs(rate - planRate_) > 0.5 || std::fabs(centreHz - planCentre_) > 0.5) plan(rate, centreHz);
    for (auto& kv : chans_) {
        Chan& c = kv.second;
        audio_.clear();
        c.tuner->feed(x, n, rate, centreHz, audio_);
        if (audio_.empty()) continue;
        for (Run& r : c.runs) r.eng->process(audio_.data(), (int)audio_.size(), captureUtc);
    }
}

uint64_t Spotter::slotsSkipped() const {
    std::lock_guard<std::mutex> lk(statMtx_);
    uint64_t n = retiredSkips_;
    for (const Run& r : single_) n += r.eng->slotsSkipped();
    for (auto& kv : chans_) for (const Run& r : kv.second.runs) n += r.eng->slotsSkipped();
    return n;
}
uint64_t Spotter::slotsDecoded() const {
    std::lock_guard<std::mutex> lk(statMtx_);
    uint64_t n = retiredDecodes_;
    for (const Run& r : single_) n += r.eng->slotsDecoded();
    for (auto& kv : chans_) for (const Run& r : kv.second.runs) n += r.eng->slotsDecoded();
    return n;
}
size_t Spotter::engines() const {
    std::lock_guard<std::mutex> lk(statMtx_);
    size_t n = single_.size();
    for (auto& kv : chans_) n += kv.second.runs.size();
    return n;
}
std::string Spotter::describe() const {
    std::lock_guard<std::mutex> lk(statMtx_);
    std::string o;
    for (const Run& r : single_) { if (!o.empty()) o += ", "; o += r.def->name; }
    for (auto& kv : chans_)
        for (const Run& r : kv.second.runs) {
            char b[64];
            std::snprintf(b, sizeof b, "%s%s %.4f", o.empty() ? "" : ", ", r.def->name.c_str(), r.dial / 1e6);
            o += b;
        }
    return o;
}

} // namespace digi
} // namespace vibe
