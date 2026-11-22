// VibeSDR V4 — WEFAX (HF weather fax) decoder.
//
// C++ port of UberSDR's ka9q audio_extensions/wefax/decoder.go. Takes mono
// int16 audio at a fixed sample rate, FM-demodulates the fax subcarrier, detects
// START/STOP/phasing lines and emits decoded image scanlines via callbacks. The
// shim wraps this in the /ws/dxcluster audio-extension protocol (0x01 line /
// 0x02 START / 0x03 stop) so the existing VibeSDR WEFAX UI works unchanged.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vibe {

// 17-tap low-pass FIR (ACfax coefficients), narrow/middle/wide.
class WefaxFIR {
public:
    explicit WefaxFIR(int bandwidth) : bw(bandwidth) {}
    double apply(double sample);
private:
    int bw;
    double buffer[17] = {0};
    int current = 0;
};

class WefaxDecoder {
public:
    enum HeaderType { HeaderImage = 0, HeaderStart = 1, HeaderStop = 2 };

    struct Config {
        int    lpm           = 120;
        int    imageWidth    = 1809;
        double carrier       = 1900.0;
        double deviation     = 400.0;
        int    bandwidth     = 1;     // 0=narrow 1=middle 2=wide
        bool   usePhasing    = true;
        bool   autoStop      = true;
        bool   autoStart     = true;
        bool   includeHeaders = false;
    };

    WefaxDecoder(int sampleRate, const Config& cfg);
    void process(const int16_t* samples, int count);

    int width() const { return imageWidth; }

    /** ★★ WHAT THE DECODER HEARD, for the background WEFAX VFO's HOURLY LOG LINE (2026-11-22: 8 h on Stuart's Pi 500 with
     *  not one `wefax:` line, and no way to tell "no signal" from "missed the start"). Counted on the decode thread as it
     *  goes — a few adds a line, one multiply-add a sample — and handed over (and reset) by takeStats(). */
    struct Stats {
        uint64_t samples = 0;            ///< audio samples in
        double   sumSq = 0;              ///< Σ x² (x in −1…1) — the audio level
        int      peak = 0;               ///< largest |sample| (int16)
        uint64_t lines = 0;              ///< lines read (whatever they held)
        int      bestStartHits = 0;      ///< most start-tone lines in any TONE_WINDOW (a START needs TONE_HITS)
        int      bestStopHits = 0;       ///< …and stop-tone lines
        double   bestStartLevel = 0;     ///< highest per-line start-tone reading (a tone line reads > TONE_LEVEL)
        int      bestLookAgree = 0;      ///< most phasing-like lines agreeing in any LOOK_WINDOW (a toneless start needs LOOK_HITS)
        int      starts = 0;             ///< charts begun (start tone, toneless phasing, a chart after the stop tone)
        int      stops = 0;              ///< stop tones
        int      phasedUsed = 0, phasedRejected = 0;   ///< phasing verdicts
    };
    Stats takeStats() { Stats s = stats_; stats_ = Stats{}; return s; }
    static constexpr int kToneWindow() { return TONE_WINDOW; }
    static constexpr int kToneHits() { return TONE_HITS; }
    static constexpr int kLookWindow() { return LOOK_WINDOW; }
    static constexpr int kLookHits() { return LOOK_HITS; }

    std::function<void(uint32_t lineNo, uint32_t width, const uint8_t* px)> onLine;
    std::function<void()> onStart;
    std::function<void()> onStop;
    /** ★ Which part of the transmission each line is — 0 standing by (noise), 1 start tone, 2 phasing, 3 image, 4 stop tone — sent
     *  ONLY when it changes (Stuart, 2026-10-04: "show the part of the transmission it is receiving such as the
     *  phasing lines"). The clients draw it as the status; the image lines themselves say "receiving". */
    std::function<void(int phase)> onPhase;
    /** ★★ WHY A CHART WAS (OR WAS NOT) PHASED, in words, for the log (2026-10-07, Stuart's JMH off a Japanese Kiwi: start tone
     *  caught, phasing ignored, and nothing said why). Each START with the tone run that fired it, and each phasing
     *  verdict — pulse position, how tightly the lines agreed, used or rejected. Rare (a few per chart), never per line. */
    std::function<void(const std::string& msg)> onDiag;

private:
    void decodeFaxLine();
    void demodulateData();
    double fourierTransformSub(const uint8_t* buf, int len, int freq);
    HeaderType detectLineType(const uint8_t* buf, int len);
    int faxPhasingLinePosition(const uint8_t* img);
    void binLine(const uint8_t* img);
    int pulseFromBins() const;
    int phasingLook();
    void beginChart(const std::string& why, bool openPhasing, int curPos);
    void decodeImageLine();
    void resampleLine();   // this demod line → imgData (slot imageLine & 1)
    void updateCorr();     // the chart/noise verdict (corrAvg) from it and the line before
    void emitLine();       // blend, auto-level, onLine

    // Config
    int lpm, imageWidth, bandwidth;
    double carrier, deviation;
    bool usePhasing, autoStop, autoStart, includeHeaders, skipHeaderDetection;

