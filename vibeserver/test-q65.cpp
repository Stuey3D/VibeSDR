// test-q65.cpp — Q65, our own receiver (decoders/digimodes/q65_code.h, q65_decoder.h), measured. V12, 2026-10-31.
//
// ★★★ WHAT IS PINNED.
//   • THE FRAME: 17 vectors captured from the shipped WSJT-X 2.7.0 encoder utility run as a black box (q65code: its
//     78 message bits and its 85 channel tones) are reproduced exactly from the bits — CRC-12, the repeat-accumulate
//     code over GF(64), the puncturing, the sync positions; and from the TEXT through ft8_lib's packer for the
//     standard messages (the two packers agree there);
//   • the Walsh-Hadamard transform exact against its definition; belief propagation exact on clean symbols and on
//     symbols with a third of them wrong;
//   • SENSITIVITY on synthetic signals (tools/q65-bench/q65_synth.h, SNR in 2500 Hz, blind — no a-priori help):
//     60A ≥ 90 % at −26 dB and every one at −24; 30A every one at −22; 15A every one at −19; 60B/C/D ≥ 80 % at −26;
//     frequency within 0.5 Hz, DT within 0.05 s, SNR within 2 dB;
//   • PURE NOISE gives no decode at all (15 s × 300, 30 s × 150, 60 s × 100 windows — every guard on);
//   • A CARRIER on the sync tone's neighbour never decodes (the all-zero codeword is every data symbol on tone 1);
//   • the LIVE decoder: 3½ minutes of audio blocks with capture times → the 60 s and 30 s transmissions each spotted
//     once, in the right period, with the right mode name ("Q65-60A", "Q65-30A").
// ★ Silent: nothing is played. Deterministic: fixed seeds.
#include "decoders/digimodes/q65_decoder.h"
#include "q65_synth.h"
extern "C" {
#include "ft8/message.h"
}

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

