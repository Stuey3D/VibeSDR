// ★★★ THE SSB MISTUNE ESTIMATOR ON REAL, OFF-AIR AUDIO (V12, 2026-10-17) — the calibration and the proof.
//
// vibedsp/ssb_offset.h estimates how far an SSB voice is mistuned, in two stages: WHERE the voice's energy sits in the
// passband (Stuart: on LSB, tuned below "deep and a bit robot sounding", above "super thin") and the harmonic comb.
// test-ssb-offset proves the arithmetic on synthetic voices; this one measures it on the air, and its gates were
// CHOSEN here — so it keeps a held-out set to show they were not fitted to the answers:
//
//   1. VOLMET at ELEVEN DIALS AT ONCE — ssb/offsets/<station>/<carrier>_usb_dial<OFF>.wav, 12 kHz, 120 s each, the
//      same broadcast recorded at dial = carrier + OFF for OFF in 0, ±50, ±100, ±200, ±500, ±1000. USB: the dial
//      above the carrier puts the audio LOW, so the truth (+d = audio d Hz HIGH) is −OFF. Exact, whoever is speaking.
//        TRAIN    Shannon 5505, RAF 5450, Shannon 3413 — the gates were chosen on these;
//        HELD OUT Shannon 8957 — never looked at while choosing;
//        WEAK     RAF 11253 (4-5 dB) — unusable: nothing may pass the gate there with a wrong answer.
//      A clip is USABLE when its whole-clip speech-to-gap ratio is ≥ 10 dB (the deep −1000 dials of the weaker
//      stations are not: the voice's strongest harmonics are below 0 Hz there).
//   2. QSOs FROM BUSY-BAND I/Q (held out too) — iq/uber_<band>_<centre>_iq192.wav (192 kHz int16, I left: the
//      Shannon reference iq/uber_volmet-ref_5505000_iq192.wav reads USB above the centre, so I + jQ is the right way
//      up) and iq/v4l_40m_7150000_2400k.cu8 (2.4 MS/s uint8 I/Q, another receiver). Demodulated HERE (shift, a
//      complex 50-2700 Hz passband, 12 kHz) at dial offsets 0, ±100, ±200, ±500, ±1000 from each station. The truth
//      is the station's own offset at the nominal dial (BASE, measured once with SLOW over the whole 120 s and
//      checked against where the voice's band ends in the spectrum — see kQsos) plus the dial offset, exactly.
//      ★ ±1000 is REPORTED, not judged: a busy band has neighbours there (at 7190.0 kHz another LSB voice sits on tune).
//   3. TWO STATIONS IN ONE PASSBAND — a deep 80 m voice on tune with a 20 m voice mixed in 600 Hz up, 6 dB down;
//      and two real QSOs where the other station is ~100 Hz away. Every believed answer must belong to a voice.
//   4. STUART'S OWN RECORDING (optional: VIBE_SSB_OWNER=<.m4a or .wav>, decoded by ffmpeg if it is on PATH) — on
//      frequency, below 500 / 1000 Hz, back on, above 500 / 1000 Hz, timestamps unknown: segmented and reported.
//
// ★ THE AUDIO IS NOT IN THE REPO (~1 GB). Point VIBE_TEST_AUDIO at the folder holding ssb/ and iq/; without it this
//   suite is NOT RUN (exit 3), never a pass. ★ SILENT: audio is read and measured, never played.
// ★ Judged (the exit status): on usable VOLMET, every |OFF| ≥ 100 answered with the right sign; FAST within 25 Hz for
//   |OFF| ≤ 200 and 60 Hz for ±500; on tune, no believed FAST answer outside ±50 Hz once settled; NO believed
//   wrong-sign answer on any VOLMET clip or any QSO within ±500 Hz. ±1000 Hz on VOLMET is the stated target (60 Hz)
//   and is reported against it, met or not — see the summary.
// ★★ §5, STEADINESS (2026-10-17, Stuart on 479: "it still flaps about a bit too") — the pan and the image through the
//   whole binaural chain at the app's scale: on tune, no lean; held off tune, the pan reverses ≤ 1 a minute, sd ≤ 0.05,
//   never the wrong way; tuned away, half the lean within 0.5 s; at ±500/±1000 the image's level difference holds
//   still (sd ≤ 0.8 dB a group). VIBE_SSB_REAL_ONLY=steady runs §5 alone. ★ The sd figures are ROBUST since 2026-10-18
//   (1.4826 × MAD: the wobble around where the pan sits; one corrected first estimate is not a flap — the reversals are).
// ★★ §6, ARRIVAL (2026-10-18, Stuart on 482: "I'm deliberately waiting off tune and it's not panning") — cold, held off tune
//   at ±100…±1000 Hz from three points in each clip, with and without the waterfall cue: never the wrong way, never on
//   tune; ±200…±500 audibly leaning (|pan| ≥ 0.15) on ≥ 95 % of runs, median ≤ 9 s, 80 % ≤ 18 s; with the cue (±500 and
//   ±1000) ≤ 1.5 s.
//   VIBE_SSB_REAL_ONLY=arrival runs §6 alone (VIBE_ARRIVE_VERBOSE=1 every run, VIBE_ARRIVE_CSV=<file> every estimate).
#include "vibedsp/ssb_offset.h"
#include "vibedsp/binaural.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

using namespace vibedsp;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("   %s   %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++;
}
static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

// ── reading ─────────────────────────────────────────────────────────────────────────────────────────────────────

// 16-bit PCM WAV. Mono → as is; stereo → interleaved (2 per frame). Reads up to `secs` from `fromSec`.
static bool readWav(const std::string& path, std::vector<int16_t>& s, int& rate, int& ch, double fromSec = 0, double secs = 1e9) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char hdr[12];
    if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) { std::fclose(f); return false; }
    int bits = 0; ch = 0; rate = 0;
    for (;;) {
        char id[4]; uint32_t len;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&len, 4, 1, f) != 1) { std::fclose(f); return false; }
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<uint8_t> b(len);
            if (std::fread(b.data(), 1, len, f) != len) { std::fclose(f); return false; }
            ch = b[2] | (b[3] << 8); rate = (int)(b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24)); bits = b[14] | (b[15] << 8);
        } else if (!std::memcmp(id, "data", 4)) {
            if (bits != 16 || ch < 1 || ch > 2) { std::fclose(f); return false; }
            const size_t frames = len / (2u * ch), a = std::min(frames, (size_t)(fromSec * rate));
            const size_t m = std::min(frames - a, (size_t)(secs * rate));
            std::fseek(f, (long)(a * 2 * ch), SEEK_CUR);
            s.resize(m * ch);
            const size_t got = std::fread(s.data(), 2 * ch, m, f);
            s.resize(got * ch);
            std::fclose(f);
            return got > 0;
        } else {
            std::fseek(f, len + (len & 1), SEEK_CUR);
        }
    }
}
static bool readMono(const std::string& path, std::vector<float>& x, int& rate) {
    std::vector<int16_t> s; int ch;
    if (!readWav(path, s, rate, ch) || ch != 1) return false;
    x.resize(s.size());
    for (size_t i = 0; i < s.size(); ++i) x[i] = s[i] / 32768.0f;
    return true;
}

// ── the offline SSB receiver ────────────────────────────────────────────────────────────────────────────────────
// ★ What a receiver does, written plainly (it is a test instrument, not the product's DSP): mix the station to 0 Hz,
//   decimate to 12 kHz complex in two FIR stages, then for each dial offset shift, keep the 50-2700 Hz sideband with a
//   complex band-pass (401 taps at 12 kHz: ~150 Hz skirts, like a real filter's) and take the real part.

using cf = std::complex<float>;

static std::vector<float> lowpass(double fc, double fs, int n) {   // windowed sinc (Blackman), unity gain, odd n
    std::vector<float> h(n);
    const int m = n / 2;
    double s = 0;
    for (int i = 0; i < n; ++i) {
        const double t = i - m, x = 2 * fc / fs;
        const double sinc = t == 0 ? x : std::sin(M_PI * x * t) / (M_PI * t);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) + 0.08 * std::cos(4 * M_PI * i / (n - 1));
        h[i] = (float)(sinc * w); s += h[i];
    }
    for (auto& v : h) v = (float)(v / s);
    return h;
}

// A streaming FIR decimator for complex samples.
struct Decimator {
    std::vector<float> h; int D; std::vector<cf> hist; size_t phase = 0;
    Decimator(double fc, double fs, int D_, int taps) : h(lowpass(fc, fs, taps)), D(D_) {}
    void push(const cf* x, size_t n, std::vector<cf>& out) {
        hist.insert(hist.end(), x, x + n);
        const size_t L = h.size();
        size_t i = phase;
        for (; i + L <= hist.size(); i += (size_t)D) {
            cf acc(0, 0);
            const cf* p = &hist[i];
            for (size_t k = 0; k < L; ++k) acc += p[k] * h[k];
            out.push_back(acc);
        }
        const size_t drop = std::min(i, hist.size());
        hist.erase(hist.begin(), hist.begin() + (long)drop);
        phase = i - drop;
    }
};

// A phase-continuous complex oscillator (renormalised, so 100 M samples do not drift in level).
struct Rotator {
    cf w, z{1, 0}; long n = 0;
    explicit Rotator(double hz, double fs) : w((float)std::cos(2 * M_PI * hz / fs), (float)std::sin(2 * M_PI * hz / fs)) {}
    cf next() { const cf r = z; z *= w; if ((++n & 1023) == 0) z /= std::abs(z); return r; }
};

// I/Q (any source) → the station at 0 Hz, 12 kHz complex. `get(i)` yields input sample i; `n` samples at `fs`.
template <class Get>
static std::vector<cf> baseband(Get get, size_t n, double fs, double relHz, int D1, int D2) {
    Rotator lo(-relHz, fs);
    const double fs1 = fs / D1;
    Decimator d1(std::min(0.4 * fs1, 20000.0), fs, D1, D1 * 8 + 1);
    Decimator d2(5000.0, fs1, D2, D2 * 24 + 1);
    std::vector<cf> chunk, s1, s2;
    const size_t C = 1 << 16;
    for (size_t a = 0; a < n; a += C) {
        const size_t m = std::min(C, n - a);
        chunk.resize(m);
        for (size_t i = 0; i < m; ++i) chunk[i] = get(a + i) * lo.next();
        s1.clear();
        d1.push(chunk.data(), m, s1);
        d2.push(s1.data(), s1.size(), s2);
    }
    return s2;
}

// 12 kHz complex baseband (carrier at 0) → SSB audio with the DIAL `dialOffHz` from the carrier. sb +1 USB, −1 LSB.
static std::vector<float> ssbAudio(const std::vector<cf>& bb, int sb, double dialOffHz) {
    const double fs = 12000, mid = 1375;
    static const std::vector<float> h = lowpass(1325, fs, 401);
    Rotator down(-dialOffHz - sb * mid, fs), up(sb * mid, fs);
    std::vector<cf> y(bb.size());
    for (size_t i = 0; i < bb.size(); ++i) y[i] = bb[i] * down.next();
    const size_t L = h.size();
    std::vector<float> out;
    out.reserve(bb.size());
    for (size_t o = 0; o + L <= y.size(); ++o) {
        cf acc(0, 0);
        const cf* p = &y[o];
        for (size_t k = 0; k < L; ++k) acc += p[k] * h[k];
        out.push_back((acc * up.next()).real());
    }
    double e = 0;
    for (float v : out) e += (double)v * v;
    const float g = (float)(0.1 / std::sqrt(e / std::max<size_t>(1, out.size()) + 1e-30));   // like an AGC: −20 dBFS RMS
    for (auto& v : out) v *= g;
    return out;
}

