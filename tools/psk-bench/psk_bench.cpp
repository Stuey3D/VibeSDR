// psk_bench.cpp — the PSK skimmer's measuring bench (scripts/test-psk.sh builds it twice: SIMD and VIBE_FORCE_SCALAR).
//   psk_bench accuracy | one <baud> <snrDb> <iq|audio> [hz] | multi | drift | noise <minutes> | cpu <seconds>
//            | wav <file.wav> [centreHz] [widthHz] | clip <out.raw> <rate> <seconds>
// Silent: nothing is played. The pass/fail checks are vibeserver/test-psk.cpp; this prints the numbers for the docs.
#include "psk_synth.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

using namespace psksynth;
using vibe::psk::Skimmer;

static double threadCpu() {
    timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9;
}
struct Run {
    std::map<int, std::string> text;
    std::map<int, double> hz;
    std::vector<vibe::psk::Spot> spots;
    void attach(Skimmer& sk) {
        sk.onText = [this](int id, double off, const std::string& t) { text[id] += t; hz[id] = off; };
        sk.onSpot = [this](const vibe::psk::Spot& s) { spots.push_back(s); };
    }
    std::string near(double f, double tol = 15) const {
        std::string o; for (auto& kv : text) if (std::fabs(hz.at(kv.first) - f) < tol) o += kv.second; return o;
    }
    size_t chars() const { size_t n = 0; for (auto& kv : text) n += kv.second.size(); return n; }
};
static Skimmer::Config cfgFor(bool iq, double rate) {
    Skimmer::Config c; c.rate = rate; c.complexIn = iq;
    if (iq) { c.loHz = -rate * 0.47; c.hiHz = rate * 0.47; c.maxSignals = 64; } else { c.loHz = 200; c.hiHz = 3600; }
    if (std::getenv("PSK_ONLY31")) c.psk63 = false;
    return c;
}
static void feed(Skimmer& sk, bool iq, const std::vector<cf>& x, const std::vector<float>& r, int block = 1024) {
    const size_t n = iq ? x.size() : r.size();
    for (size_t i = 0; i < n; i += (size_t)block) {
        const int k = (int)std::min<size_t>((size_t)block, n - i);
        if (iq) sk.feedIq(x.data() + i, k); else sk.feedReal(r.data() + i, k);
    }
}
static std::string allText() { std::string t; for (const char* q : kQso) t += q; return t; }

/** One signal: CER of the decode. */
static double oneCer(double baud, double snr, bool iq, double hz, uint64_t seed, std::string* got = nullptr, double drift = 0) {
    Rng rng(seed);
    const double rate = iq ? 48000 : 12000;
    Sig s; s.hz = hz; s.baud = baud; s.snrDb = snr; s.text = allText(); s.driftHzPerSec = drift; s.startSec = 3.0;   // ★ after the engine's own 1.7 s warm-up: steady-state CER
    std::vector<Sig> v{ s };
    std::vector<cf> x; std::vector<float> r;
    synth(v, rate, iq, duration(s) + 3.0, rng, x, r, std::getenv("PSK_NOISE") ? std::atof(std::getenv("PSK_NOISE")) : 1.0);
    Skimmer sk(cfgFor(iq, rate)); Run run; run.attach(sk);
    feed(sk, iq, x, r);
    const std::string t = run.near(hz + drift * duration(s) / 2, 40 + std::fabs(drift) * duration(s));
    if (got) *got = t;
    return cer(s.text, t);
}

static int one(double baud, double snr, bool iq, double hz) {
    std::string t;
    const double c = oneCer(baud, snr, iq, hz, std::getenv("PSK_SEED") ? std::atoi(std::getenv("PSK_SEED")) : 1, &t);
    std::printf("%s %.2f Bd at %+.1f dB (2.5 kHz), %.0f Hz: CER %.1f %%\n---\n%s\n---\n", iq ? "IQ" : "audio", baud, snr, hz, c * 100, t.c_str());
    return 0;
}

