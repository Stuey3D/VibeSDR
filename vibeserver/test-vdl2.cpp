// test-vdl2.cpp — V12 VDL Mode 2 (vibe_vdl2_fec.h, vibe_vdl2.h), docs/v12/AVIATION.md §VDL2.
//  `test-vdl2 --cu8 <file.u8> <rate> <hwCentreHz> [chanHz…]` decodes a REAL capture (VDL2_DEBUG=1: every failure).
#include "vibe_vdl2.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
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
static void putAddr(std::vector<uint8_t>& f, uint32_t addr, int type, int status, bool last) {
    const uint32_t r = (uint32_t(status) << 27) | (uint32_t(type) << 24) | (addr & 0xFFFFFFu);
    uint32_t raw = 0;
    for (int i = 0; i < 28; ++i) raw |= ((r >> i) & 1u) << (27 - i);
    for (int k = 0; k < 4; ++k) f.push_back(uint8_t(((raw >> (7 * k)) & 0x7F) << 1 | (last && k == 3 ? 1 : 0)));
}
static std::vector<uint8_t> avlcAcars(uint32_t src, uint32_t dst, const std::string& reg7, const std::string& label, char blk, const std::string& text) {
    std::vector<uint8_t> f;
    putAddr(f, dst, 4, 0, false);
    putAddr(f, src, 1, 0, true);
    f.push_back(0x10);                                  // an I frame
    f.push_back(0xFF); f.push_back(0xFF); f.push_back(0x01);
    std::vector<uint8_t> a;
    a.push_back(vibeacars::withParity('2'));
    for (char c : reg7) a.push_back(vibeacars::withParity(uint8_t(c)));
    a.push_back(vibeacars::withParity(0x15));
    for (char c : label) a.push_back(vibeacars::withParity(uint8_t(c)));
    a.push_back(vibeacars::withParity(uint8_t(blk)));
    a.push_back(vibeacars::withParity(vibeacars::STX));
    for (char c : text) a.push_back(vibeacars::withParity(uint8_t(c)));
    a.push_back(vibeacars::withParity(vibeacars::ETX));
    const uint16_t c = vibeacars::bcs(a.data(), a.size());
    a.push_back(uint8_t(c)); a.push_back(uint8_t(c >> 8)); a.push_back(0x7F);
    f.insert(f.end(), a.begin(), a.end());
    uint16_t fc = 0xFFFF;
    for (uint8_t x : f) { fc ^= x; for (int b = 0; b < 8; ++b) fc = (fc & 1) ? uint16_t((fc >> 1) ^ 0x8408) : uint16_t(fc >> 1); }
    fc = uint16_t(~fc);
    f.push_back(uint8_t(fc)); f.push_back(uint8_t(fc >> 8));
    return f;
}
/** The burst's 3-bit symbols after the unique word (header + data + parity), from frames already carrying their FCS. */
static std::vector<int> burstSymbols(const std::vector<std::vector<uint8_t>>& frames) {
    std::vector<uint8_t> bits;
    auto flag = [&] { for (int i = 0; i < 8; ++i) bits.push_back(i == 0 || i == 7 ? 0 : 1); };
    flag();
    for (const auto& f : frames) {
        int ones = 0;
        for (uint8_t x : f) for (int i = 0; i < 8; ++i) {
            const int b = (x >> i) & 1; bits.push_back(uint8_t(b));
            if (b) { if (++ones == 5) { bits.push_back(0); ones = 0; } } else ones = 0;
        }
        flag();
    }
    const int len = int(bits.size());
    const vibevdl2::Layout L((len + 7) / 8);
    std::vector<uint8_t> oct(size_t(L.octets), 0);
    for (int i = 0; i < len; ++i) oct[size_t(i / 8)] |= uint8_t(bits[size_t(i)] << (i % 8));
    std::vector<std::array<uint8_t, 255>> blk(size_t(L.blocks));
    for (auto& b : blk) b.fill(0);
    // the same column walk the receiver uses, the other way round
    vibevdl2::dealColumns(L.octets, L.blocks, vibevdl2::Rs::K, L.lastLen, 0, [&](int r, int c, int i) { blk[size_t(r)][size_t(c)] = oct[size_t(i)]; });
    for (auto& b : blk) vibevdl2::Rs::get().encode(b.data());
    std::vector<uint8_t> out(oct);
    std::vector<uint8_t> fec(size_t(L.fecOctets));
    const int fecRows = L.lastPar ? L.blocks : L.blocks - 1;
    vibevdl2::dealColumns(L.fecOctets, fecRows, vibevdl2::Rs::R, L.lastPar ? L.lastPar : vibevdl2::Rs::R, vibevdl2::Rs::K,
                          [&](int r, int c, int i) { fec[size_t(i)] = blk[size_t(r)][size_t(c)]; });
    out.insert(out.end(), fec.begin(), fec.end());
    // header + data bits, scrambled
    std::vector<uint8_t> tx;
    const uint32_t hw = vibevdl2::hdrEncode(uint32_t(len));
    for (int i = 24; i >= 0; --i) tx.push_back(uint8_t((hw >> i) & 1));
    for (uint8_t x : out) for (int i = 0; i < 8; ++i) tx.push_back(uint8_t((x >> i) & 1));
    vibevdl2::Lfsr l;
    for (auto& b : tx) b ^= uint8_t(l.next());
    while (tx.size() % 3) tx.push_back(0);
    std::vector<int> sym;
    for (size_t i = 0; i < tx.size(); i += 3) sym.push_back((tx[i] << 2) | (tx[i + 1] << 1) | tx[i + 2]);
    return sym;
}
/** Add one burst at capture rate fs: ramp-up (5 × 000), the unique word, `sym`, a raised-cosine pulse (α 0.6). */
static void addBurst(std::vector<cf32>& cap, double fs, double offHz, double A, size_t s0, const std::vector<int>& sym, double ph0) {
    std::vector<int> all(5, 0);
    for (int i = 0; i < 16; ++i) all.push_back(vibevdl2::kUw[i]);
    all.insert(all.end(), sym.begin(), sym.end());
    for (int i = 0; i < 3; ++i) all.push_back(0);
    std::vector<cf32> pts; int a = 0;
    for (int g : all) { a = (a + vibevdl2::stepOf(g)) & 7; pts.push_back(std::polar(1.0f, float(a) * float(M_PI / 4))); }
    const double sps = fs / vibevdl2::kSymRate, al = 0.6;
    auto rc = [&](double t) {   // t in symbols
        const double x = 4 * al * al * t * t;
        if (std::fabs(1 - x) < 1e-6) return M_PI / 4 * std::sin(M_PI / (2 * al)) / (M_PI / (2 * al));
        const double sc = t == 0 ? 1 : std::sin(M_PI * t) / (M_PI * t);
        return sc * std::cos(M_PI * al * t) / (1 - x);
    };
    const size_t n = size_t(double(pts.size()) * sps);
    double cph = ph0;
    for (size_t i = 0; i < n && s0 + i < cap.size(); ++i) {
        const double ts = double(i) / sps;
        const int k0 = int(std::floor(ts));
        cf32 v(0, 0);
        for (int k = k0 - 5; k <= k0 + 6; ++k) if (k >= 0 && k < int(pts.size())) v += pts[size_t(k)] * float(rc(ts - k));
        cph += 2 * M_PI * offHz / fs;
        cap[s0 + i] += v * float(A) * std::polar(1.0f, float(cph));
    }
}
struct Res { int good = 0, wrong = 0; };
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
    vibevdl2::Receiver rx(fs, hw, ch);
    Res r;
    for (size_t o = 0; o < total; o += 65536)
        rx.feed(cap.data() + o, std::min<size_t>(65536, total - o), [&](const vibevdl2::Frame& f) {
            if (!f.hasAcars) { ++r.wrong; return; }
            std::string t = f.acars.msgNo + f.acars.flight + f.acars.text;
            bool m = false; for (auto& x : sent) if (x == t) m = true;
            if (m && f.acarsCrcOk) ++r.good; else ++r.wrong;
            dig(t.data(), t.size());
        });
    return r;
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
    vibevdl2::Receiver rx(rate, hw, ch);
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
        rx.feed(z.data(), r, [&](const vibevdl2::Frame& fr) {
            ++n; acars += fr.hasAcars;
            if (fr.src.type == 1) aircraft[fr.src.addr]++;
            if (fr.dst.type == 1) aircraft[fr.dst.addr]++;
            if (!quiet) printFrame(fr);
        });
    }
    std::fclose(f);
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (size_t c = 0; c < rx.channels(); ++c) {
        const auto& s = rx.demod(c).stats();
        if (!s.syncs) continue;
        std::printf("ch %.3f: syncs %llu, header bad %llu, bursts %llu, RS fail %llu, frames %llu (FCS bad %llu), RS bytes fixed %llu, erasure saves %llu\n",
                    ch[c] / 1e6, (unsigned long long)s.syncs, (unsigned long long)s.hdrBad, (unsigned long long)s.bursts,
                    (unsigned long long)s.rsFail, (unsigned long long)s.frames, (unsigned long long)s.fcsBad,
                    (unsigned long long)s.rsFixed, (unsigned long long)s.erasureSaves);
    }
    std::printf("frames %d (ACARS %d), aircraft %zu, in %.0f s of capture; %.2f s here = %.2f %% of a core\n", n, acars, aircraft.size(),
                samples / rate, el, 100.0 * el / (samples / rate));
    return 0;
}

