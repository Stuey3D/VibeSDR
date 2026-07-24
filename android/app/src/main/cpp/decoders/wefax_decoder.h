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

    std::function<void(uint32_t lineNo, uint32_t width, const uint8_t* px)> onLine;
    std::function<void()> onStart;
    std::function<void()> onStop;

private:
    void decodeFaxLine();
    void demodulateData();
    double fourierTransformSub(const uint8_t* buf, int len, int freq);
    HeaderType detectLineType(const uint8_t* buf, int len);
    int faxPhasingLinePosition(const uint8_t* img);
    void decodeImageLine();

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
    int startIOC576Frequency = 300, stopFrequency = 450, startStopLength = 5;
    HeaderType lastType = HeaderImage;
    int typeCount = 0;

    // Phasing
    int phasingLines = 40;
    std::vector<int> phasingPos;
    int phasingLinesLeft = 0, phasingSkipData = 0;
    bool havePhasing = false;

    // Control
    bool autoStopped = false, autoStarted = false;
};

} // namespace vibe
