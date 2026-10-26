// dsc_bench — the DSC receiver's measuring bench (scripts/test-dsc.sh builds and runs it; vibeserver/test-dsc.cpp holds
// the pass/fail checks).
//
//   dsc_bench formats            every call of dsc_synth.h once, clean, HF and VHF — the line each one reads as
//   dsc_bench snr [trials]       decode rate against SNR (3 kHz), every format, HF and VHF; + Rayleigh fading on HF
//   dsc_bench noise <minutes>    pure noise, HF and VHF: false calls (must be none) and false syncs
//   dsc_bench cpu [seconds]      CPU per second of audio, HF and VHF, with calls in the noise
//   dsc_bench wav <file.wav> [centreHz]   a recording (mono int16, 12/24/48 kHz) through the MF/HF receiver
//
// ★ Silent: nothing is played. Deterministic: every draw is seeded.
#include "dsc_synth.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace vibe::dsc;
using namespace dscsynth;

namespace {
struct Result { int ok = 0, corrected = 0, unconfirmed = 0, wrong = 0, wrongUnconf = 0, missed = 0; };

/** Decode one rendered call; classify against what was sent. */
FILE* g_csv = nullptr;
void classify(const TestCall& tc, const std::vector<Call>& got, Result& r, bool verbose = false) {
    if (got.empty()) { ++r.missed; return; }
    const Call& c = got.front();
    const bool same = c.symbols == tc.syms;
    if (g_csv) std::fprintf(g_csv, "%d,%d,%d,%.3f,%.3f,%d,%.3f\n", same ? 1 : 0, (int)c.check, c.corrected, c.fixCost, c.fixGap, c.doubt, c.quality);
    if (!same) {
        if (c.check == Call::Check::Failed) ++r.wrongUnconf; else ++r.wrong;
        if (verbose || std::getenv("DSC_VERBOSE")) {
            std::string d;
            for (size_t i = 0; i < std::max(c.symbols.size(), tc.syms.size()); ++i) {
                const int a = i < tc.syms.size() ? tc.syms[i] : -1, b = i < c.symbols.size() ? c.symbols[i] : -1;
                if (a != b) d += " [" + std::to_string(i) + "]" + std::to_string(a) + "->" + std::to_string(b);
            }
            std::printf("   WRONG (%s, %s, sent %zu got %zu):%s\n", tc.name.c_str(), c.check == Call::Check::Ok ? "ok" : c.check == Call::Check::Corrected ? "corrected" : "unconf",
                        tc.syms.size(), c.symbols.size(), d.c_str());
        }
        return;
    }
    if (c.check == Call::Check::Failed) ++r.unconfirmed;
    else if (c.check == Call::Check::Corrected) ++r.corrected;
    else ++r.ok;
}

std::vector<Call> run(const std::vector<float>& audio, bool vhf, Receiver::Stats* st = nullptr) {
    RxConfig cfg; cfg.vhf = vhf; cfg.rate = 48000;
    Receiver rx(cfg);
    std::vector<Call> got;
    rx.onCall = [&](const Call& c) { got.push_back(c); };
    for (size_t i = 0; i < audio.size(); i += 4800) rx.process(audio.data() + i, (int)std::min<size_t>(4800, audio.size() - i));
    // flush: a second of silence-level noise lets the last slots complete
    std::vector<float> z(48000, 0.0f);
    rx.process(z.data(), (int)z.size());
    if (st) *st = rx.stats();
    return got;
}

int formats() {
    for (int band = 0; band < 2; ++band) {
        const bool vhf = band == 1;
        std::printf("── %s, clean\n", vhf ? "VHF ch 70" : "MF/HF");
        Rng rng(1);
        for (const auto& tc : calls()) {
            const auto bits = build::transmission(tc.syms, tc.exp, vhf ? 20 : 200);
            const auto a = render(bits, vhf, 30, rng);
            const auto got = run(a, vhf);
            bool ok = got.size() == 1 && got[0].symbols == tc.syms;
            std::string line = got.empty() ? "(nothing)" : describe(got[0]);
            for (const auto& e : tc.expect) if (line.find(e) == std::string::npos) { ok = false; std::printf("   missing \"%s\"\n", e.c_str()); }
            std::printf("%s %-26s %s\n", ok ? "ok  " : "FAIL", tc.name.c_str(), line.c_str());
        }
    }
    return 0;
}

int snr(int trials) {
    const auto cs = calls();
    for (int cond = 0; cond < 3; ++cond) {
        const bool vhf = cond == 2;
        const double fade = cond == 1 ? 0.5 : 0;
        std::printf("── %s%s: %d trials per format per SNR (%zu formats) — ok / corrected / unconfirmed / WRONG / missed\n",
                    vhf ? "VHF ch 70" : "MF/HF", fade > 0 ? ", Rayleigh fade 0.5 Hz" : "", trials, cs.size());
        const std::vector<double> snrs = vhf ? std::vector<double>{ 6, 3, 0, -2, -4, -6 }
                                    : fade > 0 ? std::vector<double>{ 0, -3, -6, -9, -12 }
                                               : std::vector<double>{ -3, -6, -8, -10, -12, -14 };
        for (double sn : snrs) {
            Result r;
            Rng rng((uint64_t)(1000 + sn * 10 + cond * 7));
            for (int t = 0; t < trials; ++t)
                for (const auto& tc : cs) {
                    const auto bits = build::transmission(tc.syms, tc.exp, vhf ? 20 : 200);
                    const auto a = render(bits, vhf, sn, rng, 0.3 + rng.uni(), 0.6, fade, 1700, (rng.uni() - 0.5) * 60);
                    classify(tc, run(a, vhf), r);
                }
            const int n = trials * (int)cs.size();
            std::printf("  %+5.0f dB  decoded %5.1f %%   (%d ok, %d corrected, %d unconfirmed, %d WRONG, %d wrong but marked unconfirmed, %d missed)\n", sn,
                        100.0 * (r.ok + r.corrected + r.unconfirmed) / n, r.ok, r.corrected, r.unconfirmed, r.wrong, r.wrongUnconf, r.missed);
        }
    }
    return 0;
}

int noise(double minutes) {
    for (int band = 0; band < 2; ++band) {
        const bool vhf = band == 1;
        RxConfig cfg; cfg.vhf = vhf;
        Receiver rx(cfg);
        int calls = 0;
        rx.onCall = [&](const Call& c) { ++calls; std::printf("   FALSE CALL: %s\n", describe(c).c_str()); };
        Rng rng(77 + band);
        std::vector<float> buf(48000);
        const long secs = (long)(minutes * 60);
        for (long s = 0; s < secs; ++s) {
            for (auto& v : buf) v = (float)(0.3 * rng.gauss());
            rx.process(buf.data(), (int)buf.size());
        }
        std::printf("%s: %.0f min of noise → %d calls, %lu phasing syncs (all rejected), %lu formats read\n",
                    vhf ? "VHF" : "MF/HF", minutes, calls, rx.stats().falseSyncs, rx.stats().syncs);
    }
    return 0;
}

int cpu(double seconds) {
    for (int band = 0; band < 2; ++band) {
        const bool vhf = band == 1;
        Rng rng(5);
        // a call every ~10 s in noise at a fair SNR
        std::vector<float> a;
        const auto cs = calls();
        size_t k = 0;
        while (a.size() < (size_t)(seconds * 48000)) {
            const auto bits = build::transmission(cs[k % cs.size()].syms, cs[k % cs.size()].exp, vhf ? 20 : 200);
            const auto r = render(bits, vhf, vhf ? 6 : -4, rng, 2.0, 2.0);
            a.insert(a.end(), r.begin(), r.end());
            ++k;
        }
        RxConfig cfg; cfg.vhf = vhf;
        Receiver rx(cfg);
        int n = 0; rx.onCall = [&](const Call&) { ++n; };
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < a.size(); i += 1024) rx.process(a.data() + i, (int)std::min<size_t>(1024, a.size() - i));
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double audioS = a.size() / 48000.0;
        std::printf("%s: %.0f s of audio (%zu calls sent, %d decoded) in %.3f s → %.3f %% of one core\n",
                    vhf ? "VHF" : "MF/HF", audioS, k, n, el, 100.0 * el / audioS);
    }
    return 0;
}

