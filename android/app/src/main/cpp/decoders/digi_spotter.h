// decoders/digi_spotter.h — one listener's DIGITAL SPOTS run (V12, 2026-10-26): every registered mode (digi_mode.h) on
// either the listener's own audio (SINGLE) or a whole band's IQ slice (BAND). Owned by a DecoderHost; everything here
// runs on that host's decode thread, except the engines' own slot workers.
//
// ★★ SINGLE ("Digital spots: Single mode") is the button there was before, and the CHEAP one: the listener's 48 kHz audio,
//    box-averaged by four to 12 kHz (the same decimation as ever), into only the modes whose published dial is WHERE THE
//    LISTENER IS TUNED (Stuart's design: Band is the button that runs every mode). One rule, atOwnDial: a WSPR-like
//    narrow mode (ModeDef::singleDialTolHz) within its tolerance of a dial; any other when at least half of
//    the listener's USB window (dial + 100 … dial + 3000 Hz, kSingleLoHz/kSingleHiHz) falls inside its own audio
//    window above one of its dials (dial + audioLoHz … audioHiHz): 14.074 → FT8, 14.078 → JS8, 14.080 → FT4,
//    14.084 → FT2, 3.578 → JS8 and FT2 (one dial). Re-chosen on every retune; an engine whose mode is still wanted is
//    KEPT (its slot in progress is not lost). Tuned to no table entry, it runs the ModeDef::singleFallback modes —
//    FT8 + FT4, what Single always ran — so it never goes silent off the table.
// ★★ BAND ("Digital spots: Band"): the host is fed the band's IQ slice (centred on the listener's dial, as the CW
//    skimmer's is). For every registered mode, every dial of it that FITS in the slice gets a DigiTuner (one per distinct
//    dial — FT2 and JS8 share 3.578 MHz) and an engine. When the slice moves (the next band, a retune), the plan is redone
//    on the next block: tuners and engines for dials still inside are KEPT (a fine retune within the band does not lose a
//    slot), the rest are dropped and new ones made.
// ★ Spots go out through `emit` with their RF frequency (dial + audio offset) and the mode's name.
#pragma once
#include "digi_mode.h"
#include "digi_tuner.h"

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vibe {
namespace digi {

class Spotter {
public:
    using cf32 = std::complex<float>;
    enum class Scope { Single, Band };
    using Emit = std::function<void(const Spot&, double rfHz)>;

    /** `ms`: the modes to run (normally modes()); tests pass their own. `dialHz`: the listener's dial now (Single mode's
     *  RF = dial + audio). `loaded`: the server's load, handed to every engine. */
    Spotter(Scope scope, const std::vector<ModeDef>& ms, Emit emit, std::function<double()> dialHz,
            std::function<bool()> loaded);
    ~Spotter();
    Scope scope() const { return scope_; }

    /** Single: `n` frames of the listener's 48 kHz audio, `captureUtc` = the last frame's. */
    void feedAudio48k(const float* x, int n, double captureUtc);
    /** Band: one block of the slice — `n` samples at `rate`, centred on RF `centreHz`, the last captured at `captureUtc`. */
    void feedIq(const cf32* x, int n, double rate, double centreHz, double captureUtc);

    /** Slots skipped by every engine so far (monotonic — the health pill's BehindWatch reads it). */
    uint64_t slotsSkipped() const;
    uint64_t slotsDecoded() const;
    /** What runs now: "FT8 14.074, FT4 14.080" — the admin page. */
    std::string describe() const;
    /** How many engines run now (tests). */
    size_t engines() const;
    /** How many engines were ever made (tests: a fine retune keeps its engines). */
    uint64_t made() const { return made_; }
    /** ★ Single: the listener's window above the dial that a mode's dial window must half-cover, and the choice itself
     *  (pure, for the tests): the names of the `ms` modes Single runs with the listener's dial at `dialHz`. */
    static constexpr double kSingleLoHz = 100, kSingleHiHz = 3000;
    static std::vector<std::string> singleModesAt(const std::vector<ModeDef>& ms, double dialHz);
    /** Is a listener at `dialHz` on one of `m`'s dials (see the .cpp)? */
    static bool atOwnDial(const ModeDef& m, double dialHz);
    /** The slice's centre and rate at the last re-plan (tests). */
    double planCentre() const { return planCentre_; }

private:
    struct Run {
        const ModeDef* def = nullptr;
        double dial = 0;
        std::unique_ptr<Engine> eng;
    };
    struct Chan {                     // ★ one tuner, and every engine on its dial
        std::unique_ptr<DigiTuner> tuner;
        std::vector<Run> runs;
    };
    void plan(double rate, double centreHz);
    void planSingle(double dialHz);
    void wire(Run& r);

    Scope scope_;
    std::vector<ModeDef> modes_;
    Emit emit_;
    std::function<double()> dialHz_;
    std::function<bool()> loaded_;

    // Single
    std::vector<Run> single_;
    int   decim_ = 0; float acc_ = 0;
    std::vector<float> buf_;
    double singleDial_ = NAN;            // ★ the dial the Single set was chosen for
    uint64_t made_ = 0;

    // Band
    std::map<long long, Chan> chans_;    // ★ keyed by the dial in Hz
    double planRate_ = 0, planCentre_ = 0;
    std::vector<float> audio_;

    mutable std::mutex statMtx_;          // ★ the stats readers are on other threads; the engine set changes here
    uint64_t retiredSkips_ = 0, retiredDecodes_ = 0;
};

} // namespace digi
} // namespace vibe
