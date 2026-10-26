// ftx_sweep — the FT8 / FT4 / FT2 decoder's sensitivity on a synthetic band with KNOWN answers, SNR by SNR, and its
// false decodes on pure noise.
//
// ★★ WHY (2026-10-26, the sensitivity work). "Decodes only ever go up" is judged twice: on real audio against a
//   WSJT-X-class reference (ft8_wav + the UberSDR's stream), and here, where every signal sent is known — so a decode
//   is either one of them or FALSE. The figure of merit is the SNR at which half the signals decode (WSJT-X's 2500 Hz
//   definition), and the false count must stay 0.
//   Signals: real 77-bit messages of every common kind (CQ, call+grid, reports, R-reports, RR73, 73) from random
//   standard calls, GFSK exactly as transmitted (BT 2 FT8, BT 1 FT4/FT2), random start (DT), random frequency (not on a
//   bin), random phase, in white noise; NON-overlapping (spaced ≥ 4 signal widths), so this measures the detector, not
//   the subtraction.
//
//   ftx_sweep [--ft4|--ft2] [--snr A:B:STEP] [--slots N] [--sigs K] [--seed S] [--noise SLOTS]
//             [--passes N] [--osd N] [--cands N] [--minscore N] [--fine 0|1] [--ap 0|1] [--aptable N] [-v]
//     --noise SLOTS   pure noise only: every decode is false (with --aptable N the AP call table holds N random calls)
//   Build: tools/ft8-bench/build.sh
#include "decoders/ft8_decoder.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/message.h"
}

using namespace vibe;

static const int kFs = 12000;
static const double kSigma = 0.02;

static void gfskAdd(std::vector<float>& out, const uint8_t* tones, int nn, int nsps, double bt, double f0, double t0,
                    double amp, double phase0) {
    const double k = 5.336446;
    std::vector<double> pulse((size_t)3 * nsps);
    for (int i = 0; i < 3 * nsps; ++i) {
        const double t = i / (double)nsps - 1.5;
        pulse[(size_t)i] = (std::erf(k * bt * (t + 0.5)) - std::erf(k * bt * (t - 0.5))) / 2;
    }
    const int nw = nn * nsps;
    std::vector<double> dphi((size_t)(nw + 2 * nsps), 2 * M_PI * f0 / kFs);
    const double peak = 2 * M_PI / nsps;
    for (int i = 0; i < nn; ++i)
        for (int j = 0; j < 3 * nsps; ++j) dphi[(size_t)(i * nsps + j)] += peak * tones[i] * pulse[(size_t)j];
    for (int j = 0; j < 2 * nsps; ++j) {
        dphi[(size_t)j] += peak * pulse[(size_t)(j + nsps)] * tones[0];
        dphi[(size_t)(j + nw)] += peak * pulse[(size_t)j] * tones[nn - 1];
    }
    const long s0 = std::lround(t0 * kFs);
    const int ramp = nsps / 8;
    double phi = phase0;
    for (int n = 0; n < nw; ++n) {
        double env = 1;
        if (n < ramp) env = (1 - std::cos(M_PI * n / ramp)) / 2;
        if (n >= nw - ramp) env = (1 - std::cos(M_PI * (nw - 1 - n) / ramp)) / 2;
        const long i = s0 + n;
        if (i >= 0 && i < (long)out.size()) out[(size_t)i] += (float)(amp * env * std::sin(phi));
        phi += dphi[(size_t)(n + nsps)];
    }
}

static std::string randCall(std::mt19937& rng) {
    static const char* pre[] = { "G", "M", "DL", "F", "EA", "I", "SP", "OK", "PA", "ON", "LA", "OH", "SM", "K", "W", "VE", "JA", "UA", "R", "YO" };
    char c[16];
    const int suf = 1 + (int)(rng() % 3);
    std::snprintf(c, sizeof c, "%s%d", pre[rng() % 20], (int)(rng() % 10));
    std::string s = c;
    for (int i = 0; i < suf; ++i) s += (char)('A' + rng() % 26);
    return s;
}
static std::string randGrid(std::mt19937& rng) {
    char g[5] = { (char)('A' + rng() % 18), (char)('A' + rng() % 18), (char)('0' + rng() % 10), (char)('0' + rng() % 10), 0 };
    return g;
}
static std::string randMsg(std::mt19937& rng) {
    const std::string a = randCall(rng), b = randCall(rng);
    char rep[8]; std::snprintf(rep, sizeof rep, "%+03d", -24 + (int)(rng() % 30));
    switch (rng() % 6) {
    case 0: return "CQ " + a + " " + randGrid(rng);
    case 1: return a + " " + b + " " + randGrid(rng);
    case 2: return a + " " + b + " " + rep;
    case 3: return a + " " + b + " R" + rep;
    case 4: return a + " " + b + " RR73";
    default: return a + " " + b + " 73";
    }
}
static std::string text(const ftx_message_t& m0) {
    ftx_message_t m = m0; char t[64] = {}; ftx_message_offsets_t o;
    if (ftx_message_decode(&m, nullptr, t, &o) != FTX_MESSAGE_RC_OK) return "?";
    return t;
}

