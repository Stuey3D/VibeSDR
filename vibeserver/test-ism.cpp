// test-ism.cpp — V12 ISM decoder (vibe_ism*.h): every protocol on its published worked examples where the source
// gives one, then end to end — bursts ENCODED FROM THE SAME PUBLISHED SPECS, modulated into noisy IQ at the rates the
// radios run, through the real front end (detector → channel → slicer) and the decoders. Then the front end's
// robustness (drift ±150 kHz, SNR, several transmitters at once, pure noise = nothing), the PRIVACY rule on the JSON,
// and a CPU row. docs/v12/ISM-DECODER.md §Tests.
// ★ Silent: no audio anywhere. ★ The encoders below are written from the specs, independently of the decoders' code
//   paths (bits → runs), so a decoder that misreads a spec fails here.
#include "vibe_ism_service.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace vibeism;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)
static bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ── a run builder: +mark / −space µs, merging like signs ─────────────────────────────────────────────────────────────
struct Tx {
    Runs r;
    void add(double us) { const int d = int(std::lround(us)); if (!d) return; if (!r.empty() && ((r.back() > 0) == (d > 0))) r.back() += d; else r.push_back(d); }
    void mark(double us) { add(us); }
    void space(double us) { add(-us); }
};
static std::vector<uint8_t> hexBytes(const char* h) {
    std::vector<uint8_t> o; for (size_t i = 0; h[i] && h[i + 1]; i += 2) { while (h[i] == ' ') ++i; unsigned v; std::sscanf(h + i, "%2x", &v); o.push_back(uint8_t(v)); }
    return o;
}
static Bits bytesToBits(const std::vector<uint8_t>& b) { Bits o; for (uint8_t x : b) for (int i = 7; i >= 0; --i) o.push_back((x >> i) & 1); return o; }

// ── IQ synthesis ─────────────────────────────────────────────────────────────────────────────────────────────────────
struct Sig { Runs runs; bool fsk = false; double offHz = 0, amp = 0.3, devHz = 35e3, startS = 0.05; };
static std::vector<cf> synth(const std::vector<Sig>& sigs, double rate, double lenS, double noise, uint32_t seed) {
    std::vector<cf> x(size_t(rate * lenS));
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0, float(noise));
    for (auto& v : x) v = cf(g(rng), g(rng));
    for (const Sig& s : sigs) {
        double t = s.startS, ph = 0;
        for (int d : s.runs) {
            const size_t a = size_t(t * rate);
            t += std::abs(d) * 1e-6;
            const size_t e = std::min(x.size(), size_t(t * rate));
            const double f = s.fsk ? s.offHz + (d > 0 ? s.devHz : -s.devHz) : s.offHz;
            for (size_t i = a; i < e; ++i) {
                ph += 2 * M_PI * f / rate;
                if (s.fsk || d > 0) x[i] += cf(float(s.amp * std::cos(ph)), float(s.amp * std::sin(ph)));
            }
        }
    }
    return x;
}
static std::vector<Reading> runFront(const std::vector<cf>& x, double rate, std::vector<Burst>* bursts = nullptr) {
    FrontEnd fe(rate);
    fe.setDcGuardHz(15e3);
    std::vector<Reading> out;
    auto on = [&](const Burst& b) { if (bursts) bursts->push_back(b); Service::decodeBurst(b, 433.92e6, out); };
    for (size_t i = 0; i < x.size(); i += 16384) fe.feed(x.data() + i, std::min<size_t>(16384, x.size() - i), on);
    fe.flush(on);
    return out;
}

