// test-rs41.cpp — V12 RS41 radiosonde (vibe_rs41.h, vibe_rs41_rx.h, vibe_rs41_tx.h), docs/v12/RS41.md.
//  The documentation's own facts cross-checked against each other, Reed–Solomon, the frame both ways, the calibration
//  and temperature, the SIMD discriminator against double precision, then OUR transmitter through the whole receiver:
//  SNR and frequency-offset sweeps, several sondes at once, 2.048 MS/s, extended frames, and noise / spurs / a
//  look-alike GFSK with zero false sondes.
//  `test-rs41 --write-clip out.cu8 rate` writes the server end-to-end test's capture (403.005 MHz, three sondes).
//  `test-rs41 --cu8 file.u8 rate hwCentreHz [loHz hiHz]` decodes a REAL capture (once there is one).
#include "vibe_rs41_rx.h"
#include "vibe_rs41_tx.h"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using vibers41::cf32;
using namespace vibers41;

struct SimSonde {
    vibers41tx::Sonde s;
    double offHz = 0, snrDb = 20, devHz = 2400, ppm = 0;
    bool continuous = false;
    explicit SimSonde(uint32_t seed) : s(seed) {}
};
struct SimResult {
    std::map<std::string, int> good;     // serial → correct frames
    int wrong = 0, sent = 0;
    double cpuS = 0, audioS = 0;
    std::vector<Frame> frames;
};

/** `secs` of capture at `fs` centred on `hw`; each sonde sends a frame every second from slot 0. Noise σ 0.02 per
 *  component; SNR = signal power over the noise in 10 kHz. Optional extras: carriers (spurs) and a random-bits GFSK. */
static SimResult simulate(double fs, double hw, std::vector<SimSonde>& ss, int secs, uint32_t seed, int maxCh = 6,
                          const std::vector<double>& spurs = {}, double lookalikeOff = 1e9, bool noiseOnly = false) {
    const double sigma = 0.02, n10k = 2 * sigma * sigma * 10e3 / fs;
    std::vector<vibers41tx::Gfsk> tx;
    for (auto& x : ss) {
        const double A = std::sqrt(n10k * std::pow(10.0, x.snrDb / 10));
        tx.emplace_back(fs, x.offHz, A, x.devHz, 0.05 + 0.013 * tx.size(), x.ppm, x.continuous);
        for (int k = 0; k < secs; ++k) tx.back().addFrame(k, vibers41tx::frameOnAir(x.s, k));
    }
    // ★ a look-alike: 4800 bit/s GFSK, random bits, the same burst rhythm — no header, no frame
    std::unique_ptr<vibers41tx::Gfsk> look;
    if (lookalikeOff < 1e8) {
        look.reset(new vibers41tx::Gfsk(fs, lookalikeOff, std::sqrt(n10k * 100), 2400, 0.2));
        std::mt19937 r(seed + 99);
        for (int k = 0; k < secs; ++k) { std::vector<uint8_t> b(320); for (auto& v : b) v = uint8_t(r()); look->addFrame(k, b); }
    }
    vibers41::Receiver rx(fs, hw, hw - 1.0e6, hw + 1.0e6, maxCh);
    SimResult res;
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, float(sigma));
    const size_t total = size_t(fs * secs), chunk = 65536;
    std::vector<cf32> buf(chunk);
    std::vector<double> spPh(spurs.size(), 0.0);
    for (size_t base = 0; base < total; base += chunk) {
        const size_t n = std::min(chunk, total - base);
        for (size_t i = 0; i < n; ++i) buf[i] = cf32(g(rng), g(rng));
        if (!noiseOnly) for (auto& t : tx) t.render(buf.data(), n, base);
        if (look) look->render(buf.data(), n, base);
        for (size_t s = 0; s < spurs.size(); ++s)
            for (size_t i = 0; i < n; ++i) { buf[i] += cf32(float(0.05 * std::cos(spPh[s])), float(0.05 * std::sin(spPh[s]))); spPh[s] += 2 * M_PI * spurs[s] / fs; }
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(buf.data(), n, [&](const Frame& f) {
            res.frames.push_back(f);
            bool ok = false;
            for (auto& x : ss) {
                if (f.serial != x.s.serial) continue;
                const int k = f.frameNo - x.s.frame0;
                if (k < 0 || k >= secs) break;
                double la, lo, h;
                x.s.at(k, la, lo, h);
                ok = f.posOk && std::fabs(f.lat - la) < 1e-5 && std::fabs(f.lon - lo) < 1e-5 && std::fabs(f.alt - h) < 0.5
                     && std::fabs(f.vU - x.s.vU) < 0.05 && std::fabs(f.hz - (hw + x.offHz)) < 1500;
                break;
            }
            if (ok) ++res.good[f.serial]; else ++res.wrong;
        });
        res.cpuS += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    res.audioS = secs;
    res.sent = int(ss.size()) * secs;
    return res;
}

