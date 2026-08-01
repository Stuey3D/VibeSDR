// VibeSDR V4 — SSTV decoder.
//
// C++ port of UberSDR's ka9q audio_extensions/sstv (which is itself based on
// slowrx by Oona Räisänen OH2EIQ): VIS code detection, FM video demodulation
// with adaptive windowing, and Linear-Hough slant correction. Takes mono int16
// audio at 12 kHz and emits the SSTV wire frames (0x07 imageStart / 0x01 line /
// 0x02 mode / 0x03 status / 0x04 sync / 0x05 complete / 0x08 redraw) that the
// existing VibeSDR DecoderClient SSTV parser already understands.
//
// Threading mirrors the Go original: process() (the audio thread) feeds a
// thread-safe circular buffer and runs VIS detection; on detection it spawns a
// video-decode thread that consumes from the buffer and emits image lines.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

extern "C" {
#include "fft/kiss_fftr.h"
}

namespace vibe {

// ── Mode spec ────────────────────────────────────────────────────────────────
enum SstvColor { SSTV_GBR = 0, SSTV_RGB = 1, SSTV_YUV = 2, SSTV_BW = 3 };

struct SstvMode {
    const char* name;
    double syncTime, porchTime, septrTime, pixelTime, lineTime;
    int imgWidth, numLines, lineHeight;
    SstvColor color;
    bool unsupported;
};

const SstvMode* sstvModeByIndex(uint8_t idx);
uint8_t sstvModeByVis(uint8_t vis);

// ── Real FFT (kiss_fftr wrapper) ─────────────────────────────────────────────
class SstvFFT {
public:
    explicit SstvFFT(int n);
    ~SstvFFT();
    void run(const float* in);               // in: n real samples
    double power(int bin) const;             // |X[bin]|^2 (0..n/2)
    double re(int bin) const, im(int bin) const;
    int size() const { return n; }
private:
    int n;
    kiss_fftr_cfg cfg;
    std::vector<kiss_fft_cpx> out;
};

// ── Circular PCM buffer (mirrors pcm_buffer.go) ──────────────────────────────
class SstvBuffer {
public:
    explicit SstvBuffer(int size);
    void write(const int16_t* s, int n);
    bool getWindow(int offset, int length, int16_t* out);
    void advanceWindow(int n);
    int  windowPtr();
    int  available();
    void reset();
private:
    std::vector<int16_t> buf;
    int size, wptr = 0, writePos = 0, fillPos = 0;
    std::mutex mu;
    int availableLocked();
};

// ── VIS detector ─────────────────────────────────────────────────────────────
class SstvVIS {
public:
    explicit SstvVIS(double sampleRate);
    // returns true on detect, sets mode index + headerShift
    bool process(SstvBuffer& pcm, uint8_t& modeOut, int& shiftOut);
    std::function<void(double)> onTone;
private:
    bool checkRange(int idx, double lo, double hi);
    int  getBin(double f) const { return (int)(f / sampleRate * fftSize); }
    double sampleRate;
    int fftSize = 2048;
    std::vector<double> headerBuf, toneBuf, hann;
    int headerPtr = 0, iter = 0;
    std::vector<float> fin;
    SstvFFT fft;
};

// ── Video demodulator ────────────────────────────────────────────────────────
struct SstvPixel { int time, x, y; uint8_t channel; };

class SstvVideo {
public:
    SstvVideo(const SstvMode* mode, double sampleRate, int headerShift, bool adaptive);
    // Demodulate consuming from pcm; lineSender(y, rgb[w*3]) called per line.
    void demodulate(SstvBuffer& pcm, double rate, int skip,
                    const std::function<void(int, const uint8_t*)>& lineSender,
                    const std::atomic<bool>& abort);
    /** RGB w*h*3. ★ `okOut` reports whether every pixel came from a REAL captured sample —
     *  false means the redraw ran past the end of what was actually decoded and the result must
     *  NOT be shown. See the slant-correction guard in videoThread(). */
    std::vector<uint8_t> redrawFromLuminance(double rate, int skip, bool* okOut = nullptr); // RGB w*h*3
    /** How far the last redraw ran past the captured audio, in samples (0 = it fitted). */
    int lastShortfallSamples = 0;
    /** Lines the last redraw could fully cover — everything below this had missing samples. */
    int lastGoodLines = 0;
    /** ★ Highest line actually RECEIVED, +1 (0 = none). A signal that fades or ends mid-picture
     *  leaves the rest blank, and the alignment pass has to know the difference between "these
     *  lines are missing" and "these lines exist but I cannot correct them": the first is
     *  harmless to leave out of a correction, the second is a tear. Public because that decision
     *  belongs to the caller, which is the only place that knows what it is about to send. */
    int linesReceived = 0;
    const std::vector<uint8_t>& syncFlags() const { return hasSync; }

