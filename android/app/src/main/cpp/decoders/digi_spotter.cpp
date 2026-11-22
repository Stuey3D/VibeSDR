// decoders/digi_spotter.cpp — see digi_spotter.h.
#include "digi_spotter.h"
#include "digi_bands.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <algorithm>

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
    if (std::isfinite(dialHz))
        for (const ModeDef& m : ms)
            if (singleCapable(m) && atOwnDial(m, dialHz)) out.push_back(m.name);
    if (out.empty())          // ★ off the table: the modes marked singleFallback (FT8 + FT4 — what Single always ran)
        for (const ModeDef& m : ms)
            if (singleCapable(m) && m.singleFallback) out.push_back(m.name);
    return out;
}

void Spotter::planSingle(double dialHz) {
    singleDial_ = dialHz;
    const std::vector<std::string> want = singleModesAt(modes_, dialHz);
    auto wanted = [&](const std::string& n) { for (const auto& w : want) if (w == n) return true; return false; };
    std::lock_guard<std::mutex> lk(statMtx_);
    for (auto it = single_.begin(); it != single_.end();) {
        // ★ kept while still wanted — unless its set-up depends on the dial and this one needs another (Q65's 15 s period)
        const bool sameSetup = !it->def->variantAt || !std::isfinite(dialHz) || it->def->variantAt(it->dial) == it->def->variantAt(dialHz);
        if (wanted(it->def->name) && sameSetup) { ++it; continue; }
        retiredSkips_ += it->eng->slotsSkipped(); retiredDecodes_ += it->eng->slotsDecoded();
        it = single_.erase(it);
    }
    for (const ModeDef& m : modes_) {
        if (!wanted(m.name)) continue;
        bool have = false;
        for (const Run& r : single_) if (r.def == &m) have = true;
        if (have) continue;
        Run r; r.def = &m; r.dial = std::isfinite(dialHz) ? dialHz : 0;
        try { r.eng = m.makeFor(r.dial); } catch (const std::exception&) { continue; }
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
        // ★ V12 beta91: an IQ mode knows the RF itself (it reads a window, not audio above a dial)
        emit_(o, std::isfinite(o.rfHz) ? o.rfHz : base + o.audioHz);
    };
    if (!r.cpu) r.cpu = std::make_shared<std::atomic<uint64_t>>(0);
    {
        auto cpu = r.cpu;
        r.eng->onCpu = [this, cpu](uint64_t ns) { cpu->fetch_add(ns, std::memory_order_relaxed); if (cpuHook_) cpuHook_(ns); };
    }
    if (!r.light) r.light = std::make_shared<std::atomic<bool>>(false);
    if (single) { r.eng->loaded = loaded_; return; }
    // ★ Band: the governor's depth OR the server's load — either says "one pass".
    auto light = r.light; auto ld = loaded_;
    r.eng->loaded = [light, ld] { return light->load(std::memory_order_relaxed) || (ld && ld()); };
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
    for (Run& r : single_) r.eng->process(buf_.data(), (int)buf_.size(), captureUtc);   // ★ planSingle chose them
}

/* ★★ IS THE LISTENER AT THIS MODE'S DIAL? One rule for every mode, two ways of reading "at":
 *   • singleDialTolHz > 0 (a narrow mode on a dial of its own — WSPR, ±150 Hz): the dial within that of one of its dials;
 *   • otherwise: at least half the listener's USB window (dial + kSingleLoHz … kSingleHiHz) lies inside the mode's own
 *     audio window above one of its dials (dial + audioLoHz … audioHiHz) — 14.074 FT8, 14.078 JS8, 14.080 FT4,
 *     14.084 FT2, 3.578 FT2 and JS8. */
bool Spotter::atOwnDial(const ModeDef& m, double dialHz) {
    if (!std::isfinite(dialHz)) return false;
    const double lo = dialHz + kSingleLoHz, hi = dialHz + kSingleHiHz, need = 0.5 * (kSingleHiHz - kSingleLoHz);
    for (const Band& b : bands())
        for (double d : dialsOf(m, b.id)) {
            if (m.singleDialTolHz > 0) { if (std::fabs(d - dialHz) <= m.singleDialTolHz) return true; continue; }
            if (std::min(hi, d + m.audioHiHz) - std::max(lo, d + m.audioLoHz) >= need) return true;
        }
    return false;
}