static int writeClip(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: test-rs41 --write-clip out.cu8 rate\n"); return 2; }
    const double fs = std::atof(argv[3]), hw = 403.005e6;
    const int secs = 20;
    const struct { const char* serial; double hz; double lat, lon, alt, vU; } def[] = {
        { "S3310411", 403.300e6, 50.218, -5.327, 3400, 5.2 },      // a Camborne-like sonde, climbing
        { "T1520789", 402.700e6, 51.410, 0.420, 18000, 5.8 },
        { "U0912345", 403.850e6, 52.300, -1.100, 26000, -14.0 },   // descending after burst
    };
    std::vector<vibers41tx::Gfsk> tx;
    const double sigma = 0.02, n10k = 2 * sigma * sigma * 10e3 / fs;
    for (int i = 0; i < 3; ++i) {
        vibers41tx::Sonde s(uint32_t(11 + i));
        s.serial = def[i].serial; s.lat = def[i].lat; s.lon = def[i].lon; s.alt = def[i].alt; s.vU = def[i].vU; s.setTx(def[i].hz);
        s.frame0 = 3000 + 17 * i;
        tx.emplace_back(fs, def[i].hz - hw, std::sqrt(n10k * std::pow(10.0, 20.0 / 10)), 2400, 0.05 + 0.31 * i);
        for (int k = 0; k < secs; ++k) tx.back().addFrame(k, vibers41tx::frameOnAir(s, k));
    }
    FILE* o = std::fopen(argv[2], "wb");
    if (!o) { std::perror(argv[2]); return 2; }
    std::mt19937 rng(42);
    std::normal_distribution<float> g(0.f, float(sigma));
    const size_t total = size_t(fs * secs), chunk = 65536;
    std::vector<cf32> buf(chunk);
    std::vector<uint8_t> u(chunk * 2);
    for (size_t base = 0; base < total; base += chunk) {
        const size_t n = std::min(chunk, total - base);
        for (size_t i = 0; i < n; ++i) buf[i] = cf32(g(rng), g(rng));
        for (auto& t : tx) t.render(buf.data(), n, base);
        auto q = [](float x) { const int v = int(std::lround(127.5f + 127.5f * x)); return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); };
        for (size_t i = 0; i < n; ++i) { u[2 * i] = q(buf[i].real()); u[2 * i + 1] = q(buf[i].imag()); }
        std::fwrite(u.data(), 1, n * 2, o);
    }
    std::fclose(o);
    std::printf("wrote %s: %zu samples at %.0f S/s, centre %.3f MHz, 3 sondes\n", argv[2], total, fs, hw / 1e6);
    return 0;
}

