// test-fctlm — V12 beta90 FUNcube BPSK 1200 telemetry (vibe_bpsk_rx.h + vibe_fctlm.h; docs/v12/SATELLITE-BEST-SERVER.md):
//   §1 the sync vector against Karn's printed sequence; the frame layout
//   §2 the FEC: our encoder → the decoder, clean, with symbol errors, both coder output orders
//   §3 ★ NOISE: no frame ever (random soft symbols through the framer; white noise through the whole receiver)
//   §4 the whole receiver: our transmitter (differential BPSK, raised cosine) with an offset, a Doppler ramp, a clock
//      error, both polarities — and a frame-rate curve against Eb/N0
//   §5 SIMD against scalar: bit-identical symbols
//   §6 the telemetry values (raw where the document gives no calibration) and the sat_tlm JSON
//   §7 cost
// Silent: no audio device. `--quick` shortens §3/§4.
#include "vibe_fctlm_tx.h"
#include "vibe_sat_orbit.h"   // its json:: parser (for the §6 checks)

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; std::printf("   ok   "); std::printf(__VA_ARGS__); std::printf("\n"); } \
                              else { ++g_fail; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
static bool g_quick = false;

using namespace vibefc;

static void s1() {
    std::printf("§1 the sync vector and the frame\n");
    const char* karn = "11111110000111011110010110010010000001000100110001011101011011000";
    std::string s; for (auto b : syncVector()) s += char('0' + b);
    CHECK(s == karn, "x⁷+x³+1 from all ones = Karn's printed 65 bits");
    CHECK(kCoded == 5132 && kSyms == 5200 && kCoded + 65 == 5197, "5132 coded + 65 sync of 5200");
    std::vector<uint8_t> f = tx::sampleFrame(3, 1);
    uint8_t ch[kSyms]; encodeFrame(f.data(), ch);
    bool syncAt80 = true; for (int j = 0; j < 65; ++j) syncAt80 &= ch[80 * j] == syncVector()[(size_t)j];
    CHECK(syncAt80, "the sync bit leads every 80-symbol column");
}

static void s2() {
    std::printf("§2 the FEC: our encoder → the decoder\n");
    std::mt19937 rng(7);
    int ok = 0, bitOk = 0;
    for (int t = 1; t <= 24; ++t) {
        std::vector<uint8_t> f = tx::sampleFrame(t, 100 + t);
        uint8_t ch[kSyms]; encodeFrame(f.data(), ch);
        int8_t soft[kSyms]; for (int i = 0; i < kSyms; ++i) soft[i] = ch[i] ? 40 : -40;
        uint8_t out[kDataBytes]; const int e = decodeFrame(soft, out);
        if (e == 0) ++ok;
        if (std::memcmp(out, f.data(), kDataBytes) == 0) ++bitOk;
    }
    CHECK(ok == 24 && bitOk == 24, "24 frame types decode clean, byte-exact (%d / %d)", ok, bitOk);
    // symbol errors: 6 % hard flips (Viterbi territory) + soft noise
    int good = 0;
    for (int k = 0; k < 20; ++k) {
        std::vector<uint8_t> f = tx::sampleFrame(1 + k % 24, k);
        uint8_t ch[kSyms]; encodeFrame(f.data(), ch);
        int8_t soft[kSyms];
        std::normal_distribution<double> g(0, 1);
        for (int i = 0; i < kSyms; ++i) { double v = (ch[i] ? 1.0 : -1.0) + 0.55 * g(rng); soft[i] = int8_t(std::max(-127.0, std::min(127.0, v * 40))); }
        uint8_t out[kDataBytes];
        if (decodeFrame(soft, out) >= 0 && std::memcmp(out, f.data(), kDataBytes) == 0) ++good;
    }
    CHECK(good >= 19, "σ = 0.55 soft noise (raw symbol error ≈ 3.5 %%): %d / 20 decode", good);
    // the other coder output order
    {
        std::vector<uint8_t> f = tx::sampleFrame(5, 55);
        uint8_t ch[kSyms]; encodeFrame(f.data(), ch);
        // swap each coded pair in place (through the de-interleaver's map)
        uint8_t sw[kSyms]; std::memcpy(sw, ch, kSyms);
        for (int k = 0; k + 1 < kCoded; k += 2) {
            auto idx = [](int kk) { return (kk % kCols) * kRows + 1 + kk / kCols; };
            std::swap(sw[idx(k)], sw[idx(k + 1)]);
        }
        int8_t soft[kSyms]; for (int i = 0; i < kSyms; ++i) soft[i] = sw[i] ? 40 : -40;
        uint8_t out[kDataBytes]; int order = -1;
        CHECK(decodeFrame(soft, out, &order) == 0 && order == 1 && std::memcmp(out, f.data(), kDataBytes) == 0, "the swapped output order is found (order %d)", order);
    }
    // ★ a frame whose RS words are beyond the code is refused, not "repaired"
    {
        std::vector<uint8_t> f = tx::sampleFrame(2, 2);
        uint8_t ch[kSyms]; encodeFrame(f.data(), ch);
        int8_t soft[kSyms]; for (int i = 0; i < kSyms; ++i) soft[i] = ch[i] ? 40 : -40;
        for (int i = 0; i < kSyms; i += 3) soft[i] = int8_t(-soft[i]);
        uint8_t out[kDataBytes];
        CHECK(decodeFrame(soft, out) < 0, "a third of the symbols wrong: refused");
    }
}

static void s3() {
    std::printf("§3 ★ noise: no frame, ever\n");
    std::mt19937 rng(11);
    std::normal_distribution<double> g(0, 1);
    Framer fr; int frames = 0; fr.onFrame = [&](const uint8_t*, int, int) { ++frames; };
    const long n = g_quick ? 1200L * 600 : 1200L * 3600;   // 10 min / 1 h of symbols
    for (long i = 0; i < n; ++i) fr.push(float(g(rng)));
    CHECK(frames == 0, "%ld random soft symbols (%.0f min at 1200 Bd): %d frames (%llu sync matches, all refused by RS)",
          n, n / 1200.0 / 60.0, frames, (unsigned long long)fr.syncs());
    // the whole receiver on white audio noise
    vibebpsk::Rx::Cfg c;
    int f2 = 0;
    Run run(c, false, Run::Out{ [&](const std::string& j) { if (j.find("sat_tlm") != std::string::npos) ++f2; }, nullptr, nullptr, nullptr });
    std::vector<float> a(48000);
    const int secs = g_quick ? 60 : 300;
    for (int s = 0; s < secs; ++s) { for (auto& v : a) v = float(0.1 * g(rng)); run.feedAudio(a.data(), (int)a.size()); }
    CHECK(f2 == 0, "%d s of white noise through the whole receiver: %d frames", secs, f2);
}

struct Result { int good = 0; uint64_t syncs = 0, failed = 0; double afc = 0; };
static Result runChain(const tx::Cfg& tc, int nFrames, bool invertRx = false, bool forceScalar = false, std::vector<float>* symsOut = nullptr, size_t blk = 4800) {
    std::vector<std::vector<uint8_t>> fr;
    for (int i = 0; i < nFrames; ++i) fr.push_back(tx::sampleFrame(1 + i % 24, 1000 + (uint32_t)i));
    std::vector<float> audio = tx::modulate(fr, tc);
    vibebpsk::Rx::Cfg c;
    Result r;
    Run run(c, invertRx, Run::Out{ nullptr, nullptr, nullptr, nullptr });
    run.rxMut().forceScalar = forceScalar;
    std::vector<uint8_t> got;
    if (symsOut) { auto& rx = run.rxMut(); auto prev = rx.onSymbol; rx.onSymbol = [prev, symsOut](float s) { symsOut->push_back(s); prev(s); }; }
    for (size_t i = 0; i < audio.size(); i += blk) run.feedAudio(audio.data() + i, (int)std::min<size_t>(blk, audio.size() - i));
    r.good = (int)run.framer().good(); r.syncs = run.framer().syncs(); r.failed = run.framer().failed(); r.afc = run.rx().afcHz();
    return r;
}

static void s4() {
    std::printf("§4 the whole receiver: our transmitter → audio → frames\n");
    const int nf = g_quick ? 6 : 12;
    tx::Cfg c; c.ebn0Db = 99;
    Result r = runChain(c, nf);
    CHECK(r.good >= nf - 1, "clean, on 1200 Hz: %d / %d frames", r.good, nf);
    /* ★★ THE SERVER's 20 ms BLOCKS, OFF CENTRE (2026-11-21): a fresh run used to think itself locked until its first
     *  frame — one hunt ran only if the first block was ≥ 53 ms, so 960-sample blocks never found ±150 Hz (the benchmark
     *  row decoded 0). Every frame, from the first. */
    {
        tx::Cfg b; b.offsetHz = 150; b.ebn0Db = 12;
        Result rb = runChain(b, 4, false, false, nullptr, 960);
        CHECK(rb.good == 4, "20 ms blocks, +150 Hz off, Eb/N0 12 dB: %d / 4 frames (the first one too)", rb.good);
        b.offsetHz = -500;
        rb = runChain(b, 4, false, false, nullptr, 960);
        CHECK(rb.good == 4, "20 ms blocks, −500 Hz off: %d / 4 frames", rb.good);
    }
    c.offsetHz = 350; c.rampHzS = 8; c.clockPpm = 60; c.phase = 2.1; c.ebn0Db = 14;
    r = runChain(c, nf);
    CHECK(r.good >= nf - 1, "+350 Hz off, an 8 Hz/s Doppler ramp, 60 ppm clock, Eb/N0 14 dB: %d / %d (AFC %.0f Hz)", r.good, nf, r.afc);
    c.offsetHz = -500; c.rampHzS = -6; c.invertBits = true;
    r = runChain(c, nf);
    CHECK(r.good >= nf - 1, "−500 Hz, the other polarity: %d / %d (AFC %.0f Hz)", r.good, nf, r.afc);
    c.differential = false; c.offsetHz = 120; c.invertBits = false;
    r = runChain(c, nf);
    CHECK(r.good >= nf - 1, "no differential code (coherent form): %d / %d", r.good, nf);
    std::printf("   frame rate against Eb/N0 (offset +200 Hz, 4 Hz/s ramp, 30 ppm):\n");
    tx::Cfg k; k.offsetHz = 200; k.rampHzS = 4; k.clockPpm = 30;
    int at8 = 0;
    for (double eb : {12.0, 10.0, 8.0, 7.0, 6.0, 5.0, 4.0, 3.0}) {
        k.ebn0Db = eb; k.seed = (uint32_t)(eb * 10);
        Result q = runChain(k, nf);
        std::printf("     Eb/N0 %4.1f dB: %2d / %d frames (%llu syncs, %llu failed)\n", eb, q.good, nf, (unsigned long long)q.syncs, (unsigned long long)q.failed);
        if (eb == 8.0) at8 = q.good;
    }
    CHECK(at8 >= nf * 3 / 4, "Eb/N0 8 dB: %d / %d frames", at8, nf);
}

static void s5() {
    std::printf("§5 SIMD (kernel %d) against scalar\n", vibebpsk::kernel());
    tx::Cfg c; c.offsetHz = 210; c.ebn0Db = 10;
    std::vector<float> a, b;
    runChain(c, 2, false, false, &a);
    runChain(c, 2, false, true, &b);
    CHECK(a.size() == b.size() && !a.empty() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0, "%zu symbols bit-identical", a.size());
}

using vibe::sat::json::Value; namespace json = vibe::sat::json;
static void s6() {
    std::printf("§6 the values and the JSON\n");
    std::vector<uint8_t> f = tx::sampleFrame(14, 0x123456, "HELLO FROM THE TEST\x01");
    Decoded d = parse(f.data());
    CHECK(d.satId == 2 && d.frameType == 14 && d.frameName == "FM1", "FUNcube-1, FM1");
    CHECK(d.seq == 0x123456 && d.mode == "Eclipse", "sequence %lld, mode %s", d.seq, d.mode.c_str());
    CHECK(d.values.size() == 58 && d.values[4].key == "eps.vbat" && d.values[4].v == 1444 && d.values[4].raw, "58 values; battery voltage raw 1444");
    CHECK(d.values[6].key == "eps.reboots" && !d.values[6].raw, "a count is exact, not raw");
    CHECK(d.fitter == "HELLO FROM THE TEST.", "fitter text, whitelisted (%s)", d.fitter.c_str());
    const std::string j = tlmJson(d, f.data(), f.size(), "AO-73", 1795600000.0);
    json::Value v;
    CHECK(json::parseValue(j, v) && v.str("type") == "sat_tlm", "sat_tlm parses");
    const json::Value* dd = v.get("data");
    CHECK(dd && dd->str("sat") == "AO-73" && dd->str("format") == "funcube-rt" && dd->num("seq") == 0x123456 && dd->get("values")
          && dd->get("values")->a.size() == 58 && dd->str("hex").size() == 512, "the contract's fields");
    Decoded w = parse(tx::sampleFrame(3, 9).data());
    CHECK(w.fitter.empty() && w.frameName == "WO3", "a whole-orbit frame carries no fitter text");
}

static void s7() {
    std::printf("§7 cost\n");
    tx::Cfg c; c.ebn0Db = 12; c.offsetHz = 150;
    std::vector<std::vector<uint8_t>> fr; for (int i = 0; i < 4; ++i) fr.push_back(tx::sampleFrame(i + 1, (uint32_t)i));
    std::vector<float> audio = tx::modulate(fr, c);
    vibebpsk::Rx::Cfg rc;
    Run run(rc, false, Run::Out{});
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < audio.size(); i += 4800) run.feedAudio(audio.data() + i, (int)std::min<size_t>(4800, audio.size() - i));
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double aud = audio.size() / 48000.0;
    std::printf("   %.1f s of audio in %.3f s: %.3f %% of one core (this machine)\n", aud, sec, 100.0 * sec / aud);
    CHECK(sec / aud < 0.05, "under 5 %% of a core here");
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--quick") g_quick = true;
    s1(); s2(); s3(); s4(); s5(); s6(); s7();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