void Spotter::plan() {
    planRate_ = 0; planCentre_ = 0;
    for (auto& kv : slices_) if (kv.second.rate > planRate_) { planRate_ = kv.second.rate; planCentre_ = kv.second.centre; }
    /* What should run: every (mode, dial) of a band whose passband fits one of the slices' flat parts — each DIAL on ONE
     *  slice (its tuner's): the one holding most of its modes, then the one it sits deepest in. */
    struct Want { long long slice = 0; std::vector<std::pair<const ModeDef*, double>> modes; };
    std::map<long long, std::vector<std::pair<const ModeDef*, double>>> cand;
    for (const ModeDef& m : modes_) {
        if (m.sampleRate != 12000) continue;     // ★ the tuners make 12 kHz; another rate needs its own tuner group
        for (const Band& b : bands())
            for (double d : dialsOf(m, b.id))
                for (auto& sk : slices_)
                    if (DigiTuner::fits(d, m.audioLoHz, m.audioHiHz, sk.second.rate, sk.second.centre)) { cand[std::llround(d)].push_back({ &m, d }); break; }
    }
    std::map<long long, Want> want;
    for (auto& kv : cand) {
        long long best = 0; int bestN = -1; double bestM = -1e300;
        for (auto& sk : slices_) {
            int n = 0; double mm = 1e300;
            for (auto& md : kv.second) {
                const double g = DigiTuner::margin(md.second, md.first->audioLoHz, md.first->audioHiHz, sk.second.rate, sk.second.centre);
                if (g >= 0) { ++n; mm = std::min(mm, g); }
            }
            if (n > bestN || (n == bestN && mm > bestM)) { best = sk.first; bestN = n; bestM = mm; }
        }
        /* ★★ V12 beta91: a dial the MAIN slice (the host's id 0) holds whole stays on it — the CW / RTTY / wide-PSK slices
         *  overlap the main one on several bands (20 m's 13.992–14.107 covers its whole cluster) and would otherwise win on
         *  margin, moving FT8 … PSK onto a slice that exists only for the skimmers: a fresh tuner mid-slot, and the
         *  audio modes' fate tied to the skimmers' slice (switched off → not cut; beyond a narrow capture). So every audio
         *  dial sits where it did before the band-skim change (test-web-spots once lost 20 m's PSK31 after the merge). */
        for (auto& sk : slices_) {
            if (sk.second.id != 0) continue;
            int n = 0;
            for (auto& md : kv.second) if (DigiTuner::margin(md.second, md.first->audioLoHz, md.first->audioHiHz, sk.second.rate, sk.second.centre) >= 0) ++n;
            if (n == (int)kv.second.size() && n > 0) { best = sk.first; bestN = n; }
        }
        if (bestN <= 0) continue;
        Want& w = want[kv.first];
        w.slice = best;
        const Slice& S = slices_[best];
        for (auto& md : kv.second)
            if (DigiTuner::fits(md.second, md.first->audioLoHz, md.first->audioHiHz, S.rate, S.centre)) w.modes.push_back(md);
    }
    /* ★★ V12 beta91 — THE IQ MODES (CW, RTTY): each WINDOW on the slice whose flat part holds it with the most room. Keyed
     *  apart from the dials (negative: the window's centre and the mode's index), one run per window, no tuner. */
    struct IqWant { long long slice = 0; const ModeDef* m = nullptr; Window w; };
    std::map<long long, IqWant> iqWant;
    for (size_t mi = 0; mi < modes_.size(); ++mi) {
        const ModeDef& m = modes_[mi];
        if (!m.iq || !m.makeWin) continue;
        for (const Band& b : bands())
            for (const Window& w : windowsOf(m, b.id)) {
                long long best = 0; double bestM = -1;
                for (auto& sk : slices_) {
                    const double g = DigiTuner::margin(w.loHz, 0.0f, (float)(w.hiHz - w.loHz), sk.second.rate, sk.second.centre);
                    if (g >= 0 && g > bestM) { bestM = g; best = sk.first; }
                }
                if (bestM < 0) continue;
                IqWant& iw = iqWant[-(std::llround(w.dial) * 32 + (long long)mi)];
                iw.slice = best; iw.m = &m; iw.w = w;
            }
    }
    std::lock_guard<std::mutex> lk(statMtx_);
    // Drop channels whose dial left every slice (their engines' counts are kept).
    for (auto it = chans_.begin(); it != chans_.end();) {
        if (want.count(it->first) || iqWant.count(it->first)) { ++it; continue; }
        for (Run& r : it->second.runs) {
            retiredSkips_ += r.eng->slotsSkipped(); retiredDecodes_ += r.eng->slotsDecoded();
            if (r.gate) { retiredIdle_ += r.gate->idleN; retiredShed_ += r.gate->shedN; }
        }
        it = chans_.erase(it);
    }
    // Make what is new.
    for (auto& kv : want) {
        Chan& c = chans_[kv.first];
        float lo = 1e9f, hi = -1e9f;
        for (auto& md : kv.second.modes) { lo = std::min(lo, md.first->audioLoHz); hi = std::max(hi, md.first->audioHiHz); }
        if (!c.tuner || c.tuner->loHz() != lo || c.tuner->hiHz() != hi || c.slice != kv.second.slice)
            c.tuner.reset(new DigiTuner((double)kv.first, lo, hi, 12000));   // ★ a dial moved to another slice: a fresh tuner
        c.slice = kv.second.slice;
        for (auto& md : kv.second.modes) {
            bool have = false;
            for (Run& r : c.runs) if (r.def == md.first) have = true;
            if (have) continue;
            Run r; r.def = md.first; r.dial = md.second;
            try { r.eng = md.first->makeFor(md.second); } catch (const std::exception&) { continue; }
            if (!r.eng) continue;
            wire(r);
            r.gate.reset(new Gate());
            r.gate->loHz = md.second + md.first->audioLoHz; r.gate->hiHz = md.second + md.first->audioHiHz;
            c.runs.push_back(std::move(r));
        }
    }
    for (auto& kv : iqWant) {
        Chan& c = chans_[kv.first];
        c.slice = kv.second.slice;            // ★ a window that moved to another slice keeps its engine (it re-reads the offsets)
        if (!c.runs.empty()) continue;
        Run r; r.def = kv.second.m; r.dial = kv.second.w.dial;
        try { r.eng = kv.second.m->makeWin(kv.second.w.loHz, kv.second.w.hiHz); } catch (const std::exception&) { continue; }
        if (!r.eng) continue;
        // ★ costVsFt8 is per 48 kHz of window, and never under half of it (a skimmer's FFT and floor cost something on any
        //   width: the chip bench's 10 kHz windows cost half a 48's — BAND-SKIM-CW-RTTY.md)
        const double widthK = std::max(0.5, (kv.second.w.hiHz - kv.second.w.loHz) / 48000.0);
        r.cost = kv.second.m->costVsFt8 * widthK; r.lightC = kv.second.m->lightCost() * widthK;
        wire(r);
        r.gate.reset(new Gate());
        r.gate->loHz = kv.second.w.loHz; r.gate->hiHz = kv.second.w.hiHz;
        c.runs.push_back(std::move(r));
    }
    for (auto& sk : slices_) sk.second.gNb = 0;   // ★ the windows are placed again on each slice's next block with bins
    statusSeq_.fetch_add(1, std::memory_order_relaxed);
}