// ── running the estimator ───────────────────────────────────────────────────────────────────────────────────────

constexpr double kSettleSec = 10;       // FAST "after its settling window": answers from 10 s in are judged
constexpr int kPkt = 240;               // 20 ms packets at 12 kHz, as the audio thread hands them over

static float tolFor(float truth) { return std::fabs(truth) >= 450 ? 60.0f : 25.0f; }
static bool wrongSign(float answer, float truth) { return std::fabs(truth) >= 100 && std::fabs(answer) > 50 && answer * truth < 0; }

struct FastRun {
    int answers = 0, settled = 0, settledOk = 0;    // every answer; those from kSettleSec on; …within tolerance
    int passed = 0, passedOk = 0, passedWrong = 0, passedWrongSign = 0, passedOutside50 = 0;
    float settledMedian = NAN;                      // median of the BELIEVED answers from kSettleSec on
    float binaural = NAN; bool binauralValid = false;   // what BinauralTuning publishes at the end (signal Hz)
    double binauralOkFrac = 0;                      // …and how much of the settled time it was within tolerance
    double cpu = 0;
};
struct SlowRun { int windows = 0, passed = 0, passedOk = 0, passedWrong = 0, passedWrongSign = 0; float median = NAN; double cpu = 0; };

static float median(std::vector<float> v) {
    if (v.empty()) return NAN;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : 0.5f * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}

static FastRun runFast(const std::vector<float>& x, int rate, float truth, int sb, bool withBinaural) {
    FastRun r;
    SsbOffsetEstimator est(rate, SsbOffsetEstimator::Profile::Fast);
    std::vector<float> believed;
    const int pkt = rate / 50;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i + pkt <= x.size(); i += pkt) {
        if (!est.push(&x[i], pkt)) continue;
        const SsbOffsetEstimate& e = est.last();
        if (!e.valid) continue;
        const double t = (double)(i + pkt) / rate;
        const bool ok = std::fabs(e.offsetHz - truth) <= tolFor(truth);
        ++r.answers;
        if (t >= kSettleSec) { ++r.settled; r.settledOk += ok; }
        if (e.passes()) {
            ++r.passed; r.passedOk += ok; r.passedWrong += !ok;
            r.passedWrongSign += wrongSign(e.offsetHz, truth);
            if (t >= kSettleSec) {
                believed.push_back(e.offsetHz);
                if (std::fabs(e.offsetHz) > 50) ++r.passedOutside50;
            }
        }
    }
    r.cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.settledMedian = median(believed);
    if (withBinaural) {
        // ★ THE BINAURAL AID END TO END: the real controller (median, hysteresis, high-voice rules), fed like the audio
        //   thread feeds it and pumped like its worker. It publishes the SIGNAL's offset from the dial: USB = +audio.
        BinauralTuning bt(false);
        bt.setEnabled(true);
        bt.setSource(sb > 0 ? BinauralTuning::SsbUsb : BinauralTuning::SsbLsb, 50, 1000);
        bt.setDial(7100000);
        std::vector<float> l(pkt), rr(pkt);
        const float want = sb * truth;
        int n = 0, good = 0;
        for (size_t i = 0; i + pkt <= x.size(); i += pkt) {
            std::copy(x.begin() + i, x.begin() + i + pkt, l.begin());
            std::copy(x.begin() + i, x.begin() + i + pkt, rr.begin());
            bt.processPlanar(l.data(), rr.data(), pkt, rate);
            if ((i / pkt) % 2 == 1) bt.pumpSsbForTest();
            if ((double)(i + pkt) / rate >= kSettleSec) {
                float h = 0;
                const bool v = bt.ssbOffset(h);
                ++n; good += v && std::fabs(h - want) <= tolFor(truth);
            }
        }
        r.binauralValid = bt.ssbOffset(r.binaural);
        r.binauralOkFrac = n ? (double)good / n : 0;
    }
    return r;
}

static SlowRun runSlow(const std::vector<float>& x, int rate, float truth) {
    SlowRun r;
    SsbOffsetEstimator est(rate, SsbOffsetEstimator::Profile::Slow);
    std::vector<float> passedV;
    const int pkt = rate / 50;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i + pkt <= x.size(); i += pkt) {
        if (!est.push(&x[i], pkt)) continue;
        const SsbOffsetEstimate& e = est.last();
        if (!e.valid) continue;
        ++r.windows;
        if (e.passes()) {
            const bool ok = std::fabs(e.offsetHz - truth) <= tolFor(truth);
            ++r.passed; r.passedOk += ok; r.passedWrong += !ok; r.passedWrongSign += wrongSign(e.offsetHz, truth);
            passedV.push_back(e.offsetHz);
        }
    }
    r.cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.median = median(passedV);
    return r;
}

// ── the tables ──────────────────────────────────────────────────────────────────────────────────────────────────

struct Tally {
    int clips = 0, fAns = 0, fSettled = 0, fSettledOk = 0, fPassed = 0, fPassedOk = 0, fWrong = 0, fWrongSign = 0;
    int sWin = 0, sPassed = 0, sPassedOk = 0, sWrong = 0, sWrongSign = 0;
    double binOk = 0; int binN = 0;
    void add(const FastRun& f, const SlowRun& s) {
        ++clips; fAns += f.answers; fSettled += f.settled; fSettledOk += f.settledOk; fPassed += f.passed; fPassedOk += f.passedOk;
        fWrong += f.passedWrong; fWrongSign += f.passedWrongSign;
        sWin += s.windows; sPassed += s.passed; sPassedOk += s.passedOk; sWrong += s.passedWrong; sWrongSign += s.passedWrongSign;
        if (!std::isnan(f.binaural) || f.binauralOkFrac > 0) { binOk += f.binauralOkFrac; ++binN; }
    }
    void print(const char* label) const {
        auto pc = [](int a, int b) { return b ? 100.0 * a / b : 0.0; };
        std::printf("   %-24s %3d clips | FAST settled %5.1f %% right · believed %5.1f %% (%d), %d wrong, %d wrong-sign"
                    " · binaural %5.1f %% of the time | SLOW believed %d/%d windows, %d wrong, %d wrong-sign\n",
                    label, clips, pc(fSettledOk, fSettled), pc(fPassed, fAns), fPassed, fWrong, fWrongSign,
                    binN ? 100.0 * binOk / binN : 0.0, sPassed, sWin, sWrong, sWrongSign);
    }
};

static void showRow(const char* name, float truth, float snr, bool usable, const FastRun& f, const SlowRun& s) {
    std::printf("   %-26s %+6.0f %4.0f dB%s | FAST %3d ans, settled %3.0f %% ok, believed %3d (%d wrong%s) median %+7.1f | "
                "bin %s%+7.1f (%3.0f %%) | SLOW %d/%d median %+7.1f\n",
                name, truth, snr, usable ? " " : "*", f.answers, f.settled ? 100.0 * f.settledOk / f.settled : 0.0, f.passed,
                f.passedWrong, f.passedWrongSign ? ", WRONG SIGN" : "", f.settledMedian,
                f.binauralValid ? "" : "(none)", f.binauralValid ? f.binaural : 0.0f, 100 * f.binauralOkFrac,
                s.passed, s.windows, s.median);
}

// ── the QSOs ────────────────────────────────────────────────────────────────────────────────────────────────────

struct Qso {
    const char* file; double centreHz, carrierHz; int sb; float base; const char* note;
};
/* ★ BASE = the station's own audio offset at the nominal dial, measured ONCE with SLOW over the whole 120 s at that
 *   dial, and cross-checked against the spectrum: an LSB voice's band ends 100-300 Hz below its carrier, a USB one's
 *   starts 100-300 Hz above. (7.1750: +12 Hz — the band ends 100-200 Hz below; 14.2820: −45 Hz, the band starts
 *   200 Hz up.) Everything else is exact: dial +Δ moves the audio −Δ on USB and +Δ on LSB. */
static const Qso kQsos[] = {
    {"uber_40m_7150000_iq192.wav", 7150000, 7087000, -1, -2, "40 m LSB"},
    {"uber_40m_7150000_iq192.wav", 7150000, 7127500, -1, -1, "40 m LSB"},
    {"uber_40m_7150000_iq192.wav", 7150000, 7175000, -1, 12, "40 m LSB"},
    {"uber_40m_7150000_iq192.wav", 7150000, 7191000, -1, 6, "40 m LSB (a neighbour on 7190.0)"},
    {"uber_80m_3700000_iq192.wav", 3700000, 3720000, -1, 0, "80 m LSB, deep voices (F0 ~95-130 Hz)"},
    {"uber_20m_14250000_iq192.wav", 14250000, 14216000, +1, -8, "20 m USB"},
    {"uber_20m_14250000_iq192.wav", 14250000, 14282000, +1, -45, "20 m USB"},
    {"uber_20m_14250000_iq192.wav", 14250000, 14224000, +1, -3, "20 m USB"},
    {"v4l_40m_7150000_2400k.cu8", 7150000, 7087000, -1, -2, "40 m LSB, the V4L dongle (8-bit, 30 s)"},
};

static std::vector<cf> qsoBaseband(const std::string& dir, const Qso& q, double secs) {
    const std::string path = dir + "/" + q.file;
    if (std::strstr(q.file, ".cu8")) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return {};
        const double fs = 2.4e6;
        std::vector<uint8_t> raw((size_t)(fs * std::min(secs, 30.0)) * 2);
        raw.resize(std::fread(raw.data(), 1, raw.size(), f) & ~(size_t)1);
        std::fclose(f);
        auto get = [&](size_t i) { return cf((raw[2 * i] - 127.5f) / 128.0f, (raw[2 * i + 1] - 127.5f) / 128.0f); };
        return baseband(get, raw.size() / 2, fs, q.carrierHz - q.centreHz, 25, 8);
    }
    std::vector<int16_t> s; int rate = 0, ch = 0;
    if (!readWav(path, s, rate, ch, 0, secs) || ch != 2) return {};
    auto get = [&](size_t i) { return cf(s[2 * i] / 32768.0f, s[2 * i + 1] / 32768.0f); };
    return baseband(get, s.size() / 2, rate, q.carrierHz - q.centreHz, 4, rate / 4 / 12000);
}