static int accuracy() {
    std::printf("PSK accuracy — CER (mean of 3 draws), SNR in 2.5 kHz [%s]\n", Skimmer::simdName());
    const double snrs[] = { 10, 0, -5, -8, -10, -12, -13, -14 };
    for (double baud : { 31.25, 62.5 }) for (int iq = 0; iq < 2; ++iq) {
        std::printf("%-6s %-5s", baud < 40 ? "PSK31" : "PSK63", iq ? "IQ48" : "audio");
        for (double snr : snrs) {
            const double s2 = baud < 40 ? snr : snr + 3;   // ★ PSK63 needs 3 dB more for the same Es/N0
            double sum = 0; for (int k = 0; k < 3; ++k) sum += oneCer(baud, s2, iq, iq ? 1700 : 1000 + 37 * k, 11 + k);
            std::printf("  %+3.0f:%5.1f%%", s2, sum / 3 * 100);
        }
        std::printf("\n");
    }
    return 0;
}

static int offsets() {
    std::printf("PSK31 offsets (IQ 48 kHz, -8 dB): ");
    for (double hz : { -20000.0, -9001.3, -100.0, 0.0, 37.7, 4321.9, 19876.5 }) std::printf(" %+.0f:%.1f%%", hz, oneCer(31.25, -8, true, hz, 5) * 100);
    std::printf("\nPSK31 audio (−8 dB): ");
    for (double hz : { 300.0, 523.4, 1500.0, 2788.8, 3300.0 }) std::printf(" %.0f:%.1f%%", hz, oneCer(31.25, -8, false, hz, 6) * 100);
    std::printf("\n");
    return 0;
}

static int drift() {
    std::printf("PSK31 drift (IQ 48 kHz, -5 dB):");
    for (double d : { 0.0, 0.05, 0.2, 0.5, 1.0, 2.0 }) std::printf("  %.2f Hz/s:%.1f%%", d, oneCer(31.25, -5, true, 1200, 7, nullptr, d) * 100);
    std::printf("\n");
    return 0;
}

/** ★ A crowded passband: 14 signals in 3 kHz, PSK31 and PSK63, one pair 31 Hz apart, one +20 dB above the rest. */
static int multi(bool verbose) {
    Rng rng(42);
    const double rate = 12000;
    std::vector<Sig> v;
    const char* calls[] = { "g4abc","dl1xyz","f5mgu","oh3uq","ea7xx","i2abc","sp9zz","ok1rr","ha5kk","yo3aa","9a1cd","lz2ef","s51gh","om3ij" };
    double hz = 420;
    for (int i = 0; i < 14; ++i) {
        Sig s; s.baud = (i % 4 == 3) ? 62.5 : 31.25;
        s.hz = hz; hz += (i == 4) ? 31.25 : (s.baud > 40 ? 230 : 160);
        s.snrDb = -6 + (i % 3) * 3; if (i == 9) s.snrDb = 14;
        s.text = std::string("cq cq de ") + calls[i] + " " + calls[i] + " " + calls[i] + " pse k\n" + kQso[1];
        s.startSec = 0.5 + 0.7 * i; s.driftHzPerSec = (i % 5 == 2) ? 0.1 : 0;
        v.push_back(s);
    }
    double dur = 0; for (auto& s : v) dur = std::max(dur, s.startSec + duration(s));
    std::vector<cf> x; std::vector<float> r;
    synth(v, rate, false, dur + 3, rng, x, r);
    Skimmer sk(cfgFor(false, rate)); Run run; run.attach(sk);
    feed(sk, false, x, r);
    int good = 0, spotted = 0, falseSpots = 0;
    std::printf("crowded passband: %zu signals, 12 kHz audio\n", v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        const std::string t = run.near(v[i].hz, 12);
        const double c = cer(v[i].text, t);
        bool sp = false; for (auto& s : run.spots) if (s.call == std::string(calls[i]).substr(0) || s.call == [&]{ std::string u = calls[i]; for (auto& ch : u) ch = (char)std::toupper(ch); return u; }()) sp = true;
        good += c < 0.05; spotted += sp;
        std::printf("  %7.2f Hz %s %+3.0f dB  CER %5.1f %%  %s\n", v[i].hz, v[i].baud > 40 ? "PSK63" : "PSK31", v[i].snrDb, c * 100, sp ? "spotted" : "");
        if (verbose && c >= 0.05) std::printf("     got: %s\n", squash(t).substr(0, 160).c_str());
    }
    for (auto& s : run.spots) {
        bool ok = false; for (auto* c : calls) { std::string u = c; for (auto& ch : u) ch = (char)std::toupper(ch); ok |= s.call == u; }
        if (!ok) { ++falseSpots; std::printf("  FALSE SPOT %s at %.1f\n", s.call.c_str(), s.offsetHz); }
    }
    std::printf("  %d/%zu decoded under 5 %% CER, %d spotted, %d false spots\n", good, v.size(), spotted, falseSpots);
    return 0;
}

