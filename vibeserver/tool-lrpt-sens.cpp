// tool-lrpt-sens.cpp — METEOR LRPT sensitivity curve: frames decoded vs Es/N0 on our own transmitter (vibe_lrpt_tx.h),
// with what a real pass has — the radio's fixed frequency error, the satellite's Doppler slope fed forward as the
// satellite engine does, a symbol-clock error and oscillator phase noise. A DEVELOPER TOOL (docs/v12/LRPT-SENSITIVITY.md).
//
//   tool-lrpt-sens [--profile o72d|o72|q72|q72d|o80i] [--es 1.0:5.0:0.5] [--seeds 4] [--periods 12] [--pn 0.1] [--slope -25] [--no-feed] [--fs 300000]
//                  [--threads 6] [--scalar]
//
// Prints one line per Es/N0: frames (VCDUs passing RS) and picture blocks as % of what was sent, then the Es/N0 at the
// 50 % frame point (linear interpolation between the two points either side).
//
// Build: c++ -std=c++17 -O3 -I android/app/src/main/cpp vibeserver/tool-lrpt-sens.cpp -o tool-lrpt-sens -lpthread
#include "vibe_lrpt_tx.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace vibelrpt;

namespace {
struct Opt {
    std::string profile = "o72d";
    double es0 = 1.0, es1 = 5.0, esStep = 0.5;
    int seeds = 4, periods = 12, threads = 6;
    double pn = 0.1, fs = 300000, ppm = 20;
    double slope = -25;     ///< Doppler slope, Hz/s
    bool noFeed = false;    ///< no satellite engine: the receiver tracks the whole Doppler itself
    bool scalar = false;
};
struct Res { uint64_t frames = 0, sent = 0, blocks = 0, wantBlocks = 0; double cpu = 0, air = 0; };

Res runOne(const Opt& o, double es, uint32_t seed) {
    constexpr double kResid = -3340, kDop0 = 585; const double kRate = o.slope;   // the 2026-11-22 pass (LRPT-LIVE-PASS.md)
    tx::TxConfig t; t.sampleRate = o.fs; t.esn0Db = es; t.seed = seed; t.phaseNoiseHz = o.pn; t.symbolPpm = o.ppm;
    t.carrierHz = kResid + kDop0; t.dopplerRateHzS = kRate;
    ReceiverConfig rc; rc.sampleRate = o.fs; rc.forceScalar = o.scalar;
    if (o.profile == "q72") { t.oqpsk = false; t.diff = false; rc.mod = ReceiverConfig::Qpsk; }
    else if (o.profile == "q72d") { t.oqpsk = false; t.diff = true; rc.mod = ReceiverConfig::Qpsk; }
    else if (o.profile == "o72") { t.oqpsk = true; t.diff = false; rc.mod = ReceiverConfig::Oqpsk; }
    else if (o.profile == "o80i") { t.oqpsk = true; t.diff = true; t.symbolRate = 80000; t.interleaved = true; rc.mod = ReceiverConfig::Oqpsk; rc.symbolRate = 80000; rc.interleaved = true; }
    else { t.oqpsk = true; t.diff = true; rc.mod = ReceiverConfig::Oqpsk; }
    const auto v = tx::buildVcdus(t, o.periods);
    const auto iq = tx::modulate(t, tx::codeBits(t, v), 1.0, 0.5);
    Receiver rx(rc);
    const size_t blk = size_t(o.fs / 20); double tt = -1.0;
    Res r;
    for (size_t at = 0; at < iq.size() / 2; at += blk) {
        const size_t n = std::min(blk, iq.size() / 2 - at);
        rx.setDopplerHz(o.noFeed ? 0.0 : kDop0 + kRate * tt);
        const auto t0 = std::chrono::steady_clock::now();
        rx.feedIq(&iq[at * 2], n);
        r.cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        tt += double(n) / o.fs;
        if (std::getenv("LRPT_TRACE")) std::printf("  t %5.2f carrier %8.1f true %8.1f lock %.2f acq %llu ok %llu fail %llu\n", tt, rx.carrierHz(), kResid + kDop0 + kRate * tt,
                                               rx.lockMetric(), (unsigned long long)rx.acquisitions(), (unsigned long long)rx.stats().rsOk, (unsigned long long)rx.stats().rsFail);
    }
    r.air = double(iq.size() / 2) / o.fs;
    r.frames = rx.stats().rsOk; r.sent = v.size();
    r.blocks = rx.stats().blocks; r.wantBlocks = uint64_t(o.periods) * 3 * kMcuPerLine;
    return r;
}
}  // namespace