static int decodeCu8(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: test-rs41 --cu8 file.u8 rate hwCentreHz [loHz hiHz]\n"); return 2; }
    const double fs = std::atof(argv[3]), hw = std::atof(argv[4]);
    const double lo = argc > 6 ? std::atof(argv[5]) : hw - fs / 2, hi = argc > 6 ? std::atof(argv[6]) : hw + fs / 2;
    FILE* f = std::fopen(argv[2], "rb");
    if (!f) { std::perror(argv[2]); return 2; }
    vibers41::Receiver rx(fs, hw, lo, hi, 8);
    std::vector<uint8_t> u(1 << 17);
    std::vector<cf32> c(u.size() / 2);
    size_t got, at = 0; int frames = 0;
    while ((got = std::fread(u.data(), 1, u.size(), f)) >= 2) {
        const size_t n = got / 2;
        for (size_t i = 0; i < n; ++i) c[i] = cf32((u[2 * i] - 127.4f) / 128, (u[2 * i + 1] - 127.4f) / 128);
        rx.feed(c.data(), n, [&](const Frame& fr) {
            ++frames;
            std::printf("%8.2f s %.4f MHz snr %4.1f rs %2d %s #%d %s lat %.5f lon %.5f alt %.0f climb %.1f sats %d cal %d\n",
                        at / fs, fr.hz / 1e6, fr.snrDb, fr.rsFixed, fr.serial.c_str(), fr.frameNo, fr.encrypted ? "ENC" : "",
                        fr.lat, fr.lon, fr.alt, fr.vU, fr.sats, fr.calIndex);
        });
        at += n;
    }
    std::fclose(f);
    std::printf("%d frames, %llu channels opened\n", frames, (unsigned long long)rx.opened());
    for (const auto& ch : rx.channels()) std::printf("  channel %.4f MHz %+.1f dB, %d frames\n", ch.hz / 1e6, ch.levelDb, ch.frames);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--write-clip")) return writeClip(argc, argv);
    if (argc > 1 && !std::strcmp(argv[1], "--cu8")) return decodeCu8(argc, argv);
    const bool quick = argc > 1 && !std::strcmp(argv[1], "--quick");
    std::mt19937 rng(5);

    std::printf("1. the documentation's facts against each other\n");
    {
        // the header as a bit string (send order), and the de-whitened header bytes, are both published; they must agree
        const char* bits = "0000100001101101010100111000100001000100011010010100100000011111";
        uint64_t p = 0; for (int i = 0; i < 64; ++i) p = (p << 1) | uint64_t(bits[i] == '1');
        CHECK(p == headerBits(), "header bits %016llx vs %016llx", (unsigned long long)p, (unsigned long long)headerBits());
        const uint8_t clear[8] = { 0x86, 0x35, 0xF4, 0x40, 0x93, 0xDF, 0x1A, 0x60 };
        bool m = true; for (int i = 0; i < 8; ++i) m &= uint8_t(headerOnAir()[i] ^ mask()[i]) == clear[i];
        CHECK(m, "the on-air header XOR the mask is the documented de-whitened header");
        CHECK(crc16(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0x29B1, "CRC-16 check value %04x", crc16(reinterpret_cast<const uint8_t*>("123456789"), 9));
        CHECK(lengthForType(0x0F) == 320 && lengthForType(0xF0) == 518 && lengthForType(0x0E) == 320 && lengthForType(0x3C) == 0, "frame type byte");
    }

    std::printf("2. Reed-Solomon (255,231), shortened to 132 and full 231 data bytes\n");
    {
        const Rs& rs = Rs::get();
        for (int half : { 132, 231 }) {
            int ok[14] = {0}, wrong = 0, refused = 0;
            for (int trial = 0; trial < 300; ++trial) {
                uint8_t cw[255] = {0};
                for (int j = 0; j < half; ++j) cw[Rs::R + j] = uint8_t(rng());
                rs.encode(cw);
                uint8_t S[Rs::R]; rs.syndromes(cw, S);
                bool zero = true; for (auto s : S) zero &= s == 0;
                if (!zero) { ++wrong; continue; }
                const int ne = trial % 14;   // 0…13 errors
                uint8_t w[255]; std::memcpy(w, cw, 255);
                std::vector<int> used;
                while (int(used.size()) < ne) { const int at = int(rng() % unsigned(Rs::R + half)); if (std::find(used.begin(), used.end(), at) == used.end()) { used.push_back(at); w[at] ^= uint8_t(1 + rng() % 255); } }
                const int r = rs.decode(w, nullptr, 0, Rs::R + half);
                if (r >= 0 && !std::memcmp(w, cw, 255)) ++ok[ne];
                else if (r >= 0) ++wrong;
                else ++refused;
            }
            int sumOk = 0; for (int e = 0; e <= 12; ++e) sumOk += ok[e];
            int expect = 0; for (int t = 0; t < 300; ++t) if (t % 14 <= 12) ++expect;
            CHECK(sumOk == expect, "%d data bytes: 0-12 errors corrected %d of %d", half, sumOk, expect);
            CHECK(wrong == 0, "%d data bytes: %d wrong words accepted (13 errors are refused or…)", half, wrong);
            std::printf("   %d data bytes: 0–12 errors %d/%d corrected, 13 errors refused %d, wrong 0\n", half, sumOk, expect, refused);
            // erasures: 24 known-position errors
            uint8_t cw[255] = {0};
            for (int j = 0; j < half; ++j) cw[Rs::R + j] = uint8_t(rng());
            rs.encode(cw);
            uint8_t w[255]; std::memcpy(w, cw, 255);
            int er[24]; for (int k = 0; k < 24; ++k) { er[k] = k * 5 + 1; w[er[k]] ^= 0x5A; }
            CHECK(rs.decode(w, er, 24, Rs::R + half) >= 0 && !std::memcmp(w, cw, 255), "24 erasures corrected");
            std::memcpy(w, cw, 255);
            int er2[16]; for (int k = 0; k < 16; ++k) { er2[k] = k * 7 + 3; w[er2[k]] ^= 0x33; }
            w[150] ^= 1; w[100] ^= 2; w[2] ^= 4; w[61] ^= 9;   // 16 erasures + 4 errors = 24 checks
            CHECK(rs.decode(w, er2, 16, Rs::R + half) >= 0 && !std::memcmp(w, cw, 255), "16 erasures + 4 errors corrected");
        }
    }

    std::printf("3. the frame both ways (no radio): blocks, CRCs, parity, whitening, GPS, both lengths, encrypted\n");
    {
        vibers41tx::Sonde s(3);
        s.serial = "S1234567"; s.lat = 52.123456; s.lon = -1.654321; s.alt = 12345.6; s.vE = 7.5; s.vN = -2.25; s.vU = 5.1;
        for (bool ext : { false, true }) {
            s.extended = ext;
            auto air = vibers41tx::frameOnAir(s, 7);
            CHECK(int(air.size()) == (ext ? 518 : 320), "length %zu", air.size());
            CHECK(!std::memcmp(air.data(), headerOnAir(), 8), "the header on air");
            std::vector<uint8_t> f = air;
            whiten(f.data(), int(f.size()));
            CHECK(lengthForType(f[kTypePos]) == int(f.size()), "the type byte says the length");
            // 10 byte errors in each codeword
            for (int k = 0; k < 10; ++k) { f[size_t(kMsgPos + 2 * (k * 9 + 1))] ^= 0xA5; f[size_t(kMsgPos + 2 * (k * 11 + 2) + 1)] ^= 0x3C; }
            const int r = correct(f.data(), int(f.size()));
            CHECK(r == 20, "%s: Reed-Solomon corrected %d (20 expected)", ext ? "extended" : "standard", r);
            Frame fr; fr.len = int(f.size());
            parse(f.data(), fr);
            double la, lo, h; s.at(7, la, lo, h);
            CHECK(fr.statusOk && fr.serial == "S1234567" && fr.frameNo == s.frame0 + 7, "status: %s #%d", fr.serial.c_str(), fr.frameNo);
            CHECK(fr.posOk && std::fabs(fr.lat - la) < 1e-6 && std::fabs(fr.lon - lo) < 1e-6 && std::fabs(fr.alt - h) < 0.05,
                  "position %.6f %.6f %.1f vs %.6f %.6f %.1f", fr.lat, fr.lon, fr.alt, la, lo, h);
            CHECK(std::fabs(fr.vE - 7.5) < 0.02 && std::fabs(fr.vN + 2.25) < 0.02 && std::fabs(fr.vU - 5.1) < 0.02, "velocity %.2f %.2f %.2f", fr.vE, fr.vN, fr.vU);
            CHECK(fr.sats == 9 && fr.timeOk && fr.ptuOk && fr.blocksBad == 0, "sats %d time %d ptu %d bad %d", fr.sats, fr.timeOk, fr.ptuOk, fr.blocksBad);
            CHECK(std::fabs(gpsToUnix(fr.gpsWeek, fr.towMs) - (s.unix0 + 7)) < 0.01, "GPS time");
        }
        s.extended = false; s.encrypted = true;
        auto f = vibers41tx::frameClear(s, 3);
        Frame fr; fr.len = 320; parse(f.data(), fr);
        CHECK(fr.encrypted && !fr.posOk && fr.statusOk, "an encrypted sonde: serial yes, position no");
        // a block whose CRC fails is not read
        s.encrypted = false;
        f = vibers41tx::frameClear(s, 3);
        f[kBlocksPos + 2 + 3] ^= 1;   // inside the status block's serial
        Frame fb; fb.len = 320; parse(f.data(), fb);
        CHECK(!fb.statusOk && fb.blocksBad == 1 && fb.posOk, "a bad status CRC: no serial, the GPS block still read");
        // a frame too damaged is refused, not mis-corrected
        f = vibers41tx::frameClear(s, 4);
        for (int k = 0; k < 40; ++k) f[size_t(kMsgPos + 2 * k)] ^= uint8_t(1 + k);
        CHECK(correct(f.data(), 320) < 0, "40 byte errors in one codeword: refused");
    }

    std::printf("4. calibration: 51 subframes, and temperature only from a complete set\n");
    {
        vibers41tx::Sonde s(4);
        s.tempC = -47.25; s.setTx(405.1e6);
        Calibration cal;
        double tShown = NAN;
        int firstT = -1;
        for (int k = 0; k < 60; ++k) {
            auto f = vibers41tx::frameClear(s, k);
            Frame fr; fr.len = 320; parse(f.data(), fr);
            cal.add(fr.calIndex, fr.cal);
            const double t = temperature(cal, fr.meas);
            if (!std::isfinite(t)) CHECK(!cal.complete(), "no temperature before the set is complete");
            else if (firstT < 0) { firstT = k; tShown = t; }
        }
        CHECK(firstT == 50, "first temperature after %d frames (51 subframes)", firstT + 1);
        CHECK(std::fabs(tShown + 47.25) < 0.02, "temperature %.3f °C (sent −47.25)", tShown);
        CHECK(std::fabs(cal.txHz() - 405.1e6) < 200, "the sonde's own frequency %.4f MHz", cal.txHz() / 1e6);
        Calibration part; for (int i = 0; i < 50; ++i) part.add(i, s.cal + 16 * i);
        uint32_t meas[12] = { 250000, 160000, 230000 };
        CHECK(!std::isfinite(temperature(part, meas)), "50 of 51: not yet calibrated");
    }

    std::printf("5. the FM discriminator (SIMD vs double precision)\n");
    {
        std::vector<cf32> z(1001);
        double ph = 0;
        for (size_t i = 0; i < z.size(); ++i) { ph += (double(rng() % 20001) / 10000.0 - 1.0) * 3.0; z[i] = std::polar(float(0.1 + (rng() % 100) / 100.0), float(ph)); }
        std::vector<float> d(1000); std::vector<cf32> tmp; cf32 prev = z[0];
        fmDisc(z.data() + 1, 1000, prev, d.data(), tmp);
        double worst = 0;
        for (size_t i = 0; i < 1000; ++i) {
            const std::complex<double> a(z[i + 1].real(), z[i + 1].imag()), b(z[i].real(), z[i].imag());
            double e = std::fabs(std::arg(a * std::conj(b)) - d[i]);
            if (e > M_PI) e = std::fabs(e - 2 * M_PI);
            worst = std::max(worst, e);
        }
        CHECK(worst < 5e-5, "worst error %.2e rad", worst);
        std::printf("   worst error %.2e rad over 1000 random steps (%s)\n", worst,
#if VIBE_NEON
                    "NEON"
#elif VIBE_SSE
                    "SSE2"
#else
                    "scalar"
#endif
        );
    }

    const double fs = 2.4e6, hw = 403.005e6;
    std::printf("6. through the receiver: SNR sweep (one sonde, 2.4 MS/s, 12 s; SNR in 10 kHz)\n");
    {
        for (double snr : { 4.0, 6.0, 7.0, 8.0, 9.0, 10.0, 12.0, 15.0, 20.0, 30.0 }) {
            std::vector<SimSonde> ss; ss.emplace_back(21);
            ss[0].offHz = 295e3; ss[0].snrDb = snr; ss[0].s.serial = "S7000001";
            SimResult r = simulate(fs, hw, ss, 12, uint32_t(snr * 10));
            const int g = r.good["S7000001"];
            std::vector<float> sn; for (const auto& f : r.frames) sn.push_back(float(f.snrDb));
            std::sort(sn.begin(), sn.end());
            const float med = sn.empty() ? -99.f : sn[sn.size() / 2];
            std::printf("   %4.1f dB: %2d of 12 frames, wrong %d, reported SNR %.1f dB\n", snr, g, r.wrong, med);
            // ★ beta96 the frame's reported SNR (S/N in the scan's 10 kHz box, docs/v12/MESSAGE-SNR.md) = the bench's own SNR
            if (snr >= 7 && snr <= 30 && sn.size() >= 5) CHECK(std::fabs(med - float(snr)) <= 2.5f, "%.0f dB: reported SNR %.1f dB (S/N in the 10 kHz box)", snr, med);
            CHECK(r.wrong == 0, "%.0f dB: %d wrong", snr, r.wrong);
            if (snr >= 9) CHECK(g >= 10, "%.0f dB: %d of 12 (≥ 10: the first second goes to the scan)", snr, g);
        }
    }
    std::printf("7. frequency offset, deviation, clock error, continuous carrier, extended frames (15 dB)\n");
    {
        struct V { double off, dev, ppm; bool cont, ext; const char* what; };
        const V vs[] = {
            { -812345.0, 2400, 0, false, false, "−812 kHz in the band" }, { 640003.7, 2400, 0, false, false, "+640 kHz, off the raster" },
            { 3000.0, 2400, 0, false, false, "3 kHz from the radio's centre" },
            { 100e3, 1600, 0, false, false, "±1.6 kHz deviation" }, { 100e3, 3200, 0, false, false, "±3.2 kHz deviation" },
            { -200e3, 2400, 150, false, false, "+150 ppm bit clock" }, { -200e3, 2400, -150, false, false, "−150 ppm bit clock" },
            { 450e3, 2400, 0, true, false, "carrier on between frames" }, { 450e3, 2400, 0, false, true, "extended 518-byte frames" },
        };
        for (const auto& v : vs) {
            std::vector<SimSonde> ss; ss.emplace_back(31);
            ss[0].offHz = v.off; ss[0].snrDb = 15; ss[0].devHz = v.dev; ss[0].ppm = v.ppm; ss[0].continuous = v.cont; ss[0].s.extended = v.ext;
            ss[0].s.serial = "N4455667";
            SimResult r = simulate(fs, hw, ss, 10, 77);
            const int g = r.good["N4455667"];
            std::printf("   %-34s %2d of 10, wrong %d\n", v.what, g, r.wrong);
            CHECK(g >= 8 && r.wrong == 0, "%s: %d of 10, wrong %d", v.what, g, r.wrong);
        }
    }
    std::printf("8. four sondes at once in one band (12, 15, 20, 25 dB; 12 s)\n");
    double rtf = 0;
    {
        std::vector<SimSonde> ss;
        const char* ser[] = { "S1000001", "S2000002", "T3000003", "U4000004" };
        const double off[] = { -700e3, -120e3, 310e3, 880e3 }, snr[] = { 12, 15, 20, 25 };
        for (int i = 0; i < 4; ++i) { ss.emplace_back(uint32_t(40 + i)); ss.back().s.serial = ser[i]; ss.back().offHz = off[i]; ss.back().snrDb = snr[i]; ss.back().s.frame0 = 100 * i; }
        SimResult r = simulate(fs, hw, ss, 12, 4242);
        for (int i = 0; i < 4; ++i) {
            std::printf("   %s %+6.0f kHz %2.0f dB: %2d of 12\n", ser[i], off[i] / 1e3, snr[i], r.good[ser[i]]);
            CHECK(r.good[ser[i]] >= 10, "%s: %d of 12", ser[i], r.good[ser[i]]);
        }
        CHECK(r.wrong == 0, "wrong %d", r.wrong);
        rtf = r.cpuS / r.audioS;
        std::printf("   CPU: %.3f s per capture second (one thread, this machine)\n", rtf);
        // ★ capacity: seven sondes with six channels → six decode, the seventh waits for a slot (none idles here)
        std::vector<SimSonde> s7;
        for (int i = 0; i < 7; ++i) { s7.emplace_back(uint32_t(60 + i)); char b[9]; std::snprintf(b, sizeof b, "S%07d", 5000000 + i); s7.back().s.serial = b; s7.back().offHz = -900e3 + i * 290e3; s7.back().snrDb = 20; }
        SimResult r7 = simulate(fs, hw, s7, 8, 77, 6);
        int heard = 0; for (auto& kv : r7.good) heard += kv.second >= 5;
        CHECK(heard == 6 && r7.wrong == 0, "seven sondes, six channels: %d decoded, wrong %d", heard, r7.wrong);
    }
    std::printf("9. 2.048 MS/s (the Pi 2's rate) and a 1 MS/s capture\n");
    for (double rate : { 2.048e6, 1.0e6 }) {
        std::vector<SimSonde> ss; ss.emplace_back(51); ss[0].s.serial = "S9988776"; ss[0].offHz = rate * 0.3; ss[0].snrDb = 15;
        ss.emplace_back(52); ss[1].s.serial = "S9988777"; ss[1].offHz = -rate * 0.2; ss[1].snrDb = 15;
        SimResult r = simulate(rate, hw, ss, 10, 99);
        std::printf("   %.3f MS/s: %d + %d of 10, wrong %d\n", rate / 1e6, r.good["S9988776"], r.good["S9988777"], r.wrong);
        CHECK(r.good["S9988776"] >= 8 && r.good["S9988777"] >= 8 && r.wrong == 0, "%.3f MS/s", rate / 1e6);
    }
    std::printf("10. no sonde: noise, carriers and a 4800 bit/s look-alike → zero sondes\n");
    {
        std::vector<SimSonde> none;
        const int secs = quick ? 20 : 60;
        SimResult r = simulate(fs, hw, none, secs, 1234, 6, { -400e3, 120e3, 905e3 }, -250e3);
        CHECK(r.frames.empty(), "%d s: %zu frames", secs, r.frames.size());
        std::printf("   %d s of noise + three carriers + a random-bit GFSK burster: %zu frames\n", secs, r.frames.size());
        // and the noise-only scan opens no channel at all
        vibers41::Receiver rx(fs, hw, hw - 1e6, hw + 1e6);
        std::normal_distribution<float> g(0.f, 0.02f);
        std::vector<cf32> b(65536);
        for (int k = 0; k < int(fs * 10 / 65536); ++k) { for (auto& v : b) v = cf32(g(rng), g(rng)); rx.feed(b.data(), b.size(), [](const Frame&) {}); }
        CHECK(rx.opened() == 0, "10 s of plain noise: %llu channels opened", (unsigned long long)rx.opened());
    }
    std::printf("11. the sample-rate budget\n");
    {
        CHECK(rtf < 0.25, "four sondes at 2.4 MS/s: %.3f of real time", rtf);
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