struct Vec { const char* msg; const char* bits; const char* tones; };
// ★ From q65code (WSJT-X 2.7.0) — message as typed, its 78 bits, its 85 tones as '0' + tone.
static const Vec kVecs[] = {
    {"54C123B1612DD272D1", "101010011000001001000111011000101100001001011011101001001110010110100011010000",
     "0[I:8I]:0LZ00\?0GTA_8100I\?00DN8pZ0h0]N05bEC6_i02^i0m]jd08Pmf0c08ne0On0g_cO0e0anmDkIe[0"},
    {"171C17149D439536B3", "001011100011100000101110001010010011101010000111001010100110110101100111010000",
     "0<TQ_;D[08;00W0fXAV8Z00TU00jP@X@0>0;<052kmjP[0XeG0<L5=06W2R0c0NW=0eF0O86<010\\oF[QBJg0"},
    {"116FDAEEB975729FAE", "001000101101111110110101110111010111001011101010111001010011111101011101010000",
     "09^ofhH<0[j00D0nNA=d700b200M@@Ka0j0D205j64nKX0SfR0_oHT0S@C]0505jc0bg0n<RW0_0=Ckk4^p;0"},
    {"423D5A4FD12AABFE22", "100001000111101010110100100111111010001001010101010101111111110001000101010000",
     "0R8[eXk:0FF00p0b6AIh400UQ00n35D\?0C08`0NgHB74c0B\\L06FLR0MLOe0;0nKD0Fh0m=Gg0H0JaUF18gY0"},
    {"F5ABC OH2BH -08", "000010001110000011010110101101011001000100011100001101110001111110101010110010",
     "03\?4G^J50M>00b0o[cL7A00n=00a2UAn0\?0C20AhdZi:h04KN0ASdY0JHno080HTX0bd0X=95070`Dj=2BgT0"},
    {"LZ1AB EA3ABC 73", "101000010011100110100000010000110110101000100001110010111001111110100101000010",
     "0YDWQAgY0Rc00j0oV3XEe00UJ003l>jS0@0_\?0;DB2^OQ0L^20pnK=0:KpJ0U0R=U0ng0p8><0T0;n]P^eXT0"},
    {"EA3ABC F5ABC -17", "011011010100010000111001011100000100011100000110101101011001111110101000100010",
     "0LEAjM5M07^00J0oYSfZA00=c00D;^JL0<0>60PYc_K`B0Z^m0fHLp0eQ9I0f0:@D0`I08>=S0j0A1:Cp8<T0"},
    {"M0XYZ LZ1AB -09", "000010100001110101110001000101010000100111001101000000100001111110101010100010",
     "03Rfb6AX0>100R0o[S7=n00ii00Ji2fn0\?04Y0jjb^_8j0p`F0dBpp0\?`6a020D<]0=h0Y4=E0G0pEU@@P8T0"},
    {"OH2BH EA3ABC -11", "101100100010001110000110111000110110101000100001110010111001111110101010000010",
     "0]S\?7igY0Rc00j0o[32hb00:e00`GYM80@0_k0\\caQ=6l01gB0RTl^0:\\2@0c02]50fo0hMfd0P0gG8;iYXT0"},
    {"D90+I0D/JUVM7", "001011101000001110101000010000111110110101111000010101100111001001001100000000",
     "0<Y\?YAof0iF00X0:=1OW\?00OK00KnbD=0\?0gW0MhMMbOH0m3V0NNk10:R^T0B0mNY0\?;0;:QO0F0UD5Va63K0"},
    {"SMN+.3Z2NNDL1", "011000001001000101010010000010011001000001110100110011000101101001101100000000",
     "0I:6C3J20ed0060Z]1e7E00GR00RUdQ>0G0SJ0F^oo<eN06MF0]]2n0ohLO0n0d870<S0SZVA090Nch_NgL`0"},
    {"K1ABC/R W9XYZ/R FN42", "000010011011110111100011010110000110000101001001110111000100101000011001100010",
     "03LhTG760:h0050YJScX500C>00]YQ]`0M0FL09cb^:Ph0QWX0iK:L0[bYP0N0\\_\\07W0j\?in0p0CHZHQNY:0"},
    {"<PJ4/K1ABC> W9XYZ", "000000110101001010110000101000000110000101001001110111000001111110100100010010",
     "01f;aY760:h0020oUC=K[003N00LK\?kj0j0aD0d:J@\\QO0:@N0C1p^0^Img0H0TWT0Q103<DT0T0;NT4ej3T0"},
    {"K1ABC W9XYZ 579 0013", "000001001101111011110001101010000110000101001001110111000010100000000011010110",
     "02>lb[760:h0030Q4G[<k00A@00pn=no070@T0=gGQ5^>0KM201G5G0`SRK0L0pkp0Ee0UeCb0a0E]C8a=Ef0"},
    {"W9XYZ K1ABC 13B WWA", "000011000010100100111011100000001001101111011110001101010110000110100100110000",
     "043UlQ:`0O>00G07UabJS003d00E3feB0K05_0\?Z7=^PJ05>K0GW\\a0ikOk0b0S\?R0Mc0`WFX0U0cPcGLGPB0"},
    {"CQ TEST K1ABC FN42", "000000000110000101011111100100000100110111101111000110101000101000011001100010",
     "0176PU5h0`700Y0YJS\?f[00\?c00DlS_]0]04o0^_a]YX@0_[T0@^j1017PK0\?028a0CI08amO0O0d48BH[k:0"},
    {"TU; K1ABC W9XYZ R 579 MA", "100001001101111011110001101010000110000101001001110111000110111111010101010110",
     "0R>lb[760:h0070pFGjIZ004M00][<QT0p0gK0d:[MiB_0jpS0UcQc0@3H]0`0URU0Ll0\\PjP0o0`HZ4emU60"},
};
// ★ The messages ft8_lib packs exactly as q65code does (standard calls, reports, grids; not RR73, which q65code sends
//   as the GRID "RR73", nor the hashed and contest types ft8_lib packs otherwise).
static bool samePacker(const char* m) {
    for (const char* x : {"F5ABC OH2BH -08", "LZ1AB EA3ABC 73", "EA3ABC F5ABC -17", "M0XYZ LZ1AB -09", "OH2BH EA3ABC -11",
                          "K1ABC/R W9XYZ/R FN42", "CQ TEST K1ABC FN42"})
        if (!std::strcmp(m, x)) return true;
    return false;
}

