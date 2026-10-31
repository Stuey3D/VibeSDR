// test-paging.cpp — the PAGING engine (decoders/paging_decoder.h): POCSAG 512 / 1200 / 2400 from ITU-R M.584-2 Annex 1,
// on synthetic discriminator audio. V12, 2026-10-31. ★ Silent; deterministic. ★ No real paging traffic is used or kept
// anywhere (Stuart's rule): every page here is made up by this file.
//
// ★★★ WHAT IS PINNED.
//   • The code against the ITU's own tables: the SYNC (Table 1) and IDLE (Table 2) codewords are valid BCH(31,21) +
//     even-parity words under OUR generator — two independent 32-bit checks of the polynomial, the bit order and the
//     parity rule; every 1- and 2-bit error in random words is corrected, 3-bit errors are never passed as clean.
//   • Every rate decodes numeric, alphanumeric (a long one across batches) and tone-only pages, normal and inverted, at
//     12 kHz (the decoder host's rate) and 48 kHz, with ±0.5 % clock error and a DC offset; AUTO names the rate; a
//     MANUAL mode at the wrong rate decodes nothing.
//   • Nothing on noise: hours of white noise, and of random NRZ at each rate, give zero pages and no detection.
//   • Sensitivity (Eb/N0 on the discriminator, 50 %) and the cost, printed.
#include "decoders/paging_decoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace vibe;
using Mode = PagingDecoder::Mode;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

struct TxPage { uint32_t addr; int fn; std::string text; bool numeric; };

/** Message chunks (20 bits each, first transmitted bit = bit 19) per M.584-2 §2.1 / §2.2. */
static std::vector<uint32_t> chunksOf(const TxPage& p) {
    std::vector<int> bits;
    if (p.numeric) {
        for (char c : p.text) {
            int v = (c >= '0' && c <= '9') ? c - '0' : c == '*' ? 10 : c == 'U' ? 11 : c == ' ' ? 12 : c == '-' ? 13 : c == ']' ? 14 : 15;
            for (int b = 0; b < 4; ++b) bits.push_back((v >> b) & 1);
        }
        while (bits.size() % 20) { for (int b = 0; b < 4; ++b) bits.push_back((12 >> b) & 1); }   // space fill
    } else {
        for (char c : p.text) for (int b = 0; b < 7; ++b) bits.push_back(((unsigned char)c >> b) & 1);
        bits.push_back(0); bits.push_back(0); bits.push_back(1); bits.push_back(0); bits.push_back(0); bits.push_back(0); bits.push_back(0);   // EOT (0x04)
        while (bits.size() % 20) bits.push_back(0);   // NUL fill
    }
    std::vector<uint32_t> ch;
    for (size_t i = 0; i < bits.size(); i += 20) { uint32_t v = 0; for (int b = 0; b < 20; ++b) v = (v << 1) | (uint32_t)bits[i + (size_t)b]; ch.push_back(v); }
    return ch;
}

/** A whole transmission's bits: preamble (576 reversals), batches, idles; pages in their frames. */
static std::vector<int> transmission(const std::vector<TxPage>& pages) {
    std::vector<uint32_t> words;   // the codeword stream without syncs (16 per batch)
    for (const auto& p : pages) {
        while ((int)(words.size() % 16) / 2 != (int)(p.addr & 7u)) words.push_back(PagingDecoder::kIdle);
        words.push_back(PagingDecoder::encodeWord(((p.addr >> 3) << 2) | (uint32_t)p.fn));
        if (!p.text.empty()) for (uint32_t c : chunksOf(p)) words.push_back(PagingDecoder::encodeWord((1u << 20) | c));
    }
    words.push_back(PagingDecoder::kIdle);
    while (words.size() % 16) words.push_back(PagingDecoder::kIdle);
    std::vector<int> bits;
    for (int i = 0; i < 576; ++i) bits.push_back(i & 1 ? 0 : 1);
    for (size_t i = 0; i < words.size(); ++i) {
        if (i % 16 == 0) for (int b = 31; b >= 0; --b) bits.push_back((int)((PagingDecoder::kSync >> b) & 1u));
        for (int b = 31; b >= 0; --b) bits.push_back((int)((words[i] >> b) & 1u));
    }
    return bits;
}

/** Discriminator audio: NRZ ±1 (1 = +), smoothed (≈ the transmitter's filter), a DC offset, clock error, white noise
 *  at Eb/N0 (one-sided N0 = 2σ²/fs, Eb = 1/baud). `lead`/`tail` seconds of noise around it. */
