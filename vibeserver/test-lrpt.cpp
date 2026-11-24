// test-lrpt.cpp — METEOR-M LRPT (docs/v12/LRPT.md): the FEC pieces against their encoders, our own LRPT transmitter
// (pictures → JPEG-like MCUs → packets → VCDUs → RS → randomiser → convolutional code → QPSK / OQPSK / 80k) through
// the whole receiver, noise (no picture, ever), the SIMD paths against scalar, and the CPU cost.
//
//   test-lrpt                 the synthetic suite
//   test-lrpt --quick         the same, shorter
//   test-lrpt --wav F [DIR] [--rate-auto] [--qpsk|--oqpsk] [--80k] [--noise R]   decode a 16-bit IQ .wav recording; PNGs into DIR
#include "vibe_lrpt.h"
#include "vibe_lrpt_run.h"
#include "vibe_lrpt_tx.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vibelrpt;

static bool writeFile(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return (std::fclose(f) == 0) && ok;
}

static void report(const Receiver& rx, double secs, double cpu) {
    const Stats s = rx.stats();
    std::printf("  %.1f s: mode %s diff %d | carrier lock %d (%.2f) sync %d | CADUs %llu RS ok %llu fail %llu fixed %llu | packets %llu bad %llu blocks %llu | scid %d | cpu %.3f s (%.2f %% of real time)\n",
                secs, rx.modeName(), rx.diffWinner(), rx.demodLocked(), rx.lockMetric(), rx.syncLocked(),
                (unsigned long long)s.cadus, (unsigned long long)s.rsOk, (unsigned long long)s.rsFail, (unsigned long long)s.rsFixed,
                (unsigned long long)s.packets, (unsigned long long)s.badPackets, (unsigned long long)s.blocks, s.scid, cpu,
                secs > 0 ? 100.0 * cpu / secs : 0.0);
    for (auto& kv : rx.channels())
        std::printf("    APID %d: %d strips, %llu blocks\n", kv.first, kv.second.strips(), (unsigned long long)kv.second.goodBlocks());
}

static int savePictures(const Receiver& rx, const std::string& dir, const std::string& stem) {
    int n = 0;
    for (auto& p : renderPictures(rx)) {
        std::string nm = p.name; for (auto& c : nm) if (c == ' ') c = '_';
        const std::string path = dir + "/" + stem + "-" + nm + ".png";
        if (writeFile(path, p.png)) { std::printf("  wrote %s (%dx%d, %zu bytes)\n", path.c_str(), p.w, p.h, p.png.size()); n++; }
    }
    return n;
}

static double g_addNoise = 0, g_rms = 0;
static int decodeWav(const char* path, const std::string& dir, const ReceiverConfig& base) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return 2; }
    uint8_t hdr[12]; if (std::fread(hdr, 1, 12, f) != 12 || std::memcmp(hdr, "RIFF", 4) || std::memcmp(hdr + 8, "WAVE", 4)) { std::printf("not a WAV\n"); std::fclose(f); return 2; }
    uint32_t rate = 0; uint16_t ch = 0, bits = 0; long dataAt = -1; uint32_t dataLen = 0;
    for (;;) {
        uint8_t c[8]; if (std::fread(c, 1, 8, f) != 8) break;
        const uint32_t len = uint32_t(c[4]) | uint32_t(c[5]) << 8 | uint32_t(c[6]) << 16 | uint32_t(c[7]) << 24;
        if (!std::memcmp(c, "fmt ", 4)) {
            uint8_t fm[16]; if (std::fread(fm, 1, 16, f) != 16) break;
            ch = uint16_t(fm[2] | fm[3] << 8); rate = uint32_t(fm[4]) | uint32_t(fm[5]) << 8 | uint32_t(fm[6]) << 16 | uint32_t(fm[7]) << 24;
            bits = uint16_t(fm[14] | fm[15] << 8);
            std::fseek(f, long(len) - 16, SEEK_CUR);
        } else if (!std::memcmp(c, "data", 4)) { dataAt = std::ftell(f); dataLen = len; break; }
        else std::fseek(f, long(len), SEEK_CUR);
    }
    if (dataAt < 0 || ch != 2 || bits != 16) { std::printf("need 2-channel 16-bit IQ (got ch %d bits %d)\n", ch, bits); std::fclose(f); return 2; }
    std::printf("%s: %u S/s, %.1f s\n", path, rate, dataLen / 4.0 / rate);
    ReceiverConfig c = base; c.sampleRate = rate;
    Receiver rx(c);
    if (std::getenv("LRPT_DIAG")) rx.diag = [](int a, int s, int m, uint32_t ms, int q) { std::printf("PKT apid %d seq %d mcu %d ms %u q %d\n", a, s, m, ms, q); };
    std::vector<int16_t> raw(size_t(rate / 10) * 2);
    std::vector<float> iq(raw.size());
    double secs = 0, cpu = 0, nextRep = 10;
    for (;;) {
        const size_t got = std::fread(raw.data(), 4, raw.size() / 2, f);
        if (!got) break;
        for (size_t i = 0; i < got * 2; i++) iq[i] = raw[i] / 32768.f;
        // ★ --noise R: add complex noise of R × the recording's own RMS (the margin test: how much worse could it be)
        if (g_addNoise > 0) {
            if (g_rms <= 0) { double p = 0; for (size_t i = 0; i < got * 2; i++) p += double(iq[i]) * iq[i]; g_rms = std::sqrt(p / double(got * 2)); }
            static std::mt19937 rng(77); std::normal_distribution<float> gn(0.f, 1.f);
            for (size_t i = 0; i < got * 2; i++) iq[i] += float(g_addNoise * g_rms) * gn(rng);
        }
        const auto t0 = std::chrono::steady_clock::now();
        rx.feedIq(iq.data(), got);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        secs += double(got) / rate;
        if (secs >= nextRep) { report(rx, secs, cpu); nextRep += 10; }
    }
    std::fclose(f);
    report(rx, secs, cpu);
    std::string stem = path; { const size_t sl = stem.rfind('/'); if (sl != std::string::npos) stem = stem.substr(sl + 1); const size_t d = stem.rfind('.'); if (d != std::string::npos) stem = stem.substr(0, d); }
    if (std::getenv("LRPT_NO_RENDER")) return rx.stats().rsOk > 0 ? 0 : 1;   // ★ the instruction count of the decode alone
    const auto t0 = std::chrono::steady_clock::now();
    const int n = savePictures(rx, dir, stem);
    std::printf("  pictures: %d (rendered in %.3f s)\n", n, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return n > 0 ? 0 : 1;
}


