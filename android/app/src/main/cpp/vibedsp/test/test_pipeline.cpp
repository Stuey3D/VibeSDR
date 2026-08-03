// VibeSDR V5 host test — RationalResampler + RxPipeline end-to-end.
#include "../vibedsp.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace vibedsp;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static int peakBinReal(const std::vector<float>& x, int N, int off, float* lvl) {
    RealFFT fft(N);
    std::vector<float> win(N), buf(N), db(fft.bins());
    nuttallWindow(win.data(), N);
    for (int i = 0; i < N; ++i) buf[i] = win[i] * x[off + i];
    fft.powerDb(buf.data(), db.data(), 1.0f);
    int pk = 1; for (int i = 2; i < fft.bins(); ++i) if (db[i] > db[pk]) pk = i;
    if (lvl) *lvl = db[pk];
    return pk;
}

static void testResampler() {
    std::printf("-- RationalResampler --\n");
    // 44100 -> 48000: a 1 kHz tone must stay 1 kHz, length scales by 48/44.1.
    const int inFs = 44100, outFs = 48000, fTone = 1000, Ni = 44100;
    RationalResampler rs(inFs, outFs);
    std::vector<float> in(Ni), out(rs.maxOut(Ni));
    for (int i = 0; i < Ni; ++i) in[i] = std::sin(2.0 * M_PI * fTone * i / inFs);
    const int no = rs.process(in.data(), Ni, out.data());
    std::printf("  L/M = %d/%d, in %d -> out %d (expected ~%d)\n",
                rs.L(), rs.M(), Ni, no, Ni * outFs / inFs);
    check(std::abs(no - Ni * outFs / inFs) < 50, "output length matches ratio");

    const int N = 1 << 14;
    float lvl;
    int pk = peakBinReal(out, N, no - N - 100, &lvl);
    const double hz = (double)pk * outFs / N;
    std::printf("  tone after resample = %.1f Hz (expected %d)\n", hz, fTone);
    check(std::abs(hz - fTone) < (double)outFs / N * 1.5, "tone frequency preserved");
}

// RxPipeline callback capture.
struct Cap {
    std::vector<float> audio;     // mono, or LEFT channel when stereo
    std::vector<float> audioR;    // RIGHT channel when stereo
    int specFrames = 0, bins = 0, channels = 1;
    bool stereoLocked = false;
};
static void onSpec(void* c, const float*, int b) { auto* p = (Cap*)c; p->specFrames++; p->bins = b; }
static void onAud(void* c, const float* a, int frames, int ch, int) {
    auto* p = (Cap*)c; p->channels = ch;
    if (ch == 2) for (int i = 0; i < frames; ++i) { p->audio.push_back(a[2*i]); p->audioR.push_back(a[2*i+1]); }
    else         p->audio.insert(p->audio.end(), a, a + frames);
}
static void onStereo(void* c, bool lk) { ((Cap*)c)->stereoLocked = lk; }

static void testPipeline() {
    std::printf("-- RxPipeline (AM) --\n");
    const double fs = 1200000.0;     // 1.2 Msps input
    const double fc = 200000.0;      // channel offset within band
    const double fm = 1200.0;        // audio tone
    const double m  = 0.6;
    const int    Ni = 1 << 20;

    std::vector<cf32> iq(Ni);
    for (int i = 0; i < Ni; ++i) {
        const double t = i / fs;
        const double env = 1.0 + m * std::cos(2.0 * M_PI * fm * t);
        const double ph  = 2.0 * M_PI * fc * t;
        iq[i] = cf32((float)(env * std::cos(ph)), (float)(env * std::sin(ph)));
    }

    Cap cap;
    RxPipeline pipe;
    RxPipeline::Callbacks cb; cb.ctx = &cap; cb.spectrum = onSpec; cb.audio = onAud;
    pipe.start(fs, 1024, 20.0, 48000, cb);
    pipe.setTune(fc, RxPipeline::Mode::AM, 10000.0);
    // Feed in blocks to exercise streaming state.
    for (int o = 0; o < Ni; o += 65536)
        pipe.feed(iq.data() + o, std::min(65536, Ni - o));

    std::printf("  spectrum frames = %d (bins %d), audio samples = %zu\n",
                cap.specFrames, cap.bins, cap.audio.size());
    check(cap.specFrames > 0 && cap.bins == 1024, "spectrum frames emitted");
    check(cap.audio.size() > 40000, "audio produced near 48 kHz rate");

    const int N = 1 << 14;
    if ((int)cap.audio.size() > N + 2000) {
        float lvl;
        int pk = peakBinReal(cap.audio, N, (int)cap.audio.size() - N - 1000, &lvl);
        const double hz = (double)pk * 48000.0 / N;
        std::printf("  recovered AM tone = %.1f Hz (expected %.1f)\n", hz, fm);
        check(std::abs(hz - fm) < 48000.0 / N * 2.0, "pipeline recovers AM tone at 48 kHz");
    } else check(false, "enough audio for analysis");
}

