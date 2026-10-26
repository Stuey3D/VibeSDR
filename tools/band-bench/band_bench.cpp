// band_bench — DIGITAL SPOTS: BAND over a real IQ recording, for CPU and instruction counts (V12, 2026-10-27).
//
//   band_bench <iq.wav> <start_utc> <centreHz> [secs=N] [gate] [budget=U] [drain] [light]
//
// Every registered mode (decoders/digi_modes.cpp) through the real Spotter, tuners and engines, as the server's decode
// thread runs them. `gate` hands each block the slice's FFT bins' power, as the server does (decoders/digi_activity.h) —
// computed BEFORE the clock starts, since the server reads them off the forward FFT it already has. `drain` waits after
// every half second of audio for the engines' workers to go quiet, so a machine slower than real time (qemu-arm: the
// Pi 2's build) decodes every slot instead of skipping them — the instruction count is then the WORK, not a race.
// `light` tells every engine it is loaded (one pass: what the governor picks first when a box is short).
// Prints the spots per mode, each mode's gate periods, and the process CPU per second of recording.
#include "decoders/digi_activity.h"
#include "decoders/digi_spotter.h"
#include "vibe_clock.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

using cf32 = std::complex<float>;
using namespace vibe;

static double cpuSecs() {
    rusage u{}; getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}
static void fftInPlace(std::vector<cf32>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / (double)len; const cf32 wl((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len) { cf32 w(1, 0); for (size_t j = 0; j < len / 2; ++j) { const cf32 u = a[i + j], v = a[i + j + len / 2] * w; a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl; } }
    }
}
/** Wait until no other thread of this process is spending CPU (the engines' slot workers have finished). */
static void quiet() {
    double last = cpuSecs(); int calm = 0;
    for (int i = 0; i < 6000 && calm < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double now = cpuSecs();
        calm = (now - last) < 0.002 ? calm + 1 : 0;
        last = now;
    }
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: band_bench <iq.wav> <start_utc> <centreHz> [secs=N] [gate] [budget=U] [drain] [light]\n"); return 2; }
    double secs = 1e9, budget = INFINITY; bool gate = false, drain = false, light = false;
    for (int i = 4; i < argc; ++i) {
        if (!std::strncmp(argv[i], "secs=", 5)) secs = std::atof(argv[i] + 5);
        else if (!std::strncmp(argv[i], "budget=", 7)) budget = std::atof(argv[i] + 7);
        else if (!std::strcmp(argv[i], "gate")) gate = true;
        else if (!std::strcmp(argv[i], "drain")) drain = true;
        else if (!std::strcmp(argv[i], "light")) light = true;
    }
    const double startUtc = std::atof(argv[2]), centre = std::atof(argv[3]);
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 3; }
    uint8_t hdr[4096]; const size_t hn = std::fread(hdr, 1, sizeof hdr, f);
    uint32_t rate = 0; uint16_t chans = 0; size_t data = 0, dlen = 0;
    for (size_t p = 12; p + 8 <= hn;) {
        const uint32_t len = hdr[p + 4] | hdr[p + 5] << 8 | hdr[p + 6] << 16 | (uint32_t)hdr[p + 7] << 24;
        if (!std::memcmp(&hdr[p], "fmt ", 4)) { chans = hdr[p + 10] | hdr[p + 11] << 8; rate = hdr[p + 12] | hdr[p + 13] << 8 | hdr[p + 14] << 16 | (uint32_t)hdr[p + 15] << 24; }
        if (!std::memcmp(&hdr[p], "data", 4)) { data = p + 8; dlen = len; break; }
        p += 8 + len + (len & 1);
    }
    if (chans != 2 || !rate || !data) { std::fprintf(stderr, "need stereo int16 I/Q\n"); return 1; }
    const size_t n = std::min<size_t>(dlen / 4, (size_t)(secs * rate));
    std::vector<int16_t> raw(n * 2);
    std::fseek(f, (long)data, SEEK_SET);
    const size_t got = std::fread(raw.data(), 4, n, f);
    std::fclose(f);
    std::vector<cf32> x(got);
    for (size_t i = 0; i < got; ++i) x[i] = cf32(raw[2 * i] / 32768.0f, raw[2 * i + 1] / 32768.0f);
    raw.clear(); raw.shrink_to_fit();

    const int N = 512, hop = N * 3 / 4;
    std::vector<float> bins;
    if (gate) {
        std::vector<cf32> hist((size_t)N, cf32(0, 0)), tmp; std::vector<float> pw((size_t)N);
        bins.reserve((got / hop + 1) * (size_t)N);
        for (size_t o = 0; o + (size_t)hop <= got; o += (size_t)hop) {
            std::copy(hist.begin() + hop, hist.end(), hist.begin());
            std::copy(&x[o], &x[o] + hop, hist.end() - hop);
            tmp = hist; fftInPlace(tmp);
            digi::slicePower(tmp.data(), N, 0, N, pw.data());
            bins.insert(bins.end(), pw.begin(), pw.end());
        }
    }
    std::map<std::string, int> per; std::map<std::string, std::set<std::string>> uniq; std::mutex gm;
    digi::Spotter sp(digi::Spotter::Scope::Band, digi::modes(), [&](const digi::Spot& s, double) {
        std::lock_guard<std::mutex> lk(gm); per[s.mode]++; uniq[s.mode].insert(s.call); }, nullptr, [light] { return light; });
    sp.setBudgetCap(budget);
    const double c0 = cpuSecs();
    const auto w0 = std::chrono::steady_clock::now();
    double nextDrain = 0.5;
    for (size_t o = 0, b = 0; o + (size_t)hop <= got; o += (size_t)hop, ++b) {
        const double t = (double)(o + hop) / rate;
        sp.feedIq(&x[o], hop, rate, centre, startUtc + t, gate ? &bins[b * (size_t)N] : nullptr, gate ? N : 0, (double)rate / N);
        if (drain && t >= nextDrain) { quiet(); nextDrain += 0.5; }
        else if (!drain && b % 64 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    quiet();
    const double used = cpuSecs() - c0, len = (double)got / rate;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    std::printf("%s: %.0f s, gate %s, budget %s, light %s — CPU %.3f s = %.3f %% of one core (wall %.1f s), slots %llu decoded / %llu skipped\n",
                argv[1], len, gate ? "on" : "off", std::isfinite(budget) ? std::to_string(budget).c_str() : "none", light ? "yes" : "no", used,
                100.0 * used / len, wall, (unsigned long long)sp.slotsDecoded(), (unsigned long long)sp.slotsSkipped());
    for (auto& kv : per) std::printf("  %-6s %4d spots %3zu calls\n", kv.first.c_str(), kv.second, uniq[kv.first].size());
    for (auto& m : sp.status())
        std::printf("  %-5s %-6s periods live %llu idle %llu shed %llu\n", m.mode.c_str(), digi::stateWord(m.state),
                    (unsigned long long)m.live, (unsigned long long)m.idle, (unsigned long long)m.shed);
    return 0;
}