// ── The synthetic suite ─────────────────────────────────────────────────────────────────────────────────────────────
static void testRs() {
    std::printf("RS(255,223) CCSDS\n");
    std::mt19937 rng(7);
    const Rs223& rs = Rs223::get();
    int okFix = 0, badFix = 0, refused = 0, miscorrect = 0;
    for (int trial = 0; trial < 400; trial++) {
        uint8_t d[223], p[32], cw[255];
        for (auto& v : d) v = uint8_t(rng());
        rs.encode(d, p);
        std::memcpy(cw, d, 223); std::memcpy(cw + 223, p, 32);
        uint8_t ref[255]; std::memcpy(ref, cw, 255);
        const int ne = trial % 22;                         // 0..21 errors
        std::vector<int> pos;
        while ((int)pos.size() < ne) { const int q = int(rng() % 255); if (std::find(pos.begin(), pos.end(), q) == pos.end()) pos.push_back(q); }
        for (int q : pos) cw[q] ^= uint8_t(1 + rng() % 255);
        const int e = rs.decode(cw);
        if (ne <= 16) { if (e == ne && !std::memcmp(cw, ref, 255)) okFix++; else badFix++; }
        else { if (e < 0) refused++; else if (std::memcmp(cw, ref, 255)) miscorrect++; }
    }
    CHECK(badFix == 0, "0..16 errors: %d wrong", badFix);
    CHECK(okFix > 280, "corrected %d", okFix);
    CHECK(miscorrect == 0, "17..21 errors miscorrected %d (refused %d)", miscorrect, refused);
    std::printf("  0–16 errors corrected %d / %d; 17–21 refused %d, miscorrected %d\n", okFix, okFix + badFix, refused, miscorrect);
    // ★★ errors AND erasures (2026-11-24): 2·errors + erasures ≤ 32 always decodes; flagged bytes that were right cost nothing
    {
        int ok = 0, bad = 0, refusedOver = 0, wrongOver = 0, n = 0;
        for (int trial = 0; trial < 600; trial++) {
            uint8_t d[223], p[32], cw[255], ref[255];
            for (auto& x : d) x = uint8_t(rng());
            rs.encode(d, p); std::memcpy(cw, d, 223); std::memcpy(cw + 223, p, 32); std::memcpy(ref, cw, 255);
            const int ne = int(rng() % 33);                         // erasures flagged
            const int nbadEra = ne ? int(rng() % (ne + 1)) : 0;     // of them really wrong
            const int room = (32 - ne) / 2;
            const bool over = trial % 5 == 4;                       // one in five: beyond the code
            const int nerr = over ? room + 1 + int(rng() % 3) : int(rng() % (room + 1));
            std::vector<int> pos;
            while ((int)pos.size() < ne + nerr) { const int q = int(rng() % 255); if (std::find(pos.begin(), pos.end(), q) == pos.end()) pos.push_back(q); }
            for (int i = 0; i < nbadEra; i++) cw[pos[size_t(i)]] ^= uint8_t(1 + rng() % 255);
            for (int i = ne; i < ne + nerr; i++) cw[pos[size_t(i)]] ^= uint8_t(1 + rng() % 255);
            const int e = rs.decode(cw, pos.data(), ne);
            n++;
            if (!over) { if (e == nbadEra + nerr && !std::memcmp(cw, ref, 255)) ok++; else { bad++; if (std::getenv("RSDBG")) std::printf("    ne %d badEra %d err %d → %d\n", ne, nbadEra, nerr, e); } }
            else { if (e < 0) refusedOver++; else if (std::memcmp(cw, ref, 255)) wrongOver++; }
        }
        CHECK(bad == 0, "errors+erasures within the bound: %d wrong of %d", bad, ok + bad);
        std::printf("  errors + erasures (2·err + era ≤ 32): %d / %d corrected; beyond: %d refused, %d miscorrected\n", ok, ok + bad, refusedOver, wrongOver);
    }
    // interleaved frame
    uint8_t v[892], f[1020];
    for (auto& x : v) x = uint8_t(rng());
    rsEncodeFrame(v, f);
    for (int i = 0; i < 60; i++) f[(i * 17) % 1020] ^= 0x5A;
    const int e = rsFrame(f);
    CHECK(e == 60 && !std::memcmp(f, v, 892), "interleaved frame: %d corrected", e);
    for (int i = 0; i < 1020; i++) f[i] = uint8_t(rng());
    CHECK(rsFrame(f) < 0, "random frame refused");
}

