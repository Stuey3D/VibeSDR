// test-fst4.cpp — FST4 and FST4W, our own receiver (decoders/digimodes/fst4_code.h, fst4_decoder.h). V12, 2026-10-31.
//
// ★★★ WHAT IS PINNED.
//   • THE FRAME: 10 vectors captured from the shipped WSJT-X 2.7.0 simulator run as a black box (fst4sim: the message
//     bits it prints and its 160 tones) reproduced exactly — the CRC-24, both LDPC codes, the Gray map, the sync;
//     belief propagation exact on clean soft bits; FST4W type 1 round trip through the packer;
//   • SENSITIVITY on synthetic GFSK (tools/fst4-bench/fst4_synth.h, SNR in 2500 Hz): FST4-60 every one at −23 dB and
//     ≥ 25 % at −26 (its 50 % point); FST4-15 every one at −16; FST4W-120 every one at −27; frequency/DT/SNR within tolerance;
//   • PURE NOISE gives nothing (FST4-15 × 200, FST4-60 × 60, FST4W-120 × 40 windows);
//   • the LIVE decoder: audio blocks with capture times → one FST4W-120 spot in the right period.
// ★ Silent; deterministic.
#include "decoders/digimodes/fst4_decoder.h"
#include "fst4_synth.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

struct Vec { bool w; const char* msg; const char* bits; const char* tones; };
static const Vec kVecs[] = {
    {false, "7EE65F53E9421CE502", "10110111100100100011011100010011011000100000111001000000100111111011101101101", "0132102332123103021201021303002310003123103201223232133131110321033303330313013210230230022031203003331022100230212310320102123332233222131120202132211001321023"},
    {false, "01670EAE679F02E8D2", "01001000100100001001010011101000011111111011010001111100100001000001101101101", "0132102310303100311023301222321012203023103201100132133103030231101332311013013210231303302222312310323100002120002310320123220011103003300230133103330101321023"},
    {false, "K1ABC DL1AAA R+01", "01000011111000110110101011100111111101001100000001111010011010100001001100100", "0132102310022302133323122210200012331323103201330102030210211020132221310201013210232303323131132210300023111123302310320121311233200332220210133120130001321023"},
    {false, "K1ABC G4ABC 73", "01000011111000110110101011100100111110001110101011001010010010100001101100100", "0132102310022302133323102230233320331023103201330132031032003212000202012321013210233102002032113232132311100032012310320133121321011110003203211030223001321023"},
    {false, "CQ G4ABC KP20", "01001010010111101000100110010100111110001110101011001010010001101010111000100", "0132102310331123303131102230233320331023103201133323030202300200310332131021013210232200320201103131101103312032122310320110220330321233121021220213313201321023"},
    {false, "EA3ABC G4ABC R+08", "00100111000110101011000011000100111110001110101011001010011010100001000011100", "0132102303120133320020102230233320331323103201330100231201230020111200332222013210230311012201212210132032320310112310320122111101332212312323102310033101321023"},
    {true, "LZ1AB EN50 43", "10100001001110011010000001000100001011001100110100", "0132102333010231330010100320202102200123103201223220320022123120033003010211013210233303302202311011301123002202022310320123313211330031301213323032232201321023"},
    {true, "K1ABC IO91 60", "00001001101111011110001101010111110000100111001000", "0132102300313221230211122003120302010023103201120212202200302213031333112032013210232331102203322120310122022000322310320110130123033010031000211233132001321023"},
    {true, "DL1AAA IO91 57", "01101000100010010100000001100111110000100111000100", "0132102313303031100013122003120100113023103201013020030033202211032201222110013210231013132032332001010313231002012310320131322200212332031302221001233001321023"},
    {true, "X3GO AA31 60", "00001100001110010101101100110000000000111111001000", "0132102300200231113202000002220300113023103201231020321003121333302332121133013210233013020222131301003211121312322310320101103202221131220111202201313101321023"},
};