// ── ENCODERS (from the published specs) ──────────────────────────────────────────────────────────────────────────────
// Oregon: OOK Manchester, 1024 Hz, bit 1 = on then off; pulses trimmed at their end.
static void manchesterTx(Tx& tx, const Bits& bits, double half, double trim) {
    std::vector<uint8_t> lv;
    for (uint8_t b : bits) { lv.push_back(b ? 1 : 0); lv.push_back(b ? 0 : 1); }
    for (size_t i = 0; i < lv.size();) {
        size_t j = i; while (j < lv.size() && lv[j] == lv[i]) ++j;
        const double d = (j - i) * half;
        if (lv[i]) { tx.mark(d - trim); tx.space(trim); } else tx.space(d);
        i = j;
    }
}
static std::vector<int> oregonNibbles(const char* hex) { std::vector<int> n; for (const char* p = hex; *p; ++p) { const char c = *p; n.push_back(c <= '9' ? c - '0' : c - 'A' + 10); } return n; }
static Bits oregonBits(const std::vector<int>& nb, int preOnes) {
    Bits b(size_t(preOnes), 1);
    b.push_back(0); b.push_back(1); b.push_back(0); b.push_back(1);
    for (int n : nb) for (int i = 0; i < 4; ++i) b.push_back((n >> i) & 1);
    for (int i = 0; i < 8; ++i) b.push_back(0);   // post-amble
    return b;
}
static std::vector<int> withChecksum(std::vector<int> nb, int len) {
    nb.resize(size_t(len - 2), 0);
    int sum = 0; for (int x : nb) sum += x;
    nb.push_back(sum & 0xF); nb.push_back((sum >> 4) & 0xF);
    return nb;
}
static Runs oregonV3(const std::vector<int>& nb) { Tx tx; manchesterTx(tx, oregonBits(nb, 24), 488.28, 138); return tx.r; }
static Runs oregonV21(const std::vector<int>& nb) {
    const Bits o = oregonBits(nb, 16);
    Bits t; for (uint8_t b : o) { t.push_back(b ^ 1); t.push_back(b); }
    Tx tx; manchesterTx(tx, t, 488.28, 96); tx.space(10900); manchesterTx(tx, t, 488.28, 96);
    return tx.r;
}
static void pwmBits(Tx& tx, const Bits& b, double one, double zero, double gap) { for (uint8_t x : b) { tx.mark(x ? one : zero); tx.space(gap); } }
static Runs wh1080(const char* hex) { Tx tx; Bits b(8, 1); Bits d = bytesToBits(hexBytes(hex)); b.insert(b.end(), d.begin(), d.end()); pwmBits(tx, b, 500, 1500, 1000); return tx.r; }
static Runs wh2(uint16_t id, double t, int h) {
    const int raw = (t < 0 ? 0x800 : 0) | int(std::lround(std::fabs(t) * 10));
    std::vector<uint8_t> by = { uint8_t(id >> 4), uint8_t(((id & 0xF) << 4) | (raw >> 8)), uint8_t(raw & 0xFF), uint8_t(h) };
    by.push_back(crc8(by.data(), 4, 0x31, 0));
    Tx tx; Bits b(8, 1); Bits d = bytesToBits(by); b.insert(b.end(), d.begin(), d.end()); pwmBits(tx, b, 500, 1500, 1000); return tx.r;
}
static Runs nexusTx(int id, int ch, double t, int h, bool batt) {
    int traw = int(std::lround(t * 10)) & 0xFFF;
    Bits w;
    auto put = [&](uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) w.push_back((v >> i) & 1); };
    put(uint32_t(id), 8); put(batt ? 1 : 0, 1); put(0, 1); put(uint32_t(ch - 1), 2); put(uint32_t(traw), 12); put(0xF, 4); put(uint32_t(h), 8);
    Tx tx;
    for (int rep = 0; rep < 12; ++rep) { tx.mark(500); tx.space(4000); for (uint8_t b : w) { tx.mark(500); tx.space(b ? 2000 : 1000); } }
    tx.mark(500);
    return tx.r;
}
static Runs nrzFsk(const Bits& b, double bitUs) { Tx tx; for (uint8_t x : b) { if (x) tx.mark(bitUs); else tx.space(bitUs); } return tx.r; }
static Runs lacrosseTx(int id, double t, int h, bool weak) {
    const int v = int(std::lround((t + 40.0) * 10));
    std::vector<uint8_t> by = { uint8_t(0x90 | (id >> 2)), uint8_t(((id & 3) << 6) | (v / 100)), uint8_t(((v / 10) % 10) << 4 | (v % 10)),
                                uint8_t((weak ? 0x80 : 0) | h) };
    by.push_back(crc8(by.data(), 4, 0x31, 0));
    std::vector<uint8_t> f = {0xAA, 0xAA, 0xAA, 0x2D, 0xD4}; f.insert(f.end(), by.begin(), by.end()); f.push_back(0);
    return nrzFsk(bytesToBits(f), 1e6 / 17241.0);
}
static Runs acuritePwm(const std::vector<uint8_t>& by) {
    Tx tx;
    for (int rep = 0; rep < 3; ++rep) {
        tx.space(2250);
        for (int i = 0; i < 4; ++i) { tx.mark(600); tx.space(600); }
        for (uint8_t x : bytesToBits(by)) { if (x) { tx.mark(400); tx.space(225); } else { tx.mark(250); tx.space(400); } }
    }
    return tx.r;
}
static Runs acurite782(const char* hex) {
    Tx tx;
    for (int rep = 0; rep < 8; ++rep) { tx.mark(540); tx.space(9000); for (uint8_t x : bytesToBits(hexBytes(hex))) { tx.mark(540); tx.space(x ? 4100 : 2100); } }
    tx.mark(540);
    return tx.r;
}
static Runs currentCostTx(const char* hex8) {
    Bits b = bytesToBits({0x55, 0x55, 0x55, 0x2D, 0xD4});
    for (uint8_t x : bytesToBits(hexBytes(hex8))) { b.push_back(x ? 1 : 0); b.push_back(x ? 0 : 1); }   // 10 = 1, 01 = 0
    for (int i = 0; i < 8; ++i) b.push_back(i & 1);
    return nrzFsk(b, 1e6 / 3918.0);
}
static Runs keeloqTx(uint32_t hop, uint32_t serial, unsigned buttons /*S0..S3 bits 0..3*/, bool vlow, double te = 400, int words = 2) {
    Bits bits;   // LSb first: 32 hop, 28 serial, S3 S0 S1 S2, VLOW, RPT
    for (int i = 0; i < 32; ++i) bits.push_back((hop >> i) & 1);
    for (int i = 0; i < 28; ++i) bits.push_back((serial >> i) & 1);
    bits.push_back((buttons >> 3) & 1); bits.push_back(buttons & 1); bits.push_back((buttons >> 1) & 1); bits.push_back((buttons >> 2) & 1);
    bits.push_back(vlow); bits.push_back(0);
    Tx tx;
    for (int w = 0; w < words; ++w) {
        for (int p = 0; p < 12; ++p) { tx.mark(te); tx.space(p == 11 ? 10 * te : te); }
        for (size_t i = 0; i < bits.size(); ++i) {
            if (bits[i]) { tx.mark(te); tx.space(2 * te); } else { tx.mark(2 * te); tx.space(te); }
        }
        tx.space(39 * te);
    }
    return tx.r;
}
static Runs fixedCodeTx(uint32_t code24, double T = 350, int reps = 6) {
    Tx tx;
    for (int r = 0; r < reps; ++r) {
        tx.mark(T); tx.space(31 * T);
        for (int i = 23; i >= 0; --i) { if ((code24 >> i) & 1) { tx.mark(3 * T); tx.space(T); } else { tx.mark(T); tx.space(3 * T); } }
    }
    tx.mark(T);
    return tx.r;
}
static Runs tpmsBTx(uint32_t id, int pRaw, int tC) {
    std::vector<uint8_t> by = { 0x5A, uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id), uint8_t(pRaw), uint8_t(tC + 50) };
    by.push_back(crc8(by.data() + 1, 6, 0x07, 0));
    Bits b; for (int i = 0; i < 16; ++i) b.push_back(1);      // a run-in we chose (the paper gives none)
    b.push_back(1); b.push_back(0); b.push_back(1);           // 3 LEARN bits
    Bits d = bytesToBits(by); b.insert(b.end(), d.begin(), d.end());
    Tx tx; manchesterTx(tx, b, 120, 0); tx.space(500);
    Runs one = tx.r;
    Tx all; for (int k = 0; k < 2; ++k) { for (int x : one) all.add(x); all.space(3000); }
    return all.r;
}
static Runs tpmsATx(uint32_t id, int p, int tC, double half = 52) {
    std::vector<uint8_t> by = { uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id), uint8_t(p), uint8_t(tC + 40), 0x12, 0x34 };
    const uint16_t c = crc16(by.data(), 8, 0x1021, 0xFFFF); by.push_back(uint8_t(c >> 8)); by.push_back(uint8_t(c));
    Bits bits = {1, 0}; Bits d = bytesToBits(by); bits.insert(bits.end(), d.begin(), d.end());
    // differential Manchester (a change at a bit's start = 0, a change in every middle), after 8 alternating halves
    std::vector<uint8_t> lv;
    for (int i = 0; i < 8; ++i) lv.push_back(uint8_t(i & 1));
    uint8_t cur = lv.back();
    for (uint8_t b : bits) {
        uint8_t first = b ? cur : uint8_t(cur ^ 1);
        lv.push_back(first); lv.push_back(uint8_t(first ^ 1)); cur = uint8_t(first ^ 1);
    }
    Tx tx; for (uint8_t l : lv) { if (l) tx.mark(half); else tx.space(half); }
    return tx.r;
}