void Spotter::feedIq(const cf32* x, int n, double rate, double centreHz, double captureUtc,
                     const float* binPow, int nb, double binHz, int sliceId) {
    if (scope_ != Scope::Band || n <= 0 || !(rate > 0)) return;
    if (!x) { feedGap(n, rate, centreHz, captureUtc); return; }   // ★ a null block: a gap of n samples (see feedGap)
    const long long key = std::llround(centreHz);
    bool replan = false;
    auto it = slices_.find(key);
    if (it == slices_.end()) {
        it = slices_.emplace(key, Slice()).first;
        it->second.rate = rate; it->second.centre = centreHz; replan = true;
    } else if (std::fabs(it->second.rate - rate) > 0.5) {
        it->second.rate = rate; replan = true;
    }
    Slice& S = it->second;
    S.lastUtc = captureUtc;
    S.id = sliceId;
    for (auto j = slices_.begin(); j != slices_.end();) {
        /* ★ A slice no longer fed has gone; and a NEW slice at the same rate within a quarter of it of an old one is that
         *  slice MOVED (a fine retune) — the old one goes at once, so its dials keep their engines and lose no audio.
         *  ★ V12 beta91: with ids, the same id is the same slice moved, and another id never is. */
        const bool byId = sliceId >= 0 && j->second.id >= 0;
        const bool moved = replan && j != it && (byId ? j->second.id == sliceId
                                                      : std::fabs(j->second.rate - rate) < 0.5 && std::fabs(j->second.centre - centreHz) < 0.25 * rate);
        if (j != it && (moved || captureUtc - j->second.lastUtc > kSliceGoneSec)) { j = slices_.erase(j); replan = true; }
        else ++j;
    }
    if (replan) plan();
    // ── the activity gate's measure: the slice's own bins (no transform here) ──
    /* ★ 0 = no bins (no gate: every window counts as occupied) · 1 = bins, the floor still warming up (hold, open only
     *  what must run) · 2 = gated. */
    int gated = 0;
    if (binPow && nb > 8 && binHz > 0) {
        if (nb != S.gNb || std::fabs(binHz - S.gBinHz) > 1e-6 || std::fabs(centreHz - S.gCentre) > 0.5) placeWindows(key, S, nb, binHz, centreHz);
        S.floor.block(binPow, nb, std::max(1, (int)std::lround(0.5 * rate / n)));   // ★ the median every half second
        for (auto& kv : chans_) if (kv.second.slice == key) for (Run& r : kv.second.runs) if (r.gate) r.gate->meter.block(binPow, nb);
        /* ★ V12 beta91: when this slice's floor first becomes ready, a LONG period's window (over 15 s — WSPR's and FST4W's
         *  120 s) forgets what it measured while the floor warmed up. A slice is (re)made when Band starts and the tuner
         *  moves its DC off the slices — exactly when the capture carries the retune's transient; kept in a 120 s mean it
         *  opened an empty window for the whole period (test-web-spots "WSPR idle", test-server-spots "WSPR's empty window
         *  was never decoded" — intermittent, before the band-skim change too). The slotted modes' short periods are left
         *  as they were (they judge afresh every slot). */
        if (S.floor.ready() && !S.wasReady) {
            S.wasReady = true;
            for (auto& kv : chans_) if (kv.second.slice == key) for (Run& r : kv.second.runs)
                if (r.gate && gatePeriod(*r.def) > 15.0) r.gate->meter.restart();
        }
        gated = S.floor.ready() ? 2 : 1;
        ++S.blocks;
    }
    {
        std::lock_guard<std::mutex> lk(statMtx_);
        loadedNow_ = loaded_ ? loaded_() : false;
        budget_.tick(captureUtc, skippedLocked(), unitsInUse());
        pauseTick(captureUtc);
    }
    S.endUtc = captureUtc; S.lastGated = gated;
    feedChans(key, S, x, n, rate, centreHz, captureUtc, gated);
}