int main(int argc, char** argv) {
    Opt o;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--profile") o.profile = val();
        else if (a == "--es") { const std::string s = val(); std::sscanf(s.c_str(), "%lf:%lf:%lf", &o.es0, &o.es1, &o.esStep); if (s.find(':') == std::string::npos) o.es1 = o.es0; }
        else if (a == "--seeds") o.seeds = std::atoi(val().c_str());
        else if (a == "--periods") o.periods = std::atoi(val().c_str());
        else if (a == "--threads") o.threads = std::max(1, std::atoi(val().c_str()));
        else if (a == "--pn") o.pn = std::atof(val().c_str());
        else if (a == "--fs") o.fs = std::atof(val().c_str());
        else if (a == "--ppm") o.ppm = std::atof(val().c_str());
        else if (a == "--scalar") o.scalar = true;
        else if (a == "--slope") o.slope = std::atof(val().c_str());
        else if (a == "--no-feed") o.noFeed = true;
        else { std::fprintf(stderr, "unknown %s\n", a.c_str()); return 2; }
    }
    std::vector<double> esList;
    for (double e = o.es0; e <= o.es1 + 1e-9; e += o.esStep) esList.push_back(e);
    struct Job { double es; uint32_t seed; Res r; };
    std::vector<Job> jobs;
    for (double e : esList) for (int s = 1; s <= o.seeds; s++) jobs.push_back({ e, uint32_t(s), {} });
    std::atomic<size_t> next{0};
    std::vector<std::thread> th;
    for (int k = 0; k < o.threads; k++) th.emplace_back([&] { for (size_t j; (j = next++) < jobs.size();) jobs[j].r = runOne(o, jobs[j].es, jobs[j].seed); });
    for (auto& t : th) t.join();
    std::printf("profile %s, %.0f S/s, %d periods × %d seeds, phase noise %.1f Hz, clock %.0f ppm, Doppler slope %.0f Hz/s%s\n", o.profile.c_str(), o.fs, o.periods, o.seeds, o.pn, o.ppm, o.slope, o.noFeed ? " (not fed forward)" : " (fed forward)");
    double prevEs = 0, prevF = -1, at50 = 0; double cpu = 0, air = 0;
    for (double e : esList) {
        Res s;
        for (auto& j : jobs) if (j.es == e) { s.frames += j.r.frames; s.sent += j.r.sent; s.blocks += j.r.blocks; s.wantBlocks += j.r.wantBlocks; cpu += j.r.cpu; air += j.r.air; }
        const double f = 100.0 * double(s.frames) / double(std::max<uint64_t>(1, s.sent));
        const double b = 100.0 * double(s.blocks) / double(std::max<uint64_t>(1, s.wantBlocks));
        std::printf("  Es/N0 %4.1f dB: frames %5.1f %% (%llu/%llu)  blocks %5.1f %%\n", e, f, (unsigned long long)s.frames, (unsigned long long)s.sent, b);
        if (at50 == 0 && prevF >= 0 && prevF < 50 && f >= 50) at50 = prevEs + (50 - prevF) / (f - prevF) * (e - prevEs);
        if (at50 == 0 && prevF < 0 && f >= 50) at50 = -e;   // already above 50 % at the first point
        prevEs = e; prevF = f;
    }
    if (at50 > 0) std::printf("50 %% frame point: Es/N0 %.2f dB\n", at50);
    else if (at50 < 0) std::printf("50 %% frame point: below %.1f dB\n", -at50);
    else std::printf("50 %% frame point: above %.1f dB\n", o.es1);
    std::printf("cpu %.2f %% of one core per receiver (real time)\n", 100 * cpu / std::max(1e-9, air));
    return 0;
}
