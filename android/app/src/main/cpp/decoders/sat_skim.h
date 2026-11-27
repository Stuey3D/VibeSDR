// decoders/sat_skim.h — ★★★ beta97 THE LIVE SATELLITE DIGITAL-MODE SKIMMER: FT8 and FT4 decoded across a linear
// transponder's WHOLE passband at once (docs/v12/SAT-SKIM.md).
//
// ★★★ WHY (Stuart, 2026-11-27): he decoded FT4 through JO-97's transponder on the XCover with nothing but the satellite
//     Doppler lock's correction ("decode here", branch b97-xpdr-tune). His decision for v2: skim the WHOLE passband, live.
//
// ── Where the IQ comes from ──────────────────────────────────────────────────────────────────────────────────────
//  The transponder finder (vibe_xpdr_run.h) already turns its fixed IQ slice by the model's Doppler at the span's centre —
//  ONE mixer for the whole passband, so every signal keeps one satellite-frame place all pass: offset o in that stream =
//  satellite frame centre + o. That stream is what this is fed (feed). The Doppler DIFFERENCE across the passband is what
//  one mixer leaves: at 145 MHz a 20 kHz passband sees at most 2.7e-5 × 20 kHz ≈ 0.5 Hz of it, at 435 MHz a 100 kHz one
//  2.7 Hz — far inside FT8's ±2.5 Hz-a-tone tolerance over a slot, and the residual RATE (the bit that smears a tone over
//  a slot) scales the same way. Nothing per window is needed.
//
// ── The windows ───────────────────────────────────────────────────────────────────────────────────────────────────
//  Overlapping USB "dials" (windowDials) every kWinStepHz across the span (+ kEdgeHz either side for a dongle's error and a
//  station at the very edge); each window is searched over kWinAudioLoHz … kWinAudioHiHz above its dial, so neighbouring
//  windows overlap by 300 Hz — wider than an FT4 signal (83 Hz) — and nothing between them is missed. A signal in the
//  overlap is decoded twice; the second copy is dropped (same mode, slot and message).
//  Each window's audio is cut by a DigiTuner (decoders/digi_tuner.h — Band mode's own receiver, unchanged: mix, polyphase
//  decimation, a sharp complex low-pass that removes the other sideband, level control) at 12 kHz.
//  ★ THE SIDEBAND. Every listed linear bird comes DOWN in USB (an inverting one turns an LSB uplink into a USB downlink —
//    WSJT-X's split-satellite practice); `lsb` in the config (the finder's "sb") mirrors the stream first, so an LSB
//    downlink is cut exactly as a USB one, and the spot's frequency is mirrored back (rfOf).
//
// ── The slots, the queue and the governor ──────────────────────────────────────────────────────────────────────────
//  Per window, ONE 15 s int16 buffer per UTC FT8 slot (360 KB): the FT8 slot, and FT4's two 7.5 s slots inside it (FT4's
//  grid is FT8's halved, so one buffer serves both — half the RAM of a buffer each). As each slot ends its decodes become
//  TASKS on one queue: FT4's first half at +7.5 s, FT8 and FT4's second half at +15 s.
//  WORKERS (Config::workers — Live: up to 4, one core left; CatchUp: 1) take tasks oldest first, at the decoders' thread
//  priority (vibeDecoderLagThread — Network > Audio > Spectrum > Decoders; vibeBackgroundThread for an unattended pass), and
//  never touch the decode thread, which only copies samples. Each owns one FT8 and one FT4 slot decoder (≈ 4 MB) — not one
//  per window.
//  ★★ THE CATCH-UP QUEUE. A task not decoded before the next slot simply waits: nothing is skipped for being late. It is
//     BOUNDED — Config::capBytes (defaultCapBytes: a 24th of the machine's RAM, 8–64 MB: ~10 MB on a 256 MB box). Past the
//     cap the OLDEST slot's tasks (every window's) are dropped and the log says so; the newest audio is always kept.
//     Spots decoded late say so ("late": the clients mark them caught up).
//  ★★ THE LOAD LADDER (digi_load_ladder.h — "the depth drops before …"): behind = in Live, a task older than one slot
//     still waiting; in CatchUp, the queue over half its cap. Each step makes every decode shallower (no deep tier → no
//     OSD/AP → one pass → fewer candidates → stricter sync); calm brings it back. Extra passes are bought exactly as the
//     live decoder buys them (Ft8Decoder::passAllowed's rule): only while this task's share of the slot has room
//     (perTaskMs) and the server is not loaded.
//  ★ THE BENCHMARK SETS THE DEFAULT, NEVER HIDES (defaultMode): Live where the benchmark's FT8 budget covers every window
//    light (FT8 + FT4 ≈ 1.2 units a window), CatchUp below that. The owner may always ask for Live.
//
// ── Memory floor (256–512 MB boxes) ───────────────────────────────────────────────────────────────────────────────
//  open buffers (windows × 360 KB, ≤ 64 windows) + the queue's cap + the workers' decoders (≈ 4 MB each) + the tuners
//  (≈ 40 KB each). JO-97 (9 windows) on a 256 MB box with one worker: 3.2 + 10.6 + 4 ≈ 18 MB at the very worst.
//
// ★ Off-air text is untrusted: spots carry only what Ft8Decoder::spotFields lets through, and the host's emitSpot
//   whitelists again. Nothing here touches a socket.
#pragma once
#include "digi_load_ladder.h"
#include "digi_mode.h"
#include "digi_tuner.h"
#include "ft8_decoder.h"

