// VibeSDR — SpectrumEngine: the waterfall/spectrum data path on its own thread (docs/NATIVE-SPECTRUM-ENGINE.md).
//
// ★★★ WHY: on a Galaxy XCover 4s the waterfall "renders smoothly then crawls for a bit" (Stuart, 2026-10-12) — the
//   UI thread held 60 fps throughout, but every row came through the JS thread (jitter buffer, SignalProcessor,
//   ring, a 1 MiB image per row, repeat rows on setTimeout), and whenever JS was busy — React renders, GC, DAB
//   updates — rows arrived late and then in a burst. This class does all of that here, on a thread of its own, with
//   its own clock: JS hands over each decoded frame (one call) and the state it owns (view, layout, settings,
//   interaction, background, covered); the UI thread asks "anything new?" once per display frame.
// ★★ It reproduces src/components/WaterfallView.tsx exactly — the reference spec is in the doc above, section
//   "WaterfallView per-frame behaviour". Two deliberate differences, both bug fixes: the repeat-row timer stops on
//   background / cover / teardown (the JS one did not), and the manual range is compared by value (the JS compared
//   a new object every time and flushed history on unrelated setting changes).
// ★ No JSI and no Skia here. Images and paths are made through the functions passed to the constructor (the
//   vibe_rnskia exports in the patched react-native-skia), so this core builds and tests on the host.
#pragma once
#include "wf_row_target.h"
#include "wf_row_clock.h"
#include "wf_row_combine.h"   // ★★ V12 2026-10-30 thinned frames combined, not dropped
#include "wf_av_sync.h"
#include "wf_zoom_fan.h"   // + wf_motion.h
#include "wf_stamps.h"
#include "spectrum_processor.h"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace vibespec {

/** Layout and behaviour from React props (WaterfallView's frameCfg + the view). */
struct EngineConfig {
    double width = 0;          // canvas width (pt)
    double wfTop = 0;          // spectrum baseline (pt)
    double specH = 0;          // spectrum height (pt)
    bool   specShow = true;
    bool   peakHold = true;
    bool   smoothTune = true;
    int    displayCapHz = 0;   // the FRAME RATE cap (0 = none): under 60 the row target divides it (wf_row_target.h rowTargetUnderCap)
    int    panelHz = 0;        // ★ the panel's maximum refresh (0 = unknown): the interaction boost's ceiling when there is no cap
    int    targetRows = 0;
    // ★★ SCALE STAMPS (wf_stamps.h): the waterfall's rows on screen (WaterfallView shaderRows) and the fewest rows
    //    between two stamps (three label heights) — 0 = unknown (an older JS): defaults.
    int    stampViewRows = 0;
    // ★★★ THE RING'S DEPTH (src/utils/wfRingSize.ts decides it in JS — the view's rows, capped by the device's RAM tier):
    //     grow-only — the engine copies its history into the deeper ring (wf_ring_grow.h). 0 / ≤ the current = no change.
    int    ringRows = 0;
    int    stampMinGapRows = 0;     // WF_TARGET_ROWS = the setting's CAP: 60 smooth / 20 default / 0 sharp — the rows actually aimed for follow the feed (wf_row_target.h)
};

/** What the UI thread picks up. Handles are OWNED by the receiver once taken (wrap them or release them). */
struct UiUpdate {
    // Waterfall
    void*    image = nullptr;      // new ring image (1024 × 1024 Gray_8) — nullptr if none since the last poll
    double   head = 0;             // uHead for that image (frameCount)
    bool     texReady = false;
    int      quant = 1;            // uQuant
    double   avgFrameMs = 150;     // uAvgMs
    double   rowRate = 0;          // ★ V12 the fixed output clock's rate, rows/s (0 = NATIVE / actual rows) — the UI's pTgt + clockPace
    uint32_t scrollGen = 0;        // changed → apply the LATEST scroll command:
    bool     glide = false;        //   (unused by the UI since 2026-10-28 — the display driver paces both ways)
    double   glideDur = 100;
    double   moLo = 1, moHi = 0;   // the image's motion range (absolute frames; lo > hi = none) — uMoLo/uMoHi
    double   fanK = 1, fanC = 0;   // the fans' screen move since the last image the UI took (x_old = fanC + fanK·x)
    int      ringRows = 1024;      // ★★ the ring depth of THIS image (the shader's uRows) — rides with the image
    // Spectrum
    uint32_t specGen = 0;          // changed → specTarget is new
    std::vector<float> specTarget; // the decimated trace target (specToSv)
    bool     specHidden = false;   // spectrum hidden → reset the trace paths, peak empty
    // ★★ Scale stamps (wf_stamps.h): changed → `stamps` is the new list (newest first)
    uint32_t stampGen = 0;
    bool stampsNew = false;
    std::vector<vibewf::WfStamp> stamps;
    // Peak
    uint32_t peakGen = 0;          // changed → peakPath is new (nullptr = the empty path)
    void*    peakPath = nullptr;
};

