// test-sondes-types.cpp — V12 radiosondes beyond the RS41: Graw DFM-06/09/17 and Meteomodem M10/M10+/M20
// (vibe_sonde_dfm.h, vibe_sonde_mxx.h, vibe_sonde_manch.h, the shared receiver vibe_rs41_rx.h), docs/v12/SONDES-TYPES.md.
//  §1 DFM pieces: Hamming(8,4) both ways, every single error corrected, every double error refused, the interleaver,
//     the header's raw chips ↔ its 16 bits, the thermistor fit against its datasheet table.
//  §2 M10 / M20 pieces: the frame check is linear (as documented) and catches every single-bit error; frames round trip.
//  §3 Each type through the WHOLE receiver from our own transmitters: SNR, offsets, deviation, chip clock.
//  §4 AUTO type: every type alone (no other type ever reported) and all four at once in one band.
//  §5 Noise, carriers and random-chip look-alikes at both chip rates: zero frames.
//  §6 CPU.
//  `test-sondes-types --cu8 file.u8 rate hwCentreHz` decodes a REAL capture (once there is one), every type.
#include "vibe_rs41_rx.h"
#include "vibe_rs41_service.h"
#include "vibe_rs41_tx.h"
#include "vibe_sonde_tx.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
using vibers41::cf32;
using vibers41::Frame;

// ── a simulated sonde of any type ──────────────────────────────────────────────────────────────────────────────────
struct Sim {
    std::string kind;                 // "RS41", "DFM", "M10", "M20"
    double offHz = 0, snrDb = 20, devHz = 0, ppm = 0;
    int dfmMode = 2, dfmId = 0xA;
    uint32_t seed = 1;
    // filled by build()
    std::string serial;
    vibesondetx::Flight fl;
    std::unique_ptr<vibesondetx::ChipFsk> fsk;
    std::unique_ptr<vibers41tx::Gfsk> gfsk;
    std::unique_ptr<vibers41tx::Sonde> rs;
    void build(double fs, double amp, int secs, double t0) {
        fl.lat = 50 + 0.3 * seed; fl.lon = -3 + 0.7 * seed; fl.alt = 1000 + 500 * seed; fl.vU = seed & 1 ? 5.5 : -12;
        if (kind == "RS41") {
            rs.reset(new vibers41tx::Sonde(seed));
            rs->serial = std::string("S") + std::to_string(1000000 + seed * 1111);
            serial = rs->serial;
            gfsk.reset(new vibers41tx::Gfsk(fs, offHz, amp, devHz ? devHz : 2400, t0, ppm));
            for (int k = 0; k < secs; ++k) gfsk->addFrame(k, vibers41tx::frameOnAir(*rs, k));
        } else if (kind == "DFM") {
            vibesondetx::Dfm d; d.fl = fl; d.mode = dfmMode; d.snId = dfmId;
            d.serial = 19000000 + seed * 4321;
            serial = dfmId == 6 ? "412345" : std::to_string(d.serial);
            fsk.reset(new vibesondetx::ChipFsk(fs, offHz, amp, devHz ? devHz : 2000, vibedfm::kChipRate, t0, ppm));
            fsk->put(0, d.chips(secs));
        } else {
            vibesondetx::Mxx m; m.fl = fl; m.m20 = kind == "M20";
            m.sn[4] = uint8_t(0x50 + seed); m.sn20 += seed << 10;
            serial = m.serialText();
            fsk.reset(new vibesondetx::ChipFsk(fs, offHz, amp, devHz ? devHz : 4800, m.m20 ? 9600.0 : 9615.0, t0, ppm));
            for (int k = 0; k < secs; ++k) fsk->put(fsk->indexAt(k), m.chips(k));
        }
    }
    void render(cf32* b, size_t n, size_t base) { if (fsk) fsk->render(b, n, base); if (gfsk) gfsk->render(b, n, base); }
    /** Is this frame right for this sonde? */
    bool judge(const Frame& f, double hw) const {
        if (kind != f.kind || serial != f.serial) return false;
        if (std::fabs(f.hz - (hw + offHz)) > 3000) return false;
        if (!f.posOk) return true;
        if (kind == "RS41") {
            const int k = f.frameNo - rs->frame0;
            double la, lo, h; rs->at(k, la, lo, h);
            return std::fabs(f.lat - la) < 1e-5 && std::fabs(f.lon - lo) < 1e-5 && std::fabs(f.alt - h) < 0.5;
        }
        // DFM epochs and M10/M20 frames are one a second; the frame number is the second (mod 256)
        if (f.frameNo < 0) {   // DFM mode 3: no frame number — any second of the flight within a metre
            for (int k = 0; k < 256; ++k) { double la, lo, h; fl.at(k, la, lo, h); if (std::fabs(f.lat - la) < 2e-6 && std::fabs(f.lon - lo) < 2e-6 && std::fabs(f.alt - h) < 0.05) return true; }
            return false;
        }
        double la, lo, h; fl.at(f.frameNo, la, lo, h);
        const double tol = kind == "DFM" ? 2e-7 : kind == "M20" ? 2e-6 : 2e-7;
        return std::fabs(f.lat - la) < tol && std::fabs(f.lon - lo) < tol && std::fabs(f.alt - h) < 0.05
               && std::fabs(f.vU - fl.vU) < 0.02;
    }
};
struct Result { std::map<std::string, int> good, pos; int wrong = 0; double cpuS = 0; std::vector<Frame> frames; };