#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace vibe {
namespace satskim {

using cf32 = std::complex<float>;

constexpr int kRate = 12000;                    ///< every window's audio rate (Ft8SlotDecoder's)
constexpr double kSlotSec = 15.0;               ///< one buffer = one FT8 slot = two FT4 slots
constexpr int kSlotSamples = 180000;            ///< 15 s at 12 kHz
constexpr int kHalfSamples = 90000;             ///< FT4's second slot starts here
constexpr double kWinAudioLoHz = 200.0;         ///< the part of a window a signal is credited to …
constexpr double kWinAudioHiHz = 3000.0;        ///< … (the tuner passes kTunerLoHz … kTunerHiHz around it)
constexpr double kWinStepHz = 2500.0;           ///< 300 Hz overlap: more than an FT4 signal's 83 Hz
constexpr float kTunerLoHz = 100.0f, kTunerHiHz = 3100.0f;
constexpr double kEdgeHz = 1400.0;              ///< skimmed beyond the span's ends (dongle error, a station at the edge)
constexpr int kMaxWindows = 64;                 ///< FO-29's 100 kHz is 41
constexpr int kStartSlackSamples = 600;         ///< a slot whose first 50 ms are missing is still decoded
constexpr size_t kHeardMax = 60;                ///< the card's list
constexpr double kLateSec = 30.0;               ///< decoded this long after its slot ended = "caught up"

/** What a spot was heard through: the bird, its NORAD number and the pass (its AOS, unix s; 0 = not known). */
struct Tag { std::string via; long long norad = 0; double passAos = 0; };

/** One decode, as the satellite card lists it. */
struct Heard {
    std::string mode, call, grid, text;
    int snr = 0;
    double rfHz = 0;        ///< satellite frame
    double slotUtc = 0;
    double decodedUtc = 0;
    bool late = false;      ///< decoded from the catch-up queue (≥ kLateSec after its slot ended)
};

/** The windows' USB dials covering [loHz, hiHz] (+ kEdgeHz each side), centred on the span. Pure (tests). */
std::vector<double> windowDials(double loHz, double hiHz);

class Skimmer {
public:
    enum class Mode { Live, CatchUp };
    struct Config {
        double centreHz = 0;                ///< satellite frame: 0 Hz of the stream fed
        double loHz = 0, hiHz = 0;          ///< the span skimmed (satellite frame)
        bool lsb = false;                   ///< the downlink's sideband is LSB (the finder's "sb"): mirror first
        Mode mode = Mode::Live;
        int workers = 0;                    ///< 0 = by mode (Live: min(4, cores − 1), CatchUp: 1)
        size_t capBytes = 0;                ///< the catch-up queue's cap; 0 = defaultCapBytes()
        bool background = false;            ///< an unattended pass: the workers in the background class
        bool ft8 = true, ft4 = true;
        std::function<void(const std::string&)> log;
        /** A decode out (a worker thread): the spot, its RF (satellite frame) and what it was heard through. */
        std::function<void(const digi::Spot&, double rfHz, const Tag&)> emit;
        /** What the audio being cut now is heard through — asked as each slot's buffer starts (the decode thread). */
        std::function<Tag()> tag;
        /** The server's load (true = no extra passes). Empty = never. */
        std::function<bool()> loaded;
    };
    explicit Skimmer(Config c);
    ~Skimmer();
    Skimmer(const Skimmer&) = delete;
    Skimmer& operator=(const Skimmer&) = delete;

    /** The decode thread: `n` samples of the Doppler-turned stream at `rate` (0 Hz = Config::centreHz), the last one
     *  captured at `captureUtc` (corrected UTC). Copies and returns — the decoding is the workers'. */
    void feed(const cf32* x, int n, double rate, double captureUtc);
    /** Audio was lost (a gap, the Doppler lock lost): the slots being cut are abandoned (their queued halves stay). */
    void gap();
    /** On / off (a listener's toggle). Off abandons the slots being cut; the queue is still worked off. */
    void setOn(bool on);
    bool on() const { return on_.load(); }
    /** No more input ever (the finder is going): what is queued is still decoded. */
    void finish();
    /** Nothing queued or decoding. */
    bool idle() const;
    /** Tests: wait until idle (true) or `sec` passes (false). */
    bool waitIdle(double sec) const;

