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
#include "wf_glide.h"
#include "spectrum_processor.h"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
    int    targetRows = 0;     // WF_TARGET_ROWS = the setting's CAP: 60 smooth / 20 default / 0 sharp — the rows actually aimed for follow the feed (wf_row_target.h)
};

/** What the UI thread picks up. Handles are OWNED by the receiver once taken (wrap them or release them). */
struct UiUpdate {
    // Waterfall
    void*    image = nullptr;      // new ring image (1024 × 1024 Gray_8) — nullptr if none since the last poll
    double   head = 0;             // uHead for that image (frameCount)
    bool     texReady = false;
    int      quant = 1;            // uQuant
    double   avgFrameMs = 150;     // uAvgMs
    uint32_t scrollGen = 0;        // changed → apply the LATEST scroll command:
    bool     glide = false;        //   true  = start a glide of glideDur (scrollFrac 0 → 1)   (WaterfallView startGlide)
    double   glideDur = 100;       //   false = stop the glide, scrollFrac = 1                  (stopGlide + settle)
    // Spectrum
    uint32_t specGen = 0;          // changed → specTarget is new
    std::vector<float> specTarget; // the decimated trace target (specToSv)
    bool     specHidden = false;   // spectrum hidden → reset the trace paths, peak empty
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
    static constexpr int RING_ROWS = 1024;

    SpectrumEngine(MakeImage makeImage, MakePath makePath, Release releaseImage, Release releasePath,
                   Clock clock = nullptr);
    ~SpectrumEngine();
    SpectrumEngine(const SpectrumEngine&) = delete;
    SpectrumEngine& operator=(const SpectrumEngine&) = delete;

    // ── From JS ──
    /** One decoded frame (raw dB, display order). Copied; returns at once. trueCenterHz NaN = absent. */
    void pushFrame(const float* bins, int n, double centerHz, double bwHz, double trueCenterHz);
    /** The same, from the native spectrum socket (spec_socket.cpp) on ITS thread, at arrival. While the socket has fed
     *  a frame in the last NATIVE_FED_MS, pushFrame (JS's copy of the same frame, delivered later) is ignored — so a
     *  server on the native socket and one on the JS path (OWRX, Kiwi) need no switch. */
    void pushFrameNative(const float* bins, int n, double centerHz, double bwHz, double trueCenterHz);
    void setView(double centerHz, double bwHz);              // the props view (viewRef) — in-flight alignment
    void setConfig(const EngineConfig& c);
    void setSettings(const vibedsp::SpectrumProcessorSettings& s);
    void setLastInteractAt(double ms);                       // lastInteractAt.current
    void setBackground(bool bg);
    void setCovered(bool covered);
    /** dB window for the axis labels (2 dB hysteresis, as liveRange). Returns true when it changed since last read. */
    bool liveRange(int& dbMin, int& dbMax);

    // ── From the UI thread ──
    /** Fill `out` with everything new since the last poll. Cheap when nothing is (returns false). */
    bool pollUi(UiUpdate& out);

    /** Host tests: run `ms` of engine time synchronously instead of on the thread. */
    void testStep(double nowMs);
    static SpectrumEngine* makeForTest(MakeImage, MakePath, Release, Release, Clock);

private:
    void pushFrameImpl(const float* bins, int n, double centerHz, double bwHz, double trueCenterHz);
    struct Frame { std::vector<float> bins; double centerHz = 0, bwHz = 0, trueCenterHz = NAN; double arrivedMs = 0; };

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
    void buildSpectrum(const vibedsp::SpectrumFrameOut& fr, bool boost);
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
    bool bg_ = false, covered_ = false;
    bool settingsDirty_ = false;
    double nativeFedUntil_ = 0;
    static constexpr double NATIVE_FED_MS = 2000;
    vibedsp::SpectrumProcessorSettings pendingSettings_;
    std::vector<Frame> framePool_;        // recycled frame buffers

    // Engine-thread state
    vibedsp::SpectrumProcessor proc_;
    std::deque<Frame> jbQueue_;
    double jbArrivalMs_ = 150, jbLastArrival_ = 0, jbDueAt_ = -1;
    bool jbPrefill_ = true;
    std::vector<uint8_t> ring_;           // RING_W × RING_ROWS
    std::vector<uint8_t> rowBuf_;         // resample scratch
    vibewf::GlideState trail_{RING_W};    // previous row's placement + glide scratch (wf_glide.h)
    double frameCount_ = 0;
    bool ringDirty_ = false;
    std::vector<uint8_t> lastRow_;        // unresampled processed row (repeat rows)
    double lastRowCenterHz_ = 0, lastRowHzPerBin_ = 0;
    double avgFrameMs_ = 150, lastFrameTs_ = 0, avgWfGapMs_ = 0, lastWfPushAt_ = 0;
    double rowCarry_ = 0;
    vibewf::RowTargetState rowTargetSt_;   // rows/s aimed for now + its feed latch (wf_row_target.h); target 0 = actual rows
    int    rowTargetCap_ = -1; // the cfg.targetRows rowTarget_ was decided under
    int extrasLeft_ = 0; double extraSpacing_ = 0, extraDueAt_ = -1;
    bool covNow_ = false, bgNow_ = false;  // engine-thread copies
    EngineConfig cfgNow_;
    double viewC_ = 0, viewBw_ = 0, interactAt_ = 0;
    std::vector<float> specScratch_, peakXY_;

    // Outbox for the UI (under mx_)
    void* outImage_ = nullptr; double outHead_ = 0; bool texReady_ = false;
    int outQuant_ = 1; double outAvgMs_ = 150;
    uint32_t scrollGen_ = 0; bool glide_ = false; double glideDur_ = 100;
    uint32_t specGen_ = 0; std::vector<float> outSpec_; bool specHidden_ = false;
    uint32_t peakGen_ = 0; void* outPeak_ = nullptr;
    uint64_t outVer_ = 0, uiSeenVer_ = 0;
    uint32_t uiSpec_ = 0;
    // liveRange (under mx_)
    int liveMin_ = -120, liveMax_ = -20; bool liveChanged_ = false;
};

}  // namespace vibespec