static void testViterbi() {
    std::printf("Viterbi K=7 r=½ (kernel %d)\n", Viterbi::kernel());
    std::mt19937 rng(3);
    std::normal_distribution<double> g(0, 1);
    const int n = 8192;
    std::vector<uint8_t> bits(n + 200);
    for (auto& b : bits) b = uint8_t(rng() & 1);
    std::vector<uint8_t> enc; convEncode(bits.data(), bits.size(), 0, enc);
    for (double ebn0 : {8.0, 4.0, 3.0, 2.0}) {
        const double sigma = std::sqrt(1.0 / (2 * 0.5 * std::pow(10, ebn0 / 10)));
        std::vector<int8_t> soft(enc.size());
        for (size_t i = 0; i < enc.size(); i++) { double v = (enc[i] ? 1.0 : -1.0) + sigma * g(rng); v *= 40; soft[i] = int8_t(std::max(-127.0, std::min(127.0, std::round(v)))); }
        Viterbi va, vs; vs.forceScalar = true;
        std::vector<uint8_t> oa(n), os(n);
        const int ma = va.decode(soft.data(), int(enc.size() / 2), 100, n - 100 - 64, oa.data());
        const int ms = vs.decode(soft.data(), int(enc.size() / 2), 100, n - 100 - 64, os.data());
        int err = 0; for (int i = 0; i < n - 164; i++) err += oa[size_t(i)] != bits[size_t(i + 100)];
        const bool same = oa == os && ma == ms;
        CHECK(same, "SIMD vs scalar differ at Eb/N0 %.0f", ebn0);
        std::printf("  Eb/N0 %.0f dB: %d bit errors in %d (SIMD == scalar: %s)\n", ebn0, err, n - 164, same ? "yes" : "NO");
        if (ebn0 >= 4) CHECK(err == 0, "errors at %.0f dB: %d", ebn0, err);
        if (ebn0 >= 3) CHECK(err < 10, "errors at %.0f dB: %d", ebn0, err);
    }
}

static void testSyncAndInterleave() {
    std::printf("ASM ambiguities, deinterleaver\n");
    // every transform's pattern scores 52 on itself and < 40 on the others' patterns
    const SyncPattern& sp = syncPattern();
    int worst = 0;
    for (int x = 0; x < 8; x++) { CHECK(syncScore(sp.pat[x], x) == 52, "self x=%d", x); for (int y = 0; y < 8; y++) if (y != x) worst = std::max(worst, syncScore(sp.pat[y], x)); }
    CHECK(worst < 40, "cross-ambiguity score %d", worst);
    std::printf("  worst cross-ambiguity score %d of 52\n", worst);
    // deinterleaver round trip
    std::vector<int8_t> d(size_t(Deinterleaver::latency()) + 5000);
    for (size_t i = 0; i < d.size(); i++) d[i] = int8_t((i * 7919) & 0x7F);
    const auto il = Deinterleaver::interleave(d);
    Deinterleaver dl; int bad = 0;
    for (size_t i = 0; i < il.size(); i++) { const int8_t o = dl.push(il[i]); if (i >= Deinterleaver::latency() && o != d[i - Deinterleaver::latency()]) bad++; }
    CHECK(bad == 0, "deinterleave mismatches %d", bad);
}

