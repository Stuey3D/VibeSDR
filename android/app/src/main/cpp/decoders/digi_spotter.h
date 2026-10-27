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
// ★★★ BAND'S CPU GOVERNOR (V12, 2026-10-27 — Stuart: "pull everything it can down, but … be mindful of CPU running that
//     many decoders at once"). Every Band engine sits behind a GATE: each gate period (the mode's slot; 4 s for the
//     free-running JTTY and PSK) its window's energy is read from the slice's own FFT bins (digi_activity.h — no transform
//     of its own). Undecided, the period's audio is HELD; the moment the window lights up it is flushed to the engine and
//     the rest goes straight in; a period that never lights up is DROPPED — its decode never happens (IDLE). Before a
//     window goes live the governor (digi_governor.h) is asked: within budget → decode (LIGHT first — one pass — then
//     FULL depth back while it fits); over → SHED, lowest priority first, counted for the health pill. The owner's
//     Always (no gate, never shed) and Off come first. status() is what the spots box header and the admin page show.
//     ★ No bins with the IQ (a test, the benchmark): every window counts as occupied — the budget still applies.
// ★ Spots go out through `emit` with their RF frequency (dial + audio offset) and the mode's name.
#pragma once
#include "digi_activity.h"
#include "digi_governor.h"
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
    /** Band: one block of the slice — `n` samples at `rate`, centred on RF `centreHz`, the last captured at `captureUtc`.
     *  ★ `binPow`/`nb`/`binHz`: the slice's FFT bins' power for this block (digi_activity.h slicePower — lowest frequency
     *  first, bin nb/2 at `centreHz`), for the activity gate. Null = no gate (every window counts as occupied). */
    void feedIq(const cf32* x, int n, double rate, double centreHz, double captureUtc,
                const float* binPow = nullptr, int nb = 0, double binHz = 0);

    /** ★ The owner's word per mode (Always / Off / Auto), asked at each gate period's start. Null = Auto for all. */
    void setOverrides(std::function<Override(const std::string& mode)> f) { std::lock_guard<std::mutex> lk(statMtx_); override_ = std::move(f); }
    /** ★ The benchmark's budget for this machine, in FT8 units (∞ = none). */
    void setBudgetCap(double units) { std::lock_guard<std::mutex> lk(statMtx_); budget_.setCap(units); }
    double budgetUnits() const { std::lock_guard<std::mutex> lk(statMtx_); return budget_.units(); }

    /** ★ One mode's state in Band, for the spots box header ("FT8 active · WSPR idle · JTTY shed") and the admin page. */
    struct ModeStatus {
        std::string mode;
        RunState state = RunState::Idle;
        int dials = 0;
        uint64_t idle = 0, shed = 0, live = 0;   ///< gate periods: dropped empty · dropped over budget · decoded
    };
    std::vector<ModeStatus> status() const;
    /** Bumped whenever a mode's state changes (the host sends the header's chips again). */
    uint64_t statusSeq() const { return statusSeq_.load(std::memory_order_relaxed); }
    /** Periods SHED (something there, over budget) — monotonic: the health pill's BehindWatch reads it. */
    uint64_t periodsShed() const;
    /** Periods dropped EMPTY — the gate's saving (admin, tests). */
    uint64_t periodsIdle() const;

    /** Slots skipped by every engine so far (monotonic — the health pill's BehindWatch reads it). */
    uint64_t slotsSkipped() const;
    uint64_t slotsDecoded() const;
    /** ★★ A GAP, BRIDGED (V12 2026-10-27 — the Pi 2's tap dropped ≈ 15 % of the radio): `n` samples of this slice that
     *  never arrived, ending at `captureUtc`, fed to its engines as ZEROS of exactly that length, so their clocks stay
     *  continuous and a slot with a hole in it still completes — what it heard whole is decoded (as DRM bridges a lost
     *  frame). Without it the next block's stamp jumped, read as lost audio, the slot re-anchored and was never cut:
     *  test-band-slice, 0.7 s lost a slot — FT8 2 calls of 80 → 77 bridged. The producer that dropped the samples calls
     *  this (it alone knows how many); stamps are never second-guessed — they jitter by milliseconds on a live box.
     *  VIBE_BAND_BRIDGE=0 switches it off (A/B). */
    void feedGap(int n, double rate, double centreHz, double captureUtc);
    /** Microseconds of zeros bridged into gaps, summed over the slices (monotonic) — Band's minute line. */
    uint64_t usBridged() const { return bridgedTotal_.load(std::memory_order_relaxed); }
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
    /** The widest slice's centre at the last re-plan (tests). */
    double planCentre() const { return planCentre_; }