static double rate(fst4::Mode m, int P, double snr, int trials, double* dtE, double* fE) {
    const bool w = m == fst4::Mode::FST4W;
    fst4::SlotDecoder d(m, P, 1500);
    fst4::SlotOptions o; if (w) { o.loHz = 1400; o.hiHz = 1600; }
    int got = 0; double de = 0, fe = 0;
    for (int t = 0; t < trials; ++t) {
        uint8_t tones[160]; std::string want;
        if (w) { fst4::encodeWspr("G4ABC", "IO91", 23, tones); want = "G4ABC IO91 23"; }
        else { const char* ms[] = {"CQ K1ABC FN42", "G4ABC DL1AAA JO62"}; fst4::encodeText(ms[t % 2], tones); want = ms[t % 2]; }
        auto a = fst4synth::noiseWindow(P, 5000u + (uint32_t)t * 13u + (uint32_t)P);
        const double f0 = w ? 1460 + 9.1 * (t % 7) : 800 + 71.3 * (t % 11), dt = -0.3 + 0.13 * (t % 5);
        fst4synth::addSignal(a, P, tones, f0, dt, snr);
        auto z = fst4synth::toBaseband(a, m, P, 1500);
        std::vector<fst4::Result> res; d.decode(z.data(), (int)z.size(), o, res);
        for (auto& r : res) if (r.text == want) { ++got; de = std::max(de, (double)std::fabs(r.dt - dt)); fe = std::max(fe, (double)std::fabs(r.f0 - f0)); }
    }
    if (dtE) *dtE = de; if (fE) *fE = fe;
    return (double)got / trials;
}