/** `secs` at `fs` centred on `hw`; noise σ 0.02 per component; SNR = signal over the noise in 10 kHz. */
static Result simulate(double fs, double hw, std::vector<Sim>& ss, int secs, uint32_t seed, const std::vector<double>& spurs = {},
                       const std::vector<std::pair<double, double>>& lookalikes = {}) {
    const double sigma = 0.02, n10k = 2 * sigma * sigma * 10e3 / fs;
    for (size_t i = 0; i < ss.size(); ++i) ss[i].build(fs, std::sqrt(n10k * std::pow(10.0, ss[i].snrDb / 10)), secs, 0.05 + 0.137 * i);
    // ★ look-alikes: random chips at a sonde's chip rate (offset, chip rate), on all the time
    std::vector<std::unique_ptr<vibesondetx::ChipFsk>> look;
    std::mt19937 lr(seed + 77);
    for (const auto& l : lookalikes) {
        look.emplace_back(new vibesondetx::ChipFsk(fs, l.first, std::sqrt(n10k * 100), l.second > 5000 ? 4800 : 2000, l.second, 0.03));
        std::vector<int8_t> c(size_t(l.second * secs));
        for (auto& v : c) v = (lr() & 1) ? 1 : -1;
        look.back()->put(0, c);
    }
    vibers41::Receiver rx(fs, hw, hw - 1.0e6, hw + 1.0e6, 6);
    Result res;
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, float(sigma));
    const size_t total = size_t(fs * secs), chunk = 65536;
    std::vector<cf32> buf(chunk);
    std::vector<double> spPh(spurs.size(), 0.0);
    for (size_t base = 0; base < total; base += chunk) {
        const size_t n = std::min(chunk, total - base);
        for (size_t i = 0; i < n; ++i) buf[i] = cf32(g(rng), g(rng));
        for (auto& s : ss) s.render(buf.data(), n, base);
        for (auto& l : look) l->render(buf.data(), n, base);
        for (size_t s = 0; s < spurs.size(); ++s)
            for (size_t i = 0; i < n; ++i) { buf[i] += cf32(float(0.05 * std::cos(spPh[s])), float(0.05 * std::sin(spPh[s]))); spPh[s] += 2 * M_PI * spurs[s] / fs; }
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(buf.data(), n, [&](const Frame& f) {
            res.frames.push_back(f);
            bool ok = false;
            for (const auto& s : ss) if (s.judge(f, hw)) { ok = true; ++res.good[s.kind + ":" + s.serial]; if (f.posOk) ++res.pos[s.kind + ":" + s.serial]; break; }
            if (!ok) {
                ++res.wrong;
                std::printf("      wrong frame: kind %s serial %s pos %d lat %.6f lon %.6f alt %.1f frame %d hz %.0f\n", f.kind, f.serial.c_str(), f.posOk, f.lat, f.lon, f.alt, f.frameNo, f.hz);
            }
        });
        res.cpuS += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    return res;
}
static int goodOf(const Result& r, const Sim& s) { auto it = r.pos.find(s.kind + ":" + s.serial); return it == r.pos.end() ? 0 : it->second; }