private:
    /** ★ A Band run's gate (see the file note). */
    struct Gate {
        WindowMeter meter;
        int64_t per = INT64_MIN;             // the gate period now (floor(utc / period))
        enum St : uint8_t { Pending, Live, Shed, Off } st = Pending;
        RunState rs = RunState::Idle;        // the governor's last word, for status()
        bool saw = false;                    // the window lit up this period
        int  hang = 0;                       // periods still to start live (the window was busy lately)
        std::vector<int16_t> buf;            // held audio (×kHoldScale), oldest first …
        std::vector<std::pair<size_t, double>> marks;   // … and each block's end offset in it with its capture UTC
        double fedUtc = -1e300;              // the capture UTC of the last sample the engine got
        uint64_t idleN = 0, shedN = 0, liveN = 0;
        double loHz = 0, hiHz = 0;           // the window in RF
    };
    struct Run {
        const ModeDef* def = nullptr;
        double dial = 0;
        std::unique_ptr<Engine> eng;
        std::shared_ptr<std::atomic<bool>> light;   // ★ the governor's depth: true = told it is loaded (one pass)
        std::unique_ptr<Gate> gate;                 // Band only
    };
    struct Chan {                     // ★ one tuner, and every engine on its dial
        std::unique_ptr<DigiTuner> tuner;
        std::vector<Run> runs;
        long long slice = 0;          // ★ the slice it is fed from (Slice key: its centre in Hz)
    };
    /** ★★ ONE SLICE OF THE CAPTURE (V12, 2026-10-27): a band's main cluster, or one of its extra slices (digi_bands.h
     *  Span::extras — 40 m's JTTY, 6 m's JTTY, 15/10 m's FT4 …). Keyed by its RF centre; each has its own gate floor. A
     *  slice not fed for kSliceGoneSec has gone (the band moved, an extra fell outside the radio's capture). */
    struct Slice {
        double rate = 0, centre = 0, lastUtc = -1e300;
        SliceFloor floor;
        uint64_t blocks = 0;          // ★ blocks with bins (the gate looks every 8th)
        int gNb = 0; double gBinHz = 0, gCentre = 0;   // the windows' placement
        double endUtc = NAN;          // ★ the capture time of the last sample fed (gap bridging)
        int lastGated = 0;            // ★ the gate's state on the last real block (a bridged gap keeps it)
        uint64_t bridged = 0;         // ★ samples of zeros put into gaps
    };
    static constexpr double kSliceGoneSec = 3.0;
    std::vector<cf32> zeros_;
    void feedChans(long long key, Slice& S, const cf32* x, int n, double rate, double centreHz, double captureUtc, int gated);
    void plan();
    void planSingle(double dialHz);
    void wire(Run& r);
    // ── the gate and the governor (Band) ──
    static double gatePeriod(const ModeDef& m);
    void placeWindows(long long key, Slice& s, int nb, double binHz, double centreHz);
    void route(Run& r, const float* a, int k, double utc, int gated, Slice& s);
    void newPeriod(Run& r, int64_t per, int gated);
    RunState ask(Run& who, bool wants);          // statMtx_ held
    void goLive(Run& r);
    void hold(Gate& g, const float* a, int k, double utc, double keepSec);
    void setState(Gate& g, RunState s);
    double unitsInUse() const;                   // statMtx_ held
    uint64_t skippedLocked() const;              // statMtx_ held

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
    std::map<long long, Slice> slices_;  // ★ keyed by the slice's centre in Hz
    double planRate_ = 0, planCentre_ = 0;   // ★ the widest slice's (tests)
    std::vector<float> audio_, scratch_;
    std::function<Override(const std::string&)> override_;
    BandBudget budget_;
    bool loadedNow_ = false;
    std::atomic<uint64_t> statusSeq_{0};
    std::atomic<uint64_t> bridgedTotal_{0};
    uint64_t retiredIdle_ = 0, retiredShed_ = 0;

    mutable std::mutex statMtx_;          // ★ the stats readers are on other threads; the engine set changes here
    uint64_t retiredSkips_ = 0, retiredDecodes_ = 0;
};

} // namespace digi
} // namespace vibe