class SpectrumEngine {
public:
    using MakeImage = void* (*)(const uint8_t* px, int w, int h, size_t rowBytes);
    using MakePath  = void* (*)(const float* xy, int nPoints, bool close);
    using Release   = void (*)(void*);
    /** `nowMs` = wall clock in ms (Date.now on the JS side — lastInteractAt is compared against it). */
    using Clock     = double (*)();

    static constexpr int RING_W = 1024;
    static constexpr int RING_ROWS = 1024;   // the ring's STARTING (and the low RAM tier's only) depth — ringRows_ grows
    static constexpr int RING_MAX_ROWS = 4096;   // the top RAM tier (wfRingSize.ts) — a sanity bound on what JS asks for

    SpectrumEngine(MakeImage makeImage, MakePath makePath, Release releaseImage, Release releasePath,
                   Clock clock = nullptr);
    ~SpectrumEngine();
    SpectrumEngine(const SpectrumEngine&) = delete;
    SpectrumEngine& operator=(const SpectrumEngine&) = delete;

    // ── From JS ──
    /** One decoded frame (raw dB, display order). Copied; returns at once. trueCenterHz NaN = absent. */
    /** ★★ trueBwHz = the bins' REAL span (SDRStatus.trueBwHz; NaN = bwHz is already theirs) — rows are placed by it. */
    void pushFrame(const float* bins, int n, double centerHz, double bwHz, double trueCenterHz, double trueBwHz = NAN);
    /** The same, from the native spectrum socket (spec_socket.cpp) on ITS thread, at arrival. While the socket has fed
     *  a frame in the last NATIVE_FED_MS, pushFrame (JS's copy of the same frame, delivered later) is ignored — so a
     *  server on the native socket and one on the JS path (OWRX, Kiwi) need no switch. */
    void pushFrameNative(const float* bins, int n, double centerHz, double bwHz, double trueCenterHz, double trueBwHz = NAN);
    /** ★★ V12 a full-capture row (SPWB v2, wf_wide.h) — the bytes as received; copied, applied on the engine thread with
     *  its ARRIVAL time (the layer interpolates between rows by it). From the native socket (…Native, its thread) or
     *  from JS (a server on the JS socket; ignored while the native socket feeds frames, as pushFrame is). */
    void pushWide(const uint8_t* p, size_t len);
    void pushWideNative(const uint8_t* p, size_t len);
    void setView(double centerHz, double bwHz);              // the props view (viewRef) — in-flight alignment
    void setConfig(const EngineConfig& c);
    void setSettings(const vibedsp::SpectrumProcessorSettings& s);
    void setLastInteractAt(double ms);                       // lastInteractAt.current
    /** ★★ V12 2026-10-30 the audio's playout delay, ms (−1 = unknown) — the clock waits for the sound (wf_av_sync.h).
     *  JS polls it at 4 Hz (VibeStreamService.audioPlayoutMs) and forwards it. */
    void setAudioDelay(double ms);
    void setBackground(bool bg);
    void setCovered(bool covered);
    /** dB window for the axis labels (2 dB hysteresis, as liveRange). Returns true when it changed since last read. */
    bool liveRange(int& dbMin, int& dbMax);
    /** ★★ The scale row `frame` (absolute) is DISPLAYED at — Hz at the left edge and across — while it is in the ring; false
     *  if not. A tap on an old row tunes by its own scale (Stuart, 2026-10-30, #57). Any thread (under mx_). */
    bool rowGeom(double frame, double& lo, double& span);

    // ── From the UI thread ──
    /** Fill `out` with everything new since the last poll. Cheap when nothing is (returns false). */
    bool pollUi(UiUpdate& out);

