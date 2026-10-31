// test-tone-id.cpp — CTCSS / DCS tone identification (decoders/tone_id.h) on synthetic NFM IQ. V12, 2026-10-31.
//
// ★★★ WHAT IS PINNED.
//   • the DCS code: the published inverted-code equivalences (023N = 047I, 025 = 244, …) through our Golay words;
//   • every one of the 51 CTCSS tones locks, at ±600 Hz of deviation under ±3 kHz of voice-band audio, 15 dB CNR, within
//     4 s (the first analysis needs 2 s of audio, then three agreeing half-second looks), to the right tone; a 151.4 beside 150.0 is told apart; ±0.4 % transmitter error still the right tone;
//   • 16 DCS codes, normal and inverted, lock to the right code (or its equivalent reading, both reported) within 5 s;
//   • NOTHING locks on: 10 minutes of noise; 5 minutes of voice-band audio with no tone; a DCS signal does not read as a
//     CTCSS tone;
//   • the vector discriminator equals atan2 (≤ 1e-4 rad);
//   • the cost, printed.
// ★ Silent; deterministic.
#include "decoders/tone_id.h"
#include "decoders/digimodes/vmath4.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

static const double kFs = 24000;

/** NFM IQ: deviation(t) = voice + sub-audible; carrier + complex noise at `cnrDb` in 16 kHz. */
struct Nfm {
    std::mt19937 rng;
    std::normal_distribution<float> g{0.f, 1.f};
    double ph = 0, v1 = 0, v2 = 0, d1 = 0;
    double fs = kFs;                       // ★ the IQ rate (the voice/DCS filters are stated at 24 kHz and scaled)
    explicit Nfm(uint32_t seed, double rate = kFs) : rng(seed), fs(rate) {}
    std::vector<std::complex<float>> make(double sec, double cnrDb, bool voice, double toneHz, double toneDev,
                                          const std::vector<int>* dcsBits = nullptr, double dcsDev = 0) {
        const double kFs = fs;
        const double a1 = 1.0 - std::pow(1.0 - 0.55, 24000.0 / kFs), a2 = 1.0 - std::pow(1.0 - 0.08, 24000.0 / kFs);
        const double a3 = 1.0 - std::pow(1.0 - 0.06, 24000.0 / kFs);
        const double vScale = std::sqrt(kFs / 24000.0);   // the same voice-band level at any rate
        const int n = (int)(sec * kFs);
        std::vector<std::complex<float>> x((size_t)n);
        const double noiseAmp = std::sqrt(std::pow(10.0, -cnrDb / 10.0) * (kFs / 16000.0) / 2.0);
        double bitPh = 0; int bi = 0;
        for (int i = 0; i < n; ++i) {
            double dev = 0;
            if (voice) {   // voice-band: noise through a crude 300–3000 Hz band-pass, ≈ ±3 kHz peaks
                const double w = g(rng) * vScale;
                v1 += a1 * (w - v1);              // low-pass ≈ 3 kHz
                const double hp = v1 - v2; v2 += a2 * (v1 - v2);     // minus its own ≈ 300 Hz low-pass
                dev += 2600.0 * hp;
            }
            if (toneHz > 0) dev += toneDev * std::sin(2 * M_PI * toneHz * i / kFs);
            if (dcsBits) {
                const double target = (*dcsBits)[(size_t)(bi % dcsBits->size())] ? dcsDev : -dcsDev;
                d1 += a3 * (target - d1);         // ≈ 230 Hz smoothing (the DCS filter)
                dev += d1;
                bitPh += 134.4 / kFs; if (bitPh >= 1) { bitPh -= 1; ++bi; }
            }
            ph += 2 * M_PI * dev / kFs;
            x[(size_t)i] = std::complex<float>((float)std::cos(ph) + (float)(noiseAmp * g(rng)), (float)std::sin(ph) + (float)(noiseAmp * g(rng)));
        }
        return x;
    }
};

static std::vector<int> dcsBitsOf(int code, bool inverted) {
    const uint32_t w = ToneId::dcsWord(code);
    std::vector<int> b(23);
    for (int i = 0; i < 23; ++i) b[(size_t)i] = (int)((w >> i) & 1u) ^ (inverted ? 1 : 0);
    return b;
}