// ── 5. the lean's STEADINESS through the whole binaural chain ───────────────────────────────────────────────────────
/* ★★★ Stuart on 479 (2026-10-17): "it still flaps about a bit too". The estimator rows above judge the NUMBER; this
 *     judges what the ear gets — the pan BinauralTuning actually applies (stats[4], after its glide), every 20 ms
 *     packet, through the real controller with the app's SSB scale (dead ±50 Hz, full at 1 kHz: binauralOffset.ts).
 *     Per clip, over the held stretch:
 *       travel / min   — Σ|Δpan| per minute: everything the image moved, glides included;
 *       reversals / min — direction changes of the pan that swing at least kRevSwing each way (a flap);
 *       sd             — the pan's standard deviation while the dial is held still;
 *       first lean     — after a dial move, how long until half the expected lean (it must stay prompt).
 *     Two ways to be off tune: TUNED AWAY (on tune for kAwaySec, then the dial moves and the clip switches to the
 *     recording at the new dial at the same instant — VOLMET's eleven dials were recorded at once) and ARRIVED (off
 *     tune from the first sample, no dial move: only the estimator, and the waterfall cue the app would add, can
 *     lean it — the cue is not simulated here). */
constexpr float kSteadyDead = 50;                        // = BIN_DEAD_SSB_HZ (binauralOffset.ts)
// = BIN_FULL_SSB_HZ. ★ VIBE_STEADY_FULL overrides it for a comparison (300 = build 479's scale); the checks are judged
//   at the app's scale only.
static float kSteadyFull = 1000;
constexpr float kRevSwing = 0.02f;                       // a reversal must swing ≥ 2 % of full lean each way
constexpr double kAwaySec = 30;                          // TUNED AWAY: on tune this long, then the dial moves
constexpr double kHeldFromSec = 15;                      // ARRIVED / ON TUNE: judged from here (FAST's settling)
constexpr double kAfterMoveSec = 3;                      // TUNED AWAY: judged from this long after the move

struct Steady {
    double travel = 0, revs = 0, sd = 0, mean = 0, maxAbs = 0, sdRaw = 0;
    double firstLean = -1;   // TUNED AWAY: s from the move; ARRIVED: s from the start — to half the expected lean
    double leanFrac = 0;     // share of the held stretch with |pan| > kRevSwing (on tune: any lean at all)
    double wrongFrac = 0;    // share leaning the WRONG way by more than kRevSwing (on tune: any way)
    double hzSd = 0, hzRevs = 0;   // the published answer itself (Hz) — the pan without the scale: sd, and reversals
                                   // of ≥ kHzSwing per minute, while it is valid
    // ★★ THE IMAGE AS THE EAR GETS IT: the level difference between the ears (dB, + = louder RIGHT) of the OUTPUT, per
    //    100 ms of speech (quieter blocks are not judged). It carries the lean AND the spread — and the spread places
    //    each audio frequency by itself, so a voice whose energy moves between its low harmonics and its sibilants
    //    moves the image with every syllable, by up to the spread's weight × kMaxDropDb. sd, and reversals of
    //    ≥ kIldSwing per minute.
    double ildMean = 0, ildSd = 0, ildRevs = 0, ildSdRaw = 0;
    float expect = 0;
    bool away = false;
};
constexpr float kHzSwing = 15;
constexpr float kIldSwing = 1.5f;   // dB

static Steady runSteady(const std::vector<float>* pre, const std::vector<float>& x, int rate, int sb, double dialOffHz,
                        float signalHz) {
    Steady s;
    s.away = pre != nullptr;
    s.expect = binauralPanForSsb(signalHz, kSteadyDead, kSteadyFull);
    BinauralTuning bt(false);
    bt.setEnabled(true);
    bt.setSource(sb > 0 ? BinauralTuning::SsbUsb : BinauralTuning::SsbLsb, kSteadyDead, kSteadyFull);
    bt.setSpread(sb > 0 ? BinauralSpread::Usb : BinauralSpread::Lsb, 50, 2700);
    const double dial0 = 7100000;
    bt.setDial(pre ? dial0 : dial0 + dialOffHz);
    const int pkt = rate / 50;
    const size_t switchAt = pre ? (size_t)(kAwaySec * rate) / pkt * pkt : 0;
    std::vector<float> l(pkt), r(pkt);
    struct Pt { double t; float pan, hz; bool valid; };
    std::vector<Pt> pts;
    struct Blk { double t, e, ild; };
    std::vector<Blk> blks;
    double bL = 0, bR = 0; int bN = 0;
    const bool noSpread = std::getenv("VIBE_STEADY_NOSPREAD") != nullptr;   // measurement aid: the lean alone
    if (noSpread) bt.setSpread(BinauralSpread::Off, 50, 2700);
    const size_t n = pre ? std::min(pre->size(), x.size()) : x.size();
    const char* trace = std::getenv("VIBE_STEADY_TRACE");   // a debugging aid: print the controller every 0.5 s
    for (size_t i = 0; i + pkt <= n; i += pkt) {
        if (pre && i == switchAt) bt.setDial(dial0 + dialOffHz);
        const std::vector<float>& src = (pre && i < switchAt) ? *pre : x;
        std::copy(src.begin() + i, src.begin() + i + pkt, l.begin());
        std::copy(src.begin() + i, src.begin() + i + pkt, r.begin());
        bt.processPlanar(l.data(), r.data(), pkt, rate);
        if ((i / pkt) % 2 == 1) bt.pumpSsbForTest();
        float st[12]; bt.stats(st, 12);
        const double t = (double)(i + pkt) / rate;
        pts.push_back({t, st[4], st[2], st[3] > 0});
        for (int k = 0; k < pkt; ++k) { bL += (double)l[k] * l[k]; bR += (double)r[k] * r[k]; }
        if (++bN == 5) {
            blks.push_back({t, bL + bR, 10 * std::log10((bR + 1e-20) / (bL + 1e-20))});
            bL = bR = 0; bN = 0;
        }
        if (trace && (i / pkt) % 25 == 0) std::printf("      t %6.1f pan %+.3f hz %+7.1f %s\n", t, st[4], st[2], st[3] > 0 ? "" : "(none)");
        const double since = pre ? t - kAwaySec : t;
        if ((!pre || i >= switchAt) && s.firstLean < 0 && std::fabs(s.expect) > 0.04f && st[4] * s.expect >= 0.5f * s.expect * s.expect)
            s.firstLean = since;
    }
    // ★ The held stretch: after the move (TUNED AWAY), or once FAST has settled — and, ARRIVED off tune, once it has
    //   leant at all (how long that takes is the first-lean figure, not a flap).
    double from = pre ? kAwaySec + kAfterMoveSec : kHeldFromSec;
    if (!pre && s.firstLean > 0) from = std::max(from, s.firstLean + kAfterMoveSec);
    std::vector<Pt> w;
    for (const Pt& p : pts) if (p.t >= from) w.push_back(p);
    if (w.size() < 2) return s;
    const double mins = w.size() * (double)pkt / rate / 60.0;
    auto reversals = [](const std::vector<double>& v, double swing) {
        double ext = v.empty() ? 0 : v[0]; int dir = 0, revs = 0;
        for (double p : v) {
            if (dir == 0) { if (p - ext >= swing) { dir = 1; ext = p; } else if (ext - p >= swing) { dir = -1; ext = p; } }
            else if (dir > 0) { if (p > ext) ext = p; else if (ext - p >= swing) { ++revs; dir = -1; ext = p; } }
            else { if (p < ext) ext = p; else if (p - ext >= swing) { ++revs; dir = 1; ext = p; } }
        }
        return revs;
    };
    double sum = 0, sum2 = 0; int lean = 0, wrong = 0;
    std::vector<double> pans, hz;
    for (size_t i = 0; i < w.size(); ++i) {
        const double p = w[i].pan;
        pans.push_back(p);
        if (w[i].valid) hz.push_back(w[i].hz);
        sum += p; sum2 += p * p;
        s.maxAbs = std::max(s.maxAbs, std::fabs(p));
        if (std::fabs(p) > kRevSwing) { ++lean; if (s.expect == 0 || p * s.expect < 0) ++wrong; }
        if (i) s.travel += std::fabs(p - w[i - 1].pan);
    }
    const double m = (double)w.size();
    s.mean = sum / m;
    s.sdRaw = std::sqrt(std::max(0.0, sum2 / m - s.mean * s.mean));
    {
        // ★ sd is ROBUST (1.4826 × the median absolute deviation, 2026-10-18): it measures the wobble around where the
        //   pan SITS, and ignores a minority stretch elsewhere. A first estimate that is later corrected (one step: the
        //   arrival vote's −588 Hz becoming −984 Hz after 94 s), or a QSO's other station taking over for 30 s, is not a
        //   flap — and a flap is what the REVERSALS count, unchanged. The plain sd is still printed (sd raw). Until the SSB
        //   curve (binauralPanForSsb) made ±100-300 Hz audible, those steps were 0.06 of a lean and the two agreed.
        std::vector<double> d(pans);
        std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
        const double med = d[d.size() / 2];
        for (double& v : d) v = std::fabs(v - med);
        std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
        s.sd = 1.4826 * d[d.size() / 2];
    }
    s.travel /= mins;
    s.revs = reversals(pans, kRevSwing) / mins;
    s.leanFrac = lean / m;
    s.wrongFrac = wrong / m;
    {
        // speech blocks only: within 10 dB of the loudest tenth of the clip's blocks
        std::vector<double> es;
        for (const Blk& b : blks) es.push_back(b.e);
        std::sort(es.begin(), es.end());
        const double gate = es.empty() ? 0 : 0.1 * es[es.size() * 9 / 10];
        std::vector<double> ild;
        for (const Blk& b : blks) if (b.t >= from && b.e >= gate) ild.push_back(b.ild);
        if (ild.size() > 1) {
            double a = 0, a2 = 0;
            for (double v : ild) { a += v; a2 += v * v; }
            a /= ild.size();
            s.ildMean = a;
            s.ildSdRaw = std::sqrt(std::max(0.0, a2 / ild.size() - a * a));
            // ★ robust, like the pan's sd (see there): the wobble around where the image sits, not a one-off step
            std::vector<double> d(ild);
            std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
            const double med = d[d.size() / 2];
            for (double& v : d) v = std::fabs(v - med);
            std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
            s.ildSd = 1.4826 * d[d.size() / 2];
            s.ildRevs = reversals(ild, kIldSwing) / mins;
        }
    }
    if (hz.size() > 1) {
        double a = 0, a2 = 0;
        for (double v : hz) { a += v; a2 += v * v; }
        a /= hz.size();
        s.hzSd = std::sqrt(std::max(0.0, a2 / hz.size() - a * a));
        s.hzRevs = reversals(hz, kHzSwing) / mins;
    }
    return s;
}