void Spotter::feedGap(int n, double rate, double centreHz, double captureUtc) {
    if (scope_ != Scope::Band || n <= 0 || !(rate > 0)) return;
    static const bool kNoBridge = [] { const char* e = std::getenv("VIBE_BAND_BRIDGE"); return e && *e == '0'; }();   // ★ A/B
    if (kNoBridge) return;
    const long long key = std::llround(centreHz);
    auto it = slices_.find(key);
    if (it == slices_.end() || std::fabs(it->second.rate - rate) > 0.5) return;   // ★ a gap in a slice not yet planned: nothing to keep whole
    Slice& S = it->second;
    if ((int)zeros_.size() < 4096) zeros_.assign(4096, cf32(0, 0));
    for (int done = 0; done < n;) {
        const int k = std::min(4096, n - done);
        done += k;
        const double t = captureUtc - (double)(n - done) / rate;   // ★ the stamp of this piece's last sample
        S.lastUtc = t; S.endUtc = t;
        feedChans(key, S, zeros_.data(), k, rate, centreHz, t, S.lastGated, true);
    }
    S.bridged += (uint64_t)n;
    bridgedTotal_.fetch_add((uint64_t)std::llround(1e6 * n / rate), std::memory_order_relaxed);
}

void Spotter::feedChans(long long key, Slice& S, const cf32* x, int n, double rate, double centreHz, double captureUtc, int gated, bool gap) {
    for (auto& kv : chans_) {
        Chan& c = kv.second;
        if (c.slice != key) continue;
        if (!c.tuner) {   // ★ V12 beta91: an IQ mode's window — the slice itself (a bridged gap is not decoded: zeros spot nothing)
            if (!gap) for (Run& r : c.runs) routeIq(r, x, n, rate, centreHz, captureUtc, gated, S);
            continue;
        }
        /* ★ A dial whose every engine is OFF or SHED needs no audio at all: its tuner is skipped too (measured on the Pi 2
         *  build, the tuners are ≈ a fifth of Band's work — docs/v12/DIGITAL-SPOTS.md). The period clock still turns. */
        bool anyWants = false;
        for (Run& r : c.runs) if (!r.gate || (r.gate->st != Gate::Off && r.gate->st != Gate::Shed)) anyWants = true;
        if (!anyWants) {
            for (Run& r : c.runs) {
                const double P = gatePeriod(*r.def);
                const int64_t per = (int64_t)std::floor((captureUtc - n / rate) / P + 1e-9);
                if (per != r.gate->per) newPeriod(r, per, gated);
            }
            continue;
        }
        audio_.clear();
        c.tuner->feed(x, n, rate, centreHz, audio_);
        if (audio_.empty()) continue;
        for (Run& r : c.runs) route(r, audio_.data(), (int)audio_.size(), captureUtc, gated, S);
    }
}

