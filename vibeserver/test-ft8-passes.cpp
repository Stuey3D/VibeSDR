// test-ft8-passes — FT8's extra decode passes, their gate, and slot timing from the capture clock (2026-10-05).
//
// ★★★ WHAT MUST HOLD. Stuart: "FT8 we have to approach with caution, that can end up being very CPU heavy." So:
//   1. a LOADED server decodes exactly as before — one pass, the same messages;
//   2. the extra passes find signals pass 1 cannot (weak ones under strong ones) and invent NONE;
//   3. the gate refuses a pass that would not end well inside the slot;
//   4. the slot is cut by when the audio was HEARD (its capture stamps), so a late decode thread or lost audio
//      does not slide it; DT and SNR come out on WSJT-X's scales.
// Synthetic band: real messages (ft8_lib's encoder), WSJT-X's GFSK, known SNR/DT/frequency, white noise.
#include "decoders/ft8_decoder.h"
#include "vibe_clock.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
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

struct Sig { std::string msg; double snr, dt, hz; };
static const int kFs = 12000;
static const double kSigma = 0.02;

static void gfskAdd(std::vector<double>& out, const uint8_t* tones, double f0, double t0, double amp, bool ft4 = false) {
    const int nsps = (int)((ft4 ? 0.048 : 0.16) * kFs), nn = ft4 ? FT4_NN : FT8_NN;
    const double bt = ft4 ? 1.0 : 2.0;
    std::vector<double> pulse((size_t)3 * nsps);
    for (int i = 0; i < 3 * nsps; ++i) {
        const double t = i / (double)nsps - 1.5;
        pulse[(size_t)i] = (std::erf(5.336446 * bt * (t + 0.5)) - std::erf(5.336446 * bt * (t - 0.5))) / 2;
    }
    std::vector<double> dphi((size_t)(nn + 2) * nsps, 2 * M_PI * f0 / kFs);
    for (int i = 0; i < nn; ++i)
        for (int j = 0; j < 3 * nsps; ++j) dphi[(size_t)(i * nsps + j)] += 2 * M_PI / nsps * tones[i] * pulse[(size_t)j];
    for (int j = 0; j < 2 * nsps; ++j) {
        dphi[(size_t)j] += 2 * M_PI / nsps * pulse[(size_t)(j + nsps)] * tones[0];
        dphi[(size_t)(j + nn * nsps)] += 2 * M_PI / nsps * pulse[(size_t)j] * tones[nn - 1];
    }
    const long s0 = std::lround(t0 * kFs);
    double phi = 0;
    for (int n = 0; n < nn * nsps; ++n) {
        const long i = s0 + n;
        if (i >= 0 && i < (long)out.size()) out[(size_t)i] += amp * std::sin(phi);
        phi += dphi[(size_t)(n + nsps)];
    }
}
/** `slots` slots of a busy band from t = 0 (a slot boundary): strong signals spread out, each with a weak one
 *  tucked 15-40 Hz above it — the case only subtraction solves. */
