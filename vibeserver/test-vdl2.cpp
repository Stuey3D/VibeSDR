// test-vdl2.cpp — V12 VDL Mode 2 (vibe_vdl2_fec.h, vibe_vdl2.h), docs/v12/AVIATION.md §VDL2.
//  `test-vdl2 --cu8 <file.u8> <rate> <hwCentreHz> [chanHz…]` decodes a REAL capture (VDL2_DEBUG=1: every failure).
#include "vibe_vdl2.h"
#include "vibe_avi_tx.h"   // ★ the transmitters (shared with the benchmark rows)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <random>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using vibevdl2::cf32;

static uint64_t g_digest = 1469598103934665603ull;
static void dig(const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; ++i) g_digest = (g_digest ^ b[i]) * 1099511628211ull; }

// ── the transmitter, for the tests: AVLC frame → HDLC bits → octets → RS + interleave → header → scramble → symbols ──
using vibeavitx::putAddr; using vibeavitx::avlcAcars;   // ★ vibe_avi_tx.h (the transmitters)
static std::vector<int> burstSymbols(const std::vector<std::vector<uint8_t>>& f) { return vibeavitx::vdl2Symbols(f); }
template <class... A> static void addBurst(A&&... a) { vibeavitx::vdl2Burst(std::forward<A>(a)...); }
struct Res { int good = 0, wrong = 0; std::vector<float> snr;   // ★ beta96 the good frames' Frame::snrDb
             float med() const { if (snr.empty()) return -99; auto v = snr; std::sort(v.begin(), v.end()); return v[v.size() / 2]; } };
static Res synth(double fs, double snrDb, int nb, uint32_t seed) {
    const double hw = 136.850e6;
    const std::vector<double> ch = { 136.775e6, 136.875e6, 136.975e6 };
    const double sigma = 0.01;   // per real component
    // SNR: signal power / noise power in the 10.5 kHz symbol bandwidth
    const double nPow = 2 * sigma * sigma * vibevdl2::kSymRate / fs;
    const double A = std::sqrt(nPow * std::pow(10.0, snrDb / 10));
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, float(sigma));
    const double per = 0.12;
    const size_t total = size_t(fs * (0.05 + per * nb));
    std::vector<cf32> cap(total);
    for (auto& v : cap) v = cf32(g(rng), g(rng));
    std::vector<std::string> sent;
    for (int i = 0; i < nb; ++i)
        for (size_t c = 0; c < ch.size(); ++c) {
            char txt[200];
            std::snprintf(txt, sizeof txt, "M%02dAEZY%04d/POS N51%03d W001%03d,%05d TEST %d CH %zu", i, int(rng() % 10000), int(rng() % 1000), int(rng() % 1000), int(rng() % 40000), i, c);
            auto f = avlcAcars(0x400000u + uint32_t(rng() % 0xFFFF), 0x10949Au, ".G-EZWK", "H1", char('0' + i % 10), txt);
            sent.push_back(txt);
            const double off = (ch[c] - hw) + (double(int(rng() % 801)) - 400.0);
            addBurst(cap, fs, off, A, size_t(fs * (0.02 + per * i + 0.01 * c)), burstSymbols({ f }), (rng() % 1000) * 0.00628);
        }
    vibevdl2::Receiver rx(fs, hw, ch, 0);
    Res r;
    for (size_t o = 0; o < total; o += 65536)
        rx.feed(cap.data() + o, std::min<size_t>(65536, total - o), [&](const vibevdl2::Frame& f) {
            if (!f.hasAcars) { ++r.wrong; return; }
            std::string t = f.acars.msgNo + f.acars.flight + f.acars.text;
            bool m = false; for (auto& x : sent) if (x == t) m = true;
            if (m && f.acarsCrcOk) { ++r.good; r.snr.push_back(f.snrDb); } else ++r.wrong;
            dig(t.data(), t.size());
        });
    return r;
}