int main() {
    std::printf("test-fst4 — FST4 / FST4W, our own receiver\n\n── the frame against the black-box simulator ──\n");
    int same = 0, bp = 0;
    for (const Vec& v : kVecs) {
        uint8_t msg[77]; for (size_t i = 0; i < std::strlen(v.bits); ++i) msg[i] = (uint8_t)(v.bits[i] - '0');
        const fst4::Code c = v.w ? fst4::Code::K74 : fst4::Code::K101;
        uint8_t cw[240], t[160]; fst4::encode(c, msg, cw); fst4::codewordToTones(cw, t);
        bool s = std::strlen(v.tones) == 160; for (int i = 0; s && i < 160; ++i) s = t[i] == v.tones[i] - '0';
        same += s;
        float llr[240]; for (int j = 0; j < 240; ++j) llr[j] = cw[j] ? 3.f : -3.f;
        uint8_t out[240]; bp += fst4::bpDecode(c, llr, 10, out) == 1 && std::memcmp(out, cw, 240) == 0;
    }
    const int nv = (int)(sizeof kVecs / sizeof kVecs[0]);
    ok(same == nv, "every black-box vector reproduced (" + std::to_string(same) + "/" + std::to_string(nv) + ")");
    ok(bp == nv, "belief propagation: clean soft bits decode in one iteration");
    {
        uint8_t t[160]; fst4::encodeWspr("K1ABC", "FN42", 37, t);
        bool found = false;
        for (const Vec& v : kVecs) if (v.w && !std::strcmp(v.msg, "K1ABC FN42 37")) found = true;
        (void)found;
        // round trip through our own unpacker
        std::string txt, call, grid; int dbm = 0;
        uint8_t msg[50] = {}; (void)msg;
        fst4::SlotDecoder d(fst4::Mode::FST4W, 120, 1500);
        auto a = fst4synth::noiseWindow(120, 1); fst4synth::addSignal(a, 120, t, 1500, 0, -15);
        auto z = fst4synth::toBaseband(a, fst4::Mode::FST4W, 120, 1500);
        fst4::SlotOptions o; o.loHz = 1400; o.hiHz = 1600;
        std::vector<fst4::Result> res; d.decode(z.data(), (int)z.size(), o, res);
        ok(res.size() == 1 && res[0].text == "K1ABC FN42 37" && res[0].call == "K1ABC" && res[0].grid == "FN42" && res[0].dbm == 37,
           "FST4W type 1 through the packer and back (" + (res.empty() ? std::string("nothing") : res[0].text) + ")");
        (void)txt; (void)call; (void)grid; (void)dbm;
    }
    std::printf("\n── sensitivity (synthetic GFSK) ──\n");
    double de, fe;
    double r = rate(fst4::Mode::FST4, 60, -23, 10, &de, &fe);
    ok(r == 1.0, "FST4-60 −23 dB: every one"); ok(de < 0.1 && fe < 0.5, "FST4-60: |DT| " + std::to_string(de) + " s, |f| " + std::to_string(fe) + " Hz");
    r = rate(fst4::Mode::FST4, 60, -26, 20, nullptr, nullptr); ok(r >= 0.25, "FST4-60 −26 dB (its 50 % point): " + std::to_string((int)(r * 100)) + " %");
    ok(rate(fst4::Mode::FST4, 15, -16, 10, nullptr, nullptr) == 1.0, "FST4-15 −16 dB: every one");
    ok(rate(fst4::Mode::FST4W, 120, -27, 8, nullptr, nullptr) == 1.0, "FST4W-120 −27 dB: every one");
    std::printf("\n── pure noise ──\n");
    struct NC { fst4::Mode m; int P, N; } ncs[] = {{fst4::Mode::FST4, 15, 200}, {fst4::Mode::FST4, 60, 60}, {fst4::Mode::FST4W, 120, 40}};
    for (const NC& c : ncs) {
        fst4::SlotDecoder d(c.m, c.P, 1500); fst4::SlotOptions o; if (c.m == fst4::Mode::FST4W) { o.loHz = 1400; o.hiHz = 1600; }
        int n = 0;
        for (int i = 0; i < c.N; ++i) { auto a = fst4synth::noiseWindow(c.P, 77u + (uint32_t)i); auto z = fst4synth::toBaseband(a, c.m, c.P, 1500); std::vector<fst4::Result> res; d.decode(z.data(), (int)z.size(), o, res); n += (int)res.size(); }
        ok(n == 0, std::string(c.m == fst4::Mode::FST4W ? "FST4W-" : "FST4-") + std::to_string(c.P) + " × " + std::to_string(c.N) + " noise windows: " + std::to_string(n) + " decodes");
    }
    std::printf("\n── the live decoder ──\n");
    {
        fst4::SlotOptions o; o.loHz = 1400; o.hiHz = 1600;
        fst4::LiveDecoder live(fst4::Mode::FST4W, {120}, 1500, o);
        std::vector<fst4::Result> got;
        live.onResult = [&](const fst4::Result& x) { got.push_back(x); };
        const double t0 = 1800000000.0 - 30;          // 30 s before an even two minutes
        const int total = 300 * 12000;
        std::vector<float> a(total);
        std::mt19937 rng(9); std::normal_distribution<float> g(0, 1); for (float& v : a) v = g(rng);
        uint8_t t[160]; fst4::encodeWspr("DL1AAA", "JO62", 30, t);
        std::vector<float> w((size_t)122 * 12000, 0.f); fst4synth::addSignal(w, 120, t, 1520, 0.1, -20);
        const long off = (long)std::lround((t0 + 30 - 1.0 - t0) * 12000);
        for (size_t i = 0; i < w.size(); ++i) if (off + (long)i < total) a[(size_t)(off + (long)i)] += w[i];
        for (int i = 0; i < total; i += 1200) { live.process(&a[(size_t)i], 1200, t0 + (i + 1200) / 12000.0); live.drain(); }
        int n = 0; for (auto& x : got) { std::printf("     %s %s slot %+.0f s dt %+.2f\n", x.modeName().c_str(), x.text.c_str(), x.slotUtc - t0, x.dt); n += x.text == "DL1AAA JO62 30" && std::fabs(x.slotUtc - (t0 + 30)) < 0.01 && std::fabs(x.dt - 0.1) < 0.1; }
        ok(n == 1 && got.size() == 1, "one FST4W-120 spot, the right period, DT within 0.1 s");
    }
    std::printf("\n%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