    /** Host tests: run `ms` of engine time synchronously instead of on the thread. */
    void testStep(double nowMs);
    static SpectrumEngine* makeForTest(MakeImage, MakePath, Release, Release, Clock);
    /** Host tests: the layer ring (null until the first SPWB) and the full-capture state's floor offset. */
    const vibewf::LayerRing* layerForTest() const { return fan_.layer.get(); }
    int ringRowsForTest() const { return ringRows_; }
    /** Host tests: the bytes the next image is made from (the displayed ring + motion planes) and its geometry. */
    const uint8_t* imageBytesForTest() { return vibewf::wfImageBytes(fan_, motion_, ring_.data(), RING_W, ringRows_); }
    const vibewf::MotionState& motionForTest() const { return motion_; }
    const vibewf::FanState& fanForTest() const { return fan_; }
    double framesForTest() const { return frameCount_; }
    const vibewf::StampState& stampsForTest() const { return stamps_; }
    double wideOffForTest() const { return wide_ ? wide_->off : NAN; }

private:
    void pushFrameImpl(const float* bins, int n, double centerHz, double bwHz, double trueCenterHz, double trueBwHz);
    void pushWideImpl(const uint8_t* p, size_t len);
    void applyWide(const std::vector<uint8_t>& bytes, double at);   // engine thread
    struct Frame { std::vector<float> bins; double centerHz = 0, bwHz = 0, trueCenterHz = NAN, trueBwHz = NAN; double arrivedMs = 0; };

    void loop();
    void runDue(double now);                 // timers + inbox, engine thread
    void enqueue(Frame&& f, double now);     // enqueueFrame
    void drain(double now);                  // drainFrame
    void handleFrame(const Frame& f, double now);
    void pushRow(const uint8_t* src, int srcN, double rowCenterHz, double rowHzPerBin, bool publish = true);
    void publishRing();
    void* makeRingImage();   // a new image of the ring as it stands (no lock), or nullptr
    void storeRingImageLocked(void* img);   // with mx_ held
    void emitRepeatRow();
    /** `at` > `now`: the trace waits for its row (WaterfallView traceShow) — queued, published by runTrace. */
    void buildSpectrum(const vibedsp::SpectrumFrameOut& fr, bool boost, double at = 0, double now = 0, bool shrunk = false);
    void publishTrace(const std::vector<float>& spec, void* peak);
    void runTrace(double now);
    void traceDrop();
    double now() const;

    // Injected
    MakeImage makeImage_; MakePath makePath_; Release releaseImage_, releasePath_; Clock clock_;

    // Thread
    std::thread thread_;
    std::mutex mx_;                       // guards the inbox, the JS-set state and the UI outbox
    std::condition_variable cv_;
    bool stop_ = false;
    bool threaded_ = true;

    // Inbox / state set from JS (under mx_)
    std::deque<Frame> inbox_;
    EngineConfig cfg_;
    double viewCenterHz_ = 0, viewBwHz_ = 0;
    double lastInteractAt_ = 0;
    double audioMs_ = -1; bool audioNew_ = false;   // ★ V12 the audio's playout delay, as JS last sent it
    bool bg_ = false, covered_ = false;
    bool settingsDirty_ = false;
    double nativeFedUntil_ = 0;
    static constexpr double NATIVE_FED_MS = 2000;
    vibedsp::SpectrumProcessorSettings pendingSettings_;
    std::vector<Frame> framePool_;        // recycled frame buffers
    struct WideIn { std::vector<uint8_t> bytes; double at = 0; };
    std::deque<WideIn> wideInbox_;        // ★ V12 full-capture rows waiting for the engine thread (newest few)