int wav(const char* path, double centre) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4)) { std::fprintf(stderr, "not a WAV\n"); return 1; }
    // walk the chunks
    size_t p = 12; uint32_t rate = 0; uint16_t ch = 1; const int16_t* pcm = nullptr; size_t ns = 0;
    while (p + 8 <= d.size()) {
        const uint32_t len = *(const uint32_t*)(d.data() + p + 4);
        if (!std::memcmp(d.data() + p, "fmt ", 4)) { ch = *(const uint16_t*)(d.data() + p + 10); rate = *(const uint32_t*)(d.data() + p + 12); }
        if (!std::memcmp(d.data() + p, "data", 4)) { pcm = (const int16_t*)(d.data() + p + 8); ns = std::min<size_t>(len, d.size() - p - 8) / 2; break; }
        p += 8 + len;
    }
    if (!pcm || !rate) { std::fprintf(stderr, "no data\n"); return 1; }
    RxConfig cfg; cfg.vhf = false; cfg.rate = rate; cfg.centreHz = centre;
    Receiver rx(cfg);
    double pos = 0;
    rx.onCall = [&](const Call& c) {
        std::printf("%8.1f s  [%s q%.2f%s]  %s\n", pos, c.check == Call::Check::Ok ? "ok" : c.check == Call::Check::Corrected ? "corrected" : "UNCONFIRMED",
                    c.quality, c.disagreed ? (" dx/rx differ " + std::to_string(c.disagreed)).c_str() : "", describe(c).c_str());
        std::fflush(stdout);
    };
    std::vector<float> buf(4096);
    const size_t frames = ns / ch;
    for (size_t i = 0; i < frames; i += buf.size()) {
        const size_t n = std::min(buf.size(), frames - i);
        for (size_t k = 0; k < n; ++k) buf[k] = pcm[(i + k) * ch] / 32768.0f;
        rx.process(buf.data(), (int)n);
        pos = (double)(i + n) / rate;
    }
    std::vector<float> z(rate, 0.0f); rx.process(z.data(), (int)z.size());
    const auto& s = rx.stats();
    std::printf("%s: %.0f s · %lu calls (%lu corrected, %lu unconfirmed, %lu mirrored) · %lu formats read · %lu syncs rejected\n",
                path, frames / (double)rate, s.calls, s.corrected, s.failed, s.inverted, s.syncs, s.falseSyncs);
    return 0;
}
} // namespace