struct SteadyTally {
    int n = 0; double travel = 0, revs = 0, sd = 0, hzSd = 0, hzRevs = 0, maxAbs = 0, ildSd = 0, ildRevs = 0, worstIldSd = 0, worstIldRevs = 0;
    double worstTravel = 0, worstRevs = 0, worstSd = 0, worstHzSd = 0, worstHzRevs = 0, worstWrong = 0;
    double firstLeanWorst = -1, firstLeanSum = 0; int leant = 0, noLean = 0;
    void add(const Steady& s) {
        ildSd += s.ildSd; ildRevs += s.ildRevs; worstIldSd = std::max(worstIldSd, s.ildSd); worstIldRevs = std::max(worstIldRevs, s.ildRevs);
        ++n; travel += s.travel; revs += s.revs; sd += s.sd; hzSd += s.hzSd; hzRevs += s.hzRevs; maxAbs = std::max(maxAbs, s.maxAbs);
        worstTravel = std::max(worstTravel, s.travel); worstRevs = std::max(worstRevs, s.revs); worstSd = std::max(worstSd, s.sd);
        worstHzSd = std::max(worstHzSd, s.hzSd); worstHzRevs = std::max(worstHzRevs, s.hzRevs); worstWrong = std::max(worstWrong, s.wrongFrac);
        if (std::fabs(s.expect) > 0.04f) {
            if (s.firstLean < 0) ++noLean;
            else { ++leant; firstLeanSum += s.firstLean; firstLeanWorst = std::max(firstLeanWorst, s.firstLean); }
        }
    }
    void print(const char* label) const {
        if (!n) return;
        char fl[64] = "-";
        if (leant) std::snprintf(fl, sizeof fl, "mean %.2f worst %.2f s", firstLeanSum / leant, firstLeanWorst);
        std::printf("   %-28s %2d | pan: travel/min %5.3f (worst %5.3f) · reversals/min %4.2f (%4.2f) · sd %.4f (%.4f) · max %.3f"
                    " · wrong-way %2.0f %% | Hz: sd %5.1f (%5.1f) · reversals/min %4.2f (%4.2f) | image: sd %.2f (%.2f) dB · reversals/min %5.1f (%5.1f)"
                    " | first lean %s%s\n",
                    label, n, travel / n, worstTravel, revs / n, worstRevs, sd / n, worstSd, maxAbs, 100 * worstWrong,
                    hzSd / n, worstHzSd, hzRevs / n, worstHzRevs, ildSd / n, worstIldSd, ildRevs / n, worstIldRevs,
                    fl, noLean ? " · SOME NEVER LEANT" : "");
    }
};

static void showSteady(const char* name, const Steady& s) {
    char fl[32] = "-";
    if (s.firstLean >= 0) std::snprintf(fl, sizeof fl, "%.2f s", s.firstLean);
    std::printf("   %-34s expect %+.3f | pan mean %+.3f sd %.4f (raw %.4f) max %.3f · travel/min %5.3f · reversals/min %4.2f · leaning %3.0f %%"
                " wrong-way %3.0f %% | Hz sd %5.1f reversals/min %4.2f | image %+5.2f dB sd %.2f (raw %.2f) reversals/min %5.1f | first lean %s\n",
                name, s.expect, s.mean, s.sd, s.sdRaw, s.maxAbs, s.travel, s.revs, 100 * s.leanFrac, 100 * s.wrongFrac, s.hzSd, s.hzRevs,
                s.ildMean, s.ildSd, s.ildSdRaw, s.ildRevs, fl);
}

static void steadySection(const std::string& volDir, const std::string& iqDir) {
    if (const char* f = std::getenv("VIBE_STEADY_FULL")) kSteadyFull = (float)std::atof(f);
    std::printf(" ── 5. the lean's STEADINESS through the binaural chain (dead ±%.0f Hz, full at %.0f Hz) ──\n", kSteadyDead, kSteadyFull);
    std::printf("   pan: + = right, 1 = full lean. reversal = a swing of ≥ %.0f %% each way. TUNED AWAY: on tune %.0f s, then the\n"
                "   dial moves (judged from %.0f s after); ARRIVED / ON TUNE: no dial move, judged from %.0f s.\n",
                100 * kRevSwing, kAwaySec, kAfterMoveSec, kHeldFromSec);
    const int buckets[] = {100, 200, 500, 1000};
    auto bucket = [&](int off) { for (int b = 0; b < 4; ++b) if (std::abs(off) == buckets[b]) return b; return -1; };
    SteadyTally onVol, onQso, two, awayVol[4], arrVol[4], awayQso[4], arrQso[4];
    // ── VOLMET: eleven dials recorded at once ──
    const char* vol[][2] = {{"shannon-5505", "5505000"}, {"raf-5450", "5450000"}, {"shannon-3413", "3413000"}, {"shannon-8957", "8957000"}};
    for (const auto& st : vol) {
        auto path = [&](int off) { return volDir + "/" + st[0] + "/" + st[1] + "_usb_dial" + std::to_string(off) + ".wav"; };
        std::vector<float> x0; int rate;
        if (!readMono(path(0), x0, rate)) continue;
        char name[80];
        std::snprintf(name, sizeof name, "%s on tune", st[0]);
        const Steady on = runSteady(nullptr, x0, rate, +1, 0, 0);
        showSteady(name, on); onVol.add(on);
        for (int off : {100, -100, 200, -200, 500, -500, 1000, -1000}) {
            std::vector<float> x;
            if (!readMono(path(off), x, rate)) continue;
            const SsbOffsetEstimate whole = SsbOffsetEstimator::estimate(x.data(), (int)x.size(), rate, SsbOffsetEstimator::Profile::Slow);
            if (!(whole.valid && whole.snrDb >= 10)) continue;   // not usable (see §1)
            const Steady a = runSteady(&x0, x, rate, +1, off, (float)-off), b = runSteady(nullptr, x, rate, +1, off, (float)-off);
            std::snprintf(name, sizeof name, "%s dial %+d tuned away", st[0], off); showSteady(name, a);
            std::snprintf(name, sizeof name, "%s dial %+d arrived", st[0], off); showSteady(name, b);
            awayVol[bucket(off)].add(a); arrVol[bucket(off)].add(b);
        }
    }
    // ── QSOs: demodulated at each dial ──
    if (exists(iqDir)) for (const Qso& q : kQsos) {
        const auto bb = qsoBaseband(iqDir, q, 120);
        if (bb.size() < 12000 * 60) continue;   // TUNED AWAY needs a minute (the V4L capture is 30 s)
        const auto x0 = ssbAudio(bb, q.sb, 0);
        char name[80];
        std::snprintf(name, sizeof name, "%.4f on tune", q.carrierHz / 1e6);
        const Steady on = runSteady(nullptr, x0, 12000, q.sb, 0, q.sb * q.base);
        showSteady(name, on); onQso.add(on);
        for (int off : {100, -100, 200, -200, 500, -500}) {
            const auto x = ssbAudio(bb, q.sb, off);
            const float sig = q.sb * q.base - off;
            const Steady a = runSteady(&x0, x, 12000, q.sb, off, sig), b = runSteady(nullptr, x, 12000, q.sb, off, sig);
            std::snprintf(name, sizeof name, "%.4f dial %+d tuned away", q.carrierHz / 1e6, off); showSteady(name, a);
            std::snprintf(name, sizeof name, "%.4f dial %+d arrived", q.carrierHz / 1e6, off); showSteady(name, b);
            awayQso[bucket(off)].add(a); arrQso[bucket(off)].add(b);
        }
    }
    // ── two voices in one QSO, a station ~100 Hz from the other ──
    if (exists(iqDir)) for (const Qso& q : {Qso{"uber_40m_7150000_iq192.wav", 7150000, 7138000, -1, 2, "the other station ~105 Hz down"},
                                            Qso{"uber_20m_14250000_iq192.wav", 14250000, 14246000, +1, 100, "two voices ~100 Hz apart"}}) {
        const auto bb = qsoBaseband(iqDir, q, 120);
        if (bb.size() < 12000 * 60) continue;
        const auto x0 = ssbAudio(bb, q.sb, 0);
        char name[80];
        std::snprintf(name, sizeof name, "%.4f two voices, on tune", q.carrierHz / 1e6);
        const Steady on = runSteady(nullptr, x0, 12000, q.sb, 0, q.sb * q.base);
        showSteady(name, on); two.add(on);
        for (int off : {200, -200, 500, -500}) {
            const auto x = ssbAudio(bb, q.sb, off);
            const float sig = q.sb * q.base - off;
            const Steady a = runSteady(&x0, x, 12000, q.sb, off, sig);
            std::snprintf(name, sizeof name, "%.4f two voices, dial %+d away", q.carrierHz / 1e6, off); showSteady(name, a);
            two.add(a);
        }
    }
    std::printf("   ── steadiness summary ──\n");
    std::printf("   (image = the OUTPUT's level difference between the ears per 100 ms of speech: lean + spread. The lean alone\n"
                "    measures ~0.4 dB sd at ±500/±1000 — the head shadow's natural colouring, one place — so that is its floor.)\n");
    onVol.print("VOLMET on tune");
    onQso.print("QSOs on tune");
    two.print("two-voice QSOs (on + away)");
    for (int b = 0; b < 4; ++b) {
        char l[64];
        std::snprintf(l, sizeof l, "VOLMET ±%d tuned away", buckets[b]); awayVol[b].print(l);
        std::snprintf(l, sizeof l, "VOLMET ±%d arrived", buckets[b]); arrVol[b].print(l);
        std::snprintf(l, sizeof l, "QSOs ±%d tuned away", buckets[b]); awayQso[b].print(l);
        std::snprintf(l, sizeof l, "QSOs ±%d arrived", buckets[b]); arrQso[b].print(l);
    }
    /* ★★★ JUDGED (at the app's scale only). Measured 2026-10-17 on this set — the figures are in the commit:
     *     before (build 479: full lean at 300 Hz, the spread placing every frame by itself) the pan reversed up to 3.4
     *     times a minute and the image's level difference had an sd of 1.4-2.0 dB, reversing 60-120 times a minute at
     *     ±200/±500; with the 1 kHz scale alone ±500/±1000 still measured 1.0-1.2 dB and 50-90 a minute. */
    if (kSteadyFull != 1000) { std::printf("   (scale overridden — the steadiness checks are not judged)\n"); return; }
    const SteadyTally* held[] = {&awayVol[0], &awayVol[1], &awayVol[2], &awayVol[3], &arrVol[0], &arrVol[1], &arrVol[2], &arrVol[3],
                                 &awayQso[0], &awayQso[1], &awayQso[2], &arrQso[0], &arrQso[1], &arrQso[2]};
    double revW = 0, sdW = 0, wrongW = 0;
    for (const SteadyTally* t : held) { revW = std::max(revW, t->worstRevs); sdW = std::max(sdW, t->worstSd); wrongW = std::max(wrongW, t->worstWrong); }
    char what[220];
    std::snprintf(what, sizeof what, "on tune (VOLMET + QSOs): no lean at all (max |pan| %.3f), the image untouched (sd %.2f dB)",
                  std::max(onVol.maxAbs, onQso.maxAbs), std::max(onVol.worstIldSd, onQso.worstIldSd));
    check(onVol.n > 0 && onQso.n > 0 && std::max(onVol.maxAbs, onQso.maxAbs) <= 0.02 && std::max(onVol.worstIldSd, onQso.worstIldSd) <= 0.1, what);
    std::snprintf(what, sizeof what, "held off tune, every clip: the pan reverses ≤ 1 a minute (worst %.2f), sd ≤ 0.05 (worst %.4f), never the wrong way (worst %.0f %%)",
                  revW, sdW, 100 * wrongW);
    check(revW <= 1.0 && sdW <= 0.05 && wrongW <= 0.01, what);
    double flW = 0; int noLean = 0;
    for (const SteadyTally* t : {&awayVol[1], &awayVol[2], &awayVol[3], &awayQso[1], &awayQso[2]}) { flW = std::max(flW, t->firstLeanWorst); noLean += t->noLean; }
    std::snprintf(what, sizeof what, "tuned away from a held voice: half the lean within 0.5 s, every time (worst %.2f s, %d never)", flW, noLean);
    check(noLean == 0 && flW >= 0 && flW <= 0.5, what);
    double ildMean = 0, ildW = 0, ildRev = 0; int nB = 0;
    for (const SteadyTally* t : {&awayVol[2], &awayVol[3], &arrVol[2], &arrVol[3], &awayQso[2], &arrQso[2]}) {
        if (!t->n) continue;
        ildMean = std::max(ildMean, t->ildSd / t->n); ildW = std::max(ildW, t->worstIldSd); ildRev = std::max(ildRev, t->ildRevs / t->n); ++nB;
    }
    std::snprintf(what, sizeof what, "±500/±1000: the image holds still — sd ≤ 0.8 dB per group (%.2f), ≤ 1.2 dB any clip (%.2f), ≤ 20 reversals/min (%.1f)",
                  ildMean, ildW, ildRev);
    check(nB > 0 && ildMean <= 0.8 && ildW <= 1.2 && ildRev <= 20, what);
}