/** ★ `test-vdl2 --write-clip out.cu8 rate` — a 20 s capture centred on 136.850 MHz (unsigned 8-bit IQ, I first) for the
 *  server's end-to-end test (scripts/test-server-avdata.mjs, through vibeserver/fake-rtl-tcp.mjs): five aircraft with
 *  fixed ICAO addresses and registrations, ACARS over AVLC on 136.775 / 136.875 / 136.975, 25 dB, every 0.6 s; three of
 *  them report a position (FMS, label 16, ADS-C), one an OOOI OUT, one free text. */
static int writeClip(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: test-vdl2 --write-clip out.cu8 rate\n"); return 2; }
    const double fs = std::atof(argv[3]), hw = 136.850e6;
    const std::vector<double> ch = { 136.775e6, 136.875e6, 136.975e6 };
    const double sigma = 0.02, nPow = 2 * sigma * sigma * vibevdl2::kSymRate / fs, A = std::sqrt(nPow * std::pow(10.0, 25.0 / 10));
    static const struct { uint32_t icao; const char* reg; const char* flight; } ac[] = {
        { 0x4CA97B, ".EI-EST", "FR1234" }, { 0x4070E8, ".G-JZHU", "LS08DE" }, { 0x3C6548, ".D-AIJH", "LH4567" },
        { 0x781361, ".B-1243", "CA0937" }, { 0x406B90, ".G-EZWK", "U22468" } };
    std::mt19937 rng(42);
    std::normal_distribution<float> g(0.f, float(sigma));
    const size_t total = size_t(fs * 20);
    std::vector<cf32> cap(total);
    for (auto& v : cap) v = cf32(g(rng), g(rng));
    int k = 0;
    for (double t = 0.1; t < 19.5; t += 0.6, ++k) {
        const auto& a = ac[k % 5];
        // ★ the applications (vibe_acars_apps.h): EI-EST an FMS position (H1), G-JZHU a label 16 position, D-AIJH an ADS-C
        //   basic report (B6), G-EZWK an OOOI OUT (QP); B-1243 free text (the body the owner's opt-in is tested with)
        char pre[16]; std::snprintf(pre, sizeof pre, "M%02dA%s", k % 100, a.flight);   // msg no (4) + flight (6)
        std::string label = "H1", body;
        switch (k % 5) {
            case 0: body = vibeavitx::fmsPos("N53214W002164", 212000, 370); break;                  // 53.357 N, 2.273 W
            case 1: label = "16"; body = "N 52.480,W  1.900,24000,6, 290"; break;
            case 2: label = "B6"; body = vibeavitx::adsCBasic("EGLLXYA", a.reg, 51.75, -0.5, 36000, 1234.5); break;
            case 3: { char t[48]; std::snprintf(t, sizeof t, "TEST MESSAGE BODY %d", k); body = t; break; }
            default: label = "QP"; body = "EGLLLEMD1015"; break;
        }
        auto f = avlcAcars(a.icao, 0x10949Au, a.reg, label, char('0' + k % 10), std::string(pre) + body);
        addBurst(cap, fs, ch[size_t(k % 3)] - hw, A, size_t(fs * t), burstSymbols({ f }), (rng() % 1000) * 0.00628);
    }
    FILE* o = std::fopen(argv[2], "wb");
    if (!o) { std::perror(argv[2]); return 2; }
    std::vector<uint8_t> u(total * 2);
    for (size_t i = 0; i < total; ++i) {
        auto q = [](float x) { const int v = int(std::lround(127.5f + 127.5f * x)); return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); };
        u[2 * i] = q(cap[i].real()); u[2 * i + 1] = q(cap[i].imag());
    }
    std::fwrite(u.data(), 1, u.size(), o);
    std::fclose(o);
    std::printf("wrote %s: %zu samples at %.0f S/s, %d bursts\n", argv[2], total, fs, k);
    return 0;
}