int miss(bool vhf, double sn, int trials, double fade) {
    const auto cs = calls();
    int m = 0, noFmt = 0, fmtNoCall = 0;
    std::map<std::string, int> per;
    Rng rng(4242);
    for (int t = 0; t < trials; ++t)
        for (const auto& tc : cs) {
            const auto bits = build::transmission(tc.syms, tc.exp, vhf ? 20 : 200);
            const auto a = render(bits, vhf, sn, rng, 0.3 + rng.uni(), 0.6, fade, 1700, (rng.uni() - 0.5) * 60);
            Receiver::Stats st;
            const auto got = run(a, vhf, &st);
            if (got.empty()) { ++m; per[tc.name]++; if (st.syncs == 0) ++noFmt; else ++fmtNoCall; }
        }
    std::printf("missed %d: %d never read a format, %d read one but made no call\n", m, noFmt, fmtNoCall);
    for (auto& kv : per) std::printf("   %-28s %d\n", kv.first.c_str(), kv.second);
    return 0;
}

int main(int argc, char** argv) {
    const std::string m = argc > 1 ? argv[1] : "formats";
    if (m == "formats") return formats();
    if (std::getenv("DSC_CSV")) g_csv = std::fopen(std::getenv("DSC_CSV"), "w");
    if (m == "snr") return snr(argc > 2 ? std::atoi(argv[2]) : 10);
    if (m == "noise") return noise(argc > 2 ? std::atof(argv[2]) : 10);
    if (m == "miss" && argc > 3) return miss(std::atoi(argv[2]) != 0, std::atof(argv[3]), argc > 4 ? std::atoi(argv[4]) : 20, argc > 5 ? std::atof(argv[5]) : 0);
    if (m == "cpu") return cpu(argc > 2 ? std::atof(argv[2]) : 120);
    if (m == "wav" && argc > 2) return wav(argv[2], argc > 3 ? std::atof(argv[3]) : 1700);
    std::fprintf(stderr, "usage: dsc_bench formats|snr [trials]|noise <min>|cpu [s]|wav <file> [centre]\n");
    return 2;
}