// ── 6. ARRIVING off tune: how long until the lean appears ───────────────────────────────────────────────────────────
/* ★★★ Stuart on 482 (2026-10-17): "binaural not working at all, no panning effect at all … at all levels of off tune",
 *     then "it's started working, but only after I've been on a signal for a while", and "I'm deliberately waiting off
 *     tune and it's not panning". TUNED AWAY from a locked voice is the dial's own arithmetic (instant, §5); ARRIVING
 *     off tune on a voice nothing has locked is the estimator alone — or the waterfall cue, when the app has one.
 *     Each run starts COLD at a fixed dial off the voice, from several points in the clip (arrival times depend on when
 *     the speech comes), and is held there for kArriveSec. Through the real controller at the app's SSB scale, spread on.
 *       first est / acc / pub — the estimator's first answer, the controller's first accepted one, the first published;
 *       audible — the pan past kAudiblePan the right way (an ear difference you can hear: see kAudiblePan);
 *       ILD — the output's level difference between the ears once it leans (dB, speech blocks, + = right).
 *     Two cue conditions: none (the estimator alone), and the waterfall cue from 1 s in, UNDERSTATED by 100 Hz (toward
 *     on tune: the cue's error is ±150 Hz and the understated side is the hard one). */
constexpr double kArriveSec = 60;
constexpr float kAudiblePan = 0.15f;   // ≈ 1.5 dB and 90 µs between the ears, plus the spread: plainly to one side

struct Arrival {
    double tEst = -1, tAcc = -1, tPub = -1, tAudible = -1;
    bool wrong = false;          // ever leant past kRevSwing the wrong way (or at all, when on tune)
    double ild = 0; int ildN = 0;
    float expect = 0, finalPan = 0;
    int estN = 0, failValid = 0, failRecon = 0, failFrames = 0, failConf = 0, failClar = 0, failLarge = 0, passN = 0;
};

static FILE* gArrCsv = nullptr;    // VIBE_ARRIVE_CSV=<path>: every estimate of every cold run (measurement aid)
static int gArrRun = 0;

static Arrival runArrival(const std::vector<float>& x, size_t from, int rate, int sb, float signalHz, bool cue) {
    Arrival a;
    const int run = ++gArrRun;
    a.expect = binauralPanForSsb(signalHz, kSteadyDead, kSteadyFull);
    BinauralTuning bt(false);
    bt.setEnabled(true);
    bt.setSource(sb > 0 ? BinauralTuning::SsbUsb : BinauralTuning::SsbLsb, kSteadyDead, kSteadyFull);
    bt.setSpread(sb > 0 ? BinauralSpread::Usb : BinauralSpread::Lsb, 50, 2700);
    const double dial = 7100000;
    bt.setDial(dial);
    // the estimator's own view, for where the time goes (a twin: the controller's is private)
    SsbOffsetEstimator est(rate, SsbOffsetEstimator::Profile::Fast);
    const int pkt = rate / 50;
    const size_t n = std::min(x.size(), from + (size_t)(kArriveSec * rate));
    std::vector<float> l(pkt), r(pkt);
    double bL = 0, bR = 0; int bN = 0;
    std::vector<std::pair<double, double>> blks;   // (energy, ild) after it leans
    bool firstPassSeen = false;
    for (size_t i = from; i + pkt <= n; i += pkt) {
        const double t = (double)(i + pkt - from) / rate;
        if (cue && t >= 1.0) {
            const float under = signalHz > 0 ? -100.0f : 100.0f;
            bt.setSsbHint(dial + signalHz + under, true);
        }
        if ((!firstPassSeen || (gArrCsv && !cue)) && est.push(&x[i], pkt)) {
            const SsbOffsetEstimate& e = est.last();
            if (gArrCsv && !cue)
                std::fprintf(gArrCsv, "%d,%.2f,%.1f,%.1f,%d,%d,%d,%.3f,%.3f,%.1f,%d,%.0f,%.0f,%d,%.0f\n", run, t, signalHz, sb * e.offsetHz,
                             e.valid, e.passes(), e.reconciled, e.confidence, e.clarity, e.snrDb, e.frames, sb * e.coarseLoHz,
                             sb * e.coarseHiHz, e.coarsePinned, e.f0Hz);
            if (!firstPassSeen) {
            ++a.estN;
            if (!e.valid) ++a.failValid;
            else if (!e.reconciled) ++a.failRecon;
            else if (e.frames < e.minFrames) ++a.failFrames;
            else if (e.confidence < e.confidenceFloor()) ++a.failConf;
            else if (e.clarity < (e.deepPinned() ? std::max(e.gate.clarity, e.gate.deepClarity) : e.gate.clarity)) ++a.failClar;
            else if (!e.passes()) ++a.failLarge;
            else { ++a.passN; firstPassSeen = true; }
            }
        }
        std::copy(x.begin() + i, x.begin() + i + pkt, l.begin());
        std::copy(x.begin() + i, x.begin() + i + pkt, r.begin());
        bt.processPlanar(l.data(), r.data(), pkt, rate);
        if (((i - from) / pkt) % 2 == 1) bt.pumpSsbForTest();
        float st[12]; bt.stats(st, 12);
        if (a.tEst < 0 && st[6] >= 1) a.tEst = t;
        if (a.tAcc < 0 && st[7] >= 1) a.tAcc = t;
        if (a.tPub < 0 && st[3] > 0) a.tPub = t;
        const float p = st[4];
        if (std::fabs(p) > kRevSwing && (a.expect == 0 || p * a.expect < 0)) a.wrong = true;
        if (a.tAudible < 0 && a.expect != 0 && p * a.expect > 0 && std::fabs(p) >= kAudiblePan) a.tAudible = t;
        a.finalPan = p;
        for (int k = 0; k < pkt; ++k) { bL += (double)l[k] * l[k]; bR += (double)r[k] * r[k]; }
        if (++bN == 5) {
            if (std::fabs(p) > kRevSwing) blks.push_back({bL + bR, 10 * std::log10((bR + 1e-20) / (bL + 1e-20))});
            bL = bR = 0; bN = 0;
        }
    }
    if (!blks.empty()) {
        std::vector<double> es; for (auto& b : blks) es.push_back(b.first);
        std::sort(es.begin(), es.end());
        const double gate = 0.1 * es[es.size() * 9 / 10];
        for (auto& b : blks) if (b.first >= gate) { a.ild += b.second; ++a.ildN; }
        if (a.ildN) a.ild /= a.ildN;
    }
    return a;
}

struct ArrTally {
    int runs = 0, audible = 0, wrong = 0; std::vector<double> times;
    void add(const Arrival& a) { ++runs; wrong += a.wrong; if (a.tAudible >= 0) { ++audible; times.push_back(a.tAudible); } }
    void print(const char* label) const {
        if (!runs) return;
        std::vector<double> t = times; std::sort(t.begin(), t.end());
        auto q = [&](double f) { return t.empty() ? -1.0 : t[std::min(t.size() - 1, (size_t)(f * t.size()))]; };
        std::printf("   %-34s %3d runs | audible within %.0f s: %3d (%3.0f %%) · median %5.1f s · 80 %% %5.1f s · worst %5.1f s | wrong-way %d\n",
                    label, runs, kArriveSec, audible, 100.0 * audible / runs, q(0.5), q(0.8), t.empty() ? -1.0 : t.back(), wrong);
    }
};

static void showArrival(const char* name, const Arrival& a) {
    std::printf("   %-40s expect %+.3f | est %5.1f acc %5.1f pub %5.1f audible %5.1f s | end pan %+.3f ILD %+5.1f dB | %s"
                " | to 1st pass: %d est (valid %d recon %d frames %d conf %d clar %d large %d)\n",
                name, a.expect, a.tEst, a.tAcc, a.tPub, a.tAudible, a.finalPan, a.ild, a.wrong ? "WRONG-WAY" : "ok",
                a.estN, a.failValid, a.failRecon, a.failFrames, a.failConf, a.failClar, a.failLarge);
}