/** Feed in 20 ms blocks; returns the first time (s) the state's label equals `want` (−1 = never), and the final label. */
static double lockTime(ToneId& t, const std::vector<std::complex<float>>& x, const std::string& want, std::string* last = nullptr) {
    double when = -1;
    const int blk = (int)(kFs * 0.02);
    for (size_t i = 0; i < x.size(); i += (size_t)blk) {
        const int n = (int)std::min<size_t>((size_t)blk, x.size() - i);
        t.feedIq(x.data() + i, n);
        if (when < 0 && !want.empty() && t.state().label() == want) when = (double)(i + n) / kFs;
    }
    if (last) *last = t.state().label();
    return when;
}

/** ★ `--write-clip FILE`: the end-to-end clip for scripts/test-server-tone.mjs — unsigned 8-bit IQ at 960 kS/s, 20 s,
 *  three NFM carriers round the clip's centre: +25 kHz CTCSS 88.5 Hz under voice, −50 kHz DCS 023N under voice,
 *  +150 kHz a plain carrier with voice and NO tone (it must stay unlabelled). Each made at 48 kHz and interpolated up. */
static int writeClip(const char* path) {
    const double R = 960000, sec = 20, sub = 48000;
    const int up = (int)(R / sub), n = (int)(sec * R);
    struct St { double off; Nfm s; std::vector<std::complex<float>> x; };
    auto bits = dcsBitsOf(23, false);
    St st[3] = {{25000, Nfm(11, sub), {}}, {-50000, Nfm(12, sub), {}}, {150000, Nfm(13, sub), {}}};
    st[0].x = st[0].s.make(sec, 60, true, 88.5, 600);
    st[1].x = st[1].s.make(sec, 60, true, 0, 0, &bits, 650);
    st[2].x = st[2].s.make(sec, 60, true, 0, 0);
    std::mt19937 rng(77); std::normal_distribution<float> g(0.f, 1.f);
    std::vector<uint8_t> out((size_t)n * 2);
    for (int i = 0; i < n; ++i) {
        std::complex<double> z(0.01 * g(rng), 0.01 * g(rng));
        for (auto& k : st) {
            const size_t j = (size_t)(i / up); const double f = (double)(i % up) / up;
            const std::complex<float> a = k.x[j], b = k.x[std::min(j + 1, k.x.size() - 1)];
            std::complex<double> c = std::complex<double>(a) * (1 - f) + std::complex<double>(b) * f;
            c /= std::max(1e-9, std::abs(c));
            const double ph = 2 * M_PI * k.off * i / R;
            z += 0.25 * c * std::complex<double>(std::cos(ph), std::sin(ph));
        }
        auto q = [](double v) { const long t = std::lround(127.5 + 127.0 * v); return (uint8_t)std::max(0L, std::min(255L, t)); };
        out[(size_t)i * 2] = q(z.real()); out[(size_t)i * 2 + 1] = q(z.imag());
    }
    FILE* f = std::fopen(path, "wb");
    if (!f) return 1;
    std::fwrite(out.data(), 1, out.size(), f); std::fclose(f);
    std::printf("wrote %s (%d samples at 960 kS/s)\n", path, n);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--write-clip") return writeClip(argv[2]);
    std::printf("test-tone-id — CTCSS / DCS identification\n\n── the vector discriminator ──\n");
    {
        double worst = 0;
        std::mt19937 r(5); std::uniform_real_distribution<float> u(-3.f, 3.f);
        for (int k = 0; k < 4000; ++k) {
            float ys[4], xs[4], o[4];
            for (int q = 0; q < 4; ++q) { ys[q] = u(r); xs[q] = u(r); }
            if (k == 0) { xs[0] = -1; ys[0] = 0; xs[1] = 0; ys[1] = 1; xs[2] = 0; ys[2] = -1; xs[3] = 1; ys[3] = 0; }
            q65v::st(o, q65v::vatan2(q65v::ld(ys), q65v::ld(xs)));
            for (int q = 0; q < 4; ++q) { double d = std::fabs(o[q] - std::atan2(ys[q], xs[q])); if (d > M_PI) d = 2 * M_PI - d; worst = std::max(worst, d); }
        }
        ok(worst < 1e-4, "vatan2 within 1e-4 rad of atan2 (worst " + std::to_string(worst) + ")");
    }

    std::printf("\n── DCS: the published inverted-code equivalences ──\n");
    {
        const int pairs[][2] = {{23, 47}, {25, 244}, {26, 464}, {31, 627}, {32, 51}, {43, 445}, {54, 413}, {65, 271}, {71, 306}, {72, 245}, {73, 506}, {74, 174}};
        int good = 0;
        for (auto& pr : pairs) {
            ToneId t(kFs);
            Nfm s(100u + (uint32_t)pr[0]);
            auto bits = dcsBitsOf(pr[0], true);   // code A sent INVERTED
            auto x = s.make(5.0, 20, false, 0, 0, &bits, 650);
            std::string last; lockTime(t, x, "", &last);
            const ToneId::State st = t.state();
            good += st.dcs && ((st.dcsCode == pr[1] && !st.dcsInverted) || (st.dcsCode == pr[0] && st.dcsInverted));
        }
        ok(good == 12, "12 published pairs (A inverted reads as B normal): " + std::to_string(good) + "/12");
    }

    std::printf("\n── CTCSS: every standard tone, under voice ──\n");
    {
        const auto& T = ToneId::ctcssTones();
        int right = 0, wrong = 0; double worstT = 0;
        for (size_t j = 0; j < T.size(); ++j) {
            ToneId t(kFs);
            Nfm s(7u + (uint32_t)j);
            auto x = s.make(4.5, 15, true, T[j] * (1.0 + 0.004 * ((int)(j % 3) - 1)), 600);
            char want[32]; std::snprintf(want, sizeof want, "CTCSS %.1f Hz", T[j]);
            std::string last;
            const double w = lockTime(t, x, want, &last);
            if (w > 0 && w <= 4.0 && last == want) { ++right; worstT = std::max(worstT, w); }
            else { ++wrong; std::printf("    %s → '%s' at %.2f s\n", want, last.c_str(), w); }
        }
        ok(right == (int)T.size(), std::to_string(right) + "/" + std::to_string(T.size()) + " tones right within 4 s (±0.4 % transmitter error, 15 dB CNR, voice on)");
        std::printf("    slowest lock %.2f s\n", worstT);
    }
    {
        ToneId t(kFs); Nfm s(99);
        auto x = s.make(4.0, 10, true, 151.4, 500);
        std::string last; lockTime(t, x, "", &last);
        ok(last == "CTCSS 151.4 Hz", "151.4 beside 150.0, 10 dB CNR: '" + last + "'");
    }

    std::printf("\n── at the rates a listener's channel runs at (the shim feeds the pipeline's channel IQ) ──\n");
    {
        int good = 0, all = 0;
        for (double r : {12000.0, 16000.0, 31250.0, 37500.0, 48000.0, 62500.0, 93750.0}) {
            ToneId t(r); Nfm s(4242u + (uint32_t)r, r);
            auto x = s.make(4.5, 15, true, 123.0, 600);
            const int blk = (int)(r * 0.02); std::string last;
            for (size_t i = 0; i < x.size(); i += (size_t)blk) t.feedIq(x.data() + i, (int)std::min<size_t>((size_t)blk, x.size() - i));
            last = t.state().label();
            ToneId t2(r); Nfm s2(5151u + (uint32_t)r, r);
            auto bits = dcsBitsOf(205, false);
            auto x2 = s2.make(5.0, 15, true, 0, 0, &bits, 650);
            for (size_t i = 0; i < x2.size(); i += (size_t)blk) t2.feedIq(x2.data() + i, (int)std::min<size_t>((size_t)blk, x2.size() - i));
            const ToneId::State d = t2.state();
            all += 2; good += (last == "CTCSS 123.0 Hz") + (d.dcs && d.dcsCode == 205 && !d.dcsInverted);
            if (last != "CTCSS 123.0 Hz" || !(d.dcs && d.dcsCode == 205)) std::printf("    %.0f Hz: '%s' / '%s'\n", r, last.c_str(), d.label().c_str());
        }
        ok(good == all, std::to_string(good) + "/" + std::to_string(all) + " right at 12 / 16 / 31.25 / 37.5 / 48 / 62.5 / 93.75 kHz");
    }

    std::printf("\n── DCS: 16 codes, normal and inverted ──\n");
    {
        const int codes[] = {23, 51, 114, 152, 205, 243, 266, 311, 365, 411, 465, 532, 606, 654, 712, 754};
        int right = 0;
        for (int ci = 0; ci < 16; ++ci)
            for (int inv = 0; inv < 2; ++inv) {
                ToneId t(kFs); Nfm s(300u + (uint32_t)ci * 2 + (uint32_t)inv);
                auto bits = dcsBitsOf(codes[ci], inv != 0);
                auto x = s.make(5.0, 15, true, 0, 0, &bits, 650);
                std::string last; lockTime(t, x, "", &last);
                const ToneId::State st = t.state();
                // ★ an inverted code IS another code's normal (023I = 047N): either reading is right, as long as both are said
                const bool r = st.dcs && ((st.dcsCode == codes[ci] && st.dcsInverted == (inv != 0)) || (st.dcsAlt == codes[ci] && st.dcsInverted != (inv != 0)));
                right += r;
                if (!r) std::printf("    %03d%c → '%s'\n", codes[ci], inv ? 'I' : 'N', last.c_str());
            }
        ok(right == 32, std::to_string(right) + "/32 DCS codes right within 5 s (voice on, 15 dB CNR)");
    }

    std::printf("\n── how weak (informational: 10 tones / 10 codes a point, voice on, 6 s) ──\n");
    for (double cnr : {9.0, 6.0, 3.0, 0.0}) {
        int c = 0, d = 0;
        for (int k = 0; k < 10; ++k) {
            const float tone = ToneId::ctcssTones()[(size_t)(k * 5)];
            ToneId t(kFs); Nfm s(900u + (uint32_t)k + (uint32_t)(cnr * 10));
            auto x = s.make(6.0, cnr, true, tone, 600);
            std::string last; lockTime(t, x, "", &last);
            char want[32]; std::snprintf(want, sizeof want, "CTCSS %.1f Hz", tone);
            c += last == want;
            const int codes[] = {23, 114, 205, 311, 411, 532, 654, 754, 152, 266};
            ToneId t2(kFs); Nfm s2(1900u + (uint32_t)k + (uint32_t)(cnr * 10));
            auto bits = dcsBitsOf(codes[k], false);
            auto x2 = s2.make(6.0, cnr, true, 0, 0, &bits, 650);
            lockTime(t2, x2, "");
            const ToneId::State st = t2.state();
            d += st.dcs && st.dcsCode == codes[k] && !st.dcsInverted;
        }
        std::printf("    CNR %4.1f dB (16 kHz): CTCSS %2d/10, DCS %2d/10\n", cnr, c, d);
    }

    std::printf("\n── nothing where there is nothing ──\n");
    {
        ToneId t(kFs); Nfm s(1234); int changes = 0;
        t.onChange = [&](const ToneId::State& st) { if (!st.label().empty()) ++changes; };
        for (int k = 0; k < 60; ++k) { auto x = s.make(10.0, 3, false, 0, 0); lockTime(t, x, ""); }
        ok(changes == 0, "10 minutes of noise (3 dB CNR): no lock (" + std::to_string(changes) + ")");
    }
    {
        ToneId t(kFs); Nfm s(4321); int changes = 0;
        t.onChange = [&](const ToneId::State& st) { if (!st.label().empty()) ++changes; };
        for (int k = 0; k < 30; ++k) { auto x = s.make(10.0, 20, true, 0, 0); lockTime(t, x, ""); }
        ok(changes == 0, "5 minutes of voice with no tone: no lock (" + std::to_string(changes) + ")");
        std::printf("    cost: %.2f ms per second of 24 kHz IQ\n", t.ms / 300.0);
    }
    {
        int ctcss = 0;
        for (int ci = 0; ci < 8; ++ci) {
            const int codes[] = {23, 114, 205, 311, 411, 532, 654, 754};
            ToneId t(kFs); Nfm s(77u + (uint32_t)ci);
            t.onChange = [&](const ToneId::State& st) { ctcss += st.ctcss; };
            auto bits = dcsBitsOf(codes[ci], false);
            auto x = s.make(8.0, 25, false, 0, 0, &bits, 650);
            lockTime(t, x, "");
        }
        ok(ctcss == 0, "8 DCS signals never read as a CTCSS tone (" + std::to_string(ctcss) + ")");
    }
    std::printf("\n%s (%d failed)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