    double samplesPerSec;
    int    samplesPerLine;

    // Demod state
    WefaxFIR firI, firQ;
    double iPrev = 0, qPrev = 0;

    // Live auto-level (contrast normalisation). The raw FM-demod comes out dark/low-contrast at the
    // 48 kHz feed rate, so — like the completed-image post-process, but LIVE and server-side so every
    // client (web/phone/watch) benefits — we track a running histogram of demod values and stretch the
    // 2nd…98th percentile to full [0,255] as each line is emitted.
    uint32_t levelHist[256] = {0};
    uint64_t levelCount = 0;
    uint8_t  levelLut[256];
    bool     levelLutReady = false;
    void updateAutoLevel(const uint8_t* line, int w);

    // Sample buffering
    std::vector<int16_t> samples;
    int sampIdx = 0;
    std::vector<uint8_t> demodData;
    int skip = 0;

    // Image state
    std::vector<uint8_t> imgData;     // rolling decoded lines
    std::vector<uint8_t> outImage;    // one blended output line
    int imageLine = 0, imgHeight = 256, imgPos = 0;
    double lineIncrFrac = 0, lineIncrAcc = 0, lineBlend = 0;

    // Header detection
    int startIOC576Frequency = 300, stopFrequency = 450;
    /** ★ The tone judged by persistence, not an unbroken run — see decodeFaxLine (measured 2026-10-07). */
    static constexpr int TONE_WINDOW = 10, TONE_HITS = 7, TONE_CLEAR = 2, TONE_SHOW = 4;
    static constexpr double TONE_LEVEL = 3.5;
    uint8_t toneRing[TONE_WINDOW] = {};   // the last TONE_WINDOW lines' HeaderType
    int toneRingPos = 0;
    bool startLatched = false, stopLatched = false;   // fired for the tone now running

    // Phasing
    int phasingLines = 40;
    std::vector<int> phasingPos;
    int phasingLinesLeft = 0, phasingSkipData = 0;
    /** ★ Phasing pulses this close (fraction of a line, either way, on the circle) agree — see decodeFaxLine. */
    static constexpr double PHASE_TOL = 0.02;
    bool havePhasing = false;

    // Control
    bool autoStopped = false, autoStarted = false;
    int lastPhase = -1, pendingPhase = -1, pendingCount = 0;
    double corrAvg = 0.0;   // smoothed line-to-line correlation — chart vs noise (decodeImageLine)
    static constexpr double CORR_ON = 0.25, CORR_OFF = 0.12;   // chart noticed / given up — see decodeFaxLine

    /* ★ A NEW CHART WITHOUT ITS START TONE — see the ★★★ note in decodeFaxLine (2026-11-15). */
    bool chartOver = false;     // a stop tone latched: whatever chart follows is a new one
    int  chartLines = 0;        // lines since this picture began (imageLine may not reset: includeHeaders)
    static constexpr int LOOK_WINDOW = 20, LOOK_HITS = 14;    // phasing-like lines that agree, of the last LOOK_WINDOW
    static constexpr double LOOK_TOL = 0.01;                  // …agree: pulses this close (fraction of a line, on the circle)
    static constexpr int LOOK_BINS = 512;                     // the cheap look's resolution across a line
    static constexpr int LOOK_SEGS = 16;                      // …and the coarse segments the rest is judged in
    /* ★ Measured (2026-11-15; demod levels, which run ~10 black to ~45 white here — the FIR's ripple, see pulseFromBins):
     *  a phasing line's pulse window stands 26–35 above its rest off air (DDK, Northwood), 22+ for 95 % of synthetic
     *  lines under heavy noise; a satellite picture's cloud on grey ocean (NOAA IR, corpus evpz11) only 7–17, and at 10
     *  it was taken for phasing — hence 20. The bright part is 3.1–3.3 % of the line clean, up to ~6 % under noise; the
     *  rest's segments (one high, one low set aside) spread ≤ 0.12 of the contrast clean and ≤ 0.35 for 88 % of noisy
     *  lines, while chart lines spread far wider (paper and ink). */
    static constexpr double LOOK_CONTRAST = 20;               // pulse window above the rest, demod levels
    static constexpr double LOOK_PULSE_MIN = 0.015, LOOK_PULSE_MAX = 0.08;   // the bright part's share of the line
    static constexpr double LOOK_REST_SPREAD = 0.35;          // the rest's segments' spread, share of the contrast
    static constexpr int MIN_CHART_LINES = 120;               // a chart this young cannot be followed by another's phasing
    Stats stats_;
    double lastStartDet = 0;                                  // this line's start-tone reading (detectLineType)
    /* ★★★ A chart out of the noise while waiting in the background (see decodeFaxLine): begun on the chart/noise verdict,
     *  ended after IDLE_END_LINES (3 minutes) of standing by. */
    static constexpr int IDLE_END_LINES = 360;
    bool fromNoise = false;
    int  idleRun = 0;
    int  lookRing[LOOK_WINDOW];                               // pulse position of each recent line, −1 not phasing-like
    int  lookPos = 0;
    int  lookBin[LOOK_BINS];
};

} // namespace vibe