static void testFrame() {
    std::printf("\n── the frame against the black-box encoder ──\n");
    int bitsOk = 0, textOk = 0, textN = 0, n = 0;
    for (const Vec& v : kVecs) {
        ++n;
        uint8_t sym[q65::kMsg] = {};
        for (int b = 0; b < 78; ++b) if (v.bits[b] == '1') sym[b / 6] |= (uint8_t)(0x20 >> (b % 6));
        uint8_t t[q65::kSymbols];
        q65::encodeTones(sym, t);
        bool same = std::strlen(v.tones) == q65::kSymbols;
        for (int i = 0; same && i < q65::kSymbols; ++i) same = t[i] == (uint8_t)(v.tones[i] - '0');
        bitsOk += same;
        if (samePacker(v.msg)) {
            ++textN;
            uint8_t t2[q65::kSymbols];
            bool s2 = q65::encodeText(v.msg, t2);
            for (int i = 0; s2 && i < q65::kSymbols; ++i) s2 = t2[i] == t[i];
            textOk += s2 && same;
        }
    }
    ok(bitsOk == n, "every black-box vector reproduced from its bits (" + std::to_string(bitsOk) + "/" + std::to_string(n) + ")");
    ok(textN >= 7 && textOk == textN, "and from the text through the packer (" + std::to_string(textOk) + "/" + std::to_string(textN) + ")");
    int syncs = 0; for (int i = 0; i < q65::kSymbols; ++i) syncs += q65::isSync(i);
    ok(syncs == 22, "22 sync symbols");
    // payload round trip
    uint8_t s[q65::kMsg], p[10], s2[q65::kMsg];
    ftx_message_t m; ftx_message_init(&m); ftx_message_encode(&m, nullptr, "CQ K1ABC FN42");
    q65::payloadToSymbols(m.payload, s); q65::symbolsToPayload(s, p); q65::payloadToSymbols(p, s2);
    ok(std::memcmp(p, m.payload, 10) == 0 && std::memcmp(s, s2, q65::kMsg) == 0, "payload ↔ symbols round trip");
}

static void testBp() {
    std::printf("\n── Walsh-Hadamard and belief propagation ──\n");
    float v[64], ref[64];
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(0.f, 1.f);
    for (int i = 0; i < 64; ++i) v[i] = u(rng);
    for (int k = 0; k < 64; ++k) { double a = 0; for (int i = 0; i < 64; ++i) a += (__builtin_popcount(i & k) & 1 ? -1.0 : 1.0) * v[i]; ref[k] = (float)a; }
    q65::wht64(v);
    float err = 0; for (int k = 0; k < 64; ++k) err = std::max(err, std::fabs(v[k] - ref[k]));
    ok(err < 1e-4f, "wht64 = the definition (max err " + std::to_string(err) + ")");
    q65::BpScratch* bp = q65::bpScratchNew();
    uint8_t msg[q65::kMsg], tones[q65::kSymbols];
    q65::encodeText("G4ABC DL1AAA JO62", tones);
    ftx_message_t m; ftx_message_init(&m); ftx_message_encode(&m, nullptr, "G4ABC DL1AAA JO62");
    uint8_t want[q65::kMsg]; q65::payloadToSymbols(m.payload, want);
    std::vector<float> intr(q65::kData * 64);
    auto fill = [&](double pWrong) {
        int d = 0;
        for (int i = 0; i < q65::kSymbols; ++i) {
            if (q65::isSync(i)) continue;
            int t = tones[i] - 1;
            if (u(rng) < pWrong) t = (t + 1 + (int)(u(rng) * 62)) % 64;
            for (int k = 0; k < 64; ++k) intr[(size_t)d * 64 + k] = k == t ? 0.6f : 0.4f / 63;
            ++d;
        }
    };
    fill(0); int it = q65::bpDecode(intr.data(), 50, msg, bp);
    ok(it == 1 && std::memcmp(msg, want, q65::kMsg) == 0, "clean symbols: decoded in one iteration");
    int good = 0;
    for (int r = 0; r < 50; ++r) { fill(0.33); it = q65::bpDecode(intr.data(), 50, msg, bp); good += it && std::memcmp(msg, want, q65::kMsg) == 0; }
    ok(good >= 45, "a third of the symbols wrong: " + std::to_string(good) + "/50 decoded");
    q65::bpScratchFree(bp);
}