struct RunResult { Stats st; int pictures = 0; double psnr = 0; double cpuFrac = 0; std::string mode; int diff = -1; };
static double psnrOf(const Receiver& rx, int apid) {
    // ★ Strip 0 is the first period HEARD, which at low SNR is not the first one sent: the best of every offset.
    auto it = rx.channels().find(apid); if (it == rx.channels().end()) return 0;
    const Channel& ch = it->second; double best = 0;
    for (int off = 0; off < 64; off++) {
        double se = 0; size_t n = 0;
        for (int s = 0; s < ch.strips(); s++) for (int c = 0; c < kMcuPerLine; c++) {
            if (!ch.has(s, c)) continue;
            for (int r = 0; r < 8; r++) for (int x = 0; x < 8; x++) {
                const int v = ch.pixels()[(size_t(s) * 8 + size_t(r)) * kLineWidth + size_t(c) * 8 + size_t(x)];
                const int w = tx::sourcePixel(apid, c * 8 + x, (s + off) * 8 + r);
                se += double(v - w) * (v - w); n++;
            }
        }
        if (n) best = std::max(best, 10 * std::log10(255.0 * 255.0 / std::max(1e-9, se / double(n))));
        if (best > 40) break;
    }
    return best;
}
static RunResult runSynthetic(const tx::TxConfig& t, int periods, ReceiverConfig rc, std::vector<uint8_t>* firstPng = nullptr) {
    const auto v = tx::buildVcdus(t, periods);
    const auto bits = tx::codeBits(t, v);
    const auto iq = tx::modulate(t, bits);
    rc.sampleRate = t.sampleRate; rc.symbolRate = t.symbolRate; rc.interleaved = t.interleaved;
    Receiver rx(rc);
    if (std::getenv("LRPT_CDIAG")) rx.setCaduDiag([&rx](int sc, int x, int vit, const int* pw) { std::printf("    CADU asm %d x %d vit %d rs %d %d %d %d | lock %.2f f %.0f\n", sc, x, vit, pw[0], pw[1], pw[2], pw[3], rx.lockMetric(), rx.carrierHz()); });
    const size_t blk = size_t(t.sampleRate / 20);
    double cpu = 0;
    for (size_t at = 0; at < iq.size() / 2; at += blk) {
        const size_t n = std::min(blk, iq.size() / 2 - at);
        const auto t0 = std::chrono::steady_clock::now();
        rx.feedIq(&iq[at * 2], n);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    RunResult r; r.st = rx.stats(); r.mode = rx.modeName(); r.diff = rx.diffWinner();
    const auto pics = renderPictures(rx);
    r.pictures = int(pics.size());
    if (firstPng && !pics.empty()) firstPng->assign(pics[0].png.begin(), pics[0].png.end());
    r.psnr = psnrOf(rx, 64);
    r.cpuFrac = cpu / (double(iq.size() / 2) / t.sampleRate);
    return r;
}
static void line(const char* what, const RunResult& r) {
    std::printf("  %-46s RS ok %4llu fail %3llu | blocks %6llu | pictures %d | PSNR ch1 %.1f dB | %s diff %d | cpu %.2f %%\n", what,
                (unsigned long long)r.st.rsOk, (unsigned long long)r.st.rsFail, (unsigned long long)r.st.blocks, r.pictures, r.psnr,
                r.mode.c_str(), r.diff, 100 * r.cpuFrac);
}

static void testChain(bool quick) {
    std::printf("Whole chain, synthetic (3 channels, q 80)\n");
    const int P = quick ? 6 : 12;                      // scan periods (1.232 s each of picture, ~0.8 s of air at 72k)
    const int expectBlocks = P * 3 * kMcuPerLine;
    {   tx::TxConfig t; ReceiverConfig rc; rc.mod = ReceiverConfig::Qpsk;
        auto r = runSynthetic(t, P, rc); line("QPSK 72k, 187.5 kS/s, clean", r);
        CHECK(r.st.blocks == uint64_t(expectBlocks), "blocks %llu of %d", (unsigned long long)r.st.blocks, expectBlocks);
        CHECK(r.psnr > 30, "PSNR %.1f", r.psnr); CHECK(r.pictures == 4, "pictures %d", r.pictures); CHECK(r.diff == 0, "diff winner %d", r.diff); }
    for (int x : {1, 2, 5, 7}) {
        tx::TxConfig t; t.transform = x; t.carrierHz = 1200; ReceiverConfig rc; rc.mod = ReceiverConfig::Qpsk;
        auto r = runSynthetic(t, P, rc); char b[80]; std::snprintf(b, sizeof b, "QPSK ambiguity %d, +1.2 kHz", x); line(b, r);
        CHECK(r.st.blocks >= uint64_t(expectBlocks * 9 / 10), "ambiguity %d: blocks %llu", x, (unsigned long long)r.st.blocks);
    }
    {   tx::TxConfig t; t.sampleRate = 300000; t.carrierHz = 2100; t.esn0Db = 10; ReceiverConfig rc; rc.mod = ReceiverConfig::Qpsk;
        auto r = runSynthetic(t, P, rc); line("QPSK at a 300 kS/s slice (the shim's at 2.4 MS/s)", r);
        CHECK(r.st.blocks >= uint64_t(expectBlocks * 9 / 10), "300 kS/s: blocks %llu", (unsigned long long)r.st.blocks); }
    {   tx::TxConfig t; t.sampleRate = 150000; t.carrierHz = -2800; t.dopplerRateHzS = 65; t.symbolPpm = 80; t.esn0Db = 12;
        ReceiverConfig rc; rc.mod = ReceiverConfig::Qpsk;
        auto r = runSynthetic(t, P, rc); line("QPSK 150 kS/s −2.8 kHz +65 Hz/s 80 ppm Es/N0 12", r);
        CHECK(r.st.blocks >= uint64_t(expectBlocks * 8 / 10), "Doppler: blocks %llu", (unsigned long long)r.st.blocks); }
    {   tx::TxConfig t; t.oqpsk = true; t.diff = true; t.carrierHz = 700; t.transform = 3;
        ReceiverConfig rc; rc.mod = ReceiverConfig::Oqpsk;
        auto r = runSynthetic(t, P, rc); line("OQPSK 72k + differential (N2-3 / N2-4 mode)", r);
        CHECK(r.st.blocks >= uint64_t(expectBlocks * 9 / 10), "OQPSK diff: blocks %llu", (unsigned long long)r.st.blocks); CHECK(r.diff == 1, "diff winner %d", r.diff); }
    {   tx::TxConfig t; t.oqpsk = true; t.diff = true; t.carrierHz = 400;
        ReceiverConfig rc;                              // AUTO: starts QPSK at 72k, must swap
        auto r = runSynthetic(t, 60, rc); line("AUTO on an OQPSK + differential signal", r);
        CHECK(r.st.blocks > 0 && r.mode == "OQPSK", "AUTO: blocks %llu mode %s", (unsigned long long)r.st.blocks, r.mode.c_str()); }
    {   // ★ the rate not named (no satellite entry): AUTO walks 72k OQPSK → 80k OQPSK interleaved → 72k QPSK
        tx::TxConfig t; t.oqpsk = true; t.diff = true; t.interleaved = true; t.symbolRate = 80000; t.sampleRate = 200000; t.carrierHz = 600;
        ReceiverConfig rc; rc.rateAuto = true;
        auto r = runSynthetic(t, 30, rc); line("rate AUTO on an 80k OQPSK interleaved signal", r);
        CHECK(r.st.blocks > 0 && r.mode == "OQPSK", "rate AUTO 80k: blocks %llu mode %s", (unsigned long long)r.st.blocks, r.mode.c_str());
        tx::TxConfig q; ReceiverConfig rq; rq.rateAuto = true;
        auto r2 = runSynthetic(q, P, rq); line("rate AUTO on 72k QPSK", r2);
        CHECK(r2.st.blocks >= uint64_t(expectBlocks * 9 / 10) && r2.mode == "QPSK", "rate AUTO 72k: blocks %llu", (unsigned long long)r2.st.blocks);
    }
    if (!quick) {
        tx::TxConfig t; t.oqpsk = true; t.diff = true; t.interleaved = true; t.symbolRate = 80000; t.sampleRate = 200000; t.carrierHz = -900;
        ReceiverConfig rc; rc.mod = ReceiverConfig::Oqpsk;
        auto r = runSynthetic(t, 8, rc); line("80k OQPSK interleaved + differential", r);
        CHECK(r.st.blocks >= uint64_t(8 * 3 * kMcuPerLine * 9 / 10), "80k: blocks %llu", (unsigned long long)r.st.blocks);
    }
    // ── sensitivity sweep (QPSK) ──
    for (double es : {6.0, 4.0, 3.0, 2.0}) {
        tx::TxConfig t; t.esn0Db = es; t.carrierHz = 500; t.seed = 11; ReceiverConfig rc; rc.mod = ReceiverConfig::Qpsk;
        auto r = runSynthetic(t, P, rc); char b[80]; std::snprintf(b, sizeof b, "QPSK Es/N0 %.0f dB", es); line(b, r);
        if (es >= 3) CHECK(r.st.blocks >= uint64_t(expectBlocks * 8 / 10), "Es/N0 %.0f: blocks %llu", es, (unsigned long long)r.st.blocks);
        CHECK(r.psnr == 0 || r.psnr > 28, "Es/N0 %.0f: PSNR %.1f — a wrong picture?", es, r.psnr);
    }
    // ── SIMD vs scalar: the same picture, byte for byte ──
    {   tx::TxConfig t; t.esn0Db = 5; t.carrierHz = 300; ReceiverConfig a, b; a.mod = b.mod = ReceiverConfig::Qpsk; b.forceScalar = true;
        std::vector<uint8_t> pa, pb;
        auto ra = runSynthetic(t, 4, a, &pa); auto rb = runSynthetic(t, 4, b, &pb);
        CHECK(!pa.empty() && pa == pb && ra.st.rsFixed == rb.st.rsFixed, "SIMD and scalar pictures differ");
        std::printf("  SIMD vs scalar (FIR + Viterbi) at Es/N0 5: identical picture %s, RS fixed %llu / %llu, cpu %.2f %% vs %.2f %%\n",
                    pa == pb ? "yes" : "NO", (unsigned long long)ra.st.rsFixed, (unsigned long long)rb.st.rsFixed, 100 * ra.cpuFrac, 100 * rb.cpuFrac); }
}

static void testNoise(bool quick) {
    std::printf("Noise only: no picture, ever\n");
    for (int mode = 0; mode < 3; mode++) {
        ReceiverConfig rc; rc.sampleRate = 187500;
        if (mode == 1) { rc.mod = ReceiverConfig::Oqpsk; }
        if (mode == 2) { rc.symbolRate = 80000; rc.interleaved = true; rc.sampleRate = 200000; }
        Receiver rx(rc);
        std::mt19937 rng(100 + mode); std::normal_distribution<float> g(0, 1);
        const double secs = quick ? 20 : 60;
        std::vector<float> iq(size_t(rc.sampleRate / 10) * 2);
        for (double t = 0; t < secs; t += 0.1) { for (auto& v : iq) v = g(rng); rx.feedIq(iq.data(), iq.size() / 2); }
        const Stats s = rx.stats();
        const auto pics = renderPictures(rx);
        CHECK(s.rsOk == 0 && pics.empty() && s.blocks == 0, "noise mode %d: RS ok %llu, pictures %zu", mode, (unsigned long long)s.rsOk, pics.size());
        std::printf("  %s, %.0f s: CADUs tried %llu, RS ok %llu, packets %llu, pictures %zu\n", mode == 0 ? "AUTO 72k" : mode == 1 ? "OQPSK 72k" : "80k interleaved",
                    secs, (unsigned long long)s.cadus, (unsigned long long)s.rsOk, (unsigned long long)s.packets, pics.size());
    }
    // a carrier and a random-bit QPSK signal with no LRPT framing: still nothing
    tx::TxConfig t; std::vector<uint8_t> bits(72000 * 2 * 15); std::mt19937 rng(9); for (auto& b : bits) b = uint8_t(rng() & 1);
    const auto iq = tx::modulate(t, bits);
    ReceiverConfig rc; rc.sampleRate = t.sampleRate; rc.mod = ReceiverConfig::Qpsk; Receiver rx(rc);
    rx.feedIq(iq.data(), iq.size() / 2);
    CHECK(rx.stats().rsOk == 0 && renderPictures(rx).empty(), "random QPSK: RS ok %llu", (unsigned long long)rx.stats().rsOk);
    std::printf("  random-bit QPSK 15 s (carrier locked %d): RS ok %llu, pictures 0\n", rx.demodLocked(), (unsigned long long)rx.stats().rsOk);
}


/* ★★★ THE LIVE PASS'S FAILURE MODES (2026-11-22, Stuart's Meteor-M2-4 pass — docs/v12/LRPT-LIVE-PASS.md; the recording
 *  itself stays out of git). What that pass had, rebuilt synthetically: OQPSK + differential at 72k in a 300 kS/s slice,
 *  the radio 3.34 kHz off (constant), the satellite's Doppler sliding −25 Hz/s near the top of the pass and fed forward by
 *  the satellite engine, and Es/N0 3–6 dB. */
namespace livepass {
constexpr double kResid = -3340, kDop0 = 585, kRate = -25;
/** One stretch of the pass: `periods` scan periods starting `t0` seconds in, VCDU counters from `ctr0`. */
std::vector<float> stretch(double t0, int periods, uint32_t ctr0, double es, double fs, uint32_t seed, uint32_t ms0) {
    tx::TxConfig t; t.sampleRate = fs; t.oqpsk = true; t.diff = true; t.esn0Db = es; t.seed = seed; t.startMs = ms0;
    t.carrierHz = kResid + kDop0 + kRate * t0; t.dopplerRateHzS = kRate;
    auto v = tx::buildVcdus(t, periods);
    for (size_t i = 0; i < v.size(); i++) { const uint32_t c = ctr0 + uint32_t(i); v[i][2] = uint8_t(c >> 16); v[i][3] = uint8_t(c >> 8); v[i][4] = uint8_t(c); }
    return tx::modulate(t, tx::codeBits(t, v));
}
/** The per-sample noise σ the transmitter uses for Es/N0 `es` (vibe_lrpt_tx.h modulate). */
double sigmaFor(double es, double fs) { return std::sqrt(fs / 72000.0 / std::pow(10.0, es / 10.0) / 2); }
}

static void testLivePass(bool quick) {
    using namespace livepass;
    std::printf("The live pass (Meteor-M2-4, 2026-11-22): OQPSK + differential, −3.34 kHz, −25 Hz/s fed forward\n");
    // ── 1. sensitivity where that pass was: the joint differential decode + acquisition on the matched filter ──
    for (double es : {3.0, 3.5, 5.0}) {
        if (quick && es == 3.0) continue;
        uint64_t blocks = 0; const int P = 8;
        for (uint32_t seed = 1; seed <= 2; seed++) {
            const auto iq = stretch(0, P, 100, es, 300000, seed, 43352840);
            ReceiverConfig rc; rc.sampleRate = 300000; rc.mod = ReceiverConfig::Oqpsk;
            Receiver rx(rc);
            const size_t blk = 15000; double tt = -0.5;
            for (size_t at = 0; at < iq.size() / 2; at += blk) {
                const size_t n = std::min(blk, iq.size() / 2 - at);
                rx.setDopplerHz(kDop0 + kRate * tt); rx.feedIq(&iq[at * 2], n); tt += n / 300000.0;
            }
            blocks += rx.stats().blocks;
        }
        const uint64_t want = uint64_t(2 * P * 3 * kMcuPerLine);
        std::printf("  Es/N0 %.1f dB: %llu of %llu picture blocks (%.0f %%)\n", es, (unsigned long long)blocks, (unsigned long long)want, 100.0 * double(blocks) / double(want));
        // ★ before beta91: 0 % at 3.5 dB and at 4 dB (the soft differential decode + the raw-slice acquisition)
        if (es >= 3.5) CHECK(blocks >= want * 6 / 10, "live-pass shape at Es/N0 %.1f: blocks %llu of %llu", es, (unsigned long long)blocks, (unsigned long long)want);
    }
    // ── 2. a fade and the recovery, through the server's Run: frames, 20 s of nothing, frames again ──
    //    ★ before beta91: the frames never came back (FrameSync::decoding() stayed true after the ASM was lost, so the
    //      acquisition was held off and the loop wandered in the fade) — and a "pass over" cut the picture in two.
    struct Case { const char* what; int inRange; double gapSec; int wantSaves; };
    const Case cases[] = {
        { "in range (the engine tracks it), 35 s of nothing: one picture across the gap", 1, 35, 1 },
        { "no pick, 35 s of nothing: the pass ends, a second picture after", -1, 35, 2 },
    };
    for (const Case& c : cases) {
        if (quick && c.inRange < 0) continue;
        const double fs = 150000;
        const int P = 6;
        const auto a = stretch(0, P, 100, 5.0, fs, 3, 43352840);
        const double tB = a.size() / 2 / fs + c.gapSec;
        const uint32_t ctrB = 100 + 200, msB = 43352840 + uint32_t(std::lround(tB / 1.232)) * 1232;
        const auto b = stretch(tB, P, ctrB, 5.0, fs, 4, msB);
        double tNow = -0.5;
        int saves = 0, partial = 0; uint64_t framesB = 0, framesA = 0;
        Run::Opts o; o.symbolRate = 72000; o.mod = ReceiverConfig::Oqpsk;
        o.dopplerHz = [&] { return kDop0 + kRate * tNow; };
        o.satInRange = [&] { return c.inRange; };
        o.pictures = [&](const std::vector<Picture>& ps, bool part) { if (!ps.empty()) { saves++; if (part) partial++; } };
        {
            Run run(o);
            const size_t blk = size_t(fs / 20);
            auto feed = [&](const float* x, size_t n) {
                for (size_t at = 0; at < n; at += blk) {
                    const size_t m = std::min(blk, n - at);
                    run.feedIq(reinterpret_cast<const std::complex<float>*>(x + at * 2), int(m), fs, 0);
                    tNow += m / fs;
                }
            };
            feed(a.data(), a.size() / 2);
            framesA = run.receiver() ? run.receiver()->stats().rsOk : 0;
            std::mt19937 rng(77); std::normal_distribution<float> g(0, float(sigmaFor(5.0, fs)));
            std::vector<float> noise(size_t(fs / 20) * 2);
            for (double t = 0; t < c.gapSec; t += 0.05) { for (auto& v : noise) v = g(rng); feed(noise.data(), noise.size() / 2); }
            const uint64_t before = run.receiver() ? run.receiver()->stats().rsOk : 0;
            feed(b.data(), b.size() / 2);
            framesB = (run.receiver() ? run.receiver()->stats().rsOk : 0) - (c.inRange < 0 ? 0 : before);
            run.finish(true);
        }
        std::printf("  %s: frames before %llu, after %llu, pictures filed %d (%d partial)\n", c.what, (unsigned long long)framesA, (unsigned long long)framesB, saves, partial);
        CHECK(framesA >= 12 && framesB >= 12, "%s: frames before %llu after %llu", c.what, (unsigned long long)framesA, (unsigned long long)framesB);
        CHECK(saves == c.wantSaves, "%s: %d pictures filed, want %d", c.what, saves, c.wantSaves);
    }
}

/* ★★★ beta92 THE PER-PASS SUMMARY LINE (the XCover's METEOR-M2-3 pass, 2026-11-23: "kept looking like it was going to work
 *  but nothing" — and nothing in the log could say how near it came). One line at LOS / the pass's end / the decoder
 *  stopping, with the best signal, the time at or above a decode's need, the rate, carrier / sync, frames, lines, gain. */
static void testPassSummary() {
    using namespace livepass;
    std::printf("\nThe per-pass summary line (beta92)\n");
    const double fs = 150000;
    // ── 1. a decoded pass, then LOS: the summary comes with the pictures, says "decoded", names the gain ──
    {
        const auto a = stretch(0, 6, 100, 5.0, fs, 3, 43352840);
        double tNow = -0.5; int inRange = 1;
        std::vector<std::string> logs;
        Run::Opts o; o.symbolRate = 72000; o.mod = ReceiverConfig::Oqpsk;
        o.dopplerHz = [&] { return kDop0 + kRate * tNow; };
        o.satInRange = [&] { return inRange; };
        o.log = [&](const std::string& l) { logs.push_back(l); };
        o.gainNote = [] { return std::string("Airspy linearity preset 12 (held, 0 clip cuts)"); };
        Run run(o);
        const size_t blk = size_t(fs / 20);
        auto feed = [&](const float* x, size_t n) {
            for (size_t at = 0; at < n; at += blk) { const size_t m = std::min(blk, n - at); run.feedIq(reinterpret_cast<const std::complex<float>*>(x + at * 2), int(m), fs, 0); tNow += m / fs; }
        };
        feed(a.data(), a.size() / 2);
        inRange = 0;   // LOS
        std::mt19937 rng(5); std::normal_distribution<float> g(0, float(sigmaFor(5.0, fs)));
        std::vector<float> noise(blk * 2);
        for (double t = 0; t < 8; t += 0.05) { for (auto& v : noise) v = g(rng); feed(noise.data(), noise.size() / 2); }
        const std::string& s = run.lastSummary();
        std::printf("  %s\n", s.c_str());
        CHECK(s.find("lrpt pass summary (LOS") == 0, "decoded pass: a summary at LOS");
        CHECK(s.find("decoded") != std::string::npos && s.find("symbol rate 72k") != std::string::npos, "says decoded, at 72k");
        CHECK(s.find("gain: Airspy linearity preset 12") != std::string::npos, "names the gain held");
        CHECK(run.passSum().frames == 0 && !run.passSum().heard, "the pass is closed with it: what follows LOS counts from nothing");
        int n = 0; for (auto& l : logs) if (l.rfind("lrpt pass summary", 0) == 0) ++n;
        run.finish(true);
        int m = 0; for (auto& l : logs) if (l.rfind("lrpt pass summary", 0) == 0) ++m;
        CHECK(n == 1 && m == 1, "one summary for the pass; the decoder stopping after LOS with nothing heard adds none (%d, %d)", n, m);
    }
    // ── 2. the "nearly" pass: a signal under what a decode needs, never synced, then LOS (no pictures to carry it) ──
    {
        const auto a = stretch(0, 4, 100, -2.0, fs, 9, 43352840);   // far under the need: heard, never a frame
        double tNow = -0.5; int inRange = 1;
        Run::Opts o; o.rateAuto = true; o.mod = ReceiverConfig::Oqpsk;
        o.dopplerHz = [&] { return kDop0 + kRate * tNow; };
        o.satInRange = [&] { return inRange; };
        Run run(o);
        const size_t blk = size_t(fs / 20);
        auto feed = [&](const float* x, size_t n) {
            for (size_t at = 0; at < n; at += blk) { const size_t m = std::min(blk, n - at); run.feedIq(reinterpret_cast<const std::complex<float>*>(x + at * 2), int(m), fs, 0); tNow += m / fs; }
        };
        feed(a.data(), a.size() / 2);
        inRange = 0;
        feed(a.data(), size_t(fs * 1.5));
        const std::string& s = run.lastSummary();
        std::printf("  %s\n", s.c_str());
        CHECK(s.find("lrpt pass summary (LOS") == 0, "a pass with no picture still gets its summary at LOS");
        CHECK(s.find("0 frames") != std::string::npos && s.find("AUTO, tried") != std::string::npos, "0 frames, and the rates AUTO tried");
        CHECK(s.find("gain: not known") != std::string::npos, "no gain note: says so");
    }
    // ── 3. the decoder stopped mid-pass: the summary says so ──
    {
        Run::Opts o; o.symbolRate = 72000;
        Run run(o);
        std::mt19937 rng(8); std::normal_distribution<float> g(0, 0.1f);
        std::vector<float> noise(size_t(fs / 20) * 2);
        for (double t = 0; t < 4; t += 0.05) { for (auto& v : noise) v = g(rng); run.feedIq(reinterpret_cast<const std::complex<float>*>(noise.data()), int(noise.size() / 2), fs, 0); }
        run.finish(true);
        std::printf("  %s\n", run.lastSummary().c_str());
        CHECK(run.lastSummary().find("lrpt pass summary (decoder stopped)") == 0, "decoder stopped: a summary");
        CHECK(run.lastSummary().find("nothing heard") != std::string::npos, "noise only: nothing heard");
    }
}

int main(int argc, char** argv) {
    ReceiverConfig c;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--qpsk")) c.mod = ReceiverConfig::Qpsk;
        if (!std::strcmp(argv[i], "--oqpsk")) c.mod = ReceiverConfig::Oqpsk;
        if (!std::strcmp(argv[i], "--80k")) { c.symbolRate = 80000; c.interleaved = true; }
        if (!std::strcmp(argv[i], "--scalar")) c.forceScalar = true;
        if (!std::strcmp(argv[i], "--rate-auto")) c.rateAuto = true;   // ★ what the clients send until the satellite engine names the rate
        if (!std::strcmp(argv[i], "--noise") && i + 1 < argc) g_addNoise = std::atof(argv[i + 1]);
    }
    if (argc >= 3 && !std::strcmp(argv[1], "--wav")) return decodeWav(argv[2], argc >= 4 && argv[3][0] != '-' ? argv[3] : ".", c);
    for (int i = 1; i + 1 < argc; i++) if (!std::strcmp(argv[i], "--es")) {
        tx::TxConfig t; t.esn0Db = std::atof(argv[i + 1]); t.carrierHz = 500; t.seed = 11;
        for (int j = 1; j + 1 < argc; j++) if (!std::strcmp(argv[j], "--seed")) t.seed = uint32_t(std::atoi(argv[j + 1]));
        ReceiverConfig rc; rc.mod = ReceiverConfig::Qpsk;
        line("one run", runSynthetic(t, 6, rc)); return 0;
    }
    bool quick = false; for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], "--quick")) quick = true;
    testRs();
    testViterbi();
    testSyncAndInterleave();
    testChain(quick);
    testNoise(quick);
    testLivePass(quick);
    testPassSummary();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
