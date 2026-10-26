// test-ft2 — FT2 (IU8LMC's fast mode) on the FT8/FT4 core: decoders/ft2_decoder.h, FtxMode::FT2 (2026-10-26).
//
// ★★★ WHAT MUST HOLD.
//   1. FT2's frame at FT2's speed: real 77-bit messages (ft8_lib's encoder, FT4's tones), GFSK BT 1.0 at 288 samples a
//      symbol, decode down to the threshold with DT, frequency and SNR right — over the whole ±0.5/+1.5 s clock range.
//   2. Pure noise decodes nothing.
//   3. The live decoder cuts a slot every 3.75 s by the capture clock, and FT8 and FT4 are untouched by FT2's
//      parameters (their window and slot are what they were).
#include "decoders/ft2_decoder.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/message.h"
}

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }
static const int kFs = 12000;
static const double kSigma = 0.02;
static const int kNsps = 288;

// one FT2 transmission: FT4's tones, GFSK BT 1.0, 288 samples a symbol (the ramps left out: they carry nothing)
static void ft2Add(std::vector<double>& out, const uint8_t* tones, double f0, double t0, double snrDb) {
    const int nsps = kNsps, nn = FT4_NN;
    const double amp = std::sqrt(2.0 * kSigma * kSigma * (2500.0 / (kFs / 2.0)) * std::pow(10.0, snrDb / 10.0));
    std::vector<double> pulse((size_t)3 * nsps);
    for (int i = 0; i < 3 * nsps; ++i) {
        const double t = i / (double)nsps - 1.5;
        pulse[(size_t)i] = (std::erf(5.336446 * (t + 0.5)) - std::erf(5.336446 * (t - 0.5))) / 2;
    }
    std::vector<double> dphi((size_t)(nn + 2) * nsps, 2 * M_PI * f0 / kFs);
    for (int i = 0; i < nn; ++i)
        for (int j = 0; j < 3 * nsps; ++j) dphi[(size_t)(i * nsps + j)] += 2 * M_PI / nsps * tones[i] * pulse[(size_t)j];
    const long s0 = std::lround(t0 * kFs);
    double phi = 0;
    for (int n = 0; n < nn * nsps; ++n) {
        const long i = s0 + n;
        if (i >= 0 && i < (long)out.size()) out[(size_t)i] += amp * std::sin(phi);
        phi += dphi[(size_t)(n + nsps)];
    }
}
static std::string text(const ftx_message_t& m0) {
    ftx_message_t m = m0; char t[64] = {}; ftx_message_offsets_t o;
    ftx_message_decode(&m, nullptr, t, &o);
    return t;
}