static std::vector<double> band(int slots, unsigned seed, std::vector<std::vector<Sig>>& truth) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> U(0, 1);
    std::vector<double> x((size_t)slots * 15 * kFs, 0.0);
    const double n2500 = kSigma * kSigma * 2500.0 / (kFs / 2.0);
    const char* pre[] = { "G", "M", "DL", "F", "EA", "I", "SP", "OK", "PA", "ON", "LA", "OH" };
    truth.assign((size_t)slots, {});
    for (int s = 0; s < slots; ++s)
        for (int i = 0; i < 20; ++i) {
            char msg[40];
            std::snprintf(msg, sizeof msg, "CQ %s%d%c%c%c %c%c%d%d", pre[rng() % 12], (int)(rng() % 10), 'A' + (int)(rng() % 26),
                          'A' + (int)(rng() % 26), 'A' + (int)(rng() % 26), 'I' + (int)(rng() % 3), 'L' + (int)(rng() % 5),
                          (int)(rng() % 10), (int)(rng() % 10));
            ftx_message_t m; ftx_message_init(&m);
            if (ftx_message_encode(&m, nullptr, msg) != FTX_MESSAGE_RC_OK) continue;
            uint8_t tones[FT8_NN]; ft8_encode(m.payload, tones);
            const bool strong = i < 10;
            const double hz = 300 + (i % 10) * 250 + (strong ? U(rng) * 50 : 15 + U(rng) * 25);
            const double snr = strong ? -6 + U(rng) * 14 : -19 + U(rng) * 6;
            const double dt = -0.4 + U(rng) * 1.0;
            gfskAdd(x, tones, hz, s * 15.0 + 0.5 + dt, std::sqrt(2 * n2500 * std::pow(10.0, snr / 10)));
            truth[(size_t)s].push_back({ msg, snr, dt, hz });
        }
    std::normal_distribution<double> N(0, kSigma);
    for (auto& v : x) v += N(rng);
    return x;
}
static std::string text(const ftx_message_t& m0) {
    ftx_message_t m = m0; char t[64] = {}; ftx_message_offsets_t o;
    ftx_message_decode(&m, nullptr, t, &o);
    return t;
}
struct Score { int hit = 0, fake = 0; double snrErr = 0, dtErr = 0; std::vector<double> weakErr, strongErr; };
static Score score(const std::vector<Ft8Result>& res, const std::vector<Sig>& truth) {
    Score sc; std::vector<double> se, de;
    for (const auto& r : res) {
        const std::string t = text(r.msg);
        const Sig* hit = nullptr;
        for (const auto& s : truth) if (s.msg == t) hit = &s;
        if (!hit) { ++sc.fake; std::printf("     not sent: \"%s\"\n", t.c_str()); continue; }
        ++sc.hit; se.push_back(r.snr - hit->snr); de.push_back(std::fabs(r.dt - hit->dt));
        (hit->snr < -10 ? sc.weakErr : sc.strongErr).push_back(r.snr - hit->snr);
    }
    if (!se.empty()) { std::sort(se.begin(), se.end()); std::sort(de.begin(), de.end()); sc.snrErr = se[se.size() / 2]; sc.dtErr = de[de.size() * 9 / 10]; }
    return sc;
}