static int decodeCu8(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: test-sondes-types --cu8 file.u8 rate hwCentreHz\n"); return 2; }
    FILE* f = std::fopen(argv[2], "rb");
    if (!f) { std::perror(argv[2]); return 2; }
    const double fs = std::atof(argv[3]), hw = std::atof(argv[4]);
    vibers41::Receiver rx(fs, hw, hw - fs / 2, hw + fs / 2, 6);
    std::vector<uint8_t> u(1 << 18);
    std::vector<cf32> b(u.size() / 2);
    size_t n, tot = 0;
    while ((n = std::fread(u.data(), 1, u.size(), f)) > 1) {
        for (size_t i = 0; i < n / 2; ++i) b[i] = cf32((u[2 * i] - 127.5f) / 127.5f, (u[2 * i + 1] - 127.5f) / 127.5f);
        rx.feed(b.data(), n / 2, [&](const Frame& fr) {
            std::printf("%8.1f s  %-4s %-12s %s %.3f MHz snr %.1f  %s %.5f %.5f %.0f m climb %.1f  temp %.1f (cal %d/%d)\n", tot / fs, fr.kind,
                        fr.serial.c_str(), fr.model.c_str(), fr.hz / 1e6, fr.snrDb, fr.posOk ? "pos" : "---", fr.lat, fr.lon, fr.alt, fr.vU, fr.tempC, fr.calN, fr.calOf);
        });
        tot += n / 2;
    }
    std::fclose(f);
    for (const auto& c : rx.channels()) std::printf("channel %.3f MHz %.1f dB %d frames %s\n", c.hz / 1e6, c.levelDb, c.frames, c.kind ? c.kind : "(no type)");
    return 0;
}

/** The server end-to-end clip (403.005 MHz, 20 s): a DFM-09 at 402.500, an M10 at 403.600, an M20 at 403.900 MHz, 20 dB.
 *  test-server-sondes.mjs (VIBE_SONDES_TYPES=1) checks the kinds, the model and the calibration rules from the server. */
