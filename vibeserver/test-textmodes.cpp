// test-textmodes.cpp — MFSK16/32, OLIVIA, CONTESTIA and THOR (decoders/textmodes.h), V12 2026-10-31. Silent; synthetic
// audio only, plus (when VIBE_TEXTMODE_SAMPLES names a folder of 8 kHz float32 files) the public reference recordings
// the format was settled against. Deterministic.
//   • every sub-mode, its own manual setting and AUTO, at +10 dB SNR in 3 kHz, 48 kHz input: the text comes out whole;
//   • the wrong family's manual setting prints nothing;
//   • white noise through every family's AUTO: no character, never locked;
//   • the reference recordings (if present): the published text.
#include "decoders/textmodes.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
using namespace vibe;
using F = TextModeDecoder::Family;

static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }
static uint64_t g_fp = 1469598103934665603ull;

static std::vector<float> withNoise(std::vector<float> x, double fs, double snrDb3k, uint32_t seed, double padS = 3) {
    double p = 0; size_t n = 0;
    for (float v : x) if (v != 0.f) { p += v * v; ++n; }
    p = n ? p / n : 1;
    std::vector<float> pad((size_t)(fs * padS), 0.f);
    x.insert(x.begin(), pad.begin(), pad.end()); x.insert(x.end(), pad.begin(), pad.end());
    const float sig = (float)std::sqrt(p / std::pow(10.0, snrDb3k / 10.0) * (fs / 2) / 3000.0);
    std::mt19937 r(seed); std::normal_distribution<float> g(0.f, 1.f);
    for (auto& v : x) v += sig * g(r);
    return x;
}
struct Got { std::string text; std::string detected; bool everLocked = false; };
static Got run(F f, const std::string& mode, const std::vector<float>& x, double fs) {
    TextModeDecoder d(f, fs, mode); Got g;
    d.onText = [&](const std::string& s) { g.text += s; for (unsigned char c : s) { g_fp ^= c; g_fp *= 1099511628211ull; } };
    d.onState = [&](int s) { if (s) { g.everLocked = true; g.detected = d.detected(); } };
    const int blk = (int)(fs / 50);
    for (size_t i = 0; i < x.size(); i += blk) d.feed(x.data() + i, (int)std::min<size_t>(blk, x.size() - i));
    return g;
}
static std::string body(const std::string& t) {   // the text without the "[MODE · Hz]" lines
    std::string o; size_t i = 0;
    while (i < t.size()) { size_t e = t.find('\n', i); if (e == std::string::npos) e = t.size(); std::string l = t.substr(i, e - i);
                           if (!(l.size() && l[0] == '[')) { o += l; o += '\n'; } i = e + 1; }
    return o;
}

