// decoders/digi_spotter.h — one listener's DIGITAL SPOTS run (V12, 2026-10-26): every registered mode (digi_mode.h) on
// either the listener's own audio (SINGLE) or a whole band's IQ slice (BAND). Owned by a DecoderHost; everything here
// runs on that host's decode thread, except the engines' own slot workers.
//
// ★★ SINGLE ("Digital spots: Single mode") is the button there was before: the listener's 48 kHz audio, box-averaged by
//    four to 12 kHz (the same decimation as ever), into every mode with `single` — FT8 and FT4 on one passband. Cheap,
//    and works on any server and any radio.
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
    void wire(Run& r);

    Scope scope_;
    std::vector<ModeDef> modes_;
    Emit emit_;
    std::function<double()> dialHz_;
    std::function<bool()> loaded_;

    // Single
    std::vector<Run> single_;
    static bool atOwnDial(const ModeDef& m, double dialHz);
    int   decim_ = 0; float acc_ = 0;
    std::vector<float> buf_;

    // Band
    std::map<long long, Chan> chans_;    // ★ keyed by the dial in Hz
    double planRate_ = 0, planCentre_ = 0;
    std::vector<float> audio_;

    mutable std::mutex statMtx_;          // ★ the stats readers are on other threads; the engine set changes here
    uint64_t retiredSkips_ = 0, retiredDecodes_ = 0;
};

} // namespace digi
} // namespace vibe