static std::vector<float> modulate(const std::vector<int>& bits, int baud, double fs, double clockErr, bool invert,
                                   double ebn0Db, uint32_t seed, double dc = 0.3, double lead = 0.5, double tail = 0.5) {
    std::mt19937 rng(seed); std::normal_distribution<float> g(0.f, 1.f);
    const double sigma = std::isfinite(ebn0Db) ? std::sqrt(fs / (2.0 * baud * std::pow(10.0, ebn0Db / 10.0))) : 0.0;
    const double br = baud * (1.0 + clockErr);
    const int n = (int)((lead + tail) * fs + bits.size() * fs / br);
    std::vector<float> x((size_t)n);
    const double a = 1.0 - std::exp(-2 * M_PI * 0.6 * baud / fs);
    double y = 0;
    for (int i = 0; i < n; ++i) {
        const double t = i / fs - lead;
        double s = 0;
        if (t >= 0) { const size_t k = (size_t)(t * br); if (k < bits.size()) s = (bits[k] ^ (invert ? 1 : 0)) ? 1.0 : -1.0; }
        y += a * (s - y);
        x[(size_t)i] = (float)(y + dc + sigma * g(rng));
    }
    return x;
}

struct Got { std::vector<PagingDecoder::Page> pages; std::string detected; };
static Got run(const std::vector<float>& x, double fs, Mode m) {
    PagingDecoder d(fs, m); Got g;
    d.onPage = [&](const PagingDecoder::Page& p) { g.pages.push_back(p); };
    const int blk = (int)(fs * 0.02);
    for (size_t i = 0; i < x.size(); i += (size_t)blk) {
        d.feed(x.data() + i, (int)std::min<size_t>((size_t)blk, x.size() - i));
        const std::string s = d.detected(); if (!s.empty()) g.detected = s;
    }
    return g;
}

static const std::vector<TxPage>& testPages() {
    static const std::vector<TxPage> p = {
        {1234567, 0, "0123456789 U-][", true},
        {200005, 3, "HELLO TEST PAGE 42", false},
        {777, 1, "", false},
        {1500003, 3, "A long alphanumeric message that runs across more than one batch: the quick brown fox 0123456789.", false},
    };
    return p;
}
static bool allRight(const Got& g, int baud, bool inv) {
    const auto& P = testPages();
    if (g.pages.size() != P.size()) return false;
    for (size_t i = 0; i < P.size(); ++i) {
        const auto& r = g.pages[i];
        if (r.address != P[i].addr || r.function != P[i].fn || r.baud != baud || r.inverted != inv || r.truncated) return false;
        if (P[i].text.empty() ? r.kind != "tone" : (r.text != P[i].text || r.kind != (P[i].numeric ? "numeric" : "alpha"))) return false;
    }
    return true;
}