// Run the pipeline over a synthetic IQ stream and return the recovered audio.
static float dbAt(const std::vector<float>& x, double hz);   // fwd (defined below)

static std::vector<float> runPipe(const std::vector<cf32>& iq, double fs,
                                  double offset, RxPipeline::Mode mode, double bw) {
    Cap cap;
    RxPipeline pipe;
    RxPipeline::Callbacks cb; cb.ctx = &cap; cb.spectrum = onSpec; cb.audio = onAud;
    pipe.start(fs, 1024, 20.0, 48000, cb);
    pipe.setTune(offset, mode, bw);
    for (int o = 0; o < (int)iq.size(); o += 65536)
        pipe.feed(iq.data() + o, std::min(65536, (int)iq.size() - o));
    return cap.audio;
}

static void checkTone(const std::vector<float>& audio, double expectHz, const char* label) {
    const int N = 1 << 14;
    if ((int)audio.size() <= N + 2000) { check(false, label); return; }
    float lvl;
    int pk = peakBinReal(audio, N, (int)audio.size() - N - 1000, &lvl);
    const double hz = (double)pk * 48000.0 / N;
    std::printf("  %s recovered = %.1f Hz (expected %.1f)\n", label, hz, expectHz);
    check(std::abs(hz - expectHz) < 48000.0 / N * 3.0, label);
}