static double rate(int P, int sub, double snr, int trials, double* dtE, double* fE, double* sE) {
    q65::SlotDecoder d(P);
    q65::SlotOptions o; o.submodes = 1u << sub; o.apCq = false;
    const char* msgs[] = {"CQ K1ABC FN42", "G4ABC DL1AAA JO62", "W9XYZ K1ABC -15", "OH2BH VK2ABC RR73"};
    int got = 0; double de = 0, fe = 0, se = 0;
    for (int t = 0; t < trials; ++t) {
        uint8_t tones[q65::kSymbols]; q65::encodeText(msgs[t % 4], tones);
        auto w = q65synth::noiseWindow(P, 31u * (uint32_t)t + 9u + (uint32_t)(P * 7 + sub));
        q65synth::Sig s; s.tones = tones; s.submode = sub; s.f0 = 700 + 53.1 * (t % 9); s.dt = -0.3 + 0.21 * (t % 6); s.snrDb = snr;
        q65synth::addSignal(w, P, s, (uint32_t)t);
        std::vector<q65::Result> res; d.decode(w.data(), (int)w.size(), o, res);
        for (auto& r : res) if (r.text == msgs[t % 4]) { ++got; de = std::max(de, (double)std::fabs(r.dt - s.dt)); fe = std::max(fe, (double)std::fabs(r.f0 - s.f0)); se = std::max(se, std::fabs(r.snr - snr)); }
    }
    if (dtE) *dtE = de; if (fE) *fE = fe; if (sE) *sE = se;
    return (double)got / trials;
}

static void testSensitivity() {
    std::printf("\n── sensitivity (synthetic, blind) ──\n");
    double de, fe, se;
    double r = rate(60, 0, -26, 20, &de, &fe, &se);
    ok(r >= 0.9, "60A −26 dB: " + std::to_string((int)(r * 100)) + " %");
    r = rate(60, 0, -24, 12, &de, &fe, &se);
    ok(r == 1.0, "60A −24 dB: every one");
    ok(de < 0.05 && fe < 0.5 && se <= 2.0, "60A: |DT| " + std::to_string(de) + " s, |f| " + std::to_string(fe) + " Hz, |SNR| " + std::to_string(se) + " dB");
    ok(rate(30, 0, -22, 12, nullptr, nullptr, nullptr) == 1.0, "30A −22 dB: every one");
    ok(rate(15, 0, -19, 12, nullptr, nullptr, nullptr) == 1.0, "15A −19 dB: every one");
    for (int sub = 1; sub <= 3; ++sub) {
        r = rate(60, sub, -26, 10, nullptr, nullptr, nullptr);
        ok(r >= 0.8, std::string("60") + (char)('A' + sub) + " −26 dB: " + std::to_string((int)(r * 100)) + " %");
    }
}

