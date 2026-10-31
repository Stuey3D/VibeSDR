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
//   • THE WIRED PATH (PagingRun — what the Android host runs): FM IQ at the channel rates the pipeline gives NFM
//     (20, 25, 31.25, 48 kHz), ±4.5 kHz deviation, AWGN filtered to the passband as the channel filter does; every rate
//     decodes; the line format and the status JSON; the modes list as data; minutes of channel noise → 0 pages;
//     a fingerprint of the discriminator + the lines, equal in a VIBE_FORCE_SCALAR build (printed — compare by hand).
#include "decoders/paging_decoder.h"
#include "vibe_paging_run.h"   // ★ the wiring's run: own discriminator on the channel IQ, the lines and the status
#include "../tools/paging-bench/flex_synth.h"   // ★ our own FLEX transmitter (synthetic pages only)
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <fstream>

#include <chrono>
#include <complex>
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


// ── the wired path: FM IQ like the pipeline's channel IQ ─────────────────────────────────────────────────────────────
/** POCSAG as FM IQ at `fs`: NRZ (1 = +dev) smoothed, phase-integrated; complex AWGN at Eb/N0 (C = 1, N0 = 2σ²/fs per
 *  dimension pair), then signal + noise through a windowed-sinc low-pass of `passHz` (the channel filter's job). */
static std::vector<std::complex<float>> fmIq(const std::vector<int>& bits, int baud, double fs, double devHz, double clockErr,
                                             bool invert, double ebn0Db, uint32_t seed, double passHz, double lead = 0.5, double tail = 0.5) {
    std::mt19937 rng(seed); std::normal_distribution<float> g(0.f, 1.f);
    const double sigma = std::isfinite(ebn0Db) ? std::sqrt(fs / (2.0 * baud * std::pow(10.0, ebn0Db / 10.0))) : 0.0;
    const double br = baud * (1.0 + clockErr);
    const int n = (int)((lead + tail) * fs + bits.size() * fs / br);
    const double a = 1.0 - std::exp(-2 * M_PI * 0.6 * baud / fs);
    std::vector<std::complex<float>> x((size_t)n);
    double y = 0, ph = 0.7;
    for (int i = 0; i < n; ++i) {
        const double t = i / fs - lead;
        double s = 0;
        if (t >= 0) { const size_t k = (size_t)(t * br); if (k < bits.size()) s = (bits[k] ^ (invert ? 1 : 0)) ? 1.0 : -1.0; }
        y += a * (s - y);
        ph += 2 * M_PI * devHz * y / fs;
        x[(size_t)i] = std::complex<float>((float)(std::cos(ph) + sigma * g(rng)), (float)(std::sin(ph) + sigma * g(rng)));
    }
    // the channel filter: 63-tap Hamming-windowed sinc, cut-off passHz/2
    const int T = 63; std::vector<float> h(T); double hs = 0;
    for (int k = 0; k < T; ++k) {
        const double m = k - (T - 1) / 2.0, fc = 0.5 * passHz / fs;
        const double v = (m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m)) * (0.54 - 0.46 * std::cos(2 * M_PI * k / (T - 1)));
        h[(size_t)k] = (float)v; hs += v;
    }
    for (auto& v : h) v = (float)(v / hs);
    std::vector<std::complex<float>> o((size_t)n);
    for (int i = 0; i < n; ++i) {
        std::complex<float> acc(0, 0);
        for (int k = 0; k < T; ++k) { const int j = i - k; if (j >= 0) acc += h[(size_t)k] * x[(size_t)j]; }
        o[(size_t)i] = acc;
    }
    return o;
}
struct RunGot { std::vector<std::string> lines; std::vector<std::string> json; std::vector<int> states; uint64_t fp = 1469598103934665603ull, fpLines = 1469598103934665603ull; };
static void fpMix(uint64_t& h, const void* d, size_t n) { const uint8_t* p = (const uint8_t*)d; for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; } }
static RunGot runIq(const std::vector<std::complex<float>>& x, double fs, PagingDecoder::Mode m) {
    RunGot g;
    PagingRun::Out o;
    o.text = [&](const std::string& t) { g.lines.push_back(t); fpMix(g.fp, t.data(), t.size()); fpMix(g.fpLines, t.data(), t.size()); };
    o.json = [&](const std::string& j) { g.json.push_back(j); };
    o.state = [&](uint8_t s) { g.states.push_back(s); };
    PagingRun r(m, std::move(o));
    const int blk = (int)(fs * 0.02);
    std::complex<float> last{1.f, 0.f};
    std::vector<float> d((size_t)blk);
    for (size_t i = 0; i < x.size(); i += (size_t)blk) {
        const int k = (int)std::min<size_t>((size_t)blk, x.size() - i);
        r.feedIq(x.data() + i, k, fs);
        PagingRun::discriminate(x.data() + i, k, last, d.data());   // ★ the same discriminator again, for the fingerprint
        fpMix(g.fp, d.data(), (size_t)k * sizeof(float));
    }
    return g;
}
static bool linesRight(const RunGot& g, int baud) {
    const auto& P = testPages();
    if (g.lines.size() != P.size()) return false;
    for (size_t i = 0; i < P.size(); ++i) {
        PagingDecoder::Page p; p.proto = "POCSAG"; p.baud = baud; p.address = P[i].addr; p.function = P[i].fn;
        p.kind = P[i].text.empty() ? "tone" : P[i].numeric ? "numeric" : "alpha"; p.text = P[i].text;
        if (g.lines[i] != PagingRun::line(p)) return false;
    }
    return true;
}