/* ── THE GATE (see the header) ──────────────────────────────────────────────────────────────────────────────────────── */
namespace {
constexpr float kHoldScale = 4096.0f;   // ★ held audio as int16 ×4096: ±8 full scale (the tuner levels to RMS 0.05 —
                                        //   44 dB of headroom), a step 46 dB under that RMS. Half the RAM of float.
double marginOf(const ModeDef& m) { return m.slotSec > 0 ? 1.0 : 2.0; }   // ★ lead-in kept from before a period
int hangOf(const ModeDef& m) { return m.slotSec > 0 ? 1 : 3; }            // ★ periods a busy window starts live
}

double Spotter::gatePeriod(const ModeDef& m) {
    if (m.gateSec > 0) return m.gateSec;
    return m.slotSec > 0 ? m.slotSec : 4.0;
}

void Spotter::placeWindows(long long key, Slice& sl, int nb, double binHz, double centreHz) {
    sl.gNb = nb; sl.gBinHz = binHz; sl.gCentre = centreHz;
    for (auto& kv : chans_)
        for (Run& r : kv.second.runs) {
            if (kv.second.slice != key) continue;
            if (!r.gate) continue;
            // ★ every bin whose centre lies within half a bin of the window
            const double klo = (r.gate->loHz - centreHz) / binHz + nb / 2, khi = (r.gate->hiHz - centreHz) / binHz + nb / 2;
            const int k0 = std::max(0, (int)std::ceil(klo - 0.5)), k1 = std::min(nb, (int)std::floor(khi + 0.5) + 1);
            r.gate->meter.place(k0, k1);
        }
}

void Spotter::setState(Gate& g, RunState s) {
    if (g.rs != s) { g.rs = s; statusSeq_.fetch_add(1, std::memory_order_relaxed); }
}

double Spotter::unitsInUse() const {
    double u = 0;
    for (auto& kv : chans_)
        for (const Run& r : kv.second.runs)
            if (r.gate && r.gate->st == Gate::Live) u += r.gate->rs == RunState::Full ? r.runCost() : r.runLight();
    return u;
}

uint64_t Spotter::skippedLocked() const {
    uint64_t n = retiredSkips_;
    for (auto& kv : chans_) for (const Run& r : kv.second.runs) n += r.eng->slotsSkipped();
    return n;
}

/* ★★ ASK THE GOVERNOR (statMtx_ held): may `who` decode now? Every Band run is in the question — what is live is
 *  committed, `who` wants or not — and every LIVE run's depth is set from the answer on the way out. */
RunState Spotter::ask(Run& who, bool wants) {
    std::vector<GovIn> in; std::vector<Run*> rs;
    for (auto& kv : chans_)
        for (Run& r : kv.second.runs) {
            if (!r.gate) continue;
            GovIn g; g.priority = r.def->priority; g.cost = r.runCost(); g.light = r.runLight();
            g.ov = override_ ? override_(r.def->name) : Override::Auto;
            g.committed = r.gate->st == Gate::Live;
            g.wants = &r == &who ? wants : (g.committed || (g.ov == Override::Always && r.gate->st != Gate::Paused));
            in.push_back(g); rs.push_back(&r);
        }
    const std::vector<RunState> out = govern(in, budget_.units(), loadedNow_);
    RunState mine = RunState::Idle;
    for (size_t i = 0; i < rs.size(); ++i) {
        Run& r = *rs[i];
        if (&r == &who) { mine = out[i]; continue; }
        if (r.gate->st != Gate::Live) continue;
        if (out[i] == RunState::Light || out[i] == RunState::Full) {
            r.light->store(out[i] == RunState::Light, std::memory_order_relaxed);
            setState(*r.gate, out[i]);
        } else if (out[i] == RunState::Shed) {
            // ★ a higher-priority mode needs the room: a free-running mode stops now (nothing is lost but its next
            //   seconds); a slotted one finishes the slot whose audio it already has, light, and is shed at its next.
            if (r.def->slotSec <= 0) { r.gate->st = Gate::Shed; ++r.gate->shedN; setState(*r.gate, RunState::Shed); stopFeeding(r); }
            else r.light->store(true, std::memory_order_relaxed);
        }
    }
    return mine;
}