/** ★ Pure noise. `stress`: the carrier finder's bar dropped to 0.4 dB, so channels open on noise all the time and only
 *  the text gate stands between them and the list. */
/** IMD: a strong PSK31 signal whose idles are measured, transmitters driven harder and harder. */
static int imd() {
    std::printf("IMD (+10 dB in 2.5 kHz, audio; PSK63 +13), the transmitter driven harder: engine / reference\n");
    for (double k : { 0.0, 0.05, 0.1, 0.2, 0.35 }) for (double baud : { 31.25, 62.5 }) {
        Rng rng(5);
        Sig s; s.hz = 1200; s.baud = baud; s.snrDb = 10 + (baud > 40 ? 3 : 0); s.cubic = k; s.startSec = 2.5; s.idleBefore = 160; s.idleAfter = 64;
        s.text = kQso[0];   // ★ the 5 s idle before it is what is measured
        std::vector<Sig> v{ s };
        std::vector<cf> x; std::vector<float> r;
        synth(v, 12000, false, duration(s) + 3, rng, x, r);
        Skimmer sk(cfgFor(false, 12000)); Run run; run.attach(sk);
        feed(sk, false, x, r);
        float got = 0; for (auto& sg : sk.signals(true)) got = sg.imdDb;
        std::printf("  %s cubic %.2f: %6.1f dB / %6.1f dB\n", baud > 40 ? "PSK63" : "PSK31", k, got, cubicImdDb(k));
    }
    return 0;
}

static int noise(double minutes, bool stress) {
    for (int iq = 0; iq < 2; ++iq) {
        const double rate = iq ? 48000 : 12000;
        Skimmer::Config c = cfgFor(iq, rate);
        if (stress) c.candidateDb = 0.4;
        Skimmer sk(c); Run run; run.attach(sk);
        Rng rng(99 + iq);
        const size_t blk = (size_t)rate;   // a second at a time
        std::vector<cf> x(blk); std::vector<float> r(blk);
        const double sg = std::sqrt(0.5);
        for (int sec = 0; sec < (int)(minutes * 60); ++sec) {
            for (size_t i = 0; i < blk; ++i) { if (iq) x[i] = cf((float)(sg * rng.gauss()), (float)(sg * rng.gauss())); else r[i] = (float)rng.gauss(); }
            feed(sk, iq, x, r);
        }
        std::printf("noise %.0f min%s, %s: %zu chars, %zu spots, %zu signals listed; tentatives dropped %llu, chars suppressed %llu, floor cal %.3f\n",
                    minutes, stress ? " (stress: channels forced open)" : "", iq ? "IQ 48 kHz" : "audio 12 kHz", run.chars(), run.spots.size(), sk.signals(true).size(),
                    (unsigned long long)sk.tentativesDropped(), (unsigned long long)sk.charsSuppressed(), sk.noiseCalibration());
        if (run.chars()) for (auto& kv : run.text) std::printf("   [%d] %s\n", kv.first, kv.second.substr(0, 120).c_str());
    }
    return 0;
}