static uint64_t wiredFingerprint(uint64_t* linesOnly = nullptr);

// ── FLEX ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct FlexSpeedT { int bps, levels; std::vector<int> phases; };
static const std::vector<FlexSpeedT>& flexSpeeds() {
    static const std::vector<FlexSpeedT> v = { {1600, 2, {0}}, {3200, 2, {0, 2}}, {3200, 4, {0, 2}}, {6400, 4, {0, 1, 2, 3}} };
    return v;
}
/** Synthetic pages for a speed: on every phase it has an alpha page (one long-addressed), a numeric and a tone. */
static std::vector<flexsyn::TxPage> flexPages(const FlexSpeedT& sp) {
    std::vector<flexsyn::TxPage> v;
    for (int p : sp.phases) {
        flexsyn::TxPage a; a.cap = 100000u + (uint32_t)p * 1111u; a.kind = "alpha"; a.text = "TEST PAGE " + std::string(1, (char)('A' + p)) + " 0123 ok?"; a.phase = p; v.push_back(a);
        flexsyn::TxPage n; n.cap = 200000u + (uint32_t)p; n.kind = "numeric"; n.text = "5551234-9"; n.phase = p; v.push_back(n);
        flexsyn::TxPage t; t.cap = 300000u + (uint32_t)p; t.kind = "tone"; t.phase = p; v.push_back(t);
        flexsyn::TxPage l; l.cap = 2500000u + (uint32_t)p * 7u; l.lng = true; l.kind = "alpha"; l.text = "LONG ADDRESS " + std::to_string(p); l.phase = p; v.push_back(l);
    }
    return v;
}
static std::string flexLine(const FlexSpeedT& sp, const flexsyn::TxPage& t) {
    PagingDecoder::Page p; p.proto = "FLEX"; p.speed = std::to_string(sp.bps) + "/" + std::to_string(sp.levels); p.address = t.cap;
    p.phase = (char)('A' + t.phase); p.kind = t.kind; p.text = t.kind == "tone" ? "" : t.text;
    return PagingRun::line(p);
}
static std::vector<std::complex<float>> flexIq(const FlexSpeedT& sp, int frames, double fs, double clk, bool inv, double cnr, uint32_t seed,
                                               const std::vector<flexsyn::TxPage>* pages = nullptr) {
    std::vector<flexsyn::Sym> s;
    const auto P = pages ? *pages : flexPages(sp);
    for (int f = 0; f < frames; ++f) { auto fr = flexsyn::frame(sp.bps, sp.levels, 3, 40 + f, f == 0 ? P : std::vector<flexsyn::TxPage>{}); s.insert(s.end(), fr.begin(), fr.end()); }
    return flexsyn::fmIq(s, fs, clk, inv, cnr, seed, std::min(16000.0, fs * 0.8));
}
/** Every expected line once, nothing else. */
static bool flexAllRight(const RunGot& g, const FlexSpeedT& sp, std::string* why = nullptr) {
    const auto P = flexPages(sp);
    int hit = 0;
    for (auto& t : P) { const std::string want = flexLine(sp, t); int c = 0; for (auto& l : g.lines) c += l == want; hit += c == 1; if (c != 1 && why) *why += " missing: " + want; }
    if (why && g.lines.size() != P.size()) for (auto& l : g.lines) *why += " got: " + l;
    return hit == (int)P.size() && g.lines.size() == P.size();
}