/** Hold a block (int16), keeping no more than `keepSec` of the newest audio. */
void Spotter::hold(Gate& g, const float* a, int k, double utc, double keepSec) {
    const size_t cap = (size_t)(keepSec * 12000.0);
    if (g.buf.size() + (size_t)k > cap + 12000) {      // ★ trimmed a second at a time, not every block
        const size_t want = cap > (size_t)k ? cap - (size_t)k : 0;
        const size_t cut = g.buf.size() > want ? g.buf.size() - want : 0;
        g.buf.erase(g.buf.begin(), g.buf.begin() + (std::ptrdiff_t)cut);
        size_t j = 0;
        for (auto& m : g.marks) if (m.first > cut) g.marks[j++] = { m.first - cut, m.second };
        g.marks.resize(j);
    }
    for (int i = 0; i < k; ++i) {
        const float v = std::max(-32767.0f, std::min(32767.0f, a[i] * kHoldScale));
        g.buf.push_back((int16_t)std::lrint(v));
    }
    g.marks.push_back({ g.buf.size(), utc });
}

/** ★ Into the engine with everything held that it has not had (each block with its own capture time), then live. */
void Spotter::goLive(Run& r) {
    Gate& g = *r.gate;
    size_t from = 0;
    for (auto& m : g.marks) {
        const size_t to = m.first;
        if (m.second > g.fedUtc + 1e-6 && to > from) {
            scratch_.resize(to - from);
            for (size_t i = from; i < to; ++i) scratch_[i - from] = g.buf[i] * (1.0f / kHoldScale);
            r.eng->process(scratch_.data(), (int)(to - from), m.second);
            g.fedUtc = m.second;
        }
        from = to;
    }
    g.buf.clear(); g.marks.clear();
    g.st = Gate::Live;
}

void Spotter::newPeriod(Run& r, int64_t per, int gated) {
    Gate& g = *r.gate;
    std::lock_guard<std::mutex> lk(statMtx_);
    if (g.per != INT64_MIN) {
        if (g.st == Gate::Pending) ++g.idleN;              // ★ nothing all period: its decode never happened
        else if (g.st == Gate::Live) ++g.liveN;
        g.hang = g.saw ? hangOf(*r.def) : std::max(0, g.hang - 1);
    }
    g.per = per; g.saw = false;
    g.meter.restart();
    // ★ Held audio from before this period's lead-in belongs to a period already judged: never flushed into this one.
    {
        const double from = per * gatePeriod(*r.def) - marginOf(*r.def);
        size_t cut = 0;
        for (auto& m : g.marks) { if (m.second > from) break; cut = m.first; }
        if (cut > 0) {
            g.buf.erase(g.buf.begin(), g.buf.begin() + (std::ptrdiff_t)cut);
            size_t j = 0;
            for (auto& m : g.marks) if (m.first > cut) g.marks[j++] = { m.first - cut, m.second };
            g.marks.resize(j);
        }
    }
    const Override ov = override_ ? override_(r.def->name) : Override::Auto;
    if (ov == Override::Off) {
        if (g.st == Gate::Live) stopFeeding(r);
        g.st = Gate::Off; g.buf.clear(); g.marks.clear(); setState(g, RunState::Off);
        return;
    }
    /* ★★ V12 beta91: a continuous mode PAUSED because the band decode is behind (pauseTick) stays paused, whatever the
     *  owner's Always — the slotted modes must not lose a slot to it, and a listener's audio comes before all of it. */
    if (pausedNow(r)) {
        if (g.st == Gate::Live) stopFeeding(r);
        g.st = Gate::Paused; g.buf.clear(); g.marks.clear(); setState(g, RunState::Paused);
        return;
    }
    const bool wants = gated == 0 || ov == Override::Always || g.hang > 0;
    const bool wasLive = g.st == Gate::Live;
    g.st = Gate::Pending;                                   // ★ not committed while the governor is asked
    const RunState d = ask(r, wants);
    if (d == RunState::Light || d == RunState::Full) {
        r.light->store(d == RunState::Light, std::memory_order_relaxed);
        if (wasLive) { g.st = Gate::Live; g.buf.clear(); g.marks.clear(); }
        else goLive(r);
        setState(g, d);
    } else if (d == RunState::Shed) {
        if (wasLive) stopFeeding(r);
        g.st = Gate::Shed; ++g.shedN; setState(g, RunState::Shed);
    } else {
        if (wasLive) stopFeeding(r);
        setState(g, RunState::Idle);                        // ★ Pending: held until the window lights up
    }
}

