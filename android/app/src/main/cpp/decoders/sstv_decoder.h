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
    /** `exact`: honour a small size (the VIS watch's ring, 2026-10-05) — otherwise at least 8 M. */
    explicit SstvBuffer(int size, bool exact = false);
    void write(const int16_t* s, int n);
    bool getWindow(int offset, int length, int16_t* out);
    void advanceWindow(int n);
    int  windowPtr();
    int  available();
    void reset();
    /** ★ Primed = the initial 1024-sample fill is done. Was `windowPtr() == 0`, which is ALSO true
     *  each time the window wraps the 8 M ring — fatal once pictures run back to back without a
     *  reset (restart on a new VIS, 2026-10-05): writes went to the fill branch and were dropped. */
    bool ready() { std::lock_guard<std::mutex> lk(mu); return primed; }
    /** Samples stored since reset, and the window's total advance — the restart's coordinates. */
    long long writtenTotal();
    long long consumed();
private:
    std::vector<int16_t> buf;
    int size, wptr = 0, writePos = 0, fillPos = 0;
    long long total = 0, advanced = 0;
    bool primed = false;
    std::mutex mu;
    int availableLocked();
};

// ── VIS detector ─────────────────────────────────────────────────────────────
class SstvVIS {
public:
    /** `byEnergy`: also accept a VIS decided by energy (decideByEnergy). ★ OFF for the watch that runs
     *  DURING a picture: picture content holds 1900/1200/1100/1300 Hz energy too, and the energy
     *  test found "VIS" in a Scottie S2 face at 10 dB and cut it short (tools/sstv_bench, 2026-10-05).
     *  The peak test asks for single tones within ±50 Hz frame by frame, which a picture does not make. */
    explicit SstvVIS(double sampleRate, bool byEnergy = true);
    bool byEnergy;
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
    /** ★ By energy (2026-10-05): the last kRing frames' power, bins specLo.., for decideByEnergy. */
    static const int kRing = 52;
    std::vector<float> spec;
    int specLo = 0, specN = 0, frames = 0;
    bool decideByEnergy(uint8_t& modeOut, int& shiftOut, int& startOffMs);
};

// ── Video demodulator ────────────────────────────────────────────────────────
struct SstvPixel { int time, x, y; uint8_t channel; };

class SstvVideo {
public:
    SstvVideo(const SstvMode* mode, double sampleRate, int headerShift, bool adaptive);
    // Demodulate consuming from pcm; lineSender(y, rgb[w*3]) called per line.
    void demodulate(SstvBuffer& pcm, double rate, int skip,
                    const std::function<void(int, const uint8_t*)>& lineSender,
                    const std::atomic<bool>& abort, const std::atomic<bool>* interrupt = nullptr);
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
    /** ★ The sync-train gate (2026-10-05, see demodulate): lines are HELD until the sync train
     *  confirms a picture, `onConfirmed` fires just before they are sent, and `endReason` says why
     *  demodulate returned. gateOnSync=false sends every line at once, as before. */
    bool gateOnSync = true;
    std::function<void()> onConfirmed;
    enum EndReason { EndComplete, EndNoSync, EndSignalLost, EndInterrupted };
    EndReason endReason = EndComplete;
    int syncLinesSeen = 0;
    const std::vector<uint8_t>& syncFlags() const { return hasSync; }
    const std::vector<float>& syncLevels() const { return syncLevel; }

    std::vector<SstvPixel> pixelGrid(double rate, int skip);
private:
    void   detectSync(SstvBuffer& pcm, int targetBin, int idx);
    double estimateSNR(SstvBuffer& pcm);
    double demodFreq(SstvBuffer& pcm, double snr, int ahead = 0);   // ahead: window centre, samples
    int    getBin(double f) const { return (int)(f / sampleRate * fftSize); }
    std::vector<uint8_t> toRGB(const std::vector<uint8_t>& img);