static int cpu(double seconds) {
    std::printf("CPU [%s], %.0f s of a busy band (a PSK31 signal every 150 Hz, -3 dB):\n", Skimmer::simdName(), seconds);
    for (double rate : { 12000.0, 24000.0, 48000.0, 96000.0 }) {
        const bool iq = rate > 12000;
        Rng rng(3);
        std::vector<Sig> v;
        const double lo = iq ? -rate * 0.45 : 300, hi = iq ? rate * 0.45 : 3400;
        for (double f = lo; f < hi; f += 150) { Sig s; s.hz = f; s.snrDb = -3; s.text = allText(); s.startSec = 0.2; v.push_back(s); }
        std::vector<cf> x; std::vector<float> r;
        synth(v, rate, iq, seconds, rng, x, r);
        Skimmer::Config c = cfgFor(iq, rate); c.maxSignals = 400;
        Skimmer sk(c); Run run; run.attach(sk);
        const double t0 = threadCpu();
        feed(sk, iq, x, r, 2048);
        const double used = threadCpu() - t0;
        std::printf("  %s %6.0f Hz: %3zu signals, %3d confirmed at the end, %.3f %% of a core (FFT %d, bin %.2f Hz, channel %.1f Hz)\n",
                    iq ? "IQ   " : "audio", rate, v.size(), sk.confirmed(), used / seconds * 100, sk.fftSize(), sk.binHz(), sk.channelRate());
    }
    return 0;
}

static bool readWav(const char* path, int& rate, int& ch, std::vector<int16_t>& pcm) {
    FILE* f = std::fopen(path, "rb"); if (!f) return false;
    char hdr[12]; if (std::fread(hdr, 1, 12, f) != 12) { std::fclose(f); return false; }
    rate = 0; ch = 0;
    for (;;) {
        char id[4]; uint32_t sz;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&sz, 4, 1, f) != 1) break;
        if (!std::memcmp(id, "fmt ", 4)) { std::vector<uint8_t> b(sz); if (std::fread(b.data(), 1, sz, f) != sz) break;
            ch = b[2] | (b[3] << 8); rate = (int)(b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24)); }
        else if (!std::memcmp(id, "data", 4)) { pcm.resize(sz / 2); const size_t got = std::fread(pcm.data(), 2, pcm.size(), f); pcm.resize(got); break; }
        else std::fseek(f, sz, SEEK_CUR);
    }
    std::fclose(f);
    return rate > 0 && ch > 0;
}

static int wav(const char* path, double centre, double width) {
    int rate, ch; std::vector<int16_t> pcm;
    if (!readWav(path, rate, ch, pcm)) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
    const bool iq = ch == 2;
    Skimmer::Config c = cfgFor(iq, rate);
    if (iq && width > 0) { c.loHz = -width / 2; c.hiHz = width / 2; }
    if (std::getenv("PSK_LO")) c.loHz = std::atof(std::getenv("PSK_LO"));   // ★ a sub-range, for looking at one signal
    if (std::getenv("PSK_HI")) c.hiHz = std::atof(std::getenv("PSK_HI"));
    c.maxSignals = 96;
    Skimmer sk(c); Run run; run.attach(sk);
    const size_t frames = pcm.size() / (size_t)ch;
    const double t0 = threadCpu();
    std::vector<cf> x; std::vector<float> r;
    for (size_t i = 0; i < frames; i += 4096) {
        const size_t k = std::min<size_t>(4096, frames - i);
        if (iq) { x.resize(k); for (size_t j = 0; j < k; ++j) x[j] = cf(pcm[2 * (i + j)] / 32768.0f, pcm[2 * (i + j) + 1] / 32768.0f); sk.feedIq(x.data(), (int)k); }
        else { r.resize(k); for (size_t j = 0; j < k; ++j) r[j] = pcm[i + j] / 32768.0f; sk.feedReal(r.data(), (int)k); }
    }
    const double used = threadCpu() - t0, secs = (double)frames / rate;
    std::printf("%s: %.0f s, %d Hz %s, CPU %.3f %% of a core (FFT %d)\n", path, secs, rate, iq ? "IQ" : "audio", used / secs * 100, sk.fftSize());
    for (auto& s : sk.signals(true)) {
        std::printf("--- #%d %.3f kHz  BPSK%d  Es/N0 %.1f dB  %s  IMD %s  %s  %u chars\n", s.id, (centre + s.offsetHz) / 1000.0, s.baud, s.snrDb,
                    vibe::psk::qualityWord(s.quality), s.imdDb < 0 ? std::to_string((int)std::lround(s.imdDb)).c_str() : "—", s.call.c_str(), s.chars);
        std::string t = run.text.count(s.id) ? run.text[s.id] : s.text;
        for (auto& ch2 : t) if (ch2 == '\n') ch2 = '|';
        std::printf("%s\n", t.c_str());
    }
    std::printf("spots:");
    for (auto& s : run.spots) std::printf(" %s(%s %.3f)", s.call.c_str(), s.how.c_str(), (centre + s.offsetHz) / 1000.0);
    std::printf("\n");
    return 0;
}