void Spotter::route(Run& r, const float* a, int k, double utc, int gated, Slice& sl) {
    if (!r.gate) { r.eng->process(a, k, utc); return; }
    Gate& g = *r.gate;
    const double P = gatePeriod(*r.def);
    const int64_t per = (int64_t)std::floor((utc - k / 12000.0) / P + 1e-9);
    if (per != g.per) newPeriod(r, per, gated);
    const double keep = P + marginOf(*r.def) + 1.0;
    switch (g.st) {
        case Gate::Live:
            r.eng->process(a, k, utc); g.fedUtc = utc;
            // ★ looked at every 8th block: fewer looks, fewer chances for noise to open a window (digi_activity.h)
            if (!g.saw && gated == 2 && (sl.blocks & 7) == 0 && g.meter.occupied(sl.floor.floor())) g.saw = true;
            if (gated == 0) g.saw = true;
            return;
        case Gate::Off:
        case Gate::Paused:
            return;
        case Gate::Shed:
            hold(g, a, k, utc, marginOf(*r.def));           // ★ only the lead-in for the next period
            return;
        case Gate::Pending: {
            hold(g, a, k, utc, keep);
            const bool lit = gated == 0 || (gated == 2 && (sl.blocks & 7) == 0 && g.meter.occupied(sl.floor.floor()));
            if (!lit) return;
            g.saw = true;
            std::lock_guard<std::mutex> lk(statMtx_);
            const RunState d = ask(r, true);
            if (d == RunState::Light || d == RunState::Full) {
                r.light->store(d == RunState::Light, std::memory_order_relaxed);
                goLive(r); setState(g, d);
            } else if (d == RunState::Shed) {
                g.st = Gate::Shed; ++g.shedN; setState(g, RunState::Shed);
                g.buf.clear(); g.marks.clear();
            }
            return;
        }
    }
}

/* ── V12 beta91: AN IQ MODE'S WINDOW — the same gate and governor, but nothing is HELD: the engine copies the slice into
 *  its own queue only while it is live (a CW or RTTY skimmer that misses the first seconds of a station loses nothing —
 *  a spot needs its call heard twice). ─────────────────────────────────────────────────────────────────────────────── */
void Spotter::routeIq(Run& r, const cf32* x, int n, double rate, double centreHz, double utc, int gated, Slice& sl) {
    Gate& g = *r.gate;
    const double P = gatePeriod(*r.def);
    const int64_t per = (int64_t)std::floor((utc - n / rate) / P + 1e-9);
    if (per != g.per) newPeriod(r, per, gated);
    switch (g.st) {
        case Gate::Live:
            r.eng->processIq(x, n, rate, centreHz, utc); g.fedUtc = utc;
            if (!g.saw && gated == 2 && (sl.blocks & 7) == 0 && g.meter.occupied(sl.floor.floor())) g.saw = true;
            if (gated == 0) g.saw = true;
            return;
        case Gate::Pending: {
            const bool lit = gated == 0 || (gated == 2 && (sl.blocks & 7) == 0 && g.meter.occupied(sl.floor.floor()));
            if (!lit) return;
            g.saw = true;
            std::lock_guard<std::mutex> lk(statMtx_);
            const RunState d = ask(r, true);
            if (d == RunState::Light || d == RunState::Full) {
                r.light->store(d == RunState::Light, std::memory_order_relaxed);
                g.st = Gate::Live; setState(g, d);
                r.eng->processIq(x, n, rate, centreHz, utc); g.fedUtc = utc;
            } else if (d == RunState::Shed) {
                g.st = Gate::Shed; ++g.shedN; setState(g, RunState::Shed);
            }
            return;
        }
        default:
            return;
    }
}

/* ★★ V12 beta91 — BAND'S PAUSE RULE (statMtx_ held, once a second of capture). Stuart: "CW and RTTY are continuous, unlike
 *  the slotted FT8 family, so if the band decode falls behind the capture … the CW then RTTY chips are paused FIRST so the
 *  FT8/FT4/WSPR chips never lose slots, and they resume when it catches up".
 *   • BEHIND = the most of: the host's queue of slice IQ not yet decoded (setBehind), and every live IQ engine's own
 *     backlog; or a slotted engine SKIPPED a slot since the last look.
 *   • Behind by more than kPauseBehindSec (or a skip): the next mode in pause order (ModeDef::pauseRank — CW 1, RTTY 2)
 *     pauses, at most one every kPauseStepSec, and its queue is dropped at once.
 *   • Caught up (under kResumeBehindSec, no skip) for kResumeCalmSec: the last one paused resumes — RTTY, then CW.
 *   The log says so plainly each time. */
