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
        bt.setSource(sb > 0 ? BinauralTuning::SsbUsb : BinauralTuning::SsbLsb, 50, 300);
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

int main() {
    std::printf("SSB mistune estimator — real off-air audio\n");
    const char* rootEnv = std::getenv("VIBE_TEST_AUDIO");
    const std::string root = rootEnv ? rootEnv : "";
    const std::string volDir = root + "/ssb/offsets", iqDir = root + "/iq";
    if (!rootEnv || !exists(volDir + "/shannon-5505")) {
        std::printf("   NOT RUN — set VIBE_TEST_AUDIO to the folder holding ssb/offsets/ and iq/ (~1 GB, not in the repo)\n");
        return 3;
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