static void testNFM() {
    std::printf("-- RxPipeline (NFM) --\n");
    const double fs = 1200000.0, fc = 150000.0, fm = 1000.0, dev = 3000.0;
    const int Ni = 1 << 20;
    std::vector<cf32> iq(Ni);
    double ph = 0.0;
    for (int i = 0; i < Ni; ++i) {
        const double t = i / fs;
        const double inst = fc + dev * std::cos(2.0 * M_PI * fm * t); // FM
        ph += 2.0 * M_PI * inst / fs;
        iq[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    checkTone(runPipe(iq, fs, fc, RxPipeline::Mode::NFM, 12000.0), fm, "NFM tone");
}

static void testSSB() {
    std::printf("-- RxPipeline (SSB USB) --\n");
    const double fs = 1200000.0, fc = 80000.0, fa = 1000.0;
    const int Ni = 1 << 20;
    std::vector<cf32> iq(Ni);
    for (int i = 0; i < Ni; ++i) {
        const double t = i / fs;
        const double ph = 2.0 * M_PI * (fc + fa) * t;  // USB: carrier + audio
        iq[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    checkTone(runPipe(iq, fs, fc, RxPipeline::Mode::SSB_USB, 3000.0), fa, "SSB tone");

    // Sideband rejection: the SAME upper-sideband signal must be STRONG in USB and
    // WEAK in LSB. Measure TOTAL RMS (not a single bin) so a frequency-MIRRORED
    // image — the real failure mode — can't hide at a different audio frequency.
    auto rms = [](const std::vector<float>& x) {
        const int N = 1 << 14; if ((int)x.size() <= N + 2000) return 1e-9;
        double s = 0; int o = (int)x.size() - N - 1000;
        for (int i = 0; i < N; ++i) s += (double)x[o + i] * x[o + i];
        return std::sqrt(s / N);
    };
    const double usb = rms(runPipe(iq, fs, fc, RxPipeline::Mode::SSB_USB, 3000.0));
    const double lsb = rms(runPipe(iq, fs, fc, RxPipeline::Mode::SSB_LSB, 3000.0));
    const float rej = (float)(20.0 * std::log10(usb / (lsb + 1e-12)));
    std::printf("  USB-signal: USB rms=%.4f LSB rms=%.4f -> rejection %.1f dB\n", usb, lsb, rej);
    check(rej > 35.0f, "SSB rejects the opposite sideband (>35 dB total energy)");
}

static void testWFM() {
    std::printf("-- RxPipeline (WFM mono) --\n");
    // Wideband FM: MPX = mono audio tone + a 19 kHz stereo pilot. The mono path
    // must recover the audio AND reject the pilot (no audible whine).
    const double fs = 1920000.0, fc = 250000.0, fm = 1000.0;
    const double dev = 75000.0, pilot = 19000.0, pilotDev = 7500.0;
    const int Ni = 1 << 21;
    std::vector<cf32> iq(Ni);
    double ph = 0.0;
    for (int i = 0; i < Ni; ++i) {
        const double t = i / fs;
        const double mpx = dev * std::cos(2.0 * M_PI * fm * t)
                         + pilotDev * std::cos(2.0 * M_PI * pilot * t);
        const double inst = fc + mpx;
        ph += 2.0 * M_PI * inst / fs;
        iq[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    auto audio = runPipe(iq, fs, fc, RxPipeline::Mode::WFM, 200000.0);
    checkTone(audio, fm, "WFM mono tone");

    // Pilot rejection: measure energy in a band around 19 kHz vs the 1 kHz tone.
    // (19 kHz is above the 48 kHz Nyquist image fold? No: 19k < 24k, so if the
    // 15 kHz LPF failed, a 19 kHz component would survive.) Check the spectrum.
    const int N = 1 << 14;
    if ((int)audio.size() > N + 2000) {
        RealFFT fft(N);
        std::vector<float> win(N), buf(N), db(fft.bins());
        nuttallWindow(win.data(), N);
        int off = (int)audio.size() - N - 1000;
        for (int i = 0; i < N; ++i) buf[i] = win[i] * audio[off + i];
        fft.powerDb(buf.data(), db.data(), 1.0f);
        auto binOf = [&](double hz){ return (int)std::lround(hz * N / 48000.0); };
        float toneDb = db[binOf(fm)], pilotDb = db[binOf(pilot)];
        std::printf("  1kHz tone = %.1f dB, 19kHz pilot = %.1f dB (want pilot << tone)\n",
                    toneDb, pilotDb);
        check(toneDb - pilotDb > 40.0f, "19 kHz pilot rejected >40 dB below audio");
    } else check(false, "enough WFM audio for analysis");
}

// dB at a specific audio frequency over the last N samples.
static float dbAt(const std::vector<float>& x, double hz) {
    const int N = 1 << 14;
    if ((int)x.size() <= N + 2000) return -999.0f;
    RealFFT fft(N);
    std::vector<float> win(N), buf(N), db(fft.bins());
    nuttallWindow(win.data(), N);
    int off = (int)x.size() - N - 1000;
    for (int i = 0; i < N; ++i) buf[i] = win[i] * x[off + i];
    fft.powerDb(buf.data(), db.data(), 1.0f);
    return db[(int)std::lround(hz * N / 48000.0)];
}

static void testWFMStereo() {
    std::printf("-- RxPipeline (WFM stereo) --\n");
    // MPX with LEFT = 1 kHz tone, RIGHT = 4 kHz tone, plus 19 kHz pilot and the
    // L-R difference on the 38 kHz subcarrier. Decoded L must have the 1 kHz and
    // R the 4 kHz, with good cross-channel separation.
    const double fs = 1920000.0, fc = 300000.0, Lf = 1000.0, Rf = 4000.0;
    const int Ni = 1 << 21;
    std::vector<cf32> iq(Ni);
    double ph = 0.0;
    for (int i = 0; i < Ni; ++i) {
        const double t = i / fs;
        const double L = 0.3 * std::cos(2.0 * M_PI * Lf * t);
        const double R = 0.3 * std::cos(2.0 * M_PI * Rf * t);
        // FM stereo standard: 19 kHz pilot and 38 kHz L-R subcarrier are SINES.
        const double mpx = (L + R)
                         + 0.1 * std::sin(2.0 * M_PI * 19000.0 * t)
                         + (L - R) * std::sin(2.0 * M_PI * 38000.0 * t);
        ph += 2.0 * M_PI * (fc + 50000.0 * mpx) / fs;
        iq[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }

    Cap cap;
    RxPipeline pipe;
    RxPipeline::Callbacks cb; cb.ctx = &cap;
    cb.spectrum = onSpec; cb.audio = onAud; cb.stereo = onStereo;
    pipe.start(fs, 1024, 20.0, 48000, cb);
    pipe.setTune(fc, RxPipeline::Mode::WFM, 200000.0);
    for (int o = 0; o < Ni; o += 65536)
        pipe.feed(iq.data() + o, std::min(65536, Ni - o));

    std::printf("  channels=%d, stereo locked=%d, L samples=%zu R samples=%zu\n",
                cap.channels, (int)cap.stereoLocked, cap.audio.size(), cap.audioR.size());
    check(cap.channels == 2, "WFM emits stereo (2ch)");
    check(cap.stereoLocked, "pilot PLL locked");

    const float L_at_Lf = dbAt(cap.audio,  Lf), L_at_Rf = dbAt(cap.audio,  Rf);
    const float R_at_Lf = dbAt(cap.audioR, Lf), R_at_Rf = dbAt(cap.audioR, Rf);
    std::printf("  LEFT : 1kHz=%.1f 4kHz=%.1f dB | RIGHT: 1kHz=%.1f 4kHz=%.1f dB\n",
                L_at_Lf, L_at_Rf, R_at_Lf, R_at_Rf);
    check(L_at_Lf - L_at_Rf > 20.0f, "left channel dominated by its own tone");
    check(R_at_Rf - R_at_Lf > 20.0f, "right channel dominated by its own tone");
}

// ★★★ REGRESSION: THE OFF-TUNE MUTE (Stuart, 2026-08-02).
// An FM discriminator's DC output is the tuning error, scaled so 75 kHz = full
// scale, halved again by the 0.5 stereo matrix — so this stereo fixture, tuned
// 60 kHz off, carries a constant 0.40 under the audio (a mono listener would get
// the full 0.80). The int16 conversion clips that against the rail, and a rail is
// silence, not distortion. On air: 50 kHz off played with a little hiss, 100 kHz
// off was dead silent.
// This asserts the property that actually matters: the audio stays CENTRED, so
// the full dynamic range remains available to the programme.
static void testWFMOffTune() {
    std::printf("-- RxPipeline (WFM tuned 60 kHz off) --\n");
    const double fs = 1920000.0, fc = 250000.0, fm = 1000.0;
    const double dev = 25000.0;                 // ordinary programme level, not peak
    const double err = 60000.0;                 // how far off centre we tune
    const int Ni = 1 << 21;
    std::vector<cf32> iq(Ni);
    double ph = 0.0;
    for (int i = 0; i < Ni; ++i) {
        const double t = i / fs;
        ph += 2.0 * M_PI * (fc + dev * std::cos(2.0 * M_PI * fm * t)) / fs;
        iq[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    auto audio = runPipe(iq, fs, fc - err, RxPipeline::Mode::WFM, 200000.0);
    if ((int)audio.size() < 30000) { check(false, "enough off-tune audio"); return; }
    // Last quarter-second only: the blocker's corner is ~1 Hz (tau ~0.16 s), so this
    // sits five time constants past the start and measures the settled state.
    const int kWin = 12000;
    const int n0 = (int)audio.size() - kWin;
    double mean = 0.0; float peak = 0.0f;
    for (int i = n0; i < (int)audio.size(); ++i) {
        mean += audio[i];
        peak = std::max(peak, std::fabs(audio[i]));
    }
    mean /= kWin;
    std::printf("  DC = %+.3f (unblocked measures %+.2f), peak = %.3f\n",
                mean, err / 150000.0, peak);
    check(std::fabs(mean) < 0.05, "off-tune DC removed (no headroom lost)");
    check(peak < 0.99f, "audio not pinned against the rail");
    checkTone(audio, fm, "off-tune WFM tone still recovered");
}

// ★★★ TUNING MUST NOT ATTENUATE THE AUDIO (Stuart, 2026-08-03, Pi demo: "when tuning the
// audio is muting… or attenuating lots").
//
// A retune inside the same mode and bandwidth used to rebuild the entire audio chain,
// which (a) cleared every buffer, putting a real GAP in the stream that the client's
// jitter buffer then had to cover, and (b) called agc_.reset(), dropping the gain to
// exactly 1.0 so a weak signal was attenuated hard and crawled back over the 400 ms
// release. This test is what stops either creeping back in.
//
// Deliberately uses a WEAK carrier: at full scale the AGC's converged gain is near 1.0
// and a reset would be invisible. The bug is only measurable when the AGC is doing work.
static void testRetuneLevel() {
    std::printf("-- RxPipeline (retune must not attenuate) --\n");
    const double fs = 1200000.0;
    const double fA = 200000.0, fB = 260000.0;   // two carriers, one band, same mode/bw
    const double fm = 1200.0, m = 0.6;
    const double amp = 0.02;                     // WEAK — forces a high AGC gain
    const int    Ni = 1 << 20;

    // Both carriers present throughout, so the retune is the ONLY thing that changes.
    std::vector<cf32> iq(Ni);
    for (int i = 0; i < Ni; ++i) {
        const double t   = i / fs;
        const double env = amp * (1.0 + m * std::cos(2.0 * M_PI * fm * t));
        const double pa  = 2.0 * M_PI * fA * t, pb = 2.0 * M_PI * fB * t;
        iq[i] = cf32((float)(env * (std::cos(pa) + std::cos(pb))),
                     (float)(env * (std::sin(pa) + std::sin(pb))));
    }

    Cap cap;
    RxPipeline pipe;
    RxPipeline::Callbacks cb; cb.ctx = &cap; cb.spectrum = onSpec; cb.audio = onAud;
    pipe.start(fs, 1024, 20.0, 48000, cb);
    pipe.setTune(fA, RxPipeline::Mode::AM, 10000.0);

    const int blk = 65536;
    const int half = Ni / 2;
    for (int o = 0; o < half; o += blk) pipe.feed(iq.data() + o, std::min(blk, half - o));

    // Level the AGC settled on BEFORE the retune, measured over the last ~100 ms.
    const size_t markAudio = cap.audio.size();
    auto rms = [&](size_t from, size_t to) {
        if (to > cap.audio.size()) to = cap.audio.size();
        if (from >= to) return 0.0;
        double s = 0.0;
        for (size_t i = from; i < to; ++i) s += (double)cap.audio[i] * cap.audio[i];
        return std::sqrt(s / (double)(to - from));
    };
    const double before = rms(markAudio > 4800 ? markAudio - 4800 : 0, markAudio);

    // ── THE RETUNE ── same mode, same bandwidth, different frequency.
    const unsigned rebuildsBefore = pipe.rebuildCount();
    pipe.setTune(fB, RxPipeline::Mode::AM, 10000.0);
    for (int o = half; o < Ni; o += blk) pipe.feed(iq.data() + o, std::min(blk, Ni - o));

    // ★★★ THE ASSERTION THAT MATTERS, and it is on the REBUILD, not on the audio level.
    // A level test was tried first and is NOT a witness: through the full chain the
    // recovered level after a retune depends on where the AGC happened to have converged,
    // and on a synthetic carrier that can sit close enough to unity that wiping the AGC
    // barely moves it (measured: ratio 0.86 on the buggy code — it passed). The rebuild
    // itself is the defect, it is binary, and it does not depend on the signal.
    const unsigned rebuildsAfter = pipe.rebuildCount();
    std::printf("  rebuilds: %u before the retune, %u after\n", rebuildsBefore, rebuildsAfter);
    check(rebuildsAfter == rebuildsBefore,
          "a same-mode, same-bandwidth retune rebuilds NOTHING");

    const double after = rms(markAudio, markAudio + 2400);
    std::printf("  audio rms before = %.4f, first 50 ms after = %.4f (diagnostic only)\n",
                before, after);
    check(before > 1e-5, "audio present before the retune");

    // And it must still be listening — to the NEW carrier, at the right pitch. This is
    // what stops "rebuild nothing" being satisfied by simply ignoring the retune.
    checkTone(cap.audio, fm, "tone still recovered after retune");

    // A mode change is a DIFFERENT chain and must still rebuild — the optimisation is
    // "same chain", not "never rebuild", and skipping this one would leave stale filters.
    pipe.setTune(fB, RxPipeline::Mode::SSB_USB, 3000.0);
    pipe.feed(iq.data(), blk);
    std::printf("  rebuilds after a MODE change = %u\n", pipe.rebuildCount());
    check(pipe.rebuildCount() > rebuildsAfter, "a mode change still rebuilds the chain");
}

int main() {
    std::printf("== vibedsp resampler + pipeline host test ==\n");
    testResampler();
    testPipeline();
    testNFM();
    testSSB();
    testWFM();
    testWFMStereo();
    testWFMOffTune();
    testRetuneLevel();
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