static void printFrame(const vibevdl2::Frame& f) {
    std::printf("%8.3f %.3f lvl %5.1f snr %4.1f ferr %+5.0f Hz rs %d hdr %d er %d | %06X %-8s -> %06X %-8s %c%s %s", f.t, f.hz / 1e6,
                f.levelDb, f.snrDb, f.freqErrHz, f.rsFixed, f.hdrFixed, f.erased, f.src.addr, vibevdl2::addrKind(f.src.type),
                f.dst.addr, vibevdl2::addrKind(f.dst.type), f.kind, f.uName.empty() ? "" : (" " + f.uName).c_str(), f.airborne ? "" : "(on ground)");
    if (f.hasAcars) {
        std::string t = f.acars.text; for (auto& c : t) if (c == '\r' || c == '\n') c = ' ';
        std::printf(" | ACARS%s %s lbl %s flt %s | %s", f.acarsCrcOk ? "" : " (BCS bad)", f.acars.reg.c_str(), f.acars.label.c_str(), f.acars.flight.c_str(), t.c_str());
    } else std::printf(" | %zu bytes", f.bytes.size());
    std::printf("\n");
}

static int cu8(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: test-vdl2 --cu8 file rate hwCentreHz [chanHz...]\n"); return 2; }
    const double rate = std::atof(argv[3]), hw = std::atof(argv[4]);
    std::vector<double> ch;
    for (int i = 5; i < argc; ++i) ch.push_back(std::atof(argv[i]));
    if (ch.empty()) ch = vibevdl2::defaultChannels();
    FILE* f = std::fopen(argv[2], "rb");
    if (!f) { std::perror(argv[2]); return 2; }
    const int nw = std::getenv("VDL2_WORKERS") ? std::atoi(std::getenv("VDL2_WORKERS")) : 0;
    vibevdl2::Receiver rx(rate, hw, ch, nw, /*lossless=*/true);
    std::vector<vibevdl2::Frame> all;
    std::vector<uint8_t> buf(1 << 18);
    std::vector<cf32> z(1 << 17);
    size_t samples = 0; int n = 0, acars = 0;
    std::map<uint32_t, int> aircraft;
    const auto t0 = std::chrono::steady_clock::now();
    const bool quiet = std::getenv("VDL2_QUIET") != nullptr;
    while (true) {
        const size_t r = std::fread(buf.data(), 2, 1 << 17, f);
        if (!r) break;
        vibeavi::u8ToCf(buf.data(), r, z.data());
        samples += r;
        rx.feed(z.data(), r, [&](const vibevdl2::Frame& fr) { all.push_back(fr); });
    }
    std::fclose(f);
    rx.flush([&](const vibevdl2::Frame& fr) { all.push_back(fr); });
    // ★ workers hand frames out in their own order: print in (channel, time) order so every run compares line for line
    std::stable_sort(all.begin(), all.end(), [](const vibevdl2::Frame& a, const vibevdl2::Frame& b) { return a.channel != b.channel ? a.channel < b.channel : a.t < b.t; });
    for (const auto& fr : all) {
        ++n; acars += fr.hasAcars;
        if (fr.src.type == 1) aircraft[fr.src.addr]++;
        if (fr.dst.type == 1) aircraft[fr.dst.addr]++;
        if (!quiet) printFrame(fr);
    }
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (size_t c = 0; c < rx.channels(); ++c) {
        const auto& s = rx.demod(c).stats();
        if (!s.syncs) continue;
        std::printf("ch %.3f: searched %.0f %%, syncs %llu, header bad %llu, bursts %llu, RS fail %llu, frames %llu (FCS bad %llu), RS bytes fixed %llu, erasure saves %llu\n",
                    ch[c] / 1e6, 100.0 * s.searched / std::max<uint64_t>(1, s.samples), (unsigned long long)s.syncs, (unsigned long long)s.hdrBad, (unsigned long long)s.bursts,
                    (unsigned long long)s.rsFail, (unsigned long long)s.frames, (unsigned long long)s.fcsBad,
                    (unsigned long long)s.rsFixed, (unsigned long long)s.erasureSaves);
    }
    std::printf("frames %d (ACARS %d), aircraft %zu, in %.0f s of capture; %.2f s here = %.2f %% of a core (%d workers, %llu blocks dropped)\n", n, acars, aircraft.size(),
                samples / rate, el, 100.0 * el / (samples / rate), rx.workers(), (unsigned long long)rx.dropped());
    return 0;
}