int main(int argc, char** argv) {
    // ★ experiment switches: VDL2_TUNE="g=0 er=0 rrc=1 q=0.75 fg=0.03 tg=0.01 coh=1 pa=0.15 pb=0.004"
    if (const char* t = std::getenv("VDL2_TUNE")) {
        auto& T = vibevdl2::tuning();
        std::string s(t);
        auto vi = [&](const char* k, bool& v) { const size_t p = s.find(k); if (p != std::string::npos) v = std::atoi(s.c_str() + p + std::strlen(k)) != 0; };
        auto vf = [&](const char* k, float& v) { const size_t p = s.find(k); if (p != std::string::npos) v = float(std::atof(s.c_str() + p + std::strlen(k))); };
        vi("g=", T.gardner); vf("bw=", T.rxBw); { const size_t q = s.find("rx="); if (q != std::string::npos) T.rxKind = std::atoi(s.c_str() + q + 3); } vf("tr=", T.rxTr); vi("coh=", T.coherent); vf("pa=", T.pllA); vf("pb=", T.pllB); vi("er=", T.erasures); vi("rrc=", T.rrc); vf("q=", T.syncQ); vf("fg=", T.freqGain); vf("tg=", T.tGain);
    }
    if (argc > 1 && !std::strcmp(argv[1], "--cu8")) return cu8(argc, argv);
    const bool quick = argc > 1 && !std::strcmp(argv[1], "--quick");
    if (argc > 1 && !std::strcmp(argv[1], "--sens")) {
        if (argc > 2) { const double snr = std::atof(argv[2]); Res r = synth(2.4e6, snr, 3, 1); std::printf("%d good %d wrong\n", r.good, r.wrong); return 0; }
        for (double fs : { 2.4e6, 2.048e6 }) for (double snr = 8; snr <= 20; snr += 2) {
            Res r = synth(fs, snr, 10, uint32_t(snr * 10 + fs / 1e5));
            std::printf("%.3f MS/s %4.1f dB: %2d of 30, wrong %d\n", fs / 1e6, snr, r.good, r.wrong);
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
                std::printf("               %4.0f dB      : %3d  wrong %d\n", snr, r.good, r.wrong);
                CHECK(r.wrong == 0, "wrong frames at %.0f dB", snr);
                if (snr >= 20) CHECK(r.good == (quick ? 9 : 24), "all frames at %.0f dB (%d)", snr, r.good);
            }
        }
    }

    std::printf("6. noise only: %s of capture, zero frames\n", quick ? "1 min" : "10 min");
    {
        const double fs = 2.4e6;
        vibevdl2::Receiver rx(fs, 136.850e6, vibevdl2::defaultChannels());
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

    std::printf("7. CPU: twelve channels at 2.4 MS/s (noise: the idle cost)\n");
    {
        const double fs = 2.4e6;
        vibevdl2::Receiver rx(fs, 136.850e6, vibevdl2::defaultChannels());
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