static bool arrivalSection(const std::string& volDir, const std::string& iqDir) {
    std::printf(" ── 6. ARRIVING off tune, cold: time to an AUDIBLE lean (|pan| ≥ %.2f the right way), held %.0f s ──\n",
                kAudiblePan, kArriveSec);
    const bool verbose = std::getenv("VIBE_ARRIVE_VERBOSE") != nullptr;
    if (const char* c = std::getenv("VIBE_ARRIVE_CSV")) {
        gArrCsv = std::fopen(c, "w");
        if (gArrCsv) std::fprintf(gArrCsv, "run,t,truth,ans,valid,passes,recon,conf,clar,snr,frames,clo,chi,pinned,f0\n");
    }
    const int buckets[] = {100, 200, 300, 500, 1000};
    auto bucket = [&](int off) { for (int b = 0; b < 5; ++b) if (std::abs(off) == buckets[b]) return b; return -1; };
    ArrTally vol[2][5], qso[2][5], onTune[2];
    const size_t starts[] = {0, 30, 60};
    const char* volSt[][2] = {{"shannon-5505", "5505000"}, {"raf-5450", "5450000"}, {"shannon-3413", "3413000"}, {"shannon-8957", "8957000"}};
    for (const auto& st : volSt) {
        auto path = [&](int off) { return volDir + "/" + st[0] + "/" + st[1] + "_usb_dial" + std::to_string(off) + ".wav"; };
        for (int off : {0, 100, -100, 200, -200, 500, -500, 1000, -1000}) {
            std::vector<float> x; int rate;
            if (!readMono(path(off), x, rate)) continue;
            if (off) {
                const SsbOffsetEstimate whole = SsbOffsetEstimator::estimate(x.data(), (int)x.size(), rate, SsbOffsetEstimator::Profile::Slow);
                if (!(whole.valid && whole.snrDb >= 10)) continue;   // not usable (see §1)
            }
            for (int c = 0; c < 2; ++c) for (size_t s0 : starts) {
                const Arrival a = runArrival(x, s0 * rate, rate, +1, (float)-off, c == 1);
                if (off == 0) onTune[c].add(a); else vol[c][bucket(off)].add(a);
                if (verbose || a.wrong) {
                    char name[96]; std::snprintf(name, sizeof name, "%s dial %+d @%zus %s", st[0], off, s0, c ? "cue" : "no cue");
                    showArrival(name, a);
                }
            }
        }
    }
    if (exists(iqDir)) for (const Qso& q : kQsos) {
        const auto bb = qsoBaseband(iqDir, q, 120);
        if (bb.size() < 12000 * 60) continue;
        for (int off : {0, 100, -100, 200, -200, 300, -300, 500, -500, 1000, -1000}) {
            const auto x = ssbAudio(bb, q.sb, off);
            const float sig = q.sb * q.base - off;
            for (int c = 0; c < 2; ++c) for (size_t s0 : starts) {
                if (s0 * 12000 + 12000 * 20 > x.size()) continue;
                const Arrival a = runArrival(x, s0 * 12000, 12000, q.sb, sig, c == 1);
                if (off == 0) onTune[c].add(a); else qso[c][bucket(off)].add(a);
                if (verbose || a.wrong) {
                    char name[96]; std::snprintf(name, sizeof name, "%.4f dial %+d @%zus %s", q.carrierHz / 1e6, off, s0, c ? "cue" : "no cue");
                    showArrival(name, a);
                }
            }
        }
    }
    std::printf("   ── arrival summary ──\n");
    for (int c = 0; c < 2; ++c) {
        char l[64];
        std::snprintf(l, sizeof l, "on tune (%s)", c ? "cue" : "no cue"); onTune[c].print(l);
        for (int b = 0; b < 5; ++b) {
            std::snprintf(l, sizeof l, "VOLMET ±%d (%s)", buckets[b], c ? "cue" : "no cue"); vol[c][b].print(l);
            std::snprintf(l, sizeof l, "QSOs ±%d (%s)", buckets[b], c ? "cue" : "no cue"); qso[c][b].print(l);
        }
    }
    /* ★★★ JUDGED. Measured 2026-10-18 — before (build 482: the plain smoothstep, no vote): ±100 and ±200 Hz NEVER audible
     *     (0 of 144 runs), ±300 92 % (median 9.2 s, 80 % 23 s), ±500 median 4.6 / 11.1 s and 80 % 16 / 25 s (VOLMET /
     *     QSOs); with the waterfall cue ±500/±1000 at 1.0-1.1 s. After (the SSB curve, the arrival vote): ±200 100 %
     *     (median 5.2 / 7.7 s, 80 % 13.7 / 16.2 s), ±300 100 % (7.7 s, 13.2 s), ±500 4.5 / 8.0 s and 6.5 / 16.0 s; with the
     *     cue every ±500/±1000 run audible by 1.1 s. ±1000 without the cue is reported only (the deep side is hard: 77-95 %).
     *     ±100 Hz is deliberately gentle (8 % lean) and not judged for audibility. */
    int wrong = 0, runs = 0;
    for (int c = 0; c < 2; ++c) { wrong += onTune[c].wrong; runs += onTune[c].runs; for (int b = 0; b < 5; ++b) { wrong += vol[c][b].wrong + qso[c][b].wrong; runs += vol[c][b].runs + qso[c][b].runs; } }
    char what[240];
    std::snprintf(what, sizeof what, "arriving: no lean the wrong way, and none at all on tune, in %d cold runs (%d did)", runs, wrong);
    check(runs > 0 && wrong == 0, what);
    auto stats = [](const ArrTally& t, double& frac, double& med, double& p80) {
        std::vector<double> v = t.times; std::sort(v.begin(), v.end());
        frac = t.runs ? (double)t.audible / t.runs : 0;
        med = v.empty() ? 1e9 : v[v.size() / 2];
        p80 = v.empty() ? 1e9 : v[std::min(v.size() - 1, (size_t)(0.8 * v.size()))];
    };
    double fracW = 1, medW = 0, p80W = 0; int nT = 0;
    for (const ArrTally* t : {&vol[0][1], &qso[0][1], &qso[0][2], &vol[0][3], &qso[0][3]}) {
        if (!t->runs) continue;
        double f, m, p; stats(*t, f, m, p);
        fracW = std::min(fracW, f); medW = std::max(medW, m); p80W = std::max(p80W, p); ++nT;
    }
    std::snprintf(what, sizeof what, "arriving ±200…±500 Hz, the voice alone: audibly leaning within %.0f s on ≥ 95 %% of runs (worst group %.0f %%), median ≤ 9 s (%.1f), 80 %% ≤ 18 s (%.1f)",
                  kArriveSec, 100 * fracW, medW, p80W);
    check(nT > 0 && fracW >= 0.95 && medW <= 9 && p80W <= 18, what);
    double cueW = 0; int cueMiss = 0, nC = 0;
    // ★ ±500 and ±1000 only: the cue here is UNDERSTATED by 100 Hz and native takes kCoarseShrinkHz off its lean, so at
    //   ±300 it leans as if 100 Hz off (8 %) — gentle by design (a cue may be a voice on tune); reported, not judged.
    for (int b = 3; b < 5; ++b) for (const ArrTally* t : {&vol[1][b], &qso[1][b]}) {
        if (!t->runs) continue;
        ++nC; cueMiss += t->runs - t->audible;
        for (double x : t->times) cueW = std::max(cueW, x);
    }
    std::snprintf(what, sizeof what, "arriving ±500…±1000 Hz with the waterfall cue: audibly leaning within 1.5 s, every run (worst %.1f s, %d never)", cueW, cueMiss);
    check(nC > 0 && cueMiss == 0 && cueW <= 1.5, what);
    return true;
}

// ── 7. THE SWEEP, through the native chain: the app's own cue stream replayed against the audio ─────────────────────
/* ★★★ Stuart on 482: "it's not panning as I tune through the band". scripts/test_binauralSweep.ts drives the REAL app tracker
 *     (binauralOffset.ts) over spectra of a composite band — real voices from these recordings, placed where the truth is
 *     exact — with the dial clicking 100 Hz / 0.3 s and 500 Hz / 0.5 s, and (VIBE_SWEEP_EVENTS=<dir>) writes what it sent
 *     native: the source, every dial change and every waterfall cue, with their times, plus the band's I/Q. Here that
 *     stream is replayed into the real BinauralTuning while the band is demodulated along the same dial path and fed as
 *     the audio — the estimator, the dial reckoning and the cue hand-off all working at once, as on the phone.
 *     Judged on frames where ONE voice fills the passband and no other touches it:
 *       • a voice ≥ 400 Hz off (past the cue's own error and its kCoarseShrinkHz): leaning the right way on ≥ 60 % of them;
 *       • the wrong way (past 5 %) on ≤ 2 % of all judged frames;
 *       • within ±60 Hz of a voice: an audible lean (≥ 0.15) on ≤ 10 % of those frames.
 *     NOT RUN without the events (run the JS half first) — never a pass. */
static bool jsonNum(const char* p, const char* key, double& v) {
    char k[48]; std::snprintf(k, sizeof k, "\"%s\":", key);
    const char* q = std::strstr(p, k);
    if (!q) return false;
    v = std::strtod(q + std::strlen(k), nullptr);
    return true;
}
static bool jsonStr(const char* p, const char* key, std::string& v) {
    char k[48]; std::snprintf(k, sizeof k, "\"%s\":\"", key);
    const char* q = std::strstr(p, k);
    if (!q) return false;
    q += std::strlen(k);
    const char* e = std::strchr(q, '"');
    if (!e) return false;
    v.assign(q, e);
    return true;
}
struct SweepEv { double t; char k; double v; bool ok; double dead, full; };

struct SweepTally { int far = 0, farRight = 0, judged = 0, wrong = 0, near = 0, nearLean = 0, runs = 0; };