int main(int argc, char** argv) {
    FtxMode mode = FtxMode::FT8;
    double snrA = -24, snrB = -14, snrStep = 1; int slots = 20, sigs = 10, noiseSlots = 0; unsigned seed = 1; bool verbose = false;
    Ft8SlotOptions o; o.maxPasses = 1; o.osdMaxErrors = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto nxt = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--ft4") mode = FtxMode::FT4;
        else if (a == "--ft2") mode = FtxMode::FT2;
        else if (a == "--snr") { const std::string s = nxt(); std::sscanf(s.c_str(), "%lf:%lf:%lf", &snrA, &snrB, &snrStep); }
        else if (a == "--slots") slots = std::atoi(nxt().c_str());
        else if (a == "--sigs") sigs = std::atoi(nxt().c_str());
        else if (a == "--seed") seed = (unsigned)std::atoi(nxt().c_str());
        else if (a == "--noise") noiseSlots = std::atoi(nxt().c_str());
        else if (a == "--passes") o.maxPasses = std::atoi(nxt().c_str());
        else if (a == "--osd") o.osdMaxErrors = std::atoi(nxt().c_str());
        else if (a == "--cands") o.maxCandidates = std::atoi(nxt().c_str());
        else if (a == "--minscore") o.minScore = std::atoi(nxt().c_str());
        else if (a == "--fine") o.fine = std::atoi(nxt().c_str());
        else if (a == "--stall") o.bpStall = std::atoi(nxt().c_str());
        else if (a == "--osd2") o.osdPairSpan = std::atoi(nxt().c_str());
        else if (a == "--finesync") o.fineMinSync = std::atoi(nxt().c_str());
        else if (a == "-v") verbose = true;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    const bool ft4 = mode != FtxMode::FT8;
    const int nn = ft4 ? FT4_NN : FT8_NN;
    const double T = mode == FtxMode::FT2 ? kFt2SymbolPeriod : ft4 ? 0.048 : 0.16;
    const int nsps = (int)std::lround(T * kFs);
    const double bt = ft4 ? 1.0 : 2.0;
    const double width = (ft4 ? 4 : 8) / T;                    // Hz the signal occupies
    const double dtNom = mode == FtxMode::FT2 ? 0.0 : 0.5;     // nominal start in the window
    const double dtLo = mode == FtxMode::FT2 ? -0.3 : -0.5, dtHi = mode == FtxMode::FT2 ? 1.2 : 1.0;
    Ft8SlotDecoder dec(kFs, mode);
    const int W = dec.windowSamples();
    std::mt19937 rng(seed);
    std::normal_distribution<double> N(0, kSigma);
    std::uniform_real_distribution<double> U(0, 1);
    const double n2500 = kSigma * kSigma * 2500.0 / (kFs / 2.0);
    const char* name = mode == FtxMode::FT2 ? "FT2" : ft4 ? "FT4" : "FT8";

    int fineHits = 0;
    auto runSlot = [&](std::vector<float>& x, const std::set<std::string>& truth, int& hit, int& fals, double& ms) {
        std::vector<Ft8Result> res; Ft8SlotStats st;
        const auto t0 = std::chrono::steady_clock::now();
        dec.decode(x.data(), W, o, res, st);
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::set<std::string> seen;
        for (const Ft8Result& r : res) {
            const std::string t = text(r.msg);
            if (!seen.insert(t).second) continue;
            if (truth.count(t)) { ++hit; if (r.fine) ++fineHits; }
            else { ++fals; if (verbose || truth.empty()) std::printf("   FALSE %s (snr %d dt %.2f hz %.1f pass %d%s)\n", t.c_str(), r.snr, r.dt, r.hz, r.pass, r.osd ? " osd" : ""); }
        }
    };

    if (noiseSlots > 0) {
        int hit = 0, fals = 0; double ms = 0;
        std::set<std::string> none;
        for (int s = 0; s < noiseSlots; ++s) {
            std::vector<float> x((size_t)W);
            for (float& v : x) v = (float)N(rng);
            runSlot(x, none, hit, fals, ms);
        }
        std::printf("%s noise: %d slots, %d false decodes, %.1f ms/slot\n", name, noiseSlots, fals, ms / noiseSlots);
        return 0;
    }
    std::printf("%s: %d signals per SNR (%d slots x %d)\n", name, slots * sigs, slots, sigs);
    for (double snr = snrA; snr <= snrB + 1e-9; snr += snrStep) {
        int hit = 0, fals = 0, sent = 0; double ms = 0; fineHits = 0;
        rng.seed(seed * 1000u + (unsigned)std::lround((snr + 100) * 10));   // ★ the same signals at an SNR, whatever the range
        for (int s = 0; s < slots; ++s) {
            std::vector<float> x((size_t)W);
            for (float& v : x) v = (float)N(rng);
            std::set<std::string> truth;
            const double span = 3000.0 - 200.0 - width;
            for (int k = 0; k < sigs; ++k) {
                ftx_message_t m; ftx_message_init(&m);
                const std::string msg = randMsg(rng);
                if (ftx_message_encode(&m, nullptr, msg.c_str()) != FTX_MESSAGE_RC_OK) continue;
                uint8_t tones[FT4_NN];
                if (ft4) ft4_encode(m.payload, tones); else ft8_encode(m.payload, tones);
                const double hz = 200.0 + span * (k + 0.1 + 0.8 * U(rng)) / sigs;
                const double dt = dtLo + (dtHi - dtLo) * U(rng);
                const double amp = std::sqrt(2 * n2500 * std::pow(10.0, snr / 10));
                gfskAdd(x, tones, nn, nsps, bt, hz, dtNom + dt, amp, 2 * M_PI * U(rng));
                truth.insert(text(m));
                ++sent;
            }
            runSlot(x, truth, hit, fals, ms);
        }
        std::printf("%s SNR %+5.1f: %4d/%4d  %5.1f %%  false %d  (second look %d)  %.1f ms/slot\n", name, snr, hit, sent, 100.0 * hit / std::max(1, sent), fals, fineHits, ms / slots);
        std::fflush(stdout);
    }
    return 0;
}