    // Engine-thread state
    vibedsp::SpectrumProcessor proc_;
    std::deque<Frame> jbQueue_;
    double jbArrivalMs_ = 150, jbLastArrival_ = 0, jbDueAt_ = -1;
    bool jbPrefill_ = true;
    std::vector<uint8_t> ring_;           // RING_W × (ringRows_ + motionImageRows): the ring + the motion rows the image carries
    int ringRows_ = RING_ROWS;            // ★★ the ring's depth (engine thread) — grows only (growRingTo, wf_ring_grow.h)
    int imgRows_ = RING_ROWS;             // the depth of the image just made (engine thread) → outRingRows_
    void growRingTo(int rows);
    std::vector<uint8_t> rowBuf_;         // resample scratch
    vibewf::MotionState motion_{RING_ROWS, RING_W};   // each slot's placement + its move to the next row (wf_motion.h)
    vibewf::FanState fan_{RING_ROWS};     // the zoom fan: per-row maps + the displayed ring, made at the first zoom (wf_zoom_fan.h)
    double frameCount_ = 0;
    bool ringDirty_ = false;
    std::vector<uint8_t> lastRow_;        // unresampled processed row (repeat rows)
    double lastRowCenterHz_ = 0, lastRowHzPerBin_ = 0;
    double avgFrameMs_ = 150, lastFrameTs_ = 0, avgWfGapMs_ = 0, lastWfPushAt_ = 0;
    double rowCarry_ = 0;
    vibewf::RowTargetState rowTargetSt_;   // rows/s aimed for now + its feed latch (wf_row_target.h); target 0 = actual rows
    int    rowTargetCap_ = -1; // the cfg.targetRows rowTarget_ was decided under
    int extrasLeft_ = 0; double extraSpacing_ = 0, extraDueAt_ = -1;
    // ★★★ V12 THE FIXED OUTPUT CLOCK (wf_row_clock.h — every mode but NATIVE): rows made on its ticks, each the newest real
    //     frame presented by then; the frames it may still show are kept here by id (a few — its queue is 4 deep).
    struct ClockRow { long id = -1; std::vector<uint8_t> row; double c = 0, h = 0, k = 0; };   // k: dB per unit (wf_row_combine.h)
    // ★★ V12 2026-10-30 (wf_row_combine.h — WaterfallView's twin): the last frame the clock has SHOWN; the frames after it
    //    are kept (≤ COMBINE_MAX_FRAMES) and combined into the next one shown instead of dropped.
    long clockShown_ = 0;
    vibewf::RowCombiner clockComb_;
    vibewf::RowClock rowClock_;
    vibewf::BoostState boost_;   // ★ the interaction boost (wf_row_clock.h boostRowRate)
    vibewf::AvSyncState av_;     // ★★ V12 2026-10-30 the waterfall waits for the sound (wf_av_sync.h)
    int lastEaseLeft_ = 0;
    std::vector<ClockRow> clockRows_;
    std::vector<long> clockOut_;
    long clockSeq_ = 0;
    // ★★★ V12 2026-10-29 (v12-wf-no-jb) THE TRACE GOES WITH ITS ROW (WaterfallView traceShow): clocked frames skip the
    //     jitter buffer, and the spectrum target waits here until its frame's row is on screen (presentation + traceLagMs).
    struct TraceItem { double at = 0; std::vector<float> spec; void* peak = nullptr; };
    std::deque<TraceItem> traceQ_;
    void runClock(double now);
    void clockOff();
    bool covNow_ = false, bgNow_ = false;  // engine-thread copies
    EngineConfig cfgNow_;
    double viewC_ = 0, viewBw_ = 0, interactAt_ = 0;
    std::vector<float> specScratch_, peakXY_;
    // ★★ V12 THE FULL-CAPTURE LAYER (wf_wide.h): the newest picture of the whole capture (null until a server sends one —
    //    an older server allocates nothing), the detailed rows' median (its floor match) and the last row's placement.
    std::unique_ptr<vibewf::WideState> wide_;
    vibewf::WideRow wideRow_;
    double detP50_ = NAN, placedLo_ = NAN, placedSpan_ = 0;
    std::vector<float> p50Scratch_;
    vibewf::StampState stamps_;            // ★★ the scale stamps' detector (engine thread)
    unsigned stampSeen_ = 0;

    // Outbox for the UI (under mx_)
    void* outImage_ = nullptr; double outHead_ = 0; bool texReady_ = false;
    double imgK_ = 1, imgC_ = 0;                     // the fans' screen move taken with the image just made (engine thread)
    double imgMoLo_ = 1, imgMoHi_ = 0;               // …and its motion range
    double outK_ = 1, outC_ = 0, outMoLo_ = 1, outMoHi_ = 0;   // composed for the UI until it takes an image (mx_)
    int outRingRows_ = RING_ROWS;                     // the depth of outImage_ (mx_)
    int outQuant_ = 1; double outAvgMs_ = 150, outRowRate_ = 0;
    uint32_t scrollGen_ = 0; bool glide_ = false; double glideDur_ = 100;
    uint32_t specGen_ = 0; std::vector<float> outSpec_; bool specHidden_ = false;
    uint32_t peakGen_ = 0; void* outPeak_ = nullptr;
    uint32_t stampGen_ = 0, uiStamp_ = 0; std::vector<vibewf::WfStamp> outStamps_;
    struct RowGeo { double frame = -1, lo = 0, span = 0; };
    std::vector<RowGeo> rowGeo_;                     // ★★ per slot, as displayed when written (mx_) — rowGeom
    uint64_t outVer_ = 0, uiSeenVer_ = 0;
    uint32_t uiSpec_ = 0;
    // liveRange (under mx_)
    int liveMin_ = -120, liveMax_ = -20; bool liveChanged_ = false;
};

}  // namespace vibespec