int main() {
    std::mt19937 rng(2026);
    std::normal_distribution<double> N(0, kSigma);
    std::printf("1. FT2 on a synthetic band\n");
    Ft2Decoder* none = nullptr; (void)none;
    Ft8SlotDecoder dec(kFs, FtxMode::FT2);
    ok(dec.windowSamples() == 208 * kNsps, "the window: 208 symbols of 24 ms (4.99 s) from the slot boundary");
    Ft8SlotOptions o; o.maxPasses = 3; o.osdMaxErrors = 24; o.osdMinScore = 0;
    const double snrs[] = { -4, -8, -10, -12, -14, -16, -18 };
    std::string line; int strongHit = 0, strongN = 0, fake = 0; double dtW = 0, hzW = 0, snrW = 0; double ms = 0; int slots = 0;
    for (double snr : snrs) {
        int hit = 0, n = 0;
        for (int rep = 0; rep < 3; ++rep) {
            std::vector<double> x((size_t)dec.windowSamples(), 0.0);
            std::vector<std::string> sent; std::vector<double> hzs, dts;
            for (int k = 0; k < 4; ++k) {
                char msg[40];
                std::snprintf(msg, sizeof msg, "CQ I%d%c%c%c JN%d%d", (int)(rng() % 10), 'A' + (int)(rng() % 26), 'A' + (int)(rng() % 26),
                              'A' + (int)(rng() % 26), (int)(rng() % 10), (int)(rng() % 10));
                ftx_message_t m; ftx_message_init(&m);
                if (ftx_message_encode(&m, nullptr, msg) != FTX_MESSAGE_RC_OK) continue;
                uint8_t tones[FT4_NN]; ft4_encode(m.payload, tones);
                const double hz = 400 + 600 * k + (double)(rng() % 100);
                const double dt = -0.4 + (double)(rng() % 18) * 0.1;          // -0.4 .. +1.3 s
                ft2Add(x, tones, hz, dt, snr);
                sent.push_back(msg); hzs.push_back(hz); dts.push_back(dt);
            }
            for (double& v : x) v += N(rng);
            std::vector<float> w(x.begin(), x.end());
            std::vector<Ft8Result> res; Ft8SlotStats st;
            const auto t0 = std::chrono::steady_clock::now();
            dec.decode(w.data(), (int)w.size(), o, res, st);
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); ++slots;
            for (const Ft8Result& r : res) {
                const std::string t = text(r.msg); bool match = false;
                for (size_t k = 0; k < sent.size(); ++k) if (t == sent[k]) {
                    match = true; ++hit;
                    if (snr >= -10) { dtW = std::max(dtW, (double)std::fabs(r.dt - dts[k])); hzW = std::max(hzW, (double)std::fabs(r.hz - hzs[k]));
                                      snrW = std::max(snrW, std::fabs(r.snr + 9.0 - snr)); }
                }
                if (!match) { ++fake; std::printf("    not sent: \"%s\"\n", t.c_str()); }
            }
            n += (int)sent.size();
        }
        char b[32]; std::snprintf(b, sizeof b, " %+.0f:%d/%d", snr, hit, n); line += b;
        if (snr >= -10) { strongHit += hit; strongN += n; }
    }
    std::printf("    FT2 decoded by SNR —%s   (%.1f ms/slot, 3 passes + OSD, this machine)\n", line.c_str(), ms / slots);
    ok(strongHit == strongN, "every signal at -10 dB and above decoded, DT -0.4 … +1.3 s");
    ok(fake == 0, "no message that was not sent");
    ok(dtW < 0.02, "DT within 20 ms (worst " + std::to_string(dtW) + " s)");
    ok(hzW < 21.0, "frequency within half a tone (worst " + std::to_string(hzW) + " Hz)");
    ok(snrW <= 4.0, "SNR (+9 dB, FT4's reference scale) within 4 dB of the truth (worst " + std::to_string(snrW) + ")");

    std::printf("2. pure noise\n");
    {
        int fakeN = 0, cands = 0;
        for (int i = 0; i < 60; ++i) {
            std::vector<float> w((size_t)dec.windowSamples());
            for (float& v : w) v = (float)N(rng);
            std::vector<Ft8Result> res; Ft8SlotStats st;
            dec.decode(w.data(), (int)w.size(), o, res, st);
            cands += st.candidates[0];
            for (const Ft8Result& r : res) { ++fakeN; std::printf("    noise decoded: \"%s\"\n", text(r.msg).c_str()); }
        }
        ok(fakeN == 0, "60 noise slots, " + std::to_string(cands) + " candidates (BP + OSD on every one): 0 decodes");
    }

    std::printf("3. the live decoder, and FT8/FT4 unchanged\n");
    {
        Ft8SlotDecoder f8(kFs, FtxMode::FT8), f4(kFs, FtxMode::FT4);
        ok(f8.windowSamples() == 93 * 1920 && f4.windowSamples() == 156 * 576, "FT8's and FT4's windows are what they were");
        Ft2Decoder live(kFs);
        std::mutex mx; std::vector<std::string> got;
        live.onSpot = [&](const std::string&, const std::string& de, const std::string&, int, float) { std::lock_guard<std::mutex> lk(mx); got.push_back(de); };
        const double utc0 = 1792998000.0;
        std::vector<double> x((size_t)(32 * kFs), 0.0);
        ftx_message_t m; ftx_message_init(&m); ftx_message_encode(&m, nullptr, "CQ IU8LMC JN70");
        uint8_t tones[FT4_NN]; ft4_encode(m.payload, tones);
        for (int s = 0; s < 8; ++s) ft2Add(x, tones, 1500, s * 3.75 + 0.1, -6);
        for (double& v : x) v += N(rng);
        std::vector<float> w(x.begin(), x.end());
        for (size_t i = 0; i + 240 <= w.size(); i += 240) {
            live.process(w.data() + i, 240, utc0 + (double)(i + 240) / kFs + 0.03);
            if ((i / 240) % 50 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (int k = 0; k < 200 && live.slotsDecoded.load() < 7; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> lk(mx);
        int n = 0; for (const auto& c : got) n += c == "IU8LMC";
        std::printf("    %d spots of IU8LMC in %u slots (skipped %u)\n", n, live.slotsDecoded.load(), live.slotsSkipped.load());
        ok(n >= 7, "a spot in (nearly) every 3.75 s slot, fed as float audio");
        ok(live.slotsSkipped.load() == 0, "nothing skipped");
    }
    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