static void testFlex() {
    std::printf("\n── FLEX: the code helpers ──\n");
    {
        int okc = 0;
        for (uint32_t v : {0x000000u, 0x1FFFF0u, 0x123450u, 0x0ABCD0u}) okc += PagingDecoder::flexChecksumOk(PagingDecoder::flexWithChecksum(v));
        ok(okc == 4, "the 4-bit checksum (M.584-2 Annex 2): the five nibbles and i20 sum to 0xF");
        ok(!PagingDecoder::flexChecksumOk(PagingDecoder::flexWithChecksum(0x123450u) ^ 0x10u), "a flipped field bit fails it");
        ok(PagingDecoder::flexInfo(1u) == (1u << 20) && PagingDecoder::flexInfo(1u << 20) == 1u, "fields go least significant bit first: i0 is the word's first information bit");
        ok(PagingDecoder::flexModeCode(1600, 2) == 0x870C && PagingDecoder::flexModeCode(6400, 4) == 0xDEA0, "the A word's mode codes for 1600/2 and 6400/4 (both confirmed outside the GPL source)");
    }
    std::printf("\n── FLEX: every speed, every phase, alpha (short + long address), numeric, tone ──\n");
    for (const auto& sp : flexSpeeds())
        for (double fs : {25000.0, 31250.0, 48000.0})
            for (bool inv : {false, true}) {
                const double clk = inv ? 0.0002 : -0.0002;
                auto x = flexIq(sp, 2, fs, clk, inv, 30.0, 7u + (uint32_t)fs + (uint32_t)sp.bps + (inv ? 1u : 0u));
                RunGot g = runIq(x, fs, Mode::Auto);
                std::string why; const bool good = flexAllRight(g, sp, &why);
                bool named = false; for (auto& j : g.json) named |= j.find("\"detected\":\"FLEX " + std::to_string(sp.bps) + "/" + std::to_string(sp.levels) + "\"") != std::string::npos;
                ok(good && named, "FLEX " + std::to_string(sp.bps) + "/" + std::to_string(sp.levels) + " @ " + std::to_string((int)fs) + " Hz" + (inv ? " inverted" : "") + ": all "
                   + std::to_string(flexPages(sp).size()) + " pages, AUTO names it" + (good ? "" : why));
            }
    std::printf("\n── FLEX: manual modes ──\n");
    {
        const auto& sp = flexSpeeds()[3];
        auto x = flexIq(sp, 2, 31250, 0, false, 30.0, 5);
        RunGot f = runIq(x, 31250, Mode::Flex), pc = runIq(x, 31250, Mode::Pocsag);
        ok(flexAllRight(f, sp), "FLEX (manual) decodes FLEX");
        ok(pc.lines.empty(), "POCSAG (manual, every rate) prints nothing on FLEX (" + std::to_string(pc.lines.size()) + ")");
        const auto bits = transmission(testPages());
        auto y = fmIq(bits, 1200, 31250, 4500, 0.0, false, 24.0, 9, 16000);
        RunGot fl = runIq(y, 31250, Mode::Flex), po = runIq(y, 31250, Mode::Pocsag);
        ok(fl.lines.empty(), "FLEX (manual) prints nothing on POCSAG (" + std::to_string(fl.lines.size()) + ")");
        ok(linesRight(po, 1200), "POCSAG (any rate) decodes POCSAG 1200");
    }
    std::printf("\n── FLEX: a channel carrying both (POCSAG, then FLEX, then POCSAG; AUTO) ──\n");
    {
        const double fs = 31250;
        const auto bits = transmission(testPages());
        auto a = fmIq(bits, 1200, fs, 4800, 0.0, false, 24.0, 11, 16000);
        auto b = flexIq(flexSpeeds()[2], 2, fs, 0.0, false, 30.0, 12);
        auto c = fmIq(bits, 2400, fs, 4800, 0.0, false, 24.0, 13, 16000);
        std::vector<std::complex<float>> x = a; x.insert(x.end(), b.begin(), b.end()); x.insert(x.end(), c.begin(), c.end());
        RunGot g = runIq(x, fs, Mode::Auto);
        int pocsag = 0, flex = 0; for (auto& l : g.lines) { pocsag += l.rfind("[POCSAG", 0) == 0; flex += l.rfind("[FLEX", 0) == 0; }
        bool n1 = false, n2 = false, n3 = false;
        for (auto& j : g.json) { n1 |= j.find("POCSAG 1200") != std::string::npos; n2 |= j.find("FLEX 3200/4") != std::string::npos; n3 |= j.find("POCSAG 2400") != std::string::npos; }
        ok(pocsag == 2 * (int)testPages().size() && flex == (int)flexPages(flexSpeeds()[2]).size() && n1 && n2 && n3,
           "both protocols on one channel: " + std::to_string(pocsag) + " POCSAG + " + std::to_string(flex) + " FLEX pages, each named in turn");
    }
    std::printf("\n── FLEX: a fragmented alphanumeric message across two frames ──\n");
    {
        const auto& sp = flexSpeeds()[0];
        std::vector<flexsyn::Sym> s;
        flexsyn::TxPage p1; p1.cap = 123456; p1.kind = "alpha"; p1.text = "FIRST HALF / "; p1.cont = true; p1.frag = 3; p1.msgNum = 9;
        flexsyn::TxPage p2 = p1; p2.text = "SECOND HALF"; p2.cont = false; p2.frag = 0;
        auto f1 = flexsyn::frame(1600, 2, 1, 10, {p1}), f2 = flexsyn::frame(1600, 2, 1, 11, {p2});
        s = f1; s.insert(s.end(), f2.begin(), f2.end());
        auto x = flexsyn::fmIq(s, 31250, 0, false, 30.0, 21, 16000);
        RunGot g = runIq(x, 31250, Mode::Auto);
        flexsyn::TxPage whole = p1; whole.text = "FIRST HALF / SECOND HALF";
        ok(g.lines.size() == 1 && g.lines[0] == flexLine(sp, whole), "one page, the two fragments joined: " + (g.lines.empty() ? std::string("(none)") : g.lines[0].substr(0, g.lines[0].size() - 1)));
    }
    std::printf("\n── FLEX: sensitivity (CNR in the 12.5 kHz channel, 10 transmissions a point, every page right) ──\n");
    for (const auto& sp : flexSpeeds()) {
        double fifty = NAN;
        std::string row;
        for (double cnr = 0; cnr <= 24; cnr += 2) {
            int good = 0;
            for (int t = 0; t < 10; ++t) { auto x = flexIq(sp, 1, 31250, 0.0001, false, cnr + 10 * std::log10(16000.0 / 12500.0), 100u + (uint32_t)t * 7u + (uint32_t)cnr); good += flexAllRight(runIq(x, 31250, Mode::Auto), sp); }
            row += " " + std::to_string((int)cnr) + ":" + std::to_string(good);
            if (std::isnan(fifty) && good >= 5) fifty = cnr;
            if (good == 10) break;
        }
        std::printf("    FLEX %d/%d —%s → 50 %% ≈ %.0f dB CNR\n", sp.bps, sp.levels, row.c_str(), fifty);
        const double lim = sp.levels == 2 ? 16 : 22;
        ok(!std::isnan(fifty) && fifty <= lim, "FLEX " + std::to_string(sp.bps) + "/" + std::to_string(sp.levels) + " reaches 50 % by " + std::to_string((int)lim) + " dB CNR");
    }
    std::printf("\n── FLEX: a public recording (VIBE_FLEX_IQ=… a 16-bit stereo IQ WAV; never in git; content NOT printed) ──\n");
    if (const char* path = std::getenv("VIBE_FLEX_IQ")) {
        std::ifstream f(path, std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (raw.size() > 44) {
            uint32_t rate = 0; std::memcpy(&rate, raw.data() + 24, 4);
            size_t off = 12; size_t dataAt = 0, dataLen = 0;
            while (off + 8 <= raw.size()) { uint32_t len; std::memcpy(&len, raw.data() + off + 4, 4); if (!std::memcmp(raw.data() + off, "data", 4)) { dataAt = off + 8; dataLen = std::min<size_t>(len, raw.size() - off - 8); break; } off += 8 + len; }
            std::vector<std::complex<float>> x(dataLen / 4);
            for (size_t i = 0; i < x.size(); ++i) { int16_t a, b; std::memcpy(&a, raw.data() + dataAt + 4 * i, 2); std::memcpy(&b, raw.data() + dataAt + 4 * i + 2, 2); x[i] = {a / 32768.f, b / 32768.f}; }
            RunGot g = runIq(x, rate, Mode::Auto);
            std::string det; for (auto& j : g.json) { auto p = j.find("\"detected\":\"FLEX"); if (p != std::string::npos) det = j.substr(p + 12, 11); }
            int alpha = 0; for (auto& l : g.lines) alpha += l.find("(tone only)") == std::string::npos;
            std::printf("    %u Hz, %.1f s: %zu pages (%d with content), detected %s — content withheld (real traffic)\n", rate, x.size() / (double)rate, g.lines.size(), alpha, det.c_str());
            ok(!g.lines.empty() && !det.empty(), "the public recording decodes and is named");
        }
    } else std::printf("    (not set — skipped)\n");
    std::printf("\n── FLEX: nothing where there is nothing ──\n");
    {
        const double fs = 31250; const int secs = 600;
        std::mt19937 r(77); std::normal_distribution<float> g(0.f, 1.f);
        RunGot got; PagingRun::Out o; o.text = [&](const std::string& t) { got.lines.push_back(t); };
        PagingRun run(Mode::Flex, std::move(o));
        std::vector<std::complex<float>> x((size_t)fs); bool det = false;
        for (int s = 0; s < secs; ++s) { for (auto& v : x) v = std::complex<float>(g(r), g(r)); run.feedIq(x.data(), (int)x.size(), fs); det |= !run.detected().empty(); }
        ok(got.lines.empty() && !det, "10 minutes of channel noise (FLEX alone): " + std::to_string(got.lines.size()) + " pages, detected " + (det ? "yes" : "no"));
        // random 4-level symbols at 3200 baud (FLEX-like, never a sync): nothing
        std::vector<flexsyn::Sym> s; std::mt19937 rr(5);
        for (int k = 0; k < 3200 * 300; ++k) { const int q = (int)(rr() & 3); s.push_back({q == 0 ? -1.f : q == 1 ? -1.f / 3 : q == 2 ? 1.f / 3 : 1.f, 1.0 / 3200}); }
        RunGot rg = runIq(flexsyn::fmIq(s, fs, 0, false, 30.0, 3, 16000), fs, Mode::Auto);
        ok(rg.lines.empty(), "5 minutes of random 4-level symbols at 3200 baud (AUTO): " + std::to_string(rg.lines.size()) + " pages");
    }
}

int main(int argc, char** argv) {
    // ★ --fingerprint: only the wired path's fingerprint (for the NEON / SSE2 / scalar identity runs under qemu)
    if (argc > 1 && std::string(argv[1]) == "--flex") { testFlex(); std::printf("\n%s (%d failed)\n", fails ? "FAILED" : "ALL PASS", fails); return fails ? 1 : 0; }   // ★ FLEX only
    if (argc > 1 && std::string(argv[1]) == "--fingerprint") { uint64_t l = 0; const uint64_t f = wiredFingerprint(&l); std::printf("fingerprint %016llx  lines %016llx\n", (unsigned long long)f, (unsigned long long)l); return 0; }
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


    std::printf("\n── the wired path: PagingRun on FM IQ at the pipeline's NFM channel rates ──\n");
    {
        // the line format and the modes as data
        PagingDecoder::Page p; p.proto = "POCSAG"; p.baud = 1200; p.address = 1234567; p.function = 3; p.kind = "alpha"; p.text = "HELLO";
        ok(PagingRun::line(p) == "[POCSAG 1200 \xc2\xb7 1234567 \xc2\xb7 F3] HELLO\n", "line: " + PagingRun::line(p).substr(0, PagingRun::line(p).size() - 1));
        p.kind = "tone"; p.text.clear(); p.truncated = true; p.address = 777;
        ok(PagingRun::line(p) == "[POCSAG 1200 \xc2\xb7 0000777 \xc2\xb7 F3] (tone only) (cut short)\n", "a tone-only page cut short reads so");
        const std::string caps = pagingCapsJson();
        ok(caps.find("\"paging\":true") != std::string::npos && caps.find("{\"id\":\"auto\",\"label\":\"AUTO\",\"group\":\"\"}") == caps.find('{')
           && caps.find("{\"id\":\"pocsag2400\",\"label\":\"2400\",\"group\":\"POCSAG\"}") != std::string::npos
           && caps.find("{\"id\":\"flex\",\"label\":\"ANY\",\"group\":\"FLEX\"}") != std::string::npos,
           "vibeserver.json fields: paging + the manual choices as data, AUTO first, each with its protocol group (FLEX one entry)");
        ok(PagingDecoder::modeFromId("pocsag512") == Mode::Pocsag512 && PagingDecoder::modeFromId("nonsense") == Mode::Auto
           && std::string(PagingDecoder::idOf(Mode::Pocsag2400)) == "pocsag2400", "mode ids round-trip; unknown → AUTO");
    }
    uint64_t fpAll = 1469598103934665603ull;
    for (double fs : {20000.0, 25000.0, 31250.0, 48000.0})
        for (int baud : {512, 1200, 2400}) {
            const bool inv = baud == 1200;
            auto x = fmIq(bits, baud, fs, 4500, inv ? 0.004 : -0.004, inv, 24.0, 40u + (uint32_t)baud + (uint32_t)fs, std::min(16000.0, fs * 0.8));
            const RunGot g = runIq(x, fs, Mode::Auto);
            fpMix(fpAll, &g.fp, sizeof g.fp);
            bool sawDet = false, sawShort = false;
            for (auto& j : g.json) { sawDet |= j.find("\"detected\":\"POCSAG " + std::to_string(baud) + "\"") != std::string::npos; sawShort |= j.find("\"short\":true") != std::string::npos; }
            char w[200]; std::snprintf(w, sizeof w, "%5.0f Hz IQ, POCSAG %4d%s: %zu lines right, status names it, locked state sent%s",
                                       fs, baud, inv ? " inverted" : "", g.lines.size(), fs < 24000 ? ", 'short' flagged" : "");
            ok(linesRight(g, baud) && sawDet && !g.states.empty() && g.states[0] == 1 && (sawShort == (fs < 24000)), w);
            if (!linesRight(g, baud)) for (auto& l : g.lines) std::printf("      %s", l.c_str());
        }
    std::printf("    fingerprint (discriminator + lines, every rate above): %016llx — must equal the VIBE_FORCE_SCALAR build's\n", (unsigned long long)fpAll);
    {
        std::printf("    sensitivity on IQ (Eb/N0, 12 transmissions a point, all four pages right, 12.5 kHz passband):\n");
        for (int cfg = 0; cfg < 4; ++cfg) {
            const int baud = cfg == 0 ? 512 : cfg == 1 ? 1200 : 2400; const double fs = cfg == 3 ? 20000.0 : 31250.0;
            std::printf("      POCSAG %4d @ %5.0f Hz:", baud, fs);
            double fifty = NAN;
            for (double eb = 6; eb <= 16.01; eb += 1) {
                int right = 0;
                for (int k = 0; k < 12; ++k) {
                    auto x = fmIq(bits, baud, fs, 4500, 0.002 * ((k % 3) - 1), k & 1, eb, 5000u + (uint32_t)k + (uint32_t)(eb * 37) + (uint32_t)baud, 12500);
                    right += linesRight(runIq(x, fs, Mode::Auto), baud);
                }
                std::printf(" %2.0f:%2d", eb, right);
                if (std::isnan(fifty) && right >= 6) fifty = eb;
            }
            // ★ the CNR in the 12.5 kHz passband = Eb/N0 + 10·log10(baud / 12500): the discriminator's FM threshold (≈ 3–5 dB
            //   CNR here) is what sets these — 512 bit/s at 15 dB Eb/N0 is ≈ 1 dB CNR. A tone-filter (non-coherent FSK)
            //   detector would beat it at the low rates; pager receivers and the usual decoders use the discriminator too.
            std::printf("  → 50 %% ≈ %.0f dB (CNR in 12.5 kHz ≈ %.1f dB)\n", fifty, fifty + 10 * std::log10(baud / 12500.0));
            const double lim = baud == 512 ? 16 : baud == 1200 ? 15 : 13;
            ok(!std::isnan(fifty) && fifty <= lim, "IQ path, POCSAG " + std::to_string(baud) + " @ " + std::to_string((int)fs) + " Hz reaches 50 % by " + std::to_string((int)lim) + " dB Eb/N0");
        }
    }
    {
        // channel noise only (the squelch is not in this path: the pager is fed noise whenever nothing transmits)
        const double fs = 31250; const int secs = 900;
        std::mt19937 r(123); std::normal_distribution<float> g(0.f, 1.f);
        RunGot got; PagingRun::Out o;
        o.text = [&](const std::string& t) { got.lines.push_back(t); };
        PagingRun run(Mode::Auto, std::move(o));
        std::vector<std::complex<float>> x((size_t)fs);
        bool det = false;
        const auto t0 = std::chrono::steady_clock::now();
        for (int s = 0; s < secs; ++s) {
            for (auto& v : x) v = std::complex<float>(g(r), g(r));
            run.feedIq(x.data(), (int)x.size(), fs);
            det |= !run.detected().empty();
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ok(got.lines.empty() && !det, "15 minutes of channel noise at 31.25 kHz (AUTO): " + std::to_string(got.lines.size()) + " pages, detected " + (det ? "yes" : "no"));
        std::printf("    cost (incl. the noise generator): %.3f ms per second of 31.25 kHz IQ\n", ms / secs);
    }

    testFlex();
    std::printf("\n%s (%d failed)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}

/** The wired path's fingerprint alone: the same twelve IQ runs as the test above (same seeds), discriminator + lines. */
static uint64_t wiredFingerprint(uint64_t* linesOnly) {
    const auto bits = transmission(testPages());
    uint64_t fpAll = 1469598103934665603ull, fpL = 1469598103934665603ull;
    for (double fs : {20000.0, 25000.0, 31250.0, 48000.0})
        for (int baud : {512, 1200, 2400}) {
            const bool inv = baud == 1200;
            auto x = fmIq(bits, baud, fs, 4500, inv ? 0.004 : -0.004, inv, 24.0, 40u + (uint32_t)baud + (uint32_t)fs, std::min(16000.0, fs * 0.8));
            const RunGot g = runIq(x, fs, Mode::Auto);
            fpMix(fpAll, &g.fp, sizeof g.fp);
            fpMix(fpL, &g.fpLines, sizeof g.fpLines);
        }
    // ★ FLEX too (every speed, 31.25 kHz): the same discriminator, then the FLEX receiver's lines
    for (const auto& sp : flexSpeeds()) {
        auto x = flexIq(sp, 2, 31250, 0.0002, false, 14.0, 90u + (uint32_t)sp.bps + (uint32_t)sp.levels);
        const RunGot g = runIq(x, 31250, Mode::Auto);
        fpMix(fpAll, &g.fp, sizeof g.fp);
        fpMix(fpL, &g.fpLines, sizeof g.fpLines);
    }
    if (linesOnly) *linesOnly = fpL;
    return fpAll;
}
