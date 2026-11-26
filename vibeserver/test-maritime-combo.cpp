// test-maritime-combo.cpp — V12 beta96 THE MARITIME COMBINATION (vibe_maritime_combo.h): AIS + VHF DSC channel 70 from one
// wide capture. docs/v12/MARITIME-COMBO.md.
//
// ★★★ WHAT IS PINNED (synthetic IQ, every bit known):
//   1. the geometry: the centre between channel 70 and AIS, the capture it needs (~6.17 MS/s), and the rule per radio —
//      offered at an RSP's 8, an Airspy R2's 10, a HackRF's 8/10/20, a USRP's 8+; NOT at an RTL's 2.4/2.56/3.2, an HF+,
//      an RSP's 2/6, an Airspy Mini's 6 or 3, an R2's 2.5 (the no-dead-control rule: not drawn there at all);
//   2. AIS bursts on BOTH channels and a DSC distress alert on channel 70, in noise, at 8 MS/s (no half-band) and 20 MS/s (a
//      half-band stage first): every ship's MMSI and the call (its MMSI, position, nature) decoded;
//   3. the same through vibeais::Service on its own thread (the shim's path): the ships in the report, the report's
//      "combo" / "dsc" fields, the DSC gallery hook called, finish() at stop; the service's thread at decoder priority;
//   4. pure noise → 0 AIS frames, 0 DSC calls;
//   5. CPU: the combination at 8 MS/s and 20 MS/s against AIS alone at 2.4 MS/s (one core of this machine, single thread).
// ★ Silent (no audio device). Bounded: IQ is generated block by block; the one 1 s buffer (the CPU run) is freed.
#include "vibe_maritime_combo.h"
#include "vibe_hdlc.h"
#include "../tools/dsc-bench/dsc_synth.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using cf32 = vibedsp::cf32;
static int fails = 0, checks = 0;
static void ok(bool c, const std::string& what) { ++checks; if (!c) ++fails; std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); std::fflush(stdout); }