int main(int argc, char** argv) {
    // ★ experiment switches: VDL2_TUNE="g=0 er=0 rrc=1 q=0.75 fg=0.03 tg=0.01 coh=1 pa=0.15 pb=0.004"
    if (const char* t = std::getenv("VDL2_TUNE")) {
        auto& T = vibevdl2::tuning();
        std::string s(t);
        auto vi = [&](const char* k, bool& v) { const size_t p = s.find(k); if (p != std::string::npos) v = std::atoi(s.c_str() + p + std::strlen(k)) != 0; };
        auto vf = [&](const char* k, float& v) { const size_t p = s.find(k); if (p != std::string::npos) v = float(std::atof(s.c_str() + p + std::strlen(k))); };
        vi("g=", T.gardner); vf("bw=", T.rxBw); vi("deep=", T.deepStop); vi("fd=", T.fdFilter); vi("poly=", T.polyInterp); { const size_t q = s.find("nt="); if (q != std::string::npos) T.interpTaps = std::atoi(s.c_str() + q + 3); } vf("beta=", T.kaiserBeta); vf("gate=", T.gate); { const size_t q = s.find("shape="); if (q != std::string::npos) T.fdShape = std::atoi(s.c_str() + q + 6); } vf("bin=", T.binHz); { const size_t q = s.find("rx="); if (q != std::string::npos) T.rxKind = std::atoi(s.c_str() + q + 3); } vf("tr=", T.rxTr); vi("coh=", T.coherent); vf("pa=", T.pllA); vf("pb=", T.pllB); vi("er=", T.erasures); vi("rrc=", T.rrc); vf("q=", T.syncQ); vf("fg=", T.freqGain); vf("tg=", T.tGain);
    }
    if (argc > 1 && !std::strcmp(argv[1], "--cu8")) return cu8(argc, argv);
    if (argc > 1 && !std::strcmp(argv[1], "--write-clip")) return writeClip(argc, argv);
    const bool quick = argc > 1 && !std::strcmp(argv[1], "--quick");
    if (argc > 1 && !std::strcmp(argv[1], "--sens")) {
        if (argc > 2) { const double snr = std::atof(argv[2]); Res r = synth(2.4e6, snr, 3, 1); std::printf("%d good %d wrong\n", r.good, r.wrong); return 0; }
        for (double fs : { 2.4e6, 2.048e6 }) for (double snr = 8; snr <= 20; snr += 2) {
            Res r = synth(fs, snr, 10, uint32_t(snr * 10 + fs / 1e5));
            std::printf("%.3f MS/s %4.1f dB: %2d of 30, wrong %d, reported SNR %.1f dB\n", fs / 1e6, snr, r.good, r.wrong, r.med());
        }
        return 0;
    }
    std::mt19937 rng(3);

    std::printf("1. the scrambler and the header code\n");
    {
        vibevdl2::Lfsr l; const uint16_t s0 = l.s; int period = 0;
        do { l.next(); ++period; } while (l.s != s0 && period < 70000);
        CHECK(period == 32767, "LFSR period %d (x^15+x+1 is primitive: 32767)", period);
        int ok = 0, tot = 0;
        for (uint32_t len : { 88u, 250u, 421u, 4802u, 15367u, 131071u }) {
            const uint32_t w = vibevdl2::hdrEncode(len);
            CHECK(vibevdl2::hdrSyndrome(w) == 0 && vibevdl2::hdrLength(w) == len, "header %u", len);
            for (int b = 0; b < 22; ++b) { uint32_t e = w ^ (1u << b); ++tot; if (vibevdl2::hdrCorrect(e) == 1 && vibevdl2::hdrLength(e) == len) ++ok; }
        }
        CHECK(ok == tot, "every single-bit header error corrected (%d of %d)", ok, tot);
        // the real header off the air (Pi 500, 136.975, 22:10:48 UTC): 250 bits, syndrome 0
        uint32_t real = 0x17C01Du;    // 0000101111100000000011101: the reserved bits zero
        CHECK(vibevdl2::hdrSyndrome(real) == 0 && vibevdl2::hdrLength(real) == 250, "real header: syndrome %u length %u", vibevdl2::hdrSyndrome(real), vibevdl2::hdrLength(real));
    }

    std::printf("2. Reed-Solomon (255,249): errors, erasures, and the shortened block\n");
    {
        const auto& rs = vibevdl2::Rs::get();
        int good = 0, tot = 0, wrongOk = 0, beyond = 0;
        for (int it = 0; it < 3000; ++it) {
            std::array<uint8_t, 255> cw{};
            for (int i = 0; i < 249; ++i) cw[size_t(i)] = uint8_t(rng());
            rs.encode(cw.data());
            const auto orig = cw;
            const int ne = int(rng() % 4), nf = int(rng() % 7);
            int eras[6], nE = 0;
            std::vector<int> used;
            auto pick = [&] { int p; do { p = int(rng() % 255); } while (std::find(used.begin(), used.end(), p) != used.end()); used.push_back(p); return p; };
            for (int k = 0; k < ne; ++k) cw[size_t(pick())] ^= uint8_t(1 + rng() % 255);
            for (int k = 0; k < nf && nE < 6; ++k) { const int p = pick(); eras[nE++] = p; cw[size_t(p)] ^= uint8_t(rng()); }
            const int res = rs.decode(cw.data(), eras, nE, [](int) { return true; });
            if (2 * ne + nE <= 6) { ++tot; if (res >= 0 && cw == orig) ++good; }
            else { ++beyond; if (res >= 0 && cw != orig) ++wrongOk; }
        }
        std::printf("   within 2e+f <= 6: %d of %d corrected; beyond: %d tries, %d decoded into a wrong word (the FCS catches those)\n", good, tot, beyond, wrongOk);
        CHECK(good == tot, "RS within its power");
        // the real block off the air: 32 data octets, 4 parity (2 erased) — clean
        const char* hex = "7e1442825e504c3a67ca1bdf0d37c8808722b610c6745e0b8605b00f5403fa0131612a12";
        std::array<uint8_t, 255> cw{};
        for (int i = 0; i < 36; ++i) { unsigned v; std::sscanf(hex + 2 * i, "%2x", &v); if (i < 32) cw[size_t(i)] = uint8_t(v); else cw[size_t(249 + i - 32)] = uint8_t(v); }
        int er[2] = { 253, 254 };
        const auto before = cw;
        const int res = rs.decode(cw.data(), er, 2, [](int i) { return i < 32 || i >= 249; });
        bool dataSame = true; for (int i = 0; i < 32; ++i) dataSame &= cw[size_t(i)] == before[size_t(i)];
        CHECK(res >= 0 && dataSame, "the real block decodes with no data byte changed (res %d)", res);
        // its HDLC frame passes the FCS
        std::vector<uint8_t> bits;
        for (int i = 0; i < 250; ++i) bits.push_back(uint8_t((cw[size_t(i / 8)] >> (i % 8)) & 1));
        auto fr = vibevdl2::hdlcFrames(bits);
        CHECK(fr.size() == 1 && vibevdl2::fcsGood(fr[0].data(), fr[0].size()), "the real frame's FCS");
        if (fr.size() == 1) {
            vibevdl2::Frame f; vibevdl2::parseFrame(fr[0], f);
            std::printf("   real frame: %06X %s -> %06X %s, %c%s\n", f.src.addr, vibevdl2::addrKind(f.src.type), f.dst.addr, vibevdl2::addrKind(f.dst.type), f.kind, f.uName.c_str());
        }
    }

    std::printf("3. the interleave: every layout round trips\n");
    {
        int bad = 0;
        for (int oct : { 1, 2, 3, 30, 31, 67, 68, 248, 249, 250, 497, 498, 499, 1000, 2047 }) {
            const vibevdl2::Layout L(oct);
            std::vector<int> where(size_t(oct), -1);
            vibevdl2::dealColumns(oct, L.blocks, vibevdl2::Rs::K, L.lastLen, 0, [&](int r, int c, int i) { where[size_t(i)] = r * 255 + c; });
            std::vector<int> seen;
            for (int w : where) { if (w < 0) ++bad; seen.push_back(w); }
            std::sort(seen.begin(), seen.end());
            if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) ++bad;
            int last = 0; for (int w : where) if (w / 255 == L.blocks - 1) ++last;
            if (last != L.lastLen) ++bad;
        }
        CHECK(bad == 0, "interleave layouts (%d faults)", bad);
    }

    std::printf("4. a whole burst through the coding layers (no radio): frames out = frames in\n");
    {
        auto f1 = avlcAcars(0x4070E8u, 0x109E6Au, ".G-JZHU", "H1", '3', "#DFB 0  001100258.7437.5N40.9910E 23.8216316");
        auto f2 = avlcAcars(0x406539u, 0x10914Au, ".G-EZGJ", "H2", '4', "02E31GMMXEGGWN45520W00444021213801M587100017G");
        auto sym = burstSymbols({ f1, f2 });
        CHECK(sym.size() > 100, "symbols %zu", sym.size());
        // the receiver's own decode path is exercised in 5; here check the first header symbols descramble to a valid header
        std::vector<uint8_t> bits; for (int s : sym) for (int b = 2; b >= 0; --b) bits.push_back(uint8_t((s >> b) & 1));
        vibevdl2::Lfsr l; uint32_t w = 0; for (int i = 0; i < 25; ++i) w = (w << 1) | uint32_t(bits[size_t(i)] ^ l.next());
        CHECK(vibevdl2::hdrSyndrome(w) == 0, "generated header");
    }

    std::printf("5. the receiver on synthetic captures (3 channels; ±400 Hz; raised cosine 0.6)\n");
    {
        for (double fs : { 2.4e6, 2.048e6 }) {
            std::printf("   %.3f MS/s   SNR (10.5 kHz) : frames of %d\n", fs / 1e6, quick ? 9 : 24);
            for (double snr : { 10.0, 12.0, 14.0, 16.0, 20.0, 30.0 }) {
                Res r = synth(fs, snr, quick ? 3 : 8, uint32_t(snr * 7 + fs / 1e5));
                std::printf("               %4.0f dB      : %3d  wrong %d  reported SNR %.1f dB\n", snr, r.good, r.wrong, r.med());
                // ★ beta96 the reported SNR (burst power over the channel floor, MESSAGE-SNR.md) follows the transmitted one
                if (r.snr.size() >= 3) CHECK(r.med() > float(snr) - 6 && r.med() < float(snr) + 6, "VDL2 reported SNR %.1f dB at %.0f dB", r.med(), snr);
                CHECK(r.wrong == 0, "wrong frames at %.0f dB", snr);
                if (snr >= 20) CHECK(r.good >= (quick ? 8 : 22), "frames at %.0f dB (%d): the RNG differs between libc++ and libstdc++, so a statistical bar, not all", snr, r.good);
            }
        }
    }

    std::printf("6. noise only: %s of capture, zero frames\n", quick ? "1 min" : "10 min");
    {
        const double fs = 2.4e6;
        vibevdl2::Receiver rx(fs, 136.850e6, vibevdl2::defaultChannels(), 0);
        std::normal_distribution<float> g(0.f, 0.05f);
        std::vector<cf32> blk(1 << 16);
        int got = 0;
        const size_t total = size_t(fs * (quick ? 60 : 600));
        for (size_t o = 0; o < total; o += blk.size()) {
            for (auto& v : blk) v = cf32(g(rng), g(rng));
            rx.feed(blk.data(), blk.size(), [&](const vibevdl2::Frame&) { ++got; });
        }
        uint64_t syncs = 0, bursts = 0; for (size_t c = 0; c < rx.channels(); ++c) { syncs += rx.demod(c).stats().syncs; bursts += rx.demod(c).stats().bursts; }
        std::printf("   %d frames (sync words matched %llu, headers passed %llu, 12 channels)\n", got, (unsigned long long)syncs, (unsigned long long)bursts);
        CHECK(got == 0, "noise produced %d frames", got);
    }

    std::printf("6b. the worker ring: the producer never waits; overwritten blocks are skipped and counted\n");
    {
        vibeavi::BlockRing ring(8, 16);
        std::atomic<bool> run{true};
        std::vector<cf32> b(16), o(16);
        for (int i = 0; i < 20; ++i) { b[0] = cf32(float(i), 0); ring.push(b.data(), i, false, run); }
        long long bi = -1; int got = 0; float first = -1;
        while (ring.pop(o.data(), bi)) { if (first < 0) first = o[0].real(); ++got; CHECK(o[0].real() == float(bi), "block %lld carries its own data", bi); }
        CHECK(got == 8 && first == 12.f && ring.dropped() == 12, "kept the newest 8 (got %d, first %.0f), dropped %llu", got, first, (unsigned long long)ring.dropped());
        // threaded == inline on a synthetic capture
        const double fs = 2.4e6;
        std::vector<std::string> ref;
        for (int w : { 0, 3 }) {
            std::mt19937 r2(5);
            vibevdl2::Receiver rx(fs, 136.850e6, { 136.775e6, 136.875e6, 136.975e6 }, w, true);
            std::normal_distribution<float> g(0.f, 0.01f);
            const double A = 0.05;
            std::vector<cf32> cap(size_t(fs * 0.5));
            for (auto& v : cap) v = cf32(g(r2), g(r2));
            for (int c = 0; c < 3; ++c) addBurst(cap, fs, -75e3 + 100e3 * c, A, size_t(fs * (0.02 + 0.1 * c)), burstSymbols({ avlcAcars(0x400100u + uint32_t(c), 0x10949Au, ".G-EZWK", "H1", '1', "THREADS TEST") }), 0.3);
            std::vector<std::string> got2;
            auto on = [&](const vibevdl2::Frame& f) { got2.push_back(std::to_string(f.channel) + ":" + std::to_string(f.src.addr)); };
            for (size_t o2 = 0; o2 < cap.size(); o2 += 65536) rx.feed(cap.data() + o2, std::min<size_t>(65536, cap.size() - o2), on);
            rx.flush(on);
            std::sort(got2.begin(), got2.end());
            if (w == 0) ref = got2; else CHECK(got2 == ref && ref.size() == 3, "3 workers decode what inline does (%zu vs %zu)", got2.size(), ref.size());
        }
    }

    std::printf("7. CPU: twelve channels at 2.4 MS/s (noise: the idle cost)\n");
    {
        const double fs = 2.4e6;
        vibevdl2::Receiver rx(fs, 136.850e6, vibevdl2::defaultChannels(), 0);
        std::vector<cf32> blk(1 << 16);
        std::normal_distribution<float> g(0.f, 0.05f);
        for (auto& v : blk) v = cf32(g(rng), g(rng));
        const int reps = int(fs * 20 / blk.size());
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < reps; ++i) rx.feed(blk.data(), blk.size(), [&](const vibevdl2::Frame&) {});
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("   %.3f s for 20 s = %.2f %% of a core here (%d half-band stages, FFT %d, %d-bin channels at %.0f S/s)\n", el, 100 * el / 20,
                    rx.front().stages(), rx.front().fftSize(), rx.front().chanBins(), rx.front().chanRate());
    }
    std::printf("digest %016llx\n", (unsigned long long)g_digest);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