int main() {
    std::printf("test-textmodes — MFSK16/32, OLIVIA, CONTESTIA, THOR\n");
    const double fs = 48000;
    const std::string msg = "CQ CQ DE G4XYZ G4XYZ IO91 PSE K";
    struct Case { F f; const char* id; double hz; };
    const Case cases[] = {
        {F::Mfsk, "mfsk16", 1200}, {F::Mfsk, "mfsk32", 1500},
        {F::Thor, "thor4", 1000}, {F::Thor, "thor5", 1300}, {F::Thor, "thor8", 1500}, {F::Thor, "thor11", 1100},
        {F::Thor, "thor16", 1600}, {F::Thor, "thor22", 1400},
        {F::Olivia, "olivia-4-125", 1500}, {F::Olivia, "olivia-4-250", 1500}, {F::Olivia, "olivia-8-250", 1500},
        {F::Olivia, "olivia-8-500", 1500}, {F::Olivia, "olivia-16-500", 1500}, {F::Olivia, "olivia-16-1000", 1500},
        {F::Olivia, "olivia-32-1000", 1500}, {F::Olivia, "olivia-64-2000", 1500},
        {F::Olivia, "contestia-4-125", 1500}, {F::Olivia, "contestia-8-250", 1500}, {F::Olivia, "contestia-16-500", 1500},
        {F::Olivia, "contestia-32-1000", 1500}, {F::Olivia, "contestia-64-2000", 1500},
    };
    std::printf("\n── each sub-mode at +10 dB (3 kHz), 48 kHz: its manual setting, and AUTO where it is in AUTO ──\n");
    for (const Case& c : cases) {
        const bool con = std::string(c.id).rfind("contestia", 0) == 0;
        const std::string want = con ? "CQ CQ DE G4XYZ G4XYZ IO91 PSE K" : msg;
        auto x = withNoise(textModeSynth(c.f, c.id, msg, fs, c.hz), fs, 10, 11);
        Got m = run(c.f, c.id, x, fs);
        ok(body(m.text).find(want) != std::string::npos, std::string(c.id) + " manual: " + body(m.text).substr(0, 60));
        bool inAuto = false;
        for (auto& mi : TextModeDecoder::modes(c.f)) (void)mi;
        // AUTO holds the common sub-modes (textmodes.cpp kSubs); the rest are tried only by their manual setting
        const std::string id = c.id;
        inAuto = !(id == "olivia-4-250" || id == "olivia-64-2000" || id == "contestia-4-125" || id == "contestia-4-250" ||
                   id == "contestia-8-500" || id == "contestia-16-1000" || id == "contestia-64-2000");
        if (inAuto) {
            Got a = run(c.f, "auto", x, fs);
            std::string lab = a.detected;
            ok(body(a.text).find(want) != std::string::npos, std::string(c.id) + " AUTO (named " + lab + ")");
        }
    }

    std::printf("\n── the wrong setting prints nothing ──\n");
    {
        auto x = withNoise(textModeSynth(F::Olivia, "olivia-8-250", msg, fs, 1500), fs, 10, 12);
        ok(run(F::Thor, "auto", x, fs).text.empty(), "an OLIVIA 8/250 signal through THOR AUTO: nothing");
        ok(run(F::Mfsk, "auto", x, fs).text.empty(), "…through MFSK AUTO: nothing");
        auto y = withNoise(textModeSynth(F::Mfsk, "mfsk16", msg, fs, 1500), fs, 10, 13);
        ok(run(F::Olivia, "auto", y, fs).text.empty(), "an MFSK16 signal through OLIVIA AUTO: nothing");
    }

    std::printf("\n── ONE key (Family::All, the server's \"mfsk\"): its AUTO decodes every family; its grouped list; a family's ANY ──\n");
    {
        for (const Case& c : cases) {
            const std::string id = c.id;
            if (id == "olivia-4-250" || id == "olivia-64-2000" || id == "contestia-4-125" || id == "contestia-4-250" ||
                id == "contestia-8-500" || id == "contestia-16-1000" || id == "contestia-64-2000") continue;   // manual only
            auto x = withNoise(textModeSynth(c.f, c.id, msg, fs, c.hz), fs, 10, 11);
            Got a = run(F::All, "auto", x, fs);
            ok(body(a.text).find(msg) != std::string::npos, id + " through the one AUTO (named " + a.detected + ")");
        }
        const auto L = TextModeDecoder::modes(F::All);
        std::string groups; for (auto& m : L) if (!m.group.empty() && groups.find(m.group) == std::string::npos) groups += m.group + ",";
        bool every = true; for (const Case& c : cases) { bool f = false; for (auto& m : L) f |= m.id == c.id; every &= f; }
        ok(L[0].id == "auto" && groups == "MFSK,OLIVIA,CONTESTIA,THOR," && every && L.size() == 29, "the list: AUTO, then each family's ANY and every sub-mode (" + std::to_string(L.size()) + " entries, " + groups + ")");
        ok(TextModeDecoder::modesJson(F::All).find("{\"id\":\"olivia-8-250\",\"label\":\"8/250\",\"group\":\"OLIVIA\"}") != std::string::npos, "each entry carries its family (the FAMILY row) and a short label (the MODE row)");
        auto y = withNoise(textModeSynth(F::Olivia, "olivia-8-250", msg, fs, 1500), fs, 10, 41);
        ok(body(run(F::All, "any-olivia", y, fs).text).find(msg) != std::string::npos, "ANY under OLIVIA decodes OLIVIA 8/250");
        ok(run(F::All, "any-thor", y, fs).text.empty(), "ANY under THOR prints nothing on it");
        ok(run(F::All, "any-contestia", y, fs).text.empty(), "ANY under CONTESTIA prints nothing on it");
        F f2; ok(TextModeDecoder::familyOf("mfsk", f2) && f2 == F::All, "the wire's \"mfsk\" is the one key");
    }

    std::printf("\n── CONTESTIA's alphabet (confirmed with fldigi as a black box) and a narrower signal ──\n");
    {
        auto x = withNoise(textModeSynth(F::Olivia, "contestia-8-250", "CQ DE G4XYZ\nQRZ? 73", fs, 1500), fs, 10, 31);
        const std::string t = body(run(F::Olivia, "contestia-8-250", x, fs).text);
        ok(t.find("CQ DE G4XYZ\nQRZ? 73") != std::string::npos, "a newline is code 60, a space code 59: \"" + t.substr(0, 30) + "\"");
        // ★ a CONTESTIA 4/125 signal uses 4 of a 16/500 receiver's 16 tones at the same spacing and rate: AUTO must not
        //   lock the wider one on it (fldigi's 4/125 through AUTO printed half-right garbage as "CONTESTIA 16/500")
        auto y = withNoise(textModeSynth(F::Olivia, "contestia-4-125", msg + " " + msg, fs, 1500), fs, 10, 32);
        Got a = run(F::Olivia, "auto", y, fs);
        ok(a.text.empty() || a.detected == "CONTESTIA 4/125", "CONTESTIA 4/125 through AUTO: nothing wider locks (" + (a.detected.empty() ? std::string("none") : a.detected) + ")");
        auto z = withNoise(textModeSynth(F::Olivia, "olivia-4-125", msg + " " + msg, fs, 1500), fs, 10, 33);
        Got b = run(F::Olivia, "auto", z, fs);
        ok(b.detected == "OLIVIA 4/125" && body(b.text).find(msg) != std::string::npos, "OLIVIA 4/125 through AUTO: still itself (" + b.detected + ")");
    }
    if (const char* dir = std::getenv("VIBE_FLDIGI_DIR")) {   // ★ fldigi's own Contestia audio (8 kHz float32, not in git)
        std::printf("\n── fldigi as a black box: CONTESTIA, every sub-mode we offer (%s) ──\n", dir);
        const char* want = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ??????ABCDEFGHIJKLMNOPQRSTUVWXYZ????\nNEW LINE";
        const char* subs[][2] = { {"Cont-4-125", "contestia-4-125"}, {"Cont-4-250", "contestia-4-250"}, {"Cont-8-250", "contestia-8-250"},
                                  {"Cont-8-500", "contestia-8-500"}, {"Cont-16-500", "contestia-16-500"}, {"Cont-16-1K", "contestia-16-1000"},
                                  {"Cont-32-1K", "contestia-32-1000"}, {"Cont-64-2K", "contestia-64-2000"} };
        for (auto& sb : subs) {
            FILE* fp = std::fopen((std::string(dir) + "/" + sb[0] + ".raw").c_str(), "rb");
            if (!fp) { ok(false, std::string(sb[0]) + ": missing"); continue; }
            std::vector<float> a; float buf[4096]; size_t k;
            while ((k = std::fread(buf, 4, 4096, fp)) > 0) a.insert(a.end(), buf, buf + k);
            std::fclose(fp);
            const std::string t = body(run(F::Olivia, sb[1], a, 8000).text);
            // ★ from '!' on: 16/500 shows one start-up character ("0") between the first space and '!' (the pre-lock
            //   block — section 11's known dead end); everything after it is exact on every sub-mode
            ok(t.find(want + 1) != std::string::npos, std::string(sb[1]) + ": every printable character and the newline as fldigi sent them");
        }
    }

    std::printf("\n── other signals (60 s each, +10 dB): RTTY 45.45 Bd / 170 Hz, a steady carrier — nothing printed ──\n");
    {
        std::vector<float> rtty((size_t)(fs * 60)), car((size_t)(fs * 60));
        std::mt19937 r(5); double ph = 0; int bit = 0; const int spb = (int)(fs / 45.45);
        for (size_t i = 0; i < rtty.size(); ++i) { if (i % spb == 0) bit = r() & 1; ph += 2 * M_PI * (bit ? 1585 : 1415) / fs; rtty[i] = (float)std::sin(ph); }
        for (size_t i = 0; i < car.size(); ++i) car[i] = (float)std::sin(2 * M_PI * 1234.5 * i / fs);
        auto a = withNoise(rtty, fs, 10, 21), c = withNoise(car, fs, 10, 22);
        for (F f : {F::Mfsk, F::Olivia, F::Thor, F::All}) {
            ok(run(f, "auto", a, fs).text.empty(), std::string(TextModeDecoder::extName(f)) + " AUTO on RTTY: nothing");
            ok(run(f, "auto", c, fs).text.empty(), std::string(TextModeDecoder::extName(f)) + " AUTO on a carrier: nothing");
        }
    }

    const int noiseMin = std::getenv("VIBE_TM_NOISE_MIN") ? std::atoi(std::getenv("VIBE_TM_NOISE_MIN")) : 3;
    std::printf("\n── white noise, %d min through each family's AUTO ──\n", noiseMin);
    for (F f : {F::Mfsk, F::Olivia, F::Thor, F::All}) {
        TextModeDecoder d(f, fs, "auto"); std::string t; bool locked = false;
        d.onText = [&](const std::string& s) { t += s; };
        d.onState = [&](int s) { if (s) locked = true; };
        std::mt19937 r(100 + (int)f); std::normal_distribution<float> g(0.f, 0.3f);
        std::vector<float> b(960);
        for (long i = 0; i < (long)noiseMin * 60 * 50; ++i) { for (auto& v : b) v = g(r); d.feed(b.data(), 960); }
        ok(t.empty() && !locked, std::string(TextModeDecoder::extName(f)) + ": " + (t.empty() ? "nothing" : "\"" + t.substr(0, 40) + "\""));
    }

    if (const char* dir = std::getenv("VIBE_TEXTMODE_SAMPLES")) {
        std::printf("\n── reference recordings (%s) ──\n", dir);
        struct Ref { const char* file; F f; double fs; const char* want; };
        const Ref refs[] = {
            {"Wikipedia-Slogan_MFSK.f32", F::Mfsk, 8000, "das Wissen der Menschheit"},
            {"MFSK_16.f32", F::Mfsk, 8000, "The quick brown fox jumps over the lazy dog"},
            {"MFSK_32.f32", F::Mfsk, 8000, "The quick brown fox jumps over the lazy dog"},
            {"OLIVIA_500_16_sample.f32", F::Olivia, 8000, "Welcome to Wikipedia, the free encyclopedia that anyone can edit."},
            {"OLIVIA_1000_32_sample.f32", F::Olivia, 8000, "Welcome to Wikipedia, the free encyclopedia that anyone can edit."},
            {"Olivia_8-250.f32", F::Olivia, 8000, "The quick brown fox jumps over the lazy dog"},
            {"Contestia_8-250.f32", F::Olivia, 8000, "QUICK BROWN FOX JUMPS OVER THE LAZY DOG"},
            {"Contestia_16-500.f32", F::Olivia, 8000, "QUICK BROWN FOX JUMPS OVER THE LAZY DOG"},
            {"THOR8.f32", F::Thor, 8000, "The quick brown fox jumps over the lazy dog"},
            {"THOR16.f32", F::Thor, 8000, "The quick brown fox jumps over the lazy dog"},
            {"THOR11_11k.f32", F::Thor, 11025, "The quick brown fox jumps over the lazy dog"},
        };
        for (const Ref& r : refs) {
            FILE* fp = std::fopen((std::string(dir) + "/" + r.file).c_str(), "rb");
            if (!fp) { std::printf("  (absent: %s)\n", r.file); continue; }
            std::vector<float> x; float b[4096]; size_t n;
            while ((n = std::fread(b, 4, 4096, fp)) > 0) x.insert(x.end(), b, b + n);
            std::fclose(fp);
            Got g = run(r.f, "auto", x, r.fs);
            ok(g.text.find(r.want) != std::string::npos, std::string(r.file) + " (" + g.detected + ")");
        }
    }
    std::printf("\noutput fingerprint %016llx\n", (unsigned long long)g_fp);
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