// ── the transmitters ────────────────────────────────────────────────────────────────────────────────────────────────
/** An AIS type 1 position report (M.1371 Annex 8) as GMSK BT 0.4 at 9600 Bd (Annex 2), `off` Hz from the radio's centre. */
static std::vector<cf32> aisBurst(uint32_t mmsi, double lat, double lon, double fs, double off, float amp) {
    std::vector<int> bits;
    auto u = [&](uint64_t v, int len) { for (int i = len - 1; i >= 0; --i) bits.push_back(int((v >> i) & 1)); };
    auto s = [&](int64_t v, int len) { u(uint64_t(v) & ((1ull << len) - 1), len); };
    u(1, 6); u(0, 2); u(mmsi, 30); u(0, 4); s(0, 8); u(123, 10); u(1, 1);
    s(std::lround(lon * 600000), 28); s(std::lround(lat * 600000), 27); u(2456, 12); u(247, 9); u(30, 6); u(0, 2); u(0, 3); u(0, 1); u(0x1234, 19);
    std::vector<uint8_t> msg((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i) if (bits[i]) msg[i / 8] |= uint8_t(1 << (7 - i % 8));
    vibehdlc::appendFcs(msg);
    std::vector<int> air;
    for (int i = 0; i < 24; ++i) air.push_back(i & 1);
    vibehdlc::flagBits(air); vibehdlc::stuffBits(msg, air); vibehdlc::flagBits(air);
    for (int i = 0; i < 8; ++i) air.push_back(1);
    std::vector<int> lv; int level = 0; vibehdlc::nrzi(air, lv, level);
    const double sps = fs / 9600.0, BT = 0.4, sg = std::sqrt(std::log(2.0)) / (2 * M_PI * BT), kk = std::sqrt(2.0) * sg;
    const size_t len = size_t((lv.size() + 6) * sps);
    std::vector<cf32> x(len);
    double ph = 0;
    for (size_t i = 0; i < len; ++i) {
        const double t = i / sps - 3.0;
        double f = 0;
        const long kc = long(std::floor(t));
        for (long j = kc - 3; j <= kc + 3; ++j) {
            if (j < 0 || j >= long(lv.size())) continue;
            const double tt = t - double(j) - 0.5;
            f += (lv[size_t(j)] ? 1.0 : -1.0) * 0.5 * (std::erf((tt + 0.5) / kk) - std::erf((tt - 0.5) / kk));
        }
        ph += 2 * M_PI * (off + 2400.0 * f) / fs;
        if (ph > 1e4) ph = std::fmod(ph, 2 * M_PI);
        x[i] = cf32(amp * float(std::cos(ph)), amp * float(std::sin(ph)));
    }
    return x;
}

/** The scene: noise, AIS bursts at given starts, and a DSC call (VHF FSK audio on FM, ±3.5 kHz) on channel 70. */
struct Scene {
    double fs = 8e6, hw = vibemar::centreHz();
    float noise = 0.02f;
    struct Burst { size_t at; std::vector<cf32> x; };
    std::vector<Burst> bursts;
    std::vector<float> dscAudio;      // 48 kHz, amplitude 1
    size_t dscAt = 0; double dscPh = 0; float dscAmp = 0.03f; double dscDev = 3500;
    std::mt19937 g{ 7 };
    std::normal_distribution<float> nd{ 0.f, 1.f };
    /** samples [from, from + n) */
    void fill(size_t from, size_t n, std::vector<cf32>& out) {
        out.resize(n);
        for (size_t i = 0; i < n; ++i) out[i] = cf32(noise * nd(g), noise * nd(g));
        for (const auto& b : bursts) {
            if (b.at >= from + n || b.at + b.x.size() <= from) continue;
            const size_t a = std::max(b.at, from), e = std::min(b.at + b.x.size(), from + n);
            for (size_t k = a; k < e; ++k) out[k - from] += b.x[k - b.at];
        }
        if (!dscAudio.empty()) {
            const double off = vibemar::kDscHz - hw;
            const size_t len = size_t(double(dscAudio.size()) * fs / 48000.0);
            const size_t a = std::max(dscAt, from), e = std::min(dscAt + len, from + n);
            for (size_t k = a; k < e; ++k) {
                const size_t ai = std::min(dscAudio.size() - 1, size_t(double(k - dscAt) * 48000.0 / fs));
                dscPh += 2 * M_PI * (off + dscDev * dscAudio[ai]) / fs;
                if (dscPh > 1e4) dscPh = std::fmod(dscPh, 2 * M_PI);
                out[k - from] += cf32(dscAmp * float(std::cos(dscPh)), dscAmp * float(std::sin(dscPh)));
            }
        }
    }
};
static const uint32_t kMmsi[4] = { 235012301u, 235012302u, 244660003u, 211444004u };
static Scene makeScene(double fs) {
    Scene s; s.fs = fs;
    const double offA = vibeais::kChanHz[0] - s.hw, offB = vibeais::kChanHz[1] - s.hw;
    for (int k = 0; k < 4; ++k)   // ★ two on each channel, apart in time
        s.bursts.push_back({ size_t((0.10 + 0.25 * k) * fs), aisBurst(kMmsi[k], 50.81234 + 0.01 * k, -1.30567 - 0.01 * k, fs, k % 2 ? offB : offA, 0.03f) });
    const auto cs = dscsynth::calls();
    const auto bits = vibe::dsc::build::transmission(cs[0].syms, cs[0].exp, 20);   // the distress alert (235012345, 50°12'N 004°10'W)
    s.dscAudio = vibe::dsc::build::modulate(bits, true, 48000);
    s.dscAt = size_t(1.2 * fs);
    return s;
}

struct Got { std::set<uint32_t> mmsi; int frames = 0; std::vector<vibe::dsc::Call> calls; };
/** The combination fed directly (no thread), `secs` of the scene in 20 ms blocks. */
static Got runDirect(Scene& sc, double secs, vibemar::Combo** keep = nullptr) {
    Got got;
    vibemar::Combo::Hooks h;
    h.call = [&](const vibe::dsc::Call& c, const std::string&, const std::string&, bool rep) { if (!rep) got.calls.push_back(c); };
    vibemar::Combo c(sc.fs, sc.hw, h);
    vibeais::Front fe(c.aisRate(), vibeais::kCentreHz);
    const size_t blk = size_t(sc.fs / 50), total = size_t(secs * sc.fs);
    std::vector<cf32> x;
    for (size_t at = 0; at < total; at += blk) {
        sc.fill(at, std::min(blk, total - at), x);
        c.feed(x.data(), x.size(), [&](const cf32* z, int k) {
            fe.feed(reinterpret_cast<const vibeais::cf*>(z), size_t(k), [&](int, const vibeais::RawFrame& f) {
                vibeais::Msg m;
                if (vibeais::decode(f.bytes, m)) { got.mmsi.insert(m.mmsi); ++got.frames; }
            });
        });
    }
    if (keep) *keep = nullptr;
    return got;
}

static double cpuNow() { return double(std::clock()) / CLOCKS_PER_SEC; }

/** ★ --write-clip FILE [rate]: one second of the scene as unsigned 8-bit IQ (rtl_tcp's), centred on 159.2625 MHz, for
 *  scripts/test-server-maritime.mjs (fake-rtl-tcp --hf FILE --hf-centre 159262500). The DSC call first, then the four ships. */
static int writeClip(const char* path, double fs) {
    Scene sc; sc.fs = fs;
    const double offA = vibeais::kChanHz[0] - sc.hw, offB = vibeais::kChanHz[1] - sc.hw;
    for (int k = 0; k < 4; ++k)
        sc.bursts.push_back({ size_t((0.55 + 0.1 * k) * fs), aisBurst(kMmsi[k], 50.81234 + 0.01 * k, -1.30567 - 0.01 * k, fs, k % 2 ? offB : offA, 0.12f) });
    const auto cs = dscsynth::calls();
    sc.dscAudio = vibe::dsc::build::modulate(vibe::dsc::build::transmission(cs[0].syms, cs[0].exp, 20), true, 48000);
    sc.dscAt = size_t(0.02 * fs); sc.dscAmp = 0.12f; sc.noise = 0.03f;
    FILE* f = std::fopen(path, "wb");
    if (!f) return 2;
    std::vector<cf32> x;
    std::vector<uint8_t> u;
    const size_t total = size_t(fs), blk = size_t(fs / 50);
    for (size_t at = 0; at < total; at += blk) {
        sc.fill(at, std::min(blk, total - at), x);
        u.resize(x.size() * 2);
        for (size_t i = 0; i < x.size(); ++i) {
            u[2 * i] = uint8_t(std::max(0.f, std::min(255.f, std::round(127.4f + 128.f * x[i].real()))));
            u[2 * i + 1] = uint8_t(std::max(0.f, std::min(255.f, std::round(127.4f + 128.f * x[i].imag()))));
        }
        std::fwrite(u.data(), 1, u.size(), f);
    }
    std::fclose(f);
    std::printf("wrote %s: 1 s at %.0f S/s centred on %.0f Hz (DSC distress 235012345 + AIS %u %u %u %u)\n", path, fs, sc.hw, kMmsi[0], kMmsi[1], kMmsi[2], kMmsi[3]);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--write-clip") return writeClip(argv[2], argc >= 4 ? std::atof(argv[3]) : 8e6);
    std::printf("── 1. the geometry and the rule per radio\n");
    {
        const double c = vibemar::centreHz(), need = vibemar::minRateHz();
        ok(std::fabs(c - 159.2625e6) < 1, "the radio sits at 159.2625 MHz, between channel 70 and AIS (" + std::to_string(c / 1e6) + ")");
        ok(std::fabs(c - vibemar::kDscHz) > 2.7e6 && std::fabs(vibeais::kChanHz[0] - c) > 2.7e6, "its DC 2.7 MHz from channel 70 and from AIS 1");
        ok(need > 6.1e6 && need < 6.2e6, "the capture it needs: " + std::to_string(need / 1e6) + " MS/s (≈ ACARS + VDL2's 6.08)");
        struct R { const char* radio; double usable; bool want; };
        const R rs[] = {
            { "RTL-SDR 2.4 MS/s", 2.4e6, false }, { "RTL-SDR 2.56", 2.56e6, false }, { "RTL-SDR 3.2", 3.2e6, false },
            { "Airspy HF+ (768 kHz usable)", 0.768e6, false },
            { "SDRplay RSP 2 MS/s", 2e6, false }, { "SDRplay RSP 6 MS/s", 6e6, false }, { "SDRplay RSP 8 MS/s", 8e6, true },
            { "Airspy R2 2.5", 2.5e6, false }, { "Airspy R2 10", 10e6, true }, { "Airspy Mini 3", 3e6, false }, { "Airspy Mini 6", 6e6, false },
            { "HackRF 8", 8e6, true }, { "HackRF 10", 10e6, true }, { "HackRF 20", 20e6, true },
            { "USRP B2xx 8", 8e6, true }, { "USRP 16", 16e6, true },
        };
        for (const auto& r : rs) ok(vibemar::fits(r.usable) == r.want, std::string(r.radio) + (r.want ? ": offered" : ": not offered (AIS stays alone)"));
        ok(!vibemar::fits(0), "an unknown rate: not offered");
        ok(vibemar::rateWhy(2.4e6).find("6.2 MS/s") != std::string::npos, "the words: " + vibemar::rateWhy(2.4e6));
        vibemar::Combo c8(8e6, c, {}), c20(20e6, c, {}), c6(6.25e6, c, {});
        ok(c8.ok() && c20.ok() && c6.ok(), "both slices inside at 6.25, 8 and 20 MS/s");
        ok(c8.stages() == 0 && c20.stages() == 1, "a half-band stage only where the capture is wider than it needs (20 MS/s: "
           + std::to_string(c20.stages()) + ")");
        ok(c8.aisRate() >= 200e3 && c8.aisRate() <= 400e3 && c20.aisRate() >= 200e3, "AIS's slice: " + std::to_string(c8.aisRate()) + " S/s at 8 MS/s, "
           + std::to_string(c20.aisRate()) + " at 20");
        ok(c8.dscRate() > 30e3 && c8.dscRate() < 60e3, "channel 70 cut to " + std::to_string(c8.dscRate()) + " S/s before the discriminator");
        std::printf("     (FFT %d points at 8 MS/s, %d at 20 MS/s)\n", c8.fftSize(), c20.fftSize());
    }

    for (double fs : { 8e6, 20e6 }) {
        std::printf("── 2. AIS on both channels + a DSC distress alert on channel 70, %.0f MS/s, fed directly\n", fs / 1e6);
        Scene sc = makeScene(fs);
        const Got g = runDirect(sc, 2.0);
        int seen = 0; for (uint32_t m : kMmsi) seen += g.mmsi.count(m) ? 1 : 0;
        ok(seen == 4, "every ship decoded, two on each channel (" + std::to_string(seen) + "/4, " + std::to_string(g.frames) + " frames)");
        ok(g.mmsi.size() == 4, "nothing else (" + std::to_string(g.mmsi.size()) + " MMSIs)");
        ok(g.calls.size() == 1, "the DSC call decoded once (" + std::to_string(g.calls.size()) + ")");
        if (!g.calls.empty()) {
            const auto& c = g.calls[0];
            const std::string line = vibe::dsc::describe(c);
            ok(c.level() == 3 && line.find("235012345") != std::string::npos && line.find("Sinking") != std::string::npos,
               "…as the distress alert from 235012345, sinking: " + line);
            ok(c.pos.known && std::fabs(c.pos.lat - 50.2) < 0.01 && std::fabs(c.pos.lon + 4.1667) < 0.01, "…with its position (the map's pin)");
            ok(c.vhf, "…heard as VHF");
        }
    }

    std::printf("── 3. through vibeais::Service on its own thread (the shim's path), 8 MS/s in real time\n");
    {
        Scene sc = makeScene(8e6);
        int hookCalls = 0, ticks = 0, finished = 0;
        std::string lastJson;
        vibemar::Combo::Hooks h;
        h.call = [&](const vibe::dsc::Call&, const std::string&, const std::string& j, bool rep) { if (!rep) ++hookCalls; lastJson = j; };
        h.tick = [&] { ++ticks; };
        h.finish = [&] { ++finished; };
        vibeais::Service svc;
        svc.start(sc.hw, sc.fs, false, nullptr, std::unique_ptr<vibeais::PreFront>(new vibemar::ComboFront(sc.fs, sc.hw, h)));
        ok(svc.combined(), "the service runs the combination");
        const size_t blk = size_t(sc.fs / 100), total = size_t(2.5 * sc.fs);
        std::vector<cf32> x;
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t at = 0; at < total; at += blk) {
            sc.fill(at, blk, x);
            svc.feed(reinterpret_cast<const vibeais::cf*>(x.data()), x.size());
            const double ahead = double(at + blk) / sc.fs - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (ahead > 0) std::this_thread::sleep_for(std::chrono::microseconds(long(ahead * 1e6)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const std::string r = svc.report();
        int seen = 0; for (uint32_t m : kMmsi) seen += r.find("\"mmsi\":" + std::to_string(m)) != std::string::npos ? 1 : 0;
        ok(seen == 4, "every ship in the report's vessels (" + std::to_string(seen) + "/4)");
        ok(r.find("\"combo\":\"ais+dsc\"") != std::string::npos && r.find("\"hw\":159262500") != std::string::npos, "the report says AIS + DSC, the radio at 159.2625 MHz");
        ok(r.find("\"dsc\":{\"hz\":156525000,\"calls\":1,") != std::string::npos, "…and channel 70's count: 1 call");
        ok(r.find("\"recent\":[{") != std::string::npos && r.find("235012345") != std::string::npos, "…with the call itself in recent");
        ok(r.find("\"centre\":162000000") != std::string::npos, "the AIS centre stays 162.000 (the clients' channels)");
        ok(hookCalls == 1 && lastJson.find("\"rf\":156525000") != std::string::npos, "the gallery's hook had the call, its RF channel 70");
        ok(ticks >= 2, "the gallery's tick ran on the service thread (" + std::to_string(ticks) + ")");
        ok(svc.droppedBlocks() == 0, "nothing dropped at 8 MS/s on this machine (" + std::to_string(svc.droppedBlocks()) + ")");
        svc.stop();
        ok(finished == 1, "stop() finished the DSC side once (the gallery's last write)");
        // ★ and AIS alone again on the same service object: no combination left behind
        svc.start(vibeais::kCentreHz, 2.4e6);
        ok(!svc.combined() && svc.report().find("\"combo\"") == std::string::npos, "restarted as AIS alone: no combo, no dsc in the report");
        svc.stop();
    }

    std::printf("── 4. noise only: nothing decoded (20 s at 6.25 MS/s)\n");
    {
        Scene sc; sc.fs = 6.25e6;   // no bursts, no call
        const Got g = runDirect(sc, 20.0);
        ok(g.frames == 0 && g.calls.empty(), "0 AIS frames, 0 DSC calls (" + std::to_string(g.frames) + ", " + std::to_string(g.calls.size()) + ")");
    }

    std::printf("── 5. CPU, one core of this machine (single thread, the decoder's own work)\n");
    {
        auto cost = [&](double fs, bool combo) {
            Scene sc = makeScene(fs);
            if (!combo) { sc.hw = vibeais::kCentreHz; sc.bursts.clear(); sc.dscAudio.clear(); }
            std::vector<cf32> one;
            sc.fill(0, size_t(fs), one);   // ★ one second, looped (freed at return)
            const int reps = 3;
            double c0;
            if (combo) {
                vibemar::Combo c(fs, sc.hw, {});
                vibeais::Front fe(c.aisRate(), vibeais::kCentreHz);
                c0 = cpuNow();
                for (int r = 0; r < reps; ++r)
                    for (size_t at = 0; at < one.size(); at += size_t(fs / 100))
                        c.feed(one.data() + at, std::min(size_t(fs / 100), one.size() - at), [&](const cf32* z, int k) {
                            fe.feed(reinterpret_cast<const vibeais::cf*>(z), size_t(k), [](int, const vibeais::RawFrame&) {});
                        });
            } else {
                vibeais::Front fe(fs, vibeais::kCentreHz);
                c0 = cpuNow();
                for (int r = 0; r < reps; ++r)
                    for (size_t at = 0; at < one.size(); at += size_t(fs / 100))
                        fe.feed(reinterpret_cast<const vibeais::cf*>(one.data() + at), std::min(size_t(fs / 100), one.size() - at), [](int, const vibeais::RawFrame&) {});
            }
            return 100.0 * (cpuNow() - c0) / reps;
        };
        const double a24 = cost(2.4e6, false), m8 = cost(8e6, true), m10 = cost(10e6, true), m20 = cost(20e6, true);
        std::printf("     AIS alone @ 2.4 MS/s: %.1f %% of one core\n     AIS + DSC @ 8 MS/s:  %.1f %%\n     AIS + DSC @ 10 MS/s: %.1f %%\n     AIS + DSC @ 20 MS/s: %.1f %%\n",
                    a24, m8, m10, m20);
        ok(m8 < 60.0, "the combination at 8 MS/s runs well inside one core here (" + std::to_string(m8) + " %)");
    }

    std::printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