    std::vector<SstvPixel> pixelGrid(double rate, int skip);
private:
    void   detectSync(SstvBuffer& pcm, int targetBin, int idx);
    double estimateSNR(SstvBuffer& pcm);
    double demodFreq(SstvBuffer& pcm, double snr);
    int    getBin(double f) const { return (int)(f / sampleRate * fftSize); }
    std::vector<uint8_t> toRGB(const std::vector<uint8_t>& img);

    const SstvMode* m;
    double sampleRate; int headerShift; bool adaptive;
    std::vector<std::vector<double>> hannWins; std::vector<int> hannLens;
    int fftSize = 1024;
    std::vector<float> fin;
    SstvFFT fft;
    std::vector<uint8_t> hasSync;   // 1/0 per sync sample
    std::vector<uint8_t> storedLum;
    /** ★★ HOW MUCH OF storedLum WAS ACTUALLY WRITTEN. The buffer is allocated with 1.3x headroom
     *  but only filled while the decode loop runs, and that loop BREAKS EARLY when the audio runs
     *  short. Everything past this index is a zero that was never a sample. */
    int storedLumWritten = 0;
};

// ── Sync corrector ───────────────────────────────────────────────────────────
class SstvSync {
public:
    SstvSync(const SstvMode* mode, double sampleRate, const std::vector<uint8_t>& hasSync)
        : m(mode), sampleRate(sampleRate), hasSync(hasSync) {}
    /** ★ `confOut` (0..1) is how much the sync data supports the answer. Near zero means the
     *  Hough peak is noise and the "correction" is a random shift — see the guard in videoThread. */
    void findSync(double& rateOut, int& skipOut, double* confOut = nullptr);
private:
    const SstvMode* m; double sampleRate; const std::vector<uint8_t>& hasSync;
};

// ── Top-level decoder ────────────────────────────────────────────────────────
class SstvDecoder {
public:
    explicit SstvDecoder(double sampleRate, bool autoSync = true, bool adaptive = true);
    ~SstvDecoder();
    void process(const int16_t* mono, int count);   // audio thread

    // Frame callbacks (already big-endian framed payloads where relevant).
    std::function<void(int w, int h)>              onImageStart;
    std::function<void(int y, int w, const uint8_t* rgb)> onLine;
    std::function<void(uint8_t idx, const std::string& name)> onMode;
    std::function<void(const std::string&)>        onStatus;
    std::function<void()>                          onSync;
    std::function<void()>                          onComplete;
    std::function<void()>                          onRedrawStart;
private:
    void videoThread();
    enum State { WaitingVIS, Decoding };
    double sampleRate; bool autoSync, adaptive;
    SstvBuffer pcm;
    SstvVIS* vis = nullptr;
    const SstvMode* mode = nullptr;
    int headerShift = 0;
    std::atomic<State> state{WaitingVIS};
    std::atomic<bool> abort{false};
    std::thread vthread;
    std::vector<int16_t> accum;
    int samps10ms;
    bool statusSent = false;
};

} // namespace vibe