static void testNoise() {
    std::printf("\n── pure noise ──\n");
    for (auto pn : {std::pair<int, int>{15, 300}, {30, 150}, {60, 100}}) {
        q65::SlotDecoder d(pn.first);
        q65::SlotOptions o;
        int n = 0;
        for (int i = 0; i < pn.second; ++i) {
            auto w = q65synth::noiseWindow(pn.first, 4242u + (uint32_t)i * 17u);
            std::vector<q65::Result> res; d.decode(w.data(), (int)w.size(), o, res);
            n += (int)res.size();
            for (auto& r : res) std::printf("     false: %s\n", r.text.c_str());
        }
        ok(n == 0, std::to_string(pn.first) + " s × " + std::to_string(pn.second) + " windows of noise: " + std::to_string(n) + " decodes");
    }
    // a carrier one tone above where a sync would be, plus noise
    q65::SlotDecoder d(60); q65::SlotOptions o;
    auto w = q65synth::noiseWindow(60, 99);
    for (size_t i = 0; i < w.size(); ++i) w[i] += 0.5f * (float)std::cos(6.283185307 * 1001.6667 * i / 12000.0);
    std::vector<q65::Result> res; d.decode(w.data(), (int)w.size(), o, res);
    ok(res.empty(), "a carrier: no decode");
}

static void testLive() {
    std::printf("\n── the live decoder ──\n");
    q65::LiveDecoder live({15, 30, 60});   // ★ the engine's periods (q65.h; 15 s added 2026-10-31)
    std::vector<q65::Result> got;
    live.onResult = [&](const q65::Result& r) { got.push_back(r); };
    const double t0 = 1800000000.0 - 20;     // 20 s before a minute
    const int total = 210 * 12000;
    std::vector<float> a(total);
    std::mt19937 rng(3); std::normal_distribution<float> g(0, 1);
    for (float& v : a) v = g(rng);
    // a 60A transmission in the minute starting t0+20 (window starts 1 s before), a 30A one in the half minute at t0+80
    uint8_t t60[q65::kSymbols], t30[q65::kSymbols], t15[q65::kSymbols];
    q65::encodeText("CQ K1ABC FN42", t60); q65::encodeText("G4ABC DL1AAA JO62", t30); q65::encodeText("CQ F5ABC JN18", t15);
    auto put = [&](const uint8_t* tones, int P, double periodStart, double f0) {
        std::vector<float> w((size_t)q65::windowSamples(*q65::periodFor(P)), 0.f);
        q65synth::Sig s; s.tones = tones; s.submode = 0; s.f0 = f0; s.dt = 0.2; s.snrDb = -18;
        q65synth::addSignal(w, P, s, 1);
        const long off = std::lround((periodStart - 1.0 - t0) * 12000);
        for (size_t i = 0; i < w.size(); ++i) if (off + (long)i >= 0 && off + (long)i < total) a[(size_t)(off + (long)i)] += w[i];
    };
    put(t60, 60, t0 + 20, 1200); put(t30, 30, t0 + 80, 2100); put(t15, 15, t0 + 125, 800);
    // ★ faster than real time, so wait for each decode (a real feed gives the worker the period itself)
    for (int i = 0; i < total; i += 960) { live.process(&a[(size_t)i], 960, t0 + (i + 960) / 12000.0); live.drain(); }
    live.drain();
    int n60 = 0, n30 = 0, n15 = 0;
    for (auto& r : got) {
        std::printf("     %s %s slot %+.0f s\n", r.mode().c_str(), r.text.c_str(), r.slotUtc - t0);
        if (r.text == "CQ K1ABC FN42" && r.mode() == "Q65-60A" && std::fabs(r.slotUtc - (t0 + 20)) < 0.01) ++n60;
        if (r.text == "G4ABC DL1AAA JO62" && r.mode() == "Q65-30A" && std::fabs(r.slotUtc - (t0 + 80)) < 0.01) ++n30;
        if (r.text == "CQ F5ABC JN18" && r.mode() == "Q65-15A" && std::fabs(r.slotUtc - (t0 + 125)) < 0.01) ++n15;
    }
    ok(n60 == 1 && n30 == 1 && n15 == 1, "one spot each, the right period and mode (" + std::to_string(n60) + ", " + std::to_string(n30) + ", " + std::to_string(n15) + ")");
    ok(got.size() == 3, "nothing else (" + std::to_string(got.size()) + ")");
}

int main() {
    std::printf("test-q65 — Q65, our own receiver\n");
    testFrame();
    testBp();
    testSensitivity();
    testNoise();
    testLive();
    std::printf("\n%s (%d failed)\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