void Spotter::pauseTick(double utc) {
    if (!std::isfinite(utc) || utc - pauseCheck_ < 1.0) return;
    pauseCheck_ = utc;
    int maxRank = 0;
    double backlog = 0;
    for (auto& kv : chans_)
        for (const Run& r : kv.second.runs) {
            maxRank = std::max(maxRank, r.def->pauseRank);
            if (r.def->iq && r.gate && r.gate->st == Gate::Live) backlog = std::max(backlog, r.eng->backlogSec());
        }
    if (maxRank == 0) { pauseLevel_ = 0; return; }
    const double ext = behind_ ? behind_() : 0.0;
    const double b = std::max(ext, backlog);
    const uint64_t sk = skippedLocked();
    const bool skipped = sk > pauseSkips_;
    pauseSkips_ = sk;
    auto names = [&](int rank) {
        std::string o;
        for (const ModeDef& m : modes_) if (m.pauseRank == rank) o += (o.empty() ? "" : "/") + m.name;
        return o;
    };
    if ((b > kPauseBehindSec || skipped) && pauseLevel_ < maxRank) {
        calmSince_ = NAN;
        if (utc - pauseAt_ < kPauseStepSec) return;
        ++pauseLevel_; pauseAt_ = utc;
        for (auto& kv : chans_)
            for (Run& r : kv.second.runs)
                if (r.gate && pausedNow(r) && r.gate->st != Gate::Paused) {
                    if (r.gate->st == Gate::Live) stopFeeding(r);
                    r.gate->st = Gate::Paused; r.gate->buf.clear(); r.gate->marks.clear();
                    setState(*r.gate, RunState::Paused);
                }
        if (log_) {
            char why[160];
            if (skipped && !(b > kPauseBehindSec)) std::snprintf(why, sizeof why, "a slotted mode skipped a slot");
            else std::snprintf(why, sizeof why, "the decode is %.1f s behind the capture", b);
            log_("Band: " + names(pauseLevel_) + " paused \xe2\x80\x94 the CPU is behind (" + why + "); the slotted modes keep their slots");
        }
        return;
    }
    if (b > kPauseBehindSec || skipped) { calmSince_ = NAN; return; }
    if (pauseLevel_ == 0) return;
    if (b >= kResumeBehindSec) { calmSince_ = NAN; return; }
    if (!std::isfinite(calmSince_)) { calmSince_ = utc; return; }
    if (utc - calmSince_ < kResumeCalmSec || utc - pauseAt_ < kResumeCalmSec) return;
    const std::string back = names(pauseLevel_);
    --pauseLevel_; pauseAt_ = utc; calmSince_ = utc;
    for (auto& kv : chans_)
        for (Run& r : kv.second.runs)
            if (r.gate && r.gate->st == Gate::Paused && !pausedNow(r)) {
                r.gate->st = Gate::Pending; r.gate->per = INT64_MIN;   // ★ judged afresh at its next block
                setState(*r.gate, RunState::Idle);
            }
    if (log_) log_("Band: " + back + " resumed \xe2\x80\x94 the decode has caught up");
}

uint64_t Spotter::periodsShed() const {
    std::lock_guard<std::mutex> lk(statMtx_);
    uint64_t n = retiredShed_;
    for (auto& kv : chans_) for (const Run& r : kv.second.runs) if (r.gate) n += r.gate->shedN;
    return n;
}
uint64_t Spotter::periodsIdle() const {
    std::lock_guard<std::mutex> lk(statMtx_);
    uint64_t n = retiredIdle_;
    for (auto& kv : chans_) for (const Run& r : kv.second.runs) if (r.gate) n += r.gate->idleN;
    return n;
}
std::vector<Spotter::ModeStatus> Spotter::status() const {
    std::lock_guard<std::mutex> lk(statMtx_);
    std::vector<ModeStatus> out;
    auto rank = [](RunState s) { switch (s) { case RunState::Full: return 4; case RunState::Light: return 3; case RunState::Shed: return 2;
                                              case RunState::Paused: return 2; case RunState::Idle: return 1; default: return 0; } };
    for (const ModeDef& m : modes_) {
        ModeStatus st; st.mode = m.name; bool any = false;
        for (auto& kv : chans_)
            for (const Run& r : kv.second.runs) {
                if (r.def != &m || !r.gate) continue;
                if (!any || rank(r.gate->rs) > rank(st.state)) st.state = r.gate->rs;
                any = true; ++st.dials;
                st.idle += r.gate->idleN; st.shed += r.gate->shedN; st.live += r.gate->liveN;
                if (r.cpu) st.cpuNs += r.cpu->load(std::memory_order_relaxed);
                st.iq = m.iq;
            }
        if (any) out.push_back(st);
    }
    return out;
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
            // ★ Band: each run's state too ("FT8 14.0740 active, WSPR 14.0956 idle") — the admin page's line
            std::snprintf(b, sizeof b, "%s%s %.4f%s%s", o.empty() ? "" : ", ", r.def->name.c_str(), r.dial / 1e6,
                          r.gate ? " " : "", r.gate ? stateWord(r.gate->rs) : "");
            o += b;
        }
    return o;
}

} // namespace digi
} // namespace vibe