int main() {
    std::printf("test-paging — POCSAG (ITU-R M.584-2 Annex 1)\n\n── the code against the ITU's tables ──\n");
    {
        uint32_t s = PagingDecoder::kSync, i = PagingDecoder::kIdle;
        ok(PagingDecoder::correctWord(s) == 0 && s == PagingDecoder::kSync, "Table 1 SYNC 0x7CD215D8 is a clean codeword under x¹⁰+x⁹+x⁸+x⁶+x⁵+x³+1 + even parity");
        ok(PagingDecoder::correctWord(i) == 0 && i == PagingDecoder::kIdle, "Table 2 IDLE 0x7A89C197 is a clean codeword");
        ok(PagingDecoder::encodeWord(PagingDecoder::kIdle >> 11) == PagingDecoder::kIdle, "encodeWord rebuilds IDLE from its 21 information bits");
        std::mt19937 r(3); int c1 = 0, c2 = 0, n = 0, bad3 = 0;
        for (int k = 0; k < 3000; ++k) {
            const uint32_t w = PagingDecoder::encodeWord(r() & 0x1FFFFFu);
            const int a = (int)(r() % 32), b = (int)((a + 1 + r() % 31) % 32), c = (int)((b + 1 + r() % 30) % 32);
            uint32_t e1 = w ^ (1u << a), e2 = w ^ (1u << a) ^ (1u << b), e3 = w ^ (1u << a) ^ (1u << b) ^ (1u << (a == c || b == c ? (c + 1) % 32 : c));
            c1 += PagingDecoder::correctWord(e1) == 1 && e1 == w;
            c2 += PagingDecoder::correctWord(e2) == 2 && e2 == w;
            const int k3 = PagingDecoder::correctWord(e3); bad3 += (k3 >= 0 && k3 <= 1); ++n;
        }
        ok(c1 == n && c2 == n, "every 1- and 2-bit error corrected (" + std::to_string(c1) + ", " + std::to_string(c2) + " of " + std::to_string(n) + ")");
        ok(bad3 == 0, "a 3-bit error never passes as clean or one bit out (" + std::to_string(bad3) + ")");
    }

    std::printf("\n── every rate, both polarities, 12 and 48 kHz, ±0.5 %% clock, DC offset ──\n");
    const auto bits = transmission(testPages());
    for (double fs : {12000.0, 48000.0})
        for (int baud : {512, 1200, 2400})
            for (int inv = 0; inv < 2; ++inv) {
                const double ce = inv ? 0.005 : -0.005;
                auto x = modulate(bits, baud, fs, ce, inv != 0, 18.0, 10u + (uint32_t)baud + (uint32_t)inv);
                const Got g = run(x, fs, Mode::Auto);
                char w[160]; std::snprintf(w, sizeof w, "%5.0f Hz, POCSAG %4d %s: %zu pages, all right, AUTO says \"%s\"", fs, baud, inv ? "inverted" : "normal  ", g.pages.size(), g.detected.c_str());
                ok(allRight(g, baud, inv != 0) && g.detected == "POCSAG " + std::to_string(baud), w);
                if (!allRight(g, baud, inv != 0)) for (auto& p : g.pages) std::printf("      %u f%d %s %d '%s'%s\n", p.address, p.function, p.kind.c_str(), p.baud, p.text.c_str(), p.truncated ? " (truncated)" : "");
            }
    {
        auto x = modulate(bits, 1200, 12000, 0.002, false, 18.0, 77);
        ok(run(x, 12000, Mode::Pocsag1200).pages.size() == testPages().size(), "MANUAL POCSAG 1200 on 1200: decodes");
        ok(run(x, 12000, Mode::Pocsag2400).pages.empty() && run(x, 12000, Mode::Pocsag512).pages.empty(), "MANUAL 2400 / 512 on a 1200 signal: nothing");
    }

    std::printf("\n── sensitivity (20 transmissions a point, all four pages right) ──\n");
    for (int cfg = 0; cfg < 4; ++cfg) {
        const int baud = cfg == 0 ? 512 : cfg == 1 ? 1200 : 2400;
        const double fsS = cfg == 3 ? 24000.0 : 12000.0;
        std::printf("    POCSAG %4d @ %2.0f kHz:", baud, fsS / 1000);
        double fifty = NAN;
        for (double eb = 4; eb <= 12.01; eb += 1) {
            int right = 0;
            for (int k = 0; k < 20; ++k) {
                auto x = modulate(bits, baud, fsS, 0.003 * ((k % 3) - 1), k & 1, eb, 1000u + (uint32_t)k + (uint32_t)(eb * 31) + (uint32_t)baud);
                right += allRight(run(x, fsS, Mode::Auto), baud, k & 1);
            }
            std::printf(" %2.0f dB %2d", eb, right);
            if (std::isnan(fifty) && right >= 10) fifty = eb;
        }
        std::printf("   → 50 %% ≈ %.0f dB Eb/N0\n", fifty);
        const double lim = (baud == 2400 && fsS < 20000) ? 13 : 11;   // ★ 5 samples a bit at 12 kHz costs 2400 bit/s ≈ 3 dB
        ok(!std::isnan(fifty) && fifty <= lim, "POCSAG " + std::to_string(baud) + " reaches 50 % by " + std::to_string((int)lim) + " dB Eb/N0");
    }

    std::printf("\n── nothing where there is nothing ──\n");
    {
        const double fs = 12000; const int secs = 3600;
        PagingDecoder d(fs); int pages = 0; bool det = false;
        d.onPage = [&](const PagingDecoder::Page&) { ++pages; };
        std::mt19937 r(99); std::normal_distribution<float> g(0.f, 1.f);
        std::vector<float> x((size_t)fs);
        const auto t0 = std::chrono::steady_clock::now();
        for (int h = 0; h < 4; ++h)
            for (int s = 0; s < secs; ++s) {
                for (auto& v : x) v = g(r);
                d.feed(x.data(), (int)x.size());
                det |= !d.detected().empty();
            }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ok(pages == 0 && !det, "4 hours of white noise (AUTO, three rates): " + std::to_string(pages) + " pages, detected " + (det ? "yes" : "no"));
        std::printf("    cost (incl. the noise generator): %.3f ms per second of 12 kHz audio\n", ms / (4.0 * secs));
    }
    for (int baud : {512, 1200, 2400}) {
        // random NRZ at the rate (looks like paging bits, carries no sync) — the bit clock locks, nothing else may
        std::mt19937 r(5u + (uint32_t)baud);
        std::vector<int> bits2((size_t)baud * 600);
        for (auto& b : bits2) b = (int)(r() & 1u);
        auto x = modulate(bits2, baud, 12000, 0.001, false, 15.0, 6u + (uint32_t)baud);
        const Got g = run(x, 12000, Mode::Auto);
        ok(g.pages.empty() && g.detected.empty(), "10 minutes of random " + std::to_string(baud) + " bit/s NRZ: " + std::to_string(g.pages.size()) + " pages");
    }

    std::printf("\n%s (%d failed)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