static bool replaySweep(const std::string& dir, const std::string& file, SweepTally& tally, bool verbose) {
    FILE* f = std::fopen((dir + "/" + file).c_str(), "rb");
    if (!f) return false;
    std::string js; char buf[65536]; size_t m;
    while ((m = std::fread(buf, 1, sizeof buf, f)) > 0) js.append(buf, m);
    std::fclose(f);
    const char* p = js.c_str();
    double fs, centre, secs, sb, from, to, step, every;
    std::string iqName;
    if (!jsonNum(p, "fs", fs) || !jsonNum(p, "centre", centre) || !jsonNum(p, "secs", secs) || !jsonNum(p, "sb", sb) ||
        !jsonNum(p, "from", from) || !jsonNum(p, "to", to) || !jsonNum(p, "step", step) || !jsonNum(p, "every", every) ||
        !jsonStr(p, "iq", iqName)) return false;
    // voices
    std::vector<double> carriers;
    {
        const char* v = std::strstr(p, "\"voices\":[");
        const char* e = v ? std::strchr(v, ']') : nullptr;
        for (const char* q = v; q && q < e; ) {
            q = std::strstr(q, "\"carrier\":");
            if (!q || q > e) break;
            carriers.push_back(std::strtod(q + 10, nullptr)); q += 10;
        }
    }
    // events
    std::vector<SweepEv> evs;
    {
        const char* q = std::strstr(p, "\"events\":[");
        while (q && (q = std::strstr(q, "{\"t\":")) != nullptr) {
            const char* e = std::strchr(q, '}');
            std::string one(q, e + 1);
            SweepEv ev{}; double t = 0, v = 0, d = 0, fl = 0; std::string k;
            jsonNum(one.c_str(), "t", t); jsonNum(one.c_str(), "v", v); jsonStr(one.c_str(), "k", k);
            ev.t = t; ev.v = v; ev.k = k.empty() ? '?' : k[0];
            ev.ok = std::strstr(one.c_str(), "\"ok\":true") != nullptr;
            if (jsonNum(one.c_str(), "dead", d)) ev.dead = d;
            if (jsonNum(one.c_str(), "full", fl)) ev.full = fl;
            evs.push_back(ev);
            q = e;
        }
    }
    // the band
    std::vector<float> iq;
    {
        FILE* g = std::fopen((dir + "/" + iqName).c_str(), "rb");
        if (!g) return false;
        std::fseek(g, 0, SEEK_END); const long bytes = std::ftell(g); std::fseek(g, 0, SEEK_SET);
        iq.resize((size_t)bytes / 4);
        if (std::fread(iq.data(), 4, iq.size(), g) != iq.size()) { std::fclose(g); return false; }
        std::fclose(g);
    }
    const size_t nIq = iq.size() / 2;   // re[0..n), im[n..2n)
    const int s1 = (int)sb;
    const int dirn = to > from ? 1 : -1;
    const int steps = (int)std::floor(std::fabs(to - from) / step + 1e-9);
    const double dur = steps * every + 3;
    auto dialAt = [&](double t) { return from + dirn * step * std::min(steps, (int)std::floor(t / every + 1e-9)); };
    // the receiver: mix the dial's sideband middle to 0 Hz (phase-continuous), half-band to 12 kHz, keep 50-2700 Hz
    const double fs2 = fs / 2;
    static const std::vector<float> hHalf = lowpass(5000, 24000, 63);
    static const std::vector<float> hSsb = lowpass(1325, 12000, 241);
    const double mid = 1375;
    double ph = 0, ph2 = 0;
    std::vector<cf> d1hist, d2hist;
    BinauralTuning bt(false);
    bt.setEnabled(true);
    bt.setSpread(s1 > 0 ? BinauralSpread::Usb : BinauralSpread::Lsb, 50, 2700);
    size_t ei = 0;
    const int pkt = 240;   // 20 ms at 12 kHz
    std::vector<float> l(pkt), r(pkt);
    long n24 = 0; int frameIdx = 0;
    double agc = 0.1;
    for (double t = 0; t < dur; t += 0.02) {
        // events up to now
        while (ei < evs.size() && evs[ei].t <= t + 1e-9) {
            const SweepEv& e = evs[ei++];
            if (e.k == 's') bt.setSource((int)e.v, (float)e.dead, (float)e.full);
            else if (e.k == 'd') bt.setDial(e.v);
            else if (e.k == 'h') bt.setSsbHint(e.v, e.ok);
        }
        const double dial = dialAt(t);
        const double w = -2 * M_PI * (dial - centre + s1 * mid) / fs;
        for (int k = 0; k < pkt; ++k) {
            // two 24 kHz samples per 12 kHz output
            for (int u = 0; u < 2; ++u) {
                const size_t i = (size_t)(n24 % (long)nIq);
                const cf x(iq[i], iq[nIq + i]);
                d1hist.push_back(x * cf((float)std::cos(ph), (float)std::sin(ph)));
                ph += w; if (ph > M_PI) ph -= 2 * M_PI; if (ph < -M_PI) ph += 2 * M_PI;
                ++n24;
            }
            if (d1hist.size() > hHalf.size()) d1hist.erase(d1hist.begin(), d1hist.begin() + (long)(d1hist.size() - hHalf.size()));
            cf a(0, 0);
            if (d1hist.size() == hHalf.size()) for (size_t j = 0; j < hHalf.size(); ++j) a += d1hist[j] * hHalf[j];
            d2hist.push_back(a);
            if (d2hist.size() > hSsb.size()) d2hist.erase(d2hist.begin());
            cf b(0, 0);
            if (d2hist.size() == hSsb.size()) for (size_t j = 0; j < hSsb.size(); ++j) b += d2hist[j] * hSsb[j];
            const cf up((float)std::cos(ph2), (float)std::sin(ph2));
            ph2 += 2 * M_PI * s1 * mid / fs2; if (ph2 > M_PI) ph2 -= 2 * M_PI; if (ph2 < -M_PI) ph2 += 2 * M_PI;
            l[k] = (b * up).real();
        }
        // a slow AGC, like the receiver's: −20 dBFS RMS
        double e = 0; for (float v : l) e += (double)v * v;
        const double rms = std::sqrt(e / pkt + 1e-20);
        agc += 0.02 * (0.1 / rms - agc);
        agc = std::min(agc, 1e4);
        for (int k = 0; k < pkt; ++k) { l[k] = (float)(l[k] * agc); r[k] = l[k]; }
        bt.processPlanar(l.data(), r.data(), pkt, 12000);
        if (((int)std::lround(t / 0.02)) % 2 == 1) bt.pumpSsbForTest();
        if (((int)std::lround(t / 0.02)) % 5 != 4) continue;   // judge per 0.1 s
        ++frameIdx;
        float st[12]; bt.stats(st, 12);
        const float pan = st[4];
        // the one voice in view (≥ 300 Hz of the passband), none other touching
        auto overlap = [&](double c) {
            const double vLo = s1 > 0 ? c + 300 : c - 2700, vHi = s1 > 0 ? c + 2700 : c - 300;
            const double pLo = s1 > 0 ? dial + 50 : dial - 2700, pHi = s1 > 0 ? dial + 2700 : dial - 50;
            return std::min(vHi, pHi) - std::max(vLo, pLo);
        };
        int inView = 0, touching = 0; double c0 = 0;
        for (double c : carriers) { const double o = overlap(c); if (o >= 300) { ++inView; c0 = c; } if (o >= 1) ++touching; }
        if (verbose && frameIdx % 5 == 0)
            std::printf("      t %5.1f dial %.0f pan %+.3f off %+7.1f%s%s truth %s\n", t, dial, pan, st[2], st[3] > 0 ? "" : "?", st[10] > 0 ? " c" : "",
                        inView == 1 ? std::to_string((int)(c0 - dial)).c_str() : "-");
        if (inView != 1 || touching != 1) continue;
        const double truth = c0 - dial;
        ++tally.judged;
        if (std::fabs(truth) >= 400) { ++tally.far; if (pan * truth > 0 && std::fabs(pan) > kRevSwing) ++tally.farRight; }
        if (std::fabs(truth) >= 100 && pan * truth < 0 && std::fabs(pan) > 0.05f) ++tally.wrong;
        if (std::fabs(truth) <= 60) { ++tally.near; if (std::fabs(pan) >= kAudiblePan) ++tally.nearLean; }
    }
    ++tally.runs;
    return true;
}

static void sweepSection() {
    const char* dirEnv = std::getenv("VIBE_SWEEP_EVENTS");
    std::printf(" ── 7. SWEEPING a band through the native chain (the app's cue stream replayed against the audio) ──\n");
    if (!dirEnv) { std::printf("   NOT RUN — set VIBE_SWEEP_EVENTS to the folder scripts/test_binauralSweep.ts wrote\n"); return; }
    const std::string dir = dirEnv;
    std::vector<std::string> files;
    if (FILE* ls = popen(("ls '" + dir + "' 2>/dev/null").c_str(), "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, ls)) {
            std::string f(line); while (!f.empty() && (f.back() == '\n' || f.back() == '\r')) f.pop_back();
            if (f.rfind("sweep_", 0) == 0 && f.find("composite") != std::string::npos && f.size() > 5 && f.substr(f.size() - 5) == ".json") files.push_back(f);
        }
        pclose(ls);
    }
    if (files.empty()) { std::printf("   NOT RUN — no composite sweeps in %s\n", dir.c_str()); return; }
    const char* only = std::getenv("VIBE_SWEEP_ONLY_FILE");
    SweepTally all;
    for (const std::string& f : files) {
        if (only && f.find(only) == std::string::npos) continue;
        SweepTally t;
        if (!replaySweep(dir, f, t, only != nullptr)) { std::printf("   (could not replay %s)\n", f.c_str()); continue; }
        std::printf("   %-58s judged %4d · ≥400 Hz off: right way %3d of %3d · wrong way %2d · on tune: audible lean %d of %d\n",
                    f.c_str(), t.judged, t.farRight, t.far, t.wrong, t.nearLean, t.near);
        all.far += t.far; all.farRight += t.farRight; all.judged += t.judged; all.wrong += t.wrong; all.near += t.near;
        all.nearLean += t.nearLean; all.runs += t.runs;
    }
    char what[240];
    std::snprintf(what, sizeof what, "sweeping (%d runs): a voice ≥ 400 Hz off leans the right way on ≥ 60 %% of frames (%d of %d)", all.runs, all.farRight, all.far);
    check(all.far > 0 && all.farRight >= 0.6 * all.far, what);
    std::snprintf(what, sizeof what, "sweeping: the wrong way on ≤ 2 %% of judged frames (%d of %d)", all.wrong, all.judged);
    check(all.judged > 0 && all.wrong <= 0.02 * all.judged, what);
    std::snprintf(what, sizeof what, "sweeping: within ±60 Hz of a voice, an audible lean on ≤ 10 %% of frames (%d of %d)", all.nearLean, all.near);
    check(all.near > 0 && all.nearLean <= 0.1 * all.near, what);
}