    const SstvMode* m;
    double sampleRate; int headerShift; bool adaptive;
    std::vector<std::vector<double>> hannWins; std::vector<int> hannLens;
    int fftSize = 1024;
    std::vector<float> fin;
    SstvFFT fft;
    std::vector<uint8_t> hasSync;   // 1/0 per sync sample
    /** ★ The same decision as a level: log10(pSync / 2·pRaw), so > 0 is exactly hasSync = 1. Lets
     *  the slant fit place each pulse edge BETWEEN two 13-sample flags (2026-10-04). */
    std::vector<float> syncLevel;
    std::vector<uint8_t> storedLum;
    /** ★★ HOW MUCH OF storedLum WAS ACTUALLY WRITTEN. The buffer is allocated with 1.3x headroom
     *  but only filled while the decode loop runs, and that loop BREAKS EARLY when the audio runs
     *  short. Everything past this index is a zero that was never a sample. */
    int storedLumWritten = 0;
};

// ── Sync corrector ───────────────────────────────────────────────────────────
class SstvSync {
public:
    SstvSync(const SstvMode* mode, double sampleRate, const std::vector<uint8_t>& hasSync,
             const std::vector<float>* syncLevel = nullptr)
        : m(mode), sampleRate(sampleRate), hasSync(hasSync), level(syncLevel) {}
    /** ★ `confOut` (0..1) is how much the sync data supports the answer: the fraction of the
     *  frame's sync lines that agree with the fitted line. Near zero means the "correction" would be
     *  a random shift — see the guard in videoThread. */
    void findSync(double& rateOut, int& skipOut, double* confOut = nullptr);
    /** ★ What the last findSync measured (2026-10-04): the coarse Hough estimate, the line fit's
     *  transmitter clock error and its standard error (ppm, + = the sender runs fast), and whether
     *  that rate passed the gate and was returned — otherwise rateOut is the nominal rate. */
    double houghPpm = 0, fitPpm = 0, fitPpmSE = 0;
    bool rateApplied = false;
private:
    const SstvMode* m; double sampleRate; const std::vector<uint8_t>& hasSync;
    const std::vector<float>* level;
};

// ── Top-level decoder ────────────────────────────────────────────────────────
class SstvDecoder {
public:
    explicit SstvDecoder(double sampleRate, bool autoSync = true, bool adaptive = true);
    ~SstvDecoder();
    void process(const int16_t* mono, int count);   // audio thread
    /** ★ Audio fed but not yet consumed (2026-10-05). For an OFFLINE feeder only — tools/sstv_harness
     *  replays a 90-minute recording far faster than real time and must not run ahead of the video
     *  thread, or the end-of-picture reset throws away the next picture's VIS. */
    int pendingSamples() { return pcm.available(); }

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
    void decodePicture();
    enum State { WaitingVIS, Decoding };
    double sampleRate; bool autoSync, adaptive;
    SstvBuffer pcm;
    SstvVIS* vis = nullptr;   ///< ★ owned by the process() thread ALONE — see visReset
    /** ★★★ The video thread asks for a fresh VIS detector; process() does the delete (audit
     *  2026-10-03). The video thread used to delete `vis` itself AFTER storing WaitingVIS, so the
     *  feeding thread could already be inside vis->process() on the object being freed. */
    std::atomic<bool> visReset{false};
    const SstvMode* mode = nullptr;
    int headerShift = 0;
    std::atomic<State> state{WaitingVIS};
    std::atomic<bool> abort{false};
    std::thread vthread;
    std::vector<int16_t> accum, chunk;
    /** ★★ THE VIS WATCH (2026-10-05): a second detector on its own small ring, run by process()
     *  while a picture decodes. Owned by the process() thread; `watchReset` restarts it per picture.
     *  On a VIS it publishes the next picture under ctlMu and sets `interrupt`; the video thread
     *  ends the current picture and jumps the main ring to `nextStart` (its writtenTotal coords). */
    SstvBuffer visWatchPcm;
    SstvVIS* visWatch = nullptr;
    bool watchReset = true;
    std::mutex ctlMu;
    bool restartPending = false;
    long long nextStart = 0;
    const SstvMode* nextMode = nullptr;
    uint8_t nextModeIdx = 0;
    int nextShift = 0;
    std::atomic<bool> interrupt{false};
    int samps10ms;
    bool statusSent = false;
};

} // namespace vibe