int main() {
    std::printf("── the gate ──\n");
    ok(!Ft8Decoder::passAllowed(15000, 0, 10, 10, true), "a LOADED server never gets a second pass, however quick the first was");
    ok(Ft8Decoder::passAllowed(15000, 0, 30, 30, false), "an idle Mac: 30 ms used of 15 s — pass 2 is granted");
    ok(!Ft8Decoder::passAllowed(15000, 0, 3500, 3500, false), "a Pi 2-class first pass of 3.5 s: 3.5 + 1.5×3.5 = 8.75 s > 40 % of 15 s — refused");
    ok(!Ft8Decoder::passAllowed(15000, 11000, 700, 700, false), "a decode starting 11 s late has 4 s left: 0.7 + 1.05 s > 40 % of it — refused");
    ok(Ft8Decoder::passAllowed(7500, 0, 200, 200, false), "FT4 (7.5 s): 200 ms used — granted");

    std::printf("── passes on a busy synthetic band (strong signals with weak ones under them) ──\n");
    std::vector<std::vector<Sig>> truth;
    const int slots = 4;
    const auto x = band(slots, 11, truth);
    Ft8SlotDecoder dec(kFs, false);
    const int win = dec.windowSamples();
    ok(win == 93 * 1920, "the window is 93 symbols (14.88 s) from slot + leadSec");
    Score one, three, gated; double ms1 = 0, ms3 = 0; int passes3 = 0, identical = 0;
    for (int s = 0; s < slots; ++s) {
        auto cut = [&] {
            std::vector<float> w((size_t)win, 0.0f);
            const long i0 = std::lround((s * 15.0 + Ft8SlotDecoder::leadSec(false)) * kFs);
            for (int i = 0; i < win; ++i) if (i0 + i >= 0 && i0 + i < (long)x.size()) w[(size_t)i] = (float)x[(size_t)(i0 + i)];
            return w;
        };
        Ft8SlotOptions o1; o1.maxPasses = 1;
        Ft8SlotOptions o3; o3.maxPasses = 3;
        Ft8SlotOptions og; og.maxPasses = 3; og.mayRunPass = [](int, double, double) { return false; };   // loaded
        std::vector<Ft8Result> r1, r3, rg; Ft8SlotStats s1, s3, sg;
        auto w = cut(); dec.decode(w.data(), win, o1, r1, s1);
        w = cut(); dec.decode(w.data(), win, o3, r3, s3);
        w = cut(); dec.decode(w.data(), win, og, rg, sg);
        const Score a = score(r1, truth[(size_t)s]), b = score(r3, truth[(size_t)s]), g = score(rg, truth[(size_t)s]);
        one.hit += a.hit; one.fake += a.fake; three.hit += b.hit; three.fake += b.fake; gated.hit += g.hit; gated.fake += g.fake;
        one.snrErr += a.snrErr / slots; three.dtErr = std::max(three.dtErr, b.dtErr);
        three.weakErr.insert(three.weakErr.end(), b.weakErr.begin(), b.weakErr.end());
        three.strongErr.insert(three.strongErr.end(), b.strongErr.begin(), b.strongErr.end());
        ms1 += s1.passMs[0]; ms3 += s3.passMs[0] + s3.passMs[1] + s3.passMs[2]; passes3 += s3.passes;
        bool same = rg.size() == r1.size() && sg.passes == 1;
        for (size_t i = 0; same && i < r1.size(); ++i) same = std::memcmp(r1[i].msg.payload, rg[i].msg.payload, 10) == 0;
        identical += same;
    }
    const int sent = slots * 20;
    std::printf("     1 pass: %d of %d, %.1f ms/slot · up to 3: %d of %d, %.1f ms/slot, %.2f passes/slot\n",
                one.hit, sent, ms1 / slots, three.hit, sent, ms3 / slots, (double)passes3 / slots);
    ok(identical == slots, "a gate that says no (a loaded server) = ONE pass and exactly the single-pass messages, every slot");
    ok(one.fake == 0 && three.fake == 0, "no message decoded that was not sent (1 pass: " + std::to_string(one.fake) + ", 3: " + std::to_string(three.fake) + ")");
    ok(three.hit >= one.hit + sent / 5, "the extra passes recover the weak signals under strong ones (" + std::to_string(one.hit) + " -> " + std::to_string(three.hit) + ")");
    ok(passes3 > slots, "with signals left to find, a second pass actually ran");
    ok(three.dtErr < 0.06, "DT on WSJT-X's scale: 90 % within 60 ms of the truth (" + std::to_string(three.dtErr).substr(0, 5) + " s)");
    // ★ kSnrCal is set against jt9 on real audio; on this white-noise band that reads ~+3 dB over the textbook
    //   2500 Hz figure. What must hold is the SCALE: weak and strong signals off by the same amount (the old
    //   score*0.5-24, and a noise floor read in the signal's own bins, squashed 40 dB into 20).
    auto med = [](std::vector<double> v) { if (v.empty()) return 99.0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    const double wE = med(three.weakErr), sE = med(three.strongErr);
    ok(std::fabs(one.snrErr) <= 4.0, "SNR in 2500 Hz: median error " + std::to_string(one.snrErr).substr(0, 5) + " dB (calibrated to jt9)");
    ok(std::fabs(wE - sE) <= 2.0, "...and the same error for weak (< -10 dB: " + std::to_string(wE).substr(0, 5) + ") and strong signals (" + std::to_string(sE).substr(0, 5) + ")");

    std::printf("── FT4: the same passes on its 7.5 s slots ──\n");
    {
        std::mt19937 rng(5); std::uniform_real_distribution<double> U(0, 1);
        const int n4 = 4;
        std::vector<double> y((size_t)(n4 * 7.5 * kFs), 0.0);
        const double n2500 = kSigma * kSigma * 2500.0 / (kFs / 2.0);
        std::vector<std::vector<Sig>> t4((size_t)n4);
        for (int s = 0; s < n4; ++s)
            for (int i = 0; i < 16; ++i) {
                char msg[40];
                std::snprintf(msg, sizeof msg, "CQ G%d%c%c%c IO%d%d", (int)(rng() % 10), 'A' + (int)(rng() % 26), 'A' + (int)(rng() % 26),
                              'A' + (int)(rng() % 26), (int)(rng() % 10), (int)(rng() % 10));
                ftx_message_t m; ftx_message_init(&m);
                if (ftx_message_encode(&m, nullptr, msg) != FTX_MESSAGE_RC_OK) continue;
                uint8_t tones[FT4_NN]; ft4_encode(m.payload, tones);
                const bool strong = i < 8;
                const double hz = 400 + (i % 8) * 330 + (strong ? U(rng) * 40 : 40 + U(rng) * 40);
                const double snr = strong ? -4 + U(rng) * 10 : -14 + U(rng) * 4;
                gfskAdd(y, tones, hz, s * 7.5 + 0.5 + (-0.2 + U(rng) * 0.6), std::sqrt(2 * n2500 * std::pow(10.0, snr / 10)), true);
                t4[(size_t)s].push_back({ msg, snr, 0, hz });
            }
        std::normal_distribution<double> N(0, kSigma);
        for (auto& v : y) v += N(rng);
        Ft8SlotDecoder d4(kFs, true);
        Score a, b;
        for (int s = 0; s < n4; ++s) {
            for (int passes : { 1, 3 }) {
                std::vector<float> w((size_t)d4.windowSamples(), 0.0f);
                const long i0 = std::lround((s * 7.5 + Ft8SlotDecoder::leadSec(true)) * kFs);
                for (int i = 0; i < d4.windowSamples(); ++i) if (i0 + i >= 0 && i0 + i < (long)y.size()) w[(size_t)i] = (float)y[(size_t)(i0 + i)];
                Ft8SlotOptions o; o.maxPasses = passes;
                std::vector<Ft8Result> r; Ft8SlotStats st;
                d4.decode(w.data(), d4.windowSamples(), o, r, st);
                const Score sc = score(r, t4[(size_t)s]);
                Score& acc = passes == 1 ? a : b;
                acc.hit += sc.hit; acc.fake += sc.fake;
            }
        }
        std::printf("     FT4: 1 pass %d of %d · up to 3: %d\n", a.hit, n4 * 16, b.hit);
        ok(a.fake == 0 && b.fake == 0, "FT4: no message decoded that was not sent");
        ok(b.hit > a.hit, "FT4: subtraction recovers signals pass 1 missed (" + std::to_string(a.hit) + " -> " + std::to_string(b.hit) + ")");
    }

    std::printf("── the live decoder: capture stamps, the load probe ──\n");
    auto live = [&](bool loaded, double dropAt, unsigned& reanch, int& passes) {
        std::set<std::string> got;
        Ft8Decoder d(kFs, false);
        d.loadProbe = [loaded] { return loaded; };
        std::mutex m;
        d.onSpot = [&](const std::string& to, const std::string& de, const std::string& g, int, float) {
            std::lock_guard<std::mutex> lk(m); got.insert(to + " " + de + " " + g); };
        // ★ Stamps as if heard in real time from a slot boundary; `dropAt` s in, 2 s of audio are LOST (the
        //   stamps jump), and the audio after the drop is the band's next slot, on time.
        const double t0 = std::floor(vibeUtcNow() / 15.0) * 15.0 + 15.0;
        std::vector<int16_t> pcm(x.size());
        for (size_t i = 0; i < x.size(); ++i) pcm[i] = (int16_t)std::max(-32768.0, std::min(32767.0, std::round(x[i] * 32767.0)));
        size_t i = 0; double lost = 0;
        while (i < pcm.size()) {
            const int n = (int)std::min<size_t>(240, pcm.size() - i);
            if (dropAt > 0 && lost == 0 && (double)i / kFs >= dropAt) { lost = 2.0; i += (size_t)(2.0 * kFs); continue; }
            d.process(pcm.data() + i, n, t0 + (double)(i + (size_t)n) / kFs);
            i += (size_t)n;
            if (i % (size_t)(15 * kFs) < 240) {   // let each slot's decode finish before the next (no skip)
                const long want = (long)(i / (size_t)(15 * kFs)) - (lost > 0 ? 1 : 0);
                for (int w = 0; w < 500 && (long)d.slotsDecoded.load() < want; ++w)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        for (int w = 0; w < 300 && d.slotsDecoded.load() + d.slotsSkipped.load() < (unsigned)slots - (dropAt > 0 ? 1u : 0u); ++w)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        reanch = d.reanchors.load(); passes = d.lastPasses.load();
        return got.size();
    };
    unsigned ra = 0; int pa = 0, pb = 0;
    const size_t loadedN = live(true, 0, ra, pa);
    const size_t idleN = live(false, 0, ra, pb);
    std::printf("     loaded: %zu spots, last decode %d pass(es) · idle: %zu spots, %d pass(es)\n", loadedN, pa, idleN, pb);
    ok(pa == 1, "a loaded server: the live decoder runs ONE pass");
    ok(pb >= 2, "an idle server: the live decoder runs the extra passes");
    ok(idleN > loadedN, "...and they find more stations");
    ok(loadedN >= (size_t)one.hit * 9 / 10, "the live path (stamped ring buffer) decodes what the slot decoder does on the same audio");
    const size_t dropN = live(false, 20.0, ra, pb);
    ok(ra == 1, "2 s of LOST audio re-anchors the slot clock once");
    ok(dropN >= idleN / 2, "...and the slots after the drop still decode (" + std::to_string(dropN) + " spots)");

    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