int main() {
    std::printf("SSB mistune estimator — real off-air audio\n");
    const char* rootEnv = std::getenv("VIBE_TEST_AUDIO");
    const std::string root = rootEnv ? rootEnv : "";
    const std::string volDir = root + "/ssb/offsets", iqDir = root + "/iq";
    if (!rootEnv || !exists(volDir + "/shannon-5505")) {
        std::printf("   NOT RUN — set VIBE_TEST_AUDIO to the folder holding ssb/offsets/ and iq/ (~1 GB, not in the repo)\n");
        return 3;
    }
    // ★ VIBE_SSB_REAL_ONLY=steady runs §5 alone (a minute or two, not the whole calibration) — for tuning the lean.
    const char* only = std::getenv("VIBE_SSB_REAL_ONLY");
    if (only && !std::strcmp(only, "sweep")) {
        sweepSection();
        std::printf("%s\n", fails ? "SSB OFFSET (REAL, SWEEP ONLY): FAILURES" : "SSB OFFSET (REAL, SWEEP ONLY): all passed");
        return fails ? 1 : 0;
    }
    if (only && !std::strcmp(only, "arrival")) {
        arrivalSection(volDir, iqDir);
        std::printf("%s\n", fails ? "SSB OFFSET (REAL, ARRIVAL ONLY): FAILURES" : "SSB OFFSET (REAL, ARRIVAL ONLY): all passed");
        return fails ? 1 : 0;
    }
    if (only && !std::strcmp(only, "steady")) {
        steadySection(volDir, iqDir);
        std::printf("%s\n", fails ? "SSB OFFSET (REAL, STEADINESS ONLY): FAILURES" : "SSB OFFSET (REAL, STEADINESS ONLY): all passed");
        return fails ? 1 : 0;
    }
    std::printf("   FAST = the binaural aid's profile (an answer per ~0.6 s of voice, 8 s memory); SLOW = transcription (10 s\n"
                "   windows). 'settled' = answers from %.0f s in; 'believed' = passes the profile's gate; tolerance 25 Hz,\n"
                "   60 Hz from ±450 Hz. 'bin' = BinauralTuning's published offset at the end (signal Hz) and the share of the\n"
                "   settled time it was within tolerance. * = not usable (speech-to-gap < 10 dB).\n", kSettleSec);

    // ── 1. VOLMET ──
    struct St { const char* dir; const char* carrier; const char* role; };
    const St stations[] = {{"shannon-5505", "5505000", "train"}, {"raf-5450", "5450000", "train"},
                           {"shannon-3413", "3413000", "train"}, {"shannon-8957", "8957000", "held-out"},
                           {"raf-11253", "11253000", "weak"}};
    const int offs[] = {0, 50, -50, 100, -100, 200, -200, 500, -500, 1000, -1000};
    Tally train, held, weak, qsoT, qsoFar;
    int fNotSign = 0, fAcc = 0, fOnTune = 0, fWrongSignAny = 0, k1000Met = 0, k1000Usable = 0;
    double fastCpu = 0, slowCpu = 0, secs = 0;
    std::vector<std::string> missed;
    for (const St& st : stations) {
        std::printf(" ── VOLMET %s (%s) ──\n", st.dir, st.role);
        for (int off : offs) {
            const std::string f = volDir + "/" + st.dir + "/" + st.carrier + "_usb_dial" + std::to_string(off) + ".wav";
            std::vector<float> x; int rate;
            if (!readMono(f, x, rate)) { std::printf("   (missing %s)\n", f.c_str()); continue; }
            const float truth = (float)-off;
            const SsbOffsetEstimate whole = SsbOffsetEstimator::estimate(x.data(), (int)x.size(), rate, SsbOffsetEstimator::Profile::Slow);
            const bool usable = whole.valid && whole.snrDb >= 10 && std::strcmp(st.role, "weak") != 0;
            const FastRun fr = runFast(x, rate, truth, +1, true);
            const SlowRun sr = runSlow(x, rate, truth);
            fastCpu += fr.cpu; slowCpu += sr.cpu; secs += (double)x.size() / rate;
            char name[64];
            std::snprintf(name, sizeof name, "%s dial %+d", st.dir, off);
            showRow(name, truth, whole.snrDb, usable, fr, sr);
            (std::strcmp(st.role, "train") == 0 ? train : std::strcmp(st.role, "held-out") == 0 ? held : weak).add(fr, sr);
            // the judged rules
            if (fr.passedWrongSign || sr.passedWrongSign) { ++fWrongSignAny; missed.push_back(std::string(name) + ": a believed WRONG-SIGN answer"); }
            if (!usable) continue;
            if (off == 0 && fr.passedOutside50) { ++fOnTune; missed.push_back(std::string(name) + ": on tune, believed answers outside ±50 Hz"); }
            if (std::abs(off) >= 100 && !(fr.settledMedian * truth > 0)) { ++fNotSign; missed.push_back(std::string(name) + ": sign not answered right"); }
            const bool accOk = std::fabs(fr.settledMedian - truth) <= tolFor(truth);
            if (std::abs(off) == 1000) { ++k1000Usable; k1000Met += accOk; if (!accOk) missed.push_back(std::string(name) + ": ±1000 target missed (reported)"); }
            else if (!accOk) { ++fAcc; missed.push_back(std::string(name) + ": FAST median outside tolerance"); }
        }
    }

    // ── 2. QSOs from the busy-band I/Q ──
    std::printf(" ── QSOs demodulated from busy-band I/Q (held out; ±1000 reported only) ──\n");
    int qsoWrongSign = 0;
    std::vector<float> deep80, voice20;   // kept for the two-station mix
    if (!exists(iqDir)) std::printf("   (no %s — QSO section NOT RUN)\n", iqDir.c_str());
    else for (const Qso& q : kQsos) {
        const auto bb = qsoBaseband(iqDir, q, 120);
        if (bb.size() < 12000 * 20) { std::printf("   (missing or short: %s)\n", q.file); continue; }
        std::printf("   %s %.4f MHz %s, base %+.0f Hz:\n", q.file, q.carrierHz / 1e6, q.note, q.base);
        for (int off : {0, 100, -100, 200, -200, 500, -500, 1000, -1000}) {
            const auto x = ssbAudio(bb, q.sb, off);
            const float truth = q.base + (q.sb > 0 ? -off : off);
            const FastRun fr = runFast(x, 12000, truth, q.sb, true);
            const SlowRun sr = runSlow(x, 12000, truth);
            const SsbOffsetEstimate whole = SsbOffsetEstimator::estimate(x.data(), (int)x.size(), 12000, SsbOffsetEstimator::Profile::Slow);
            char name[64];
            std::snprintf(name, sizeof name, "  %.4f dial %+d", q.carrierHz / 1e6, off);
            showRow(name, truth, whole.snrDb, true, fr, sr);
            if (std::abs(off) <= 500) {
                qsoT.add(fr, sr);
                if (fr.passedWrongSign || sr.passedWrongSign) { ++qsoWrongSign; missed.push_back(std::string(q.file) + name + ": a believed WRONG-SIGN answer"); }
            } else qsoFar.add(fr, sr);
            if (off == 0 && q.carrierHz == 3720000 && std::strstr(q.file, "uber")) deep80 = x;
            if (q.carrierHz == 14282000) {
                // the 20 m voice placed 600 Hz up in an LSB-style passband: USB dial so its audio sits at +600
                if (off == 0) voice20 = ssbAudio(bb, +1, q.base - 600);
            }
        }
    }

    // ── 3. two stations in one passband ──
    std::printf(" ── two stations in one passband ──\n");
    if (!deep80.empty() && !voice20.empty()) {
        const size_t n = std::min(deep80.size(), voice20.size());
        std::vector<float> mix(n);
        for (size_t i = 0; i < n; ++i) mix[i] = deep80[i] + 0.5f * voice20[i];   // the second voice 6 dB down
        SsbOffsetEstimator est(12000, SsbOffsetEstimator::Profile::Fast);
        int believed = 0, a = 0, b = 0, neither = 0;
        for (size_t i = 0; i + kPkt <= n; i += kPkt)
            if (est.push(&mix[i], kPkt) && est.last().passes()) {
                const float d = est.last().offsetHz;
                ++believed;
                if (std::fabs(d) <= 25) ++a; else if (std::fabs(d - 600) <= 25) ++b; else ++neither;
            }
        std::printf("   80 m deep voice on tune + 20 m voice at +600 Hz, −6 dB: %d believed — %d the first (0), %d the second (+600), %d neither\n",
                    believed, a, b, neither);
        char what[160];
        std::snprintf(what, sizeof what, "two voices: every believed answer belongs to one of them (%d of %d did not)", neither, believed);
        check(believed > 0 && neither * 20 <= believed, what);   // ≤ 5 %: a blend may slip through, never a habit
    }
    for (const Qso& q : {Qso{"uber_40m_7150000_iq192.wav", 7150000, 7138000, -1, 2, "40 m LSB QSO, the other station ~105 Hz down"},
                         Qso{"uber_20m_14250000_iq192.wav", 14250000, 14246000, +1, 100, "20 m USB, two voices ~100 Hz apart"}}) {
        if (!exists(iqDir)) break;
        const auto bb = qsoBaseband(iqDir, q, 120);
        if (bb.empty()) continue;
        const auto x = ssbAudio(bb, q.sb, 0);
        SsbOffsetEstimator est(12000, SsbOffsetEstimator::Profile::Fast);
        std::vector<float> got;
        for (size_t i = 0; i + kPkt <= x.size(); i += kPkt)
            if (est.push(&x[i], kPkt) && est.last().passes()) got.push_back(est.last().offsetHz);
        std::sort(got.begin(), got.end());
        std::printf("   %.4f MHz (%s): %zu believed, 10/50/90 %% at %+.0f / %+.0f / %+.0f Hz (reported)\n", q.carrierHz / 1e6, q.note,
                    got.size(), got.empty() ? 0 : got[got.size() / 10], got.empty() ? 0 : got[got.size() / 2],
                    got.empty() ? 0 : got[got.size() * 9 / 10]);
    }

    // ── 4. Stuart's own recording (optional) ──
    if (const char* own = std::getenv("VIBE_SSB_OWNER")) {
        std::printf(" ── Stuart's in-app LSB recording: on → below 500/1000 → back on → above 500/1000 (reported) ──\n");
        std::vector<float> x;
        const std::string p = own;
        if (p.size() > 4 && p.substr(p.size() - 4) == ".wav") { int r; if (!readMono(p, x, r) || r != 12000) x.clear(); }
        else {
            const std::string cmd = "ffmpeg -loglevel error -i '" + p + "' -ac 1 -ar 12000 -f s16le - 2>/dev/null";
            if (FILE* f = popen(cmd.c_str(), "r")) {
                int16_t buf[4096]; size_t m;
                while ((m = std::fread(buf, 2, 4096, f)) > 0) for (size_t i = 0; i < m; ++i) x.push_back(buf[i] / 32768.0f);
                pclose(f);
            }
        }
        if (x.empty()) std::printf("   (could not read %s — ffmpeg on PATH?)\n", own);
        else {
            // ★ The dial moved during it and nothing told the estimator, so FAST's memory would hold each old answer for
            //   seconds (see shiftAudio): read it in 1.5 s windows with no memory instead.
            SsbOffsetEstimator::Config c = SsbOffsetEstimator::Config::forProfile(SsbOffsetEstimator::Profile::Fast);
            c.memoryVoicedSec = 0; c.windowVoicedSec = 1.5f; c.maxWindowSec = 6; c.minVoicedSec = 0.75f;
            SsbOffsetEstimator est(12000, c);
            std::printf("   t (s)  answer   conf clarity believed  (LSB: + = audio high = the dial above the station)\n");
            for (size_t i = 0; i + kPkt <= x.size(); i += kPkt)
                if (est.push(&x[i], kPkt) && est.last().valid) {
                    const auto& e = est.last();
                    std::printf("   %5.1f %+7.0f   %.2f  %.2f    %s\n", (double)i / 12000, e.offsetHz, e.confidence, e.clarity, e.passes() ? "yes" : "-");
                }
        }
    }

    steadySection(volDir, iqDir);
    arrivalSection(volDir, iqDir);
    sweepSection();

    // ── summary ──
    std::printf(" ── summary ──\n");
    train.print("VOLMET train (3 stns)");
    held.print("VOLMET held-out (8957)");
    weak.print("VOLMET weak (11253)");
    qsoT.print("QSOs ±0-500 (held-out)");
    qsoFar.print("QSOs ±1000 (reported)");
    std::printf("   cost over %.0f s of VOLMET: FAST %.3f %% of one core, SLOW %.3f %% (this machine; everything, not just analysis)\n",
                secs, 100 * fastCpu / secs, 100 * slowCpu / secs);
    std::printf("   ±1000 Hz on usable VOLMET: %d of %d within the 60 Hz target\n", k1000Met, k1000Usable);
    for (const auto& m : missed) std::printf("     · %s\n", m.c_str());
    char what[200];
    std::snprintf(what, sizeof what, "no believed WRONG-SIGN answer on any VOLMET clip (%d clips had one)", fWrongSignAny);
    check(fWrongSignAny == 0, what);
    std::snprintf(what, sizeof what, "…nor on any QSO within ±500 Hz (%d had one)", qsoWrongSign);
    check(qsoWrongSign == 0, what);
    std::snprintf(what, sizeof what, "usable VOLMET, |OFF| ≥ 100: the sign right every time (%d missed)", fNotSign);
    check(fNotSign == 0, what);
    std::snprintf(what, sizeof what, "usable VOLMET: FAST within 25 Hz to ±200, 60 Hz at ±500 (%d missed)", fAcc);
    check(fAcc == 0, what);
    std::snprintf(what, sizeof what, "usable VOLMET on tune: no believed FAST answer outside ±50 Hz once settled (%d clips had one)", fOnTune);
    check(fOnTune == 0, what);
    std::snprintf(what, sizeof what, "FAST costs well under 1 %% of a core (%.3f %% here)", 100 * fastCpu / secs);
    check(100 * fastCpu / secs < 1.0, what);

    std::printf("%s\n", fails ? "SSB OFFSET (REAL): FAILURES" : "SSB OFFSET (REAL): all passed");
    return fails ? 1 : 0;
}