static const Reading* findModel(const std::vector<Reading>& v, const std::string& modelPart) {
    for (const auto& r : v) if (r.model.find(modelPart) != std::string::npos) return &r;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** ★ --write-clip <out.u8> <radio centre Hz> <rate>: a few seconds of rtl_tcp u8 IQ holding an Oregon THGR810, a WH1080,
 *  a TPMS (twice, so it confirms) and a fixed-code remote at known 433 MHz frequencies — the replay for
 *  scripts/test-server-ism.mjs (fake-rtl-tcp.mjs --ism). */
static int writeClip(const char* out, double centre, double rate) {
    std::vector<Sig> s;
    Sig a{oregonV3(withChecksum(oregonNibbles("F824" "2" "6B" "0" "481" "0" "26" "0"), 17))}; a.offHz = 433.92e6 - centre; a.startS = 0.3; s.push_back(a);
    Sig b{wh1080("a1820d5a0306004e08d3")}; b.offHz = 433.85e6 - centre; b.startS = 0.9; s.push_back(b);
    Sig c{tpmsBTx(0x11223344, 88, 18)}; c.offHz = 433.95e6 - centre; c.startS = 1.5; s.push_back(c);
    Sig c2 = c; c2.startS = 2.1; s.push_back(c2);
    Sig d{fixedCodeTx(0xA5C3E8)}; d.offHz = 434.00e6 - centre; d.startS = 2.6; s.push_back(d);
    const auto x = synth(s, rate, 3.4, 0.02, 77);
    FILE* f = std::fopen(out, "wb");
    if (!f) return 1;
    std::vector<uint8_t> u(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) {
        u[2 * i]     = uint8_t(std::max(0.0f, std::min(255.0f, std::round(x[i].real() * 127.5f + 127.5f))));
        u[2 * i + 1] = uint8_t(std::max(0.0f, std::min(255.0f, std::round(x[i].imag() * 127.5f + 127.5f))));
    }
    std::fwrite(u.data(), 1, u.size(), f);
    std::fclose(f);
    printf("wrote %zu samples (%.1f s) centred on %.3f MHz\n", x.size(), x.size() / rate, centre / 1e6);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--write-clip") return writeClip(argv[2], std::atof(argv[3]), std::atof(argv[4]));
    printf("test-ism — the V12 ISM decoder\n");

    // 1. Published worked examples, straight into the field parsers
    {
        printf(" worked examples\n");
        Reading r;
        // Oregon doc p13: THGR122NX "1D20485C480882835" → ch 3, rolling 85, battery low, −8.4 °C, 28 %, checksum 0x53
        CHECK(oregon::parse(oregonNibbles("1D20485C480882835"), true, r), "Oregon example 1 rejected");
        CHECK(approx(r.tempC, -8.4, 1e-9) && approx(r.hum, 28, 0) && r.channel == 3 && r.battOk == 0 && r.pubId == "85",
              "Oregon example 1 fields: %.1f %.0f ch%d batt%d id %s", r.tempC, r.hum, r.channel, r.battOk, r.pubId.c_str());
        r = Reading();
        CHECK(oregon::parse(oregonNibbles("1D2016B1091073A14"), true, r), "Oregon example 2 rejected");
        CHECK(approx(r.tempC, 19.0, 1e-9) && approx(r.hum, 37, 0) && r.channel == 1 && r.battOk == 1, "Oregon example 2 fields");
        CHECK(!oregon::parse(oregonNibbles("1D2016B1091073A15"), true, r), "Oregon: a bad checksum accepted");
        // WH1080 (Sangeelee) — three messages, CRC-8 0x31 over bytes 0–8
        r = Reading();
        CHECK(fineoffset::wh1080Fields(hexBytes("a1820d5a030600 4e08d3"), false, r), "WH1080 example 1 rejected");
        CHECK(approx(r.tempC, 12.5, 1e-9) && r.hum == 90 && approx(r.windKmh, 3 * 0.34 * 3.6, 1e-9) && approx(r.gustKmh, 6 * 0.34 * 3.6, 1e-9)
              && approx(r.rainMm, 23.4, 1e-9) && approx(r.windDir, 180, 0), "WH1080 example 1 fields %.1f %.0f %.2f %.1f %.0f", r.tempC, r.hum, r.windKmh, r.rainMm, r.windDir);
        r = Reading(); CHECK(fineoffset::wh1080Fields(hexBytes("a1820d59020500 4e0a07"), false, r) && approx(r.tempC, 12.5, 1e-9) && r.hum == 89
                             && approx(r.windDir, 225, 0), "WH1080 example 2");
        r = Reading(); CHECK(fineoffset::wh1080Fields(hexBytes("a1820e5d020400 4e0686"), false, r) && approx(r.tempC, 12.6, 1e-9), "WH1080 example 3");
        // WH1080 V2 FSK (sevenwatt) — 3.9 °C, 71 %
        r = Reading(); CHECK(fineoffset::wh1080Fields(hexBytes("a4f0274700 0003c60cfe"), true, r) && approx(r.tempC, 3.9, 1e-9) && r.hum == 71
                             && r.pubId == "4F", "WH1080 FSK example %.1f %.0f %s", r.tempC, r.hum, r.pubId.c_str());
        // Acurite 592 family (rayshobby part 2): 34 % / ~21 °C, 35 % / ~24 °C, 16 % / ~36 °C — sum checksum ours
        {
            const char* ex[3] = {"9302442209BEC2", "930244A309D85D", "930244900AD74A"};
            const double t[3] = {21.4, 24.0, 36.7}; const int h[3] = {34, 35, 16};
            for (int i = 0; i < 3; ++i) {
                Burst b; b.ook = acuritePwm(hexBytes(ex[i]));
                std::vector<Reading> o; acurite::decodePwm(b, o);
                CHECK(o.size() == 1 && approx(o[0].tempC, t[i], 1e-9) && o[0].hum == h[i], "Acurite 592 example %d", i + 1);
            }
        }
        // Acurite 00899 rain (part 3): 0, 1, 13 tips
        {
            const char* ex[3] = {"72F8F0000000005A", "72F8F000000081DB", "72F8F00000008DE7"};
            const char* want[3] = {"0 bucket tips", "1 bucket tip", "13 bucket tips"};
            for (int i = 0; i < 3; ++i) {
                Burst b; b.ook = acuritePwm(hexBytes(ex[i]));
                std::vector<Reading> o; acurite::decodePwm(b, o);
                CHECK(o.size() == 1 && o[0].extra == want[i], "Acurite 899 example %d: %s", i + 1, o.empty() ? "nothing" : o[0].extra.c_str());
            }
        }
        // Acurite 00782 (part 1): 31.8 °C, −0.5 °C
        {
            Burst b; b.ook = acurite782("8B813E3D"); std::vector<Reading> o; acurite::decode782(b, o);
            CHECK(o.size() == 1 && approx(o[0].tempC, 31.8, 1e-9), "Acurite 782 31.8");
            Burst c; c.ook = acurite782("8B8FFB17"); o.clear(); acurite::decode782(c, o);
            CHECK(o.size() == 1 && approx(o[0].tempC, -0.5, 1e-9), "Acurite 782 -0.5");
        }
        // Current Cost (Kelly wiki): address 63B, 390 W
        {
            Burst b; b.fsk = currentCostTx("063B818600000000"); std::vector<Reading> o; currentcost::decode(b, o);
            CHECK(o.size() == 1 && o[0].pubId == "63B" && approx(o[0].watts, 390, 0), "Current Cost example: %s %.0f",
                  o.empty() ? "-" : o[0].pubId.c_str(), o.empty() ? 0.0 : o[0].watts);
        }
    }

    // 2. End to end: each family encoded, modulated into IQ at 2.4 MS/s, +120 kHz off the capture centre
    struct Case { const char* name; Sig s; const char* model; double temp; };
    std::vector<Case> cases;
    cases.push_back({"Oregon v3 THGR810", {oregonV3(withChecksum(oregonNibbles("F824" "2" "6B" "0" "481" "0" "26" "0"), 17))}, "THGR810", 18.4});
    cases.push_back({"Oregon v2.1 THGR122NX", {oregonV21(withChecksum(oregonNibbles("1D20" "4" "85" "0" "480" "8" "82" "8"), 17))}, "THGR122NX", -8.4});
    cases.push_back({"WH1080", {wh1080("a1820d5a0306004e08d3")}, "WH1080", 12.5});
    cases.push_back({"WH2", {wh2(0x1A5, 21.3, 55)}, "WH2", 21.3});
    cases.push_back({"Nexus", {nexusTx(0x5C, 2, -3.7, 56, true)}, "temperature/humidity", -3.7});
    { Sig s{lacrosseTx(0x2B, 20.2, 61, false)}; s.fsk = true; s.devHz = 40e3; cases.push_back({"LaCrosse TX29", s, "IT+", 20.2}); }
    cases.push_back({"Acurite 592", {acuritePwm(hexBytes("9302442209BEC2"))}, "592TX", 21.4});
    cases.push_back({"Acurite 782", {acurite782("8B813E3D")}, "00782", 31.8});
    { Sig s{currentCostTx("063B818600000000")}; s.fsk = true; s.devHz = 30e3; cases.push_back({"Current Cost", s, "transmitter", NAN}); }
    cases.push_back({"KeeLoq", {keeloqTx(0xDEADBEEF, 0x0ABCDEF, 0x2, false)}, "Key fob", NAN});
    cases.push_back({"EV1527", {fixedCodeTx(0xA5C3E8)}, "Doorbell", NAN});
    cases.push_back({"TPMS B", {tpmsBTx(0x11223344, 88, 18)}, "type B", 18});
    { Sig s{tpmsATx(0x55667788, 0x80, 21)}; s.fsk = true; s.devHz = 40e3; cases.push_back({"TPMS A", s, "type A", 21}); }
    printf(" end to end at 2.4 MS/s, +120 kHz, 20 dB-ish SNR\n");
    for (auto& c : cases) {
        c.s.offHz = 120e3;
        const auto x = synth({c.s}, 2.4e6, 0.05 + std::abs(c.s.runs.size() ? 0 : 0) + 0.9, 0.02, 1);
        std::vector<Burst> bs;
        const auto got = runFront(x, 2.4e6, &bs);
        const Reading* r = findModel(got, c.model);
        CHECK(r != nullptr, "%s: not decoded (%zu bursts, %zu readings)", c.name, bs.size(), got.size());
        if (r && c.temp == c.temp) CHECK(approx(r->tempC, c.temp, 0.051), "%s: %.2f °C, want %.2f", c.name, r->tempC, c.temp);
        if (r) {
            CHECK(approx(r->freqHz, 433.92e6 + 120e3, 12e3), "%s: frequency %.0f", c.name, r->freqHz);
            printf("   %-22s → %s\n", c.name, readable(*r, 1).c_str());
        }
    }
    // the TPMS B values exactly
    {
        Sig s{tpmsBTx(0x11223344, 88, 18)}; s.offHz = -60e3;
        const auto got = runFront(synth({s}, 2.4e6, 1.0, 0.02, 2), 2.4e6);
        const Reading* r = findModel(got, "type B");
        CHECK(r && approx(r->kpa, 220, 1e-9) && r->confirm, "TPMS B: 220 kPa, confirm flag");
    }

    // 3. Drift: ±150 kHz around the band centre, and the capture rates the radios run (Oregon v3, the slowest-edged)
    printf(" drift and capture rates\n");
    {
        const Runs o = oregonV3(withChecksum(oregonNibbles("F824" "1" "22" "0" "571" "0" "45" "0"), 17));
        int ok = 0, n = 0;
        for (double off = -150e3; off <= 150e3; off += 25e3) {
            if (std::fabs(off) < 20e3) continue;
            Sig s{o}; s.offHz = off; ++n;
            if (findModel(runFront(synth({s}, 2.4e6, 0.5, 0.02, uint32_t(off + 1e6)), 2.4e6), "THGR810")) ++ok;
        }
        CHECK(ok == n, "drift: %d of %d offsets decoded", ok, n);
        printf("   drift −150…+150 kHz: %d/%d\n", ok, n);
        const double rates[] = {1.024e6, 2.048e6, 2.4e6, 2.56e6, 3.0e6, 6.0e6, 10.0e6};
        for (double rt : rates) {
            Sig s{wh1080("a1820d5a0306004e08d3")}; s.offHz = std::min(150e3, rt * 0.3);
            const bool got = findModel(runFront(synth({s}, rt, 0.5, 0.02, 7), rt), "WH1080") != nullptr;
            Sig s2{o}; s2.offHz = -std::min(150e3, rt * 0.3);
            const bool got2 = findModel(runFront(synth({s2}, rt, 0.5, 0.02, 8), rt), "THGR810") != nullptr;
            CHECK(got && got2, "rate %.3f MS/s: WH1080 %d, Oregon %d", rt / 1e6, got, got2);
        }
    }

    // 4. SNR: Oregon v3 and WH1080 at falling signal levels (noise σ 0.02 per component)
    printf(" sensitivity\n");
    {
        const Runs o = oregonV3(withChecksum(oregonNibbles("F824" "1" "22" "0" "571" "0" "45" "0"), 17));
        for (double amp : {0.1, 0.03, 0.015, 0.008, 0.005}) {
            int ok = 0;
            for (int k = 0; k < 6; ++k) { Sig s{o}; s.offHz = 90e3; s.amp = amp; if (findModel(runFront(synth({s}, 2.4e6, 0.4, 0.02, uint32_t(100 + k)), 2.4e6), "THGR810")) ++ok; }
            // ★ SNR in the 250 kHz channel: amp² / (2σ² × 250k / 2.4M)
            const double snr = 10 * std::log10(amp * amp / (2 * 0.02 * 0.02 * 250e3 / 2.4e6));
            printf("   Oregon amp %.3f (channel SNR %+.1f dB): %d/6\n", amp, snr, ok);
            if (amp >= 0.015) CHECK(ok == 6, "Oregon at amp %.3f: %d/6", amp, ok);
        }
    }

    // 5. Several transmitters at once, at different frequencies
    {
        Sig a{oregonV3(withChecksum(oregonNibbles("F824" "3" "11" "0" "052" "8" "90" "0"), 17))}; a.offHz = -200e3;
        Sig b{wh1080("a1820d5a0306004e08d3")}; b.offHz = 60e3; b.startS = 0.08;
        Sig c{fixedCodeTx(0x123458)}; c.offHz = 300e3; c.startS = 0.06;
        const auto got = runFront(synth({a, b, c}, 2.4e6, 0.8, 0.02, 9), 2.4e6);
        CHECK(findModel(got, "THGR810") && findModel(got, "WH1080") && findModel(got, "Doorbell"), "three at once: %zu readings", got.size());
        const Reading* ao = findModel(got, "THGR810");
        CHECK(ao && approx(ao->tempC, -25.0, 1e-9), "three at once: Oregon −25.0 °C");
    }

    // 6. Pure noise: nothing — 20 s at 2.4 MS/s, two seeds; and a CW carrier that never stops
    printf(" false positives\n");
    {
        size_t total = 0, bursts = 0;
        for (uint32_t seed : {11u, 12u}) {
            std::vector<Burst> bs;
            total += runFront(synth({}, 2.4e6, 10.0, 0.02, seed), 2.4e6, &bs).size();
            bursts += bs.size();
        }
        CHECK(total == 0, "pure noise decoded %zu readings", total);
        printf("   20 s of noise: %zu bursts detected, %zu readings\n", bursts, total);
        Sig cw; cw.runs = {3000000}; cw.offHz = 170e3; cw.startS = 0.01;
        CHECK(runFront(synth({cw}, 2.4e6, 3.5, 0.02, 13), 2.4e6).empty(), "a CW carrier decoded as something");
    }

    // 7. PRIVACY: the JSON never carries a TPMS ID, a fob's serial or hop code, or a fixed code
    // ★ SUSPECTED KEY FOBS (FobWatch) — modelled on Stuart's car key as recorded 2026-10-25 (FSK ±42.7 kHz, Manchester
    //   200 µs half-bits after an alternating preamble, 68 ms frames, three 161 ms apart). Never a protocol: the shape.
    printf(" suspected key fobs\n");
    {
        // one frame: 16 bits of preamble (1010…), then a fixed made-up payload, Manchester, 200 µs halves = 2.5 kbit/s
        auto keyFrame = []() {
            Bits b; for (int i = 0; i < 16; ++i) b.push_back(i & 1);
            const Bits p = bytesToBits(hexBytes("3C 96 A5 0F E1 72 5B C8 19 D4 6E 0B 93 A7 2D F0 58"));
            b.insert(b.end(), p.begin(), p.end());
            Tx tx; manchesterTx(tx, b, 200, 0); return tx.r;
        };
        const Runs fr = keyFrame();
        // END TO END: three frames through the real front end at 2.4 MS/s → bursts the decoders refuse, FobWatch counts ONE press
        {
            std::vector<Sig> sigs;
            for (int k = 0; k < 3; ++k) { Sig s; s.runs = fr; s.fsk = true; s.devHz = 42.7e3; s.offHz = 214e3; s.amp = 0.3; s.startS = 0.05 + 0.161 * k; sigs.push_back(s); }
            std::vector<Burst> bs;
            const auto x = synth(sigs, 2.4e6, 0.7, 0.01, 77);
            const auto dec = runFront(x, 2.4e6, &bs);
            CHECK(dec.empty(), "fob: a protocol decoder claimed the key (%zu)", dec.size());
            double T = 0, len = 0; size_t cand = 0;
            for (const Burst& b : bs) if (FobWatch::candidate(b, T, len)) ++cand;
            CHECK(cand >= 3, "fob: %zu of %zu bursts look like a key frame (T %.0f µs, %.1f ms)", cand, bs.size(), T, len);
            FobWatch fw; int presses = 0; Reading got;
            for (const Burst& b : bs) { Reading r; if (fw.offer(b, 433.92e6, r)) { ++presses; got = r; } }
            CHECK(presses == 1, "fob: three frames are %d presses, want 1", presses);
            CHECK(got.kind == "suspectfob" && got.secret && approx(got.freqHz, 433.92e6 + 214e3, 10e3), "fob: the reading (%s, %.0f Hz)",
                  got.kind.c_str(), got.freqHz);
            Tracker tr; tr.accept(got, 1.0);
            const std::string j = tr.json(1.0);
            CHECK(j.find("Suspected key fob #1 \xc2\xb7 button press") != std::string::npos && j.find("\"kind\":\"suspectfob\"") != std::string::npos,
                  "fob: the line %s", j.c_str());
            CHECK(j.find("\"id\"") == std::string::npos && j.find("3C96") == std::string::npos, "fob: an id or bits in the JSON");
        }
        // THE RULES, on bursts made directly (seconds of timing without seconds of IQ)
        auto frame = [&](double t, double offHz, double lvl = -10) {
            Burst b; b.tSec = t; b.offHz = offHz; b.fsk = fr; b.fskDevHz = 42.7e3; b.levelDb = float(lvl); b.noiseDb = -45; return b;
        };
        auto count = [&](const std::vector<Burst>& v, FobWatch& fw) { int n = 0; for (const Burst& b : v) { Reading r; if (fw.offer(b, 433.92e6, r)) ++n; } return n; };
        {
            FobWatch fw; std::vector<Burst> v;
            for (int k = 0; k < 6; ++k) v.push_back(frame(1.0 + 1.2 * k, 360e3));          // a sensor: one frame every 1.2 s
            CHECK(count(v, fw) == 0, "fob: single frames every 1.2 s counted as presses");
        }
        {
            FobWatch fw; std::vector<Burst> v;
            for (int k = 0; k < 12; ++k) v.push_back(frame(1.0 + 0.161 * k, 214e3));        // a held button
            CHECK(count(v, fw) == 1, "fob: a held button is not one press");
        }
        {
            FobWatch fw; std::vector<Burst> v;
            for (double t0 : {1.0, 2.3}) for (int k = 0; k < 3; ++k) v.push_back(frame(t0 + 0.161 * k, 214e3));   // 1.3 s apart
            CHECK(count(v, fw) == 2, "fob: two presses 1.3 s apart are not two");
        }
        {
            FobWatch fw; std::vector<Burst> v;                                              // overload images ±120 kHz
            for (int k = 0; k < 3; ++k) { v.push_back(frame(1.0 + 0.161 * k, 214e3)); v.push_back(frame(1.001 + 0.161 * k, 94e3, -12)); v.push_back(frame(1.002 + 0.161 * k, 334e3, -12)); }
            CHECK(count(v, fw) == 1, "fob: a press and its images are not one press");
        }
        {
            FobWatch fw; std::vector<Burst> v;                                              // presses on a 10 s timer = a sensor
            for (int p = 0; p < 5; ++p) for (int k = 0; k < 2; ++k) v.push_back(frame(1.0 + 10.0 * p + 0.161 * k, 500e3));
            const int n = count(v, fw);
            CHECK(n == 3, "fob: a 10 s timer gave %d presses before it was recognised, want 3", n);
            CHECK(fw.takePeriodic().size() == 1, "fob: the timer was not reported");
        }
        {
            FobWatch fw; Burst b = frame(1.0, 214e3); b.fsk.clear();                         // OOK: never
            Burst b2 = b; b2.tSec = 1.161;
            CHECK(count({b, b2}, fw) == 0, "fob: OOK bursts counted");
            Burst w = frame(2.0, 214e3, -38); Burst w2 = frame(2.161, 214e3, -38);          // 7 dB over the noise: too weak to call
            CHECK(count({w, w2}, fw) == 0, "fob: a 7 dB burst counted");
        }
    }

    printf(" privacy\n");
    {
        Tracker tr;
        std::vector<Reading> v;
        Burst kb; kb.ook = keeloqTx(0xDEADBEEF, 0x0ABCDEF, 0x3, true); keeloq::decode(kb, v);
        Burst fb; fb.ook = fixedCodeTx(0xA5C3E8); fixedcode::decode(fb, v);
        Burst tb; tb.ook = tpmsBTx(0x11223344, 88, 18); tpms::decodeB(tb, v);
        Burst tb2; tb2.ook = tpmsBTx(0x99AABBCC, 92, 20); tpms::decodeB(tb2, v);
        CHECK(v.size() == 4, "privacy setup: %zu readings", v.size());
        for (int pass = 0; pass < 2; ++pass) for (const auto& r : v) tr.accept(r, 10.0 + pass);   // twice: TPMS B confirms
        const std::string j = tr.json(12.0);
        const char* banned[] = {"DEADBEEF", "deadbeef", "ABCDEF", "abcdef", "A5C3E", "a5c3e", "11223344", "99AABBCC", "99aabbcc",
                                "3735928559", "11259375", "10863592", "287454020"};
        for (const char* s : banned) CHECK(j.find(s) == std::string::npos, "privacy: the JSON carries %s", s);
        CHECK(j.find("TPMS #1") != std::string::npos && j.find("TPMS #2") != std::string::npos, "privacy: two cars, two nicknames");
        CHECK(j.find("Key fob #1") != std::string::npos && j.find("button 1+2") != std::string::npos && j.find("battery low") != std::string::npos,
              "privacy: the fob line");
        CHECK(j.find("Doorbell / remote #1") != std::string::npos, "privacy: the remote line");
        CHECK(j.find("\"id\"") == std::string::npos, "privacy: an id field on a secret kind");
        printf("   %s\n", j.substr(0, 400).c_str());
        // the same car again → the same nickname; a NEW server (new salt) numbers afresh but never shows the ID either
        Tracker tr2;
        tr2.accept(v[3], 1); tr2.accept(v[3], 2);
        CHECK(tr2.json(3).find("TPMS #1") != std::string::npos, "privacy: numbering is per server");
        // a weather sensor DOES carry its channel and short ID
        Tracker tw; Reading w; w.kind = "weather"; w.make = "Oregon Scientific"; w.model = "THGR810"; w.channel = 2; w.pubId = "6B";
        w.tempC = 18.4; w.hum = 62; tw.accept(w, 1);
        const std::string wj = tw.json(1);
        CHECK(wj.find("Oregon Scientific THGR810 \xc2\xb7 18.4 \xc2\xb0" "C \xc2\xb7 62 % RH \xc2\xb7 ch 2") != std::string::npos, "the weather line: %s", wj.c_str());
        CHECK(wj.find("\"id\":\"6B\"") != std::string::npos, "the weather ID");
    }

    // 8. Repeats fold; units
    {
        Tracker tr; Reading w; w.kind = "weather"; w.make = "Fine Offset"; w.model = "WH2"; w.pubId = "1A5"; w.tempC = 21.3; w.hum = 55;
        tr.accept(w, 1.0); tr.accept(w, 1.5); tr.accept(w, 2.0);
        CHECK(tr.size() == 1 && tr.json(2).find("\"count\":1") != std::string::npos, "repeats fold into one heard");
        tr.accept(w, 60.0);
        CHECK(tr.json(60).find("\"count\":2") != std::string::npos, "a minute later counts again");
        Reading t; t.kind = "tpms"; t.secret = true; t.make = "TPMS"; t.model = "type B"; t.kpa = 220; t.tempC = 18;
        CHECK(readable(t, 2).find("32 PSI") != std::string::npos, "PSI by default: %s", readable(t, 2).c_str());
        Units kp; kp.pressure = 1; CHECK(readable(t, 2, kp).find("220 kPa") != std::string::npos, "kPa");
        Units br; br.pressure = 2; CHECK(readable(t, 2, br).find("2.20 bar") != std::string::npos, "bar");
        Reading a; a.kind = "weather"; a.make = "Fine Offset"; a.model = "WH1080"; a.windKmh = 14; a.windDir = 225; a.rainMm = 2.3; a.tempC = 11.2;
        CHECK(readable(a, 0) == "Fine Offset WH1080 \xc2\xb7 11.2 \xc2\xb0" "C \xc2\xb7 wind 14 km/h SW \xc2\xb7 rain 2.3 mm", "the wind line: %s", readable(a, 0).c_str());
        Units mph; mph.mph = true; CHECK(readable(a, 0, mph).find("wind 9 mph SW") != std::string::npos, "mph");
    }

    // 9. Bands: landing by location, reachability
    {
        auto all = [](const Band&) { return true; };
        CHECK(std::string(landingBand(52.2, -0.9, all)->id) == "433", "UK lands on 433.92");
        CHECK(std::string(landingBand(40.7, -74.0, all)->id) == "315", "US lands on 315");
        CHECK(std::string(landingBand(NAN, NAN, all)->id) == "433", "no position lands on 433.92");
        auto no315 = [](const Band& b) { return std::string(b.id) != "315"; };
        CHECK(std::string(landingBand(40.7, -74.0, no315)->id) == "915", "US without 315 → 915");
        auto none = [](const Band&) { return false; };
        CHECK(landingBand(52.2, -0.9, none) == nullptr, "an HF+ (no band) has no landing");
    }

    // 10. CPU: the front end over 10 s of a busy 433 band at 2.4 MS/s
    {
        std::vector<Sig> sigs;
        for (int k = 0; k < 10; ++k) { Sig s{k % 2 ? wh1080("a1820d5a0306004e08d3") : oregonV3(withChecksum(oregonNibbles("F824" "1" "22" "0" "571" "0" "45" "0"), 17))};
                                       s.offHz = -330e3 + 60e3 * k; s.startS = 0.3 + k * 0.9; sigs.push_back(s); }   // ★ none at DC (the guard)
        const auto x = synth(sigs, 2.4e6, 10.0, 0.02, 21);
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<Burst> bs;
        const auto got = runFront(x, 2.4e6, &bs);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf(" CPU: 10 s of capture at 2.4 MS/s (10 bursts) in %.3f s = %.1f %% of one core on this machine; %zu decoded\n",
               sec, sec * 10.0, got.size());
        CHECK(got.size() >= 10, "CPU run decoded %zu of 10", got.size());   // ★ a burst may decode twice (a repeat): the tracker folds it
    }

    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails ? 1 : 0;
}