static int writeClip(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: test-sondes-types --write-clip out.cu8 rate\n"); return 2; }
    const double fs = std::atof(argv[3]), hw = 403.005e6;
    const int secs = 20;
    const double sigma = 0.02, n10k = 2 * sigma * sigma * 10e3 / fs, amp = std::sqrt(n10k * 100);
    std::vector<std::unique_ptr<vibesondetx::ChipFsk>> tx;
    vibesondetx::Dfm d; d.serial = 19123456; d.fl.lat = 52.2; d.fl.lon = 0.13; d.fl.alt = 4200; d.fl.vU = 5.1; d.tempC = -18.0;
    tx.emplace_back(new vibesondetx::ChipFsk(fs, 402.500e6 - hw, amp, 2000, vibedfm::kChipRate, 0.05));
    tx.back()->put(0, d.chips(secs));
    for (int m20 = 0; m20 < 2; ++m20) {
        vibesondetx::Mxx m; m.m20 = m20; m.fl.lat = m20 ? 48.85 : 50.6; m.fl.lon = m20 ? 2.35 : 3.1; m.fl.alt = m20 ? 9000 : 15000; m.fl.vU = m20 ? -15 : 4.5;
        tx.emplace_back(new vibesondetx::ChipFsk(fs, (m20 ? 403.900e6 : 403.600e6) - hw, amp, 4800, m20 ? 9600.0 : 9615.0, 0.11 + 0.2 * m20));
        for (int k = 0; k < secs; ++k) tx.back()->put(tx.back()->indexAt(k), m.chips(k));
    }
    FILE* o = std::fopen(argv[2], "wb");
    if (!o) { std::perror(argv[2]); return 2; }
    std::mt19937 rng(43);
    std::normal_distribution<float> g(0.f, float(sigma));
    const size_t total = size_t(fs * secs), chunk = 65536;
    std::vector<cf32> buf(chunk);
    std::vector<uint8_t> u(chunk * 2);
    for (size_t base = 0; base < total; base += chunk) {
        const size_t n = std::min(chunk, total - base);
        for (size_t i = 0; i < n; ++i) buf[i] = cf32(g(rng), g(rng));
        for (auto& t : tx) t->render(buf.data(), n, base);
        auto q = [](float x) { const int v = int(std::lround(127.5f + 127.5f * x)); return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); };
        for (size_t i = 0; i < n; ++i) { u[2 * i] = q(buf[i].real()); u[2 * i + 1] = q(buf[i].imag()); }
        std::fwrite(u.data(), 1, n * 2, o);
    }
    std::fclose(o);
    std::printf("wrote %s: %zu samples at %.0f S/s, centre %.3f MHz: DFM-09 19123456 @402.500, M10 %s @403.600, M20 %s @403.900\n", argv[2], total, fs, hw / 1e6,
                vibesondetx::Mxx().serialText().c_str(), [] { vibesondetx::Mxx m; m.m20 = true; return m.serialText(); }().c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--cu8")) return decodeCu8(argc, argv);
    if (argc > 1 && !std::strcmp(argv[1], "--write-clip")) return writeClip(argc, argv);
    const bool quick = argc > 1 && !std::strcmp(argv[1], "--quick");
    const double fs = 2.4e6, hw = 403.005e6;

    std::printf("§1 DFM pieces\n");
    {
        int bad = 0, fixed = 0, refused = 0, pairs = 0;
        for (int v = 0; v < 16; ++v) {
            uint8_t c[8], nib;
            vibedfm::hamEncode(uint8_t(v), c);
            if (vibedfm::hamDecode(c, nib) != 0 || nib != v) ++bad;
            for (int e = 0; e < 8; ++e) { uint8_t d[8]; std::memcpy(d, c, 8); d[e] ^= 1; if (vibedfm::hamDecode(d, nib) == 1 && nib == v) ++fixed; }
            for (int e = 0; e < 8; ++e) for (int e2 = e + 1; e2 < 8; ++e2) { uint8_t d[8]; std::memcpy(d, c, 8); d[e] ^= 1; d[e2] ^= 1; ++pairs; if (vibedfm::hamDecode(d, nib) < 0) ++refused; }
        }
        CHECK(bad == 0, "clean codewords %d bad", bad);
        CHECK(fixed == 128, "single errors corrected %d of 128", fixed);
        CHECK(refused == pairs, "double errors refused %d of %d", refused, pairs);
        // the interleaver: a block's sent bit L·j+i is codeword i's bit j
        uint8_t nibs[13], back[13], sent[104];
        for (int i = 0; i < 13; ++i) nibs[i] = uint8_t((i * 7 + 3) & 15);
        vibedfm::encodeBlock(nibs, 13, sent);
        uint8_t c3[8]; vibedfm::hamEncode(nibs[3], c3);
        bool il = true; for (int j = 0; j < 8; ++j) if (sent[13 * j + 3] != c3[j]) il = false;
        CHECK(il, "interleaver layout");
        CHECK(vibedfm::decodeBlock(sent, 13, back) == 0 && !std::memcmp(nibs, back, 13), "block round trip");
        // the header: its raw chips under "01" = 1 are its published 16 bits
        uint16_t h = 0;
        for (int i = 0; i < 16; ++i) { const char a = vibedfm::kRawHeader[2 * i], b = vibedfm::kRawHeader[2 * i + 1]; CHECK(a != b, "pair %d is Manchester", i); h = uint16_t(h << 1 | (a == '0' && b == '1')); }
        CHECK(h == vibedfm::kHeader, "raw header → %04X", h);
        // the thermistor: our fit against the datasheet table (25 °C = 5 kΩ, −40 °C = 20.96 · R25, +40 °C = 0.57742 · R25)
        const auto& th = vibedfm::Thermistor::get();
        CHECK(std::fabs(th.celsius(5000) - 25) < 0.3, "25 °C → %.2f", th.celsius(5000));
        CHECK(std::fabs(th.celsius(20.96 * 5000) + 40) < 0.3, "−40 °C → %.2f", th.celsius(20.96 * 5000));
        CHECK(std::fabs(th.celsius(0.57742 * 5000) - 40) < 0.3, "+40 °C → %.2f", th.celsius(0.57742 * 5000));
        CHECK(std::fabs(th.celsius(th.ohms(-61.3)) + 61.3) < 0.01, "ohms ↔ celsius");
        CHECK(std::fabs(vibedfm::fl24(vibesondetx::toFl24(123456.789)) - 123456.789) < 0.5, "fl24");
    }

    std::printf("§2 M10 / M20 pieces\n");
    {
        std::mt19937 r(5);
        bool lin = true;
        for (int t = 0; t < 200; ++t) {
            uint8_t a[60], b[60], x[60];
            for (int i = 0; i < 60; ++i) { a[i] = uint8_t(r()); b[i] = uint8_t(r()); x[i] = a[i] ^ b[i]; }
            if (vibemxx::check(x, 60) != (vibemxx::check(a, 60) ^ vibemxx::check(b, 60))) lin = false;
        }
        CHECK(lin, "the check is linear");
        for (int m20 = 0; m20 < 2; ++m20) {
            vibesondetx::Mxx m; m.m20 = m20;
            const auto f = m.frame(17);
            Frame fr;
            CHECK(vibemxx::parse(f.data(), int(f.size()), fr), "%s parses", m20 ? "M20" : "M10");
            double la, lo, h; m.fl.at(17, la, lo, h);
            CHECK(fr.posOk && std::fabs(fr.lat - la) < 2e-6 && std::fabs(fr.lon - lo) < 2e-6 && std::fabs(fr.alt - h) < 0.02, "%s position %.6f %.6f %.2f", m20 ? "M20" : "M10", fr.lat, fr.lon, fr.alt);
            CHECK(fr.serial == m.serialText() && !fr.serial.empty(), "serial %s", fr.serial.c_str());
            CHECK(std::fabs(fr.unixTime - (m.fl.unix0 + 17)) < 1.01, "time %.0f vs %.0f", fr.unixTime, m.fl.unix0 + 17);
            int caught = 0, flips = 0;
            for (size_t i = 0; i < f.size() * 8; ++i) { auto g = f; g[i >> 3] ^= uint8_t(0x80 >> (i & 7)); ++flips; Frame x; if (!vibemxx::parse(g.data(), int(g.size()), x)) ++caught; }
            CHECK(caught == flips, "%s: %d of %d single-bit errors refused", m20 ? "M20" : "M10", caught, flips);
        }
        CHECK(vibesondetx::Mxx().serialText() == "903-2-24660", "M10 serial %s", vibesondetx::Mxx().serialText().c_str());
        vibesondetx::Mxx m20; m20.m20 = true;
        CHECK(m20.serialText() == "504-2-01234", "M20 serial %s", m20.serialText().c_str());
    }

    std::printf("§3 each type through the receiver (2.4 MS/s)\n");
    const int secs = quick ? 14 : 20;
    for (const char* kind : { "DFM", "M10", "M20" }) {
        std::printf("   %s SNR sweep:", kind);
        const std::vector<double> snrs = quick ? std::vector<double>{ 8, 12, 20 } : std::vector<double>{ 4, 6, 8, 10, 12, 15, 20, 30 };
        int wrong = 0;
        std::map<double, int> got;
        for (double snr : snrs) {
            std::vector<Sim> ss(1); ss[0].kind = kind; ss[0].offHz = 297e3; ss[0].snrDb = snr; ss[0].seed = 3;
            const Result r = simulate(fs, hw, ss, secs, 100 + uint32_t(snr));
            got[snr] = goodOf(r, ss[0]); wrong += r.wrong;
            std::printf(" %.0f dB %d", snr, got[snr]);
        }
        std::printf(" (positions of %d s)\n", secs);
        CHECK(wrong == 0, "%s: %d wrong frames in the sweep", kind, wrong);
        CHECK(got[20] >= secs - 6, "%s: 20 dB gave %d positions", kind, got[20]);
        CHECK(got[12] >= secs - 8, "%s: 12 dB gave %d positions", kind, got[12]);
    }
    {
        // offsets, deviations, chip clock, DFM mode 3, DFM-17 and DFM-06 serial forms
        struct V { const char* kind; double off, dev, ppm; int mode, id; const char* what; };
        const V vs[] = {
            { "DFM", -812e3, 1500, 0, 2, 0xA, "−812 kHz, ±1.5 kHz" }, { "DFM", 640e3, 3500, 0, 2, 0xA, "+640 kHz, ±3.5 kHz" },
            { "DFM", 3e3, 2000, 300, 2, 0xA, "3 kHz from centre, +300 ppm" }, { "DFM", 200e3, 2000, -300, 3, 0xB, "mode 3, DFM-17, −300 ppm" },
            { "DFM", 150e3, 2000, 0, 2, 6, "DFM-06" },
            { "M10", -700e3, 3000, 0, 0, 0, "−700 kHz, ±3 kHz" }, { "M10", 500e3, 7000, 0, 0, 0, "+500 kHz, ±7 kHz" },
            { "M10", 100e3, 4800, 400, 0, 0, "+400 ppm" }, { "M20", -250e3, 4800, -400, 0, 0, "−400 ppm" },
            { "M20", 820e3, 6000, 0, 0, 0, "+820 kHz, ±6 kHz" },
        };
        for (const auto& v : vs) {
            std::vector<Sim> ss(1); ss[0].kind = v.kind; ss[0].offHz = v.off; ss[0].devHz = v.dev; ss[0].ppm = v.ppm; ss[0].snrDb = 18;
            ss[0].dfmMode = v.mode; ss[0].dfmId = v.id; ss[0].seed = 4;
            const Result r = simulate(fs, hw, ss, secs, 7);
            const int g = goodOf(r, ss[0]);
            std::printf("   %s %s: %d positions, %d wrong\n", v.kind, v.what, g, r.wrong);
            CHECK(g >= secs - 8 && r.wrong == 0, "%s %s: %d positions, %d wrong", v.kind, v.what, g, r.wrong);
        }
        // the DFM temperature: only once channels 0…5 are in, and then the transmitter's
        std::vector<Sim> ss(1); ss[0].kind = "DFM"; ss[0].offHz = -300e3; ss[0].snrDb = 20; ss[0].seed = 9;
        const Result r = simulate(fs, hw, ss, secs, 8);
        int withT = 0, early = 0; double tLast = NAN;
        for (const auto& f : r.frames) { if (std::isfinite(f.tempC)) { ++withT; tLast = f.tempC; if (f.calN < f.calOf) ++early; } }
        CHECK(withT > 0 && std::fabs(tLast + 21.5) < 0.3 && early == 0, "DFM-09 temperature %.2f (%d frames, %d before the calibration)", tLast, withT, early);
        ss[0].dfmId = 0xC; ss[0].kind = "DFM";
        const Result r2 = simulate(fs, hw, ss, secs, 8);
        int t2 = 0; for (const auto& f : r2.frames) if (std::isfinite(f.tempC)) ++t2;
        CHECK(t2 == 0 && !r2.frames.empty() && r2.frames.back().calOf == 0 && r2.frames.back().model == "DFM-09P", "DFM-09P: no temperature (%d)", t2);
    }

    std::printf("§4 AUTO type\n");
    {
        int cross = 0;
        for (const char* kind : { "RS41", "DFM", "M10", "M20" }) {
            std::vector<Sim> ss(1); ss[0].kind = kind; ss[0].offHz = -120e3; ss[0].snrDb = 25; ss[0].seed = 5;
            const Result r = simulate(fs, hw, ss, secs, 21);
            int other = 0; for (const auto& f : r.frames) if (std::strcmp(f.kind, kind)) ++other;
            cross += other;
            CHECK(other == 0 && r.wrong == 0 && goodOf(r, ss[0]) >= secs - 8, "%s alone: %d positions, %d of another type, %d wrong", kind, goodOf(r, ss[0]), other, r.wrong);
        }
        std::printf("   each type alone: %d frames reported as another type\n", cross);
        std::vector<Sim> ss(5);
        const char* k[5] = { "RS41", "DFM", "M10", "M20", "DFM" };
        const double off[5] = { -700e3, -350e3, 50e3, 420e3, 760e3 };
        for (int i = 0; i < 5; ++i) { ss[i].kind = k[i]; ss[i].offHz = off[i]; ss[i].snrDb = 18 + i; ss[i].seed = uint32_t(10 + i); }
        const Result r = simulate(fs, hw, ss, secs, 33);
        std::printf("   all at once:");
        for (const auto& s : ss) std::printf(" %s %s %d,", s.kind.c_str(), s.serial.c_str(), goodOf(r, s));
        std::printf(" wrong %d, %.3f s CPU for %d s\n", r.wrong, r.cpuS, secs);
        for (const auto& s : ss) CHECK(goodOf(r, s) >= secs - 8, "%s together: %d", s.kind.c_str(), goodOf(r, s));
        CHECK(r.wrong == 0, "together: %d wrong", r.wrong);
        // the service's table: kinds and models in the JSON, one row each
        vibers41::Tracker tr;
        for (const auto& f : r.frames) tr.add(f, 100.0);
        const std::string j = tr.json(100.5);
        CHECK(tr.size() == 5, "five rows (%zu)", tr.size());
        CHECK(j.find("\"kind\":\"DFM\"") != std::string::npos && j.find("\"kind\":\"M10\"") != std::string::npos && j.find("\"kind\":\"M20\"") != std::string::npos
              && j.find("\"kind\":\"RS41\"") != std::string::npos && j.find("\"model\":\"DFM-09\"") != std::string::npos, "the JSON's kinds");
        CHECK(j.find("\"calOf\":51") != std::string::npos && j.find("\"calOf\":6") != std::string::npos && j.find("\"calOf\":0") != std::string::npos, "calOf per type");
    }

    std::printf("§5 noise, carriers, look-alikes\n");
    {
        std::vector<Sim> none;
        const int ns = quick ? 20 : 60;
        const Result r = simulate(fs, hw, none, ns, 55, { -400e3, 123e3, 610e3 },
                                  { { -600e3, 2500 }, { -200e3, 9600 }, { 300e3, 9615 }, { 700e3, 2500 } });
        CHECK(r.frames.empty(), "%zu frames from %d s of noise + carriers + random-chip DFM / M10 / M20 look-alikes", r.frames.size(), ns);
        std::printf("   %d s: %zu frames\n", ns, r.frames.size());
    }

    std::printf("§6 CPU\n");
    {
        std::vector<Sim> ss(4);
        const char* k[4] = { "DFM", "M10", "M20", "RS41" };
        for (int i = 0; i < 4; ++i) { ss[i].kind = k[i]; ss[i].offHz = -600e3 + 400e3 * i; ss[i].snrDb = 20; ss[i].seed = uint32_t(20 + i); }
        const Result r = simulate(fs, hw, ss, secs, 66);
        std::printf("   four sondes (one of each type) at 2.4 MS/s: %.4f s CPU per capture second (one thread)\n", r.cpuS / secs);
        CHECK(r.cpuS / secs < 0.25, "real time: %.3f", r.cpuS / secs);
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