    struct Stats {
        Mode mode = Mode::Live;
        int windows = 0, workers = 0, depth = 0;
        size_t queuedTasks = 0, queuedSlots = 0, queuedBytes = 0, capBytes = 0, peakBytes = 0;
        uint64_t tasksDecoded = 0, tasksDropped = 0, slotsDropped = 0, spots = 0, lateSpots = 0, dupes = 0;
        double lagSec = 0;            ///< how far behind the capture the oldest waiting task is (0 = none waiting)
        double lastTaskMs = 0, meanTaskMs = 0, cpuMsPerSlot = 0;
    };
    Stats stats() const;
    std::vector<Heard> heard(size_t max = kHeardMax) const;
    /** The "skim" object the finder's status carries (no braces' owner: the caller writes "\"skim\":" + this). */
    std::string statusJson(bool waitingForLock) const;
    const std::vector<double>& dials() const { return dials_; }
    /** A window's audio offset → satellite-frame RF (Hz). */
    double rfOf(int window, double audioHz) const;
    const Config& config() const { return c_; }

    /** ★ The benchmark's default: Live when its FT8 budget (units; NaN / ∞ = none measured) covers `windows` light. */
    static Mode defaultMode(double budgetUnits, int windows);
    static const char* modeName(Mode m) { return m == Mode::Live ? "live" : "catchup"; }
    /** ★ A 24th of this machine's RAM, 8–64 MB. */
    static size_t defaultCapBytes();
    /** Bytes one window-slot buffer holds. */
    static constexpr size_t kBufBytes = (size_t)kSlotSamples * sizeof(int16_t);
    /** ★ Keep `s` alive until its queue is worked off (at most `maxSec`), on a thread of its own, then let it go. */
    static void retire(std::shared_ptr<Skimmer> s, double maxSec = 1800.0);

private:
    struct Buf;
    struct Pool;
    enum Kind : uint8_t { kFt8 = 0, kFt4a = 1, kFt4b = 2 };
    struct Task { std::shared_ptr<Buf> buf; Kind kind; };
    struct Win {
        double dial = 0;              ///< in the cut frame (mirrored for LSB)
        std::unique_ptr<digi::DigiTuner> tuner;
        std::shared_ptr<Buf> cur;     ///< the slot being cut
        std::vector<float> out;       ///< scratch: this block's audio
        std::mutex bookMtx;           ///< the books are a worker's while it decodes this window
        std::unique_ptr<FtxApBook> book8, book4;
    };
    struct Worker;

    void build_(double rate);
    void startBuf_(Win& w, int wi, double slotUtc, int pos);
    void enqueue_(std::vector<Task>& ts);       // q_ locked inside
    void dropOldestLocked_();
    void workerLoop_(int idx);
    void decode_(Worker& wk, const Task& t);
    void judge_(double captureUtc);
    size_t tasksPerSlot_() const { return (size_t)dials_.size() * ((c_.ft8 ? 1 : 0) + (c_.ft4 ? 2 : 0)); }

    Config c_;
    std::vector<double> dials_;
    std::vector<std::unique_ptr<Win>> wins_;
    std::shared_ptr<Pool> pool_;
    std::atomic<bool> on_{true};
    bool finished_ = false;
    // the decode thread's
    double rate_ = 0;
    int64_t inTotal_ = 0;
    double utc0_ = 0;
    bool anchored_ = false;
    int64_t outTotal_ = 0;
    std::vector<cf32> mir_;
    double lastJudge_ = -1e300;
    double lastCapture_ = 0;
    // the queue
    mutable std::mutex q_;
    std::condition_variable qcv_;
    mutable std::condition_variable idleCv_;
    std::deque<Task> tasks_;
    int busy_ = 0;
    size_t queuedBytes_ = 0, peakBytes_ = 0;
    bool stop_ = false;
    std::atomic<bool> abort_{false};
    std::vector<std::thread> threads_;
    std::vector<std::unique_ptr<Worker>> workers_;
    double perTaskMs_ = 1000.0;
    // results
    mutable std::mutex r_;
    std::deque<Heard> heard_;
    std::map<std::string, double> seen_;   // dedupe: mode|slot|message → slot UTC (pruned)
    uint64_t tasksDecoded_ = 0, tasksDropped_ = 0, slotsDropped_ = 0, spots_ = 0, late_ = 0, dupes_ = 0;
    double lastTaskMs_ = 0, sumTaskMs_ = 0;
    uint64_t cpuNs_ = 0;
    std::shared_ptr<digi::LoadLadder> ladder_;
    size_t capBytes_ = 0;
    int nWorkers_ = 1;
};

} // namespace satskim
} // namespace vibe