/** ★ A synthetic HF clip for the server/web tests (scripts/test-server-psk.mjs, test-web-psk.mjs): what a fake rtl_tcp
 *  replays — unsigned 8-bit IQ at `rate` (960 kHz), centred on 14.0710 MHz — five PSK stations calling CQ, at the
 *  offsets below (one PSK63). Written as it is made (no whole-clip buffers: 40 s at 960 kHz is 38 M samples). */
static const struct ClipSt { double off; const char* call; double baud, snr; } kClip[] = {
    { -3000, "g4abc", 31.25, 4 }, { -1100, "dl1xyz", 31.25, 6 }, { 700, "ea7xx", 31.25, 8 }, { 2300, "ok1rr", 62.5, 9 }, { 4100, "sp9zz", 31.25, 5 } };
static int clip(const char* out, double rate, double seconds) {
    Rng rng(2026);
    struct Live { std::vector<int8_t> d; double T, amp, t0; std::complex<double> ph, step; };
    std::vector<Live> L;
    for (const ClipSt& c : kClip) {
        std::string t; while (t.size() < seconds * c.baud / 6) t += std::string("cq cq cq de ") + c.call + " " + c.call + " " + c.call + " pse k\n";
        const std::vector<int> b = bits(t, 40, 0);
        Live l; l.T = 1.0 / c.baud;
        l.d.assign(b.size() + 2, 0);               // d[0] = 0: the ramp up; a zero bit flips the sign
        int8_t cur = 1; for (size_t k = 0; k < b.size(); ++k) { if (!b[k]) cur = (int8_t)-cur; l.d[k + 1] = cur; }
        // mean power so that P / (noise in 2500 Hz) = snr (noise power 1 over `rate`); the envelope's mean square ≈ 0.75
        l.amp = std::sqrt(std::pow(10.0, c.snr / 10.0) * 2500.0 / rate / 0.75);
        l.t0 = 0.3 + rng.uni();
        l.ph = std::polar(1.0, rng.uni() * 2 * M_PI); l.step = std::polar(1.0, 2 * M_PI * c.off / rate);
        L.push_back(l);
    }
    FILE* f = std::fopen(out, "wb"); if (!f) { std::perror(out); return 1; }
    const size_t n = (size_t)(seconds * rate), blk = 65536;
    std::vector<uint8_t> buf(blk * 2);
    const double sg = std::sqrt(0.5);
    auto q = [](double v) { const int x = (int)std::lround(127.5 + v * 18.0); return (uint8_t)std::max(0, std::min(255, x)); };
    for (size_t i0 = 0; i0 < n; i0 += blk) {
        const size_t k = std::min(blk, n - i0);
        for (size_t j = 0; j < k; ++j) {
            const double t = (double)(i0 + j) / rate;
            double re = sg * rng.gauss(), im = sg * rng.gauss();
            for (auto& l : L) {
                l.ph *= l.step;
                const double u = (t - l.t0) / l.T;      // symbols since the start: centre k at u = k
                if (u < 0) continue;
                const size_t kk = (size_t)u;
                if (kk + 1 >= l.d.size()) continue;
                const double w = 0.5 * (1 + std::cos(M_PI * (u - (double)kk)));
                const double a = l.amp * (l.d[kk] * w + l.d[kk + 1] * (1 - w));
                re += a * l.ph.real(); im += a * l.ph.imag();
            }
            buf[2 * j] = q(re); buf[2 * j + 1] = q(im);
        }
        for (auto& l : L) l.ph /= std::abs(l.ph);
        std::fwrite(buf.data(), 1, 2 * k, f);
    }
    std::fclose(f);
    std::printf("%s: %.0f s at %.0f Hz, %zu stations\n", out, seconds, rate, L.size());
    return 0;
}

/** ★ Decode a clip made by `clip` (as the server's skimmer would see it: a 12 kHz slice) — the clip's own check. */
static int clipCheck(const char* path, double rate) {
    FILE* f = std::fopen(path, "rb"); if (!f) return 1;
    // a 48 kHz slice by a plain 20:1 boxcar (the bench only: the server cuts its slice from its own FFT)
    const int D = (int)std::lround(rate / 48000);
    Skimmer sk(cfgFor(true, rate / D)); Run run; run.attach(sk);
    std::vector<uint8_t> b(2 * 48000 * D); std::vector<cf> x;
    size_t got;
    while ((got = std::fread(b.data(), 1, b.size(), f)) > 0) {
        x.clear();
        for (size_t i = 0; i + 2 * D <= got; i += 2 * D) {
            cf a(0, 0); for (int k = 0; k < D; ++k) a += cf((b[i + 2 * k] - 127.5f) / 18.0f, (b[i + 2 * k + 1] - 127.5f) / 18.0f);
            x.push_back(a / (float)D);
        }
        sk.feedIq(x.data(), (int)x.size());
    }
    std::fclose(f);
    for (auto& s : sk.signals(true)) std::printf("  %+7.1f Hz BPSK%d %s: %s\n", s.offsetHz, s.baud, s.call.c_str(), squash(s.text).substr(0, 60).c_str());
    return 0;
}

int main(int argc, char** argv) {
    const std::string m = argc > 1 ? argv[1] : "accuracy";
    if (m == "accuracy") return accuracy();
    if (m == "offsets") return offsets();
    if (m == "pt" && argc > 4) {   // ★ one point, mean of 6 draws (for tuning)
        const bool iq = std::string(argv[4]) == "iq"; double sum = 0;
        for (int k = 0; k < 6; ++k) {
            std::string got;
            const double c = oneCer(std::atof(argv[2]), std::atof(argv[3]), iq, iq ? 1700 + 311 * k : 1000 + 37 * k, 21 + k, &got);
            sum += c;
            if (std::getenv("PSK_SHOW")) std::printf("  draw %d: %.1f %%  %s\n", k, c * 100, squash(got).substr(0, 200).c_str());
        }
        std::printf("%s %s %s: %.1f %%\n", argv[2], argv[3], argv[4], sum / 6 * 100); return 0;
    }
    if (m == "one" && argc > 4) return one(std::atof(argv[2]), std::atof(argv[3]), std::string(argv[4]) == "iq", argc > 5 ? std::atof(argv[5]) : 1000);
    if (m == "multi") return multi(argc > 2);
    if (m == "drift") return drift();
    if (m == "imd") return imd();
    if (m == "drift1" && argc > 2) { std::string t; const double c = oneCer(31.25, -5, true, 1200, 7, &t, std::atof(argv[2])); std::printf("CER %.1f%%\n%s\n", c * 100, t.c_str()); return 0; }
    if (m == "noise") return noise(argc > 2 ? std::atof(argv[2]) : 5, argc > 3);
    if (m == "cpu") return cpu(argc > 2 ? std::atof(argv[2]) : 30);
    if (m == "wav" && argc > 2) return wav(argv[2], argc > 3 ? std::atof(argv[3]) : 0, argc > 4 ? std::atof(argv[4]) : 0);
    if (m == "clip" && argc > 4) return clip(argv[2], std::atof(argv[3]), std::atof(argv[4]));
    if (m == "clipcheck" && argc > 3) return clipCheck(argv[2], std::atof(argv[3]));
    std::fprintf(stderr, "usage: psk_bench accuracy|offsets|one <baud> <snr> <iq|audio> [hz]|multi [v]|drift|noise <min>|cpu <s>|wav <f> [centre] [width]|clip <out> <rate> <s>\n");
    return 2;
}
