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
#include <cstring>
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
/** AUTO holds the common sub-modes (textmodes.cpp kSubs, `common`); the rest are tried only by their manual setting */
static bool inAuto(const std::string& id) {
    static const char* manualOnly[] = {"olivia-4-250", "olivia-64-2000", "contestia-4-125", "contestia-4-250",
        "contestia-4-1000", "contestia-4-2000", "contestia-8-125", "contestia-8-500", "contestia-8-2000",
        "contestia-16-250", "contestia-16-1000", "contestia-16-2000", "contestia-32-2000", "contestia-64-500",
        "contestia-64-1000", "contestia-64-2000"};
    for (const char* m : manualOnly) if (id == m) return false;
    return true;
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
        // ★ the other eleven of fldigi's standard Contestia sub-modes (2026-10-31, ninth run: DECODERS-WAVE2.md section 23)
        {F::Olivia, "contestia-4-500", 1500}, {F::Olivia, "contestia-4-1000", 1500}, {F::Olivia, "contestia-4-2000", 1500},
        {F::Olivia, "contestia-8-125", 1500}, {F::Olivia, "contestia-8-1000", 1500}, {F::Olivia, "contestia-8-2000", 1500},
        {F::Olivia, "contestia-16-250", 1500}, {F::Olivia, "contestia-16-2000", 1500}, {F::Olivia, "contestia-32-2000", 1500},
        {F::Olivia, "contestia-64-500", 1500}, {F::Olivia, "contestia-64-1000", 1500},
    };
    std::printf("\n── each sub-mode at +10 dB (3 kHz), 48 kHz: its manual setting, and AUTO where it is in AUTO ──\n");
    for (const Case& c : cases) {
        const bool con = std::string(c.id).rfind("contestia", 0) == 0;
        const std::string want = con ? "CQ CQ DE G4XYZ G4XYZ IO91 PSE K" : msg;
        auto x = withNoise(textModeSynth(c.f, c.id, msg, fs, c.hz), fs, 10, 11);
        Got m = run(c.f, c.id, x, fs);
        ok(body(m.text).find(want) != std::string::npos, std::string(c.id) + " manual: " + body(m.text).substr(0, 60));
        if (inAuto(c.id)) {
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
            if (!inAuto(id)) continue;   // manual only
            auto x = withNoise(textModeSynth(c.f, c.id, msg, fs, c.hz), fs, 10, 11);
            Got a = run(F::All, "auto", x, fs);
            std::string lab = id; for (auto& m : TextModeDecoder::modes(F::All)) if (m.id == id) lab = m.group + " " + m.label;
            if (id.rfind("mfsk", 0) == 0) lab = "MFSK" + id.substr(4); else if (id.rfind("thor", 0) == 0) lab = "THOR " + id.substr(4);
            ok(body(a.text).find(msg) != std::string::npos && a.detected == lab, id + " through the one AUTO (named " + a.detected + ")");
        }
        const auto L = TextModeDecoder::modes(F::All);
        std::string groups; for (auto& m : L) if (!m.group.empty() && groups.find(m.group) == std::string::npos) groups += m.group + ",";
        bool every = true; for (const Case& c : cases) { bool f = false; for (auto& m : L) f |= m.id == c.id; every &= f; }
        ok(L[0].id == "auto" && groups == "MFSK,OLIVIA,CONTESTIA,THOR," && every && L.size() == 40, "the list: AUTO, then each family's ANY and every sub-mode (" + std::to_string(L.size()) + " entries, " + groups + ")");
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
        std::printf("\n── fldigi as a black box: CONTESTIA, all 19 standard sub-modes (%s) ──\n", dir);
        const char* want = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ??????ABCDEFGHIJKLMNOPQRSTUVWXYZ????\nNEW LINE";
        // file, id, and the SNR (dB in 3 kHz) of the edge: the lowest 2 dB step where 10 of 10 noise seeds gave the whole
        // text in the ninth run's sweep (DECODERS-WAVE2.md section 23)
        struct Fl { const char* file; const char* id; double edge; };
        const Fl subs[] = {
            {"Cont-4-125", "contestia-4-125", -10}, {"Cont-4-250", "contestia-4-250", -8}, {"Cont-4-500", "contestia-4-500", -4},
            {"Cont-4-1K", "contestia-4-1000", -2}, {"Cont-4-2K", "contestia-4-2000", 2}, {"Cont-8-125", "contestia-8-125", -14},
            {"Cont-8-250", "contestia-8-250", -12}, {"Cont-8-500", "contestia-8-500", -8}, {"Cont-8-1K", "contestia-8-1000", -4},
            {"Cont-8-2K", "contestia-8-2000", -2}, {"Cont-16-250", "contestia-16-250", -12}, {"Cont-16-500", "contestia-16-500", -10},
            {"Cont-16-1K", "contestia-16-1000", -4}, {"Cont-16-2K", "contestia-16-2000", -4}, {"Cont-32-1K", "contestia-32-1000", -8},
            {"Cont-32-2K", "contestia-32-2000", -4}, {"Cont-64-500", "contestia-64-500", -12}, {"Cont-64-1K", "contestia-64-1000", -10},
            {"Cont-64-2K", "contestia-64-2000", -10} };
        for (const Fl& sb : subs) {
            FILE* fp = std::fopen((std::string(dir) + "/" + sb.file + ".raw").c_str(), "rb");
            if (!fp) { ok(false, std::string(sb.file) + ": missing"); continue; }
            std::vector<float> a; float buf[4096]; size_t k;
            while ((k = std::fread(buf, 4, 4096, fp)) > 0) a.insert(a.end(), buf, buf + k);
            std::fclose(fp);
            const std::string t = body(run(F::Olivia, sb.id, a, 8000).text);
            // ★ from '!' on: 16/500 shows one start-up character ("0") between the first space and '!' (the pre-lock
            //   block — section 11's known dead end); everything after it is exact on every sub-mode
            ok(t.find(want + 1) != std::string::npos, std::string(sb.id) + ": every printable character and the newline as fldigi sent them");
            // ★ the same audio in white noise (seeds 1000…, the sweep's): at the edge 2 of 3 whole, 4 dB above it 3 of 3
            double p = 0; size_t np = 0; for (float v : a) if (std::fabs(v) > 1e-4f) { p += v * v; ++np; } p /= np ? np : 1;
            for (double snr : {sb.edge, sb.edge + 4}) {
                int whole = 0;
                for (int seed = 1000; seed < 1003; ++seed) {
                    std::vector<float> x(a);
                    const float sig = (float)std::sqrt(p / std::pow(10.0, snr / 10.0) * 4000.0 / 3000.0);
                    std::mt19937 r(seed); std::normal_distribution<float> g(0.f, 1.f);
                    for (auto& v : x) v += sig * g(r);
                    whole += run(F::Olivia, sb.id, x, 8000).text.find(want + 1) != std::string::npos;
                }
                const bool edge = snr == sb.edge;
                ok(whole >= (edge ? 2 : 3), std::string(sb.id) + " at " + std::to_string((int)snr) + " dB: " + std::to_string(whole) + "/3 whole");
            }
            // ★ …and through the one key's AUTO: a sub-mode in AUTO is found and named; a manual-only one locks NOTHING
            //   (no other receiver of the same spacing and rate — e.g. 16/2000 under 4/500 and 8/1000 — reads it)
            Got g = run(F::All, "auto", a, 8000);
            const std::string label = std::string("CONTESTIA ") + (std::strchr(sb.id + 10, '-') ? std::string(sb.id + 10).replace(std::string(sb.id + 10).find('-'), 1, "/") : "");
            if (inAuto(sb.id)) ok(g.detected == label && body(g.text).find(want + 1) != std::string::npos, std::string(sb.id) + " through the one AUTO: named " + g.detected);
            else ok(g.text.empty() && !g.everLocked, std::string(sb.id) + " (manual only) through the one AUTO: nothing" + (g.detected.empty() ? "" : " — locked " + g.detected));
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

    std::printf("\n── white noise, 1 min through every CONTESTIA sub-mode's manual setting ──\n");
    {
        int quiet = 0, total = 0; std::string loud;
        for (const auto& m : TextModeDecoder::modes(F::All)) {
            if (m.group != "CONTESTIA") continue;
            TextModeDecoder d(F::All, fs, m.id); std::string t; bool locked = false;
            d.onText = [&](const std::string& s) { t += s; };
            d.onState = [&](int s) { if (s) locked = true; };
            std::mt19937 r(300 + total); std::normal_distribution<float> g(0.f, 0.3f);
            std::vector<float> b(960);
            for (long i = 0; i < 60L * 50; ++i) { for (auto& v : b) v = g(r); d.feed(b.data(), 960); }
            ++total; if (t.empty() && !locked) ++quiet; else loud += m.id + " ";
        }
        ok(total == 20 && quiet == total, "nothing printed, never locked: " + std::to_string(quiet) + "/" + std::to_string(total) + (loud.empty() ? "" : " (" + loud + ")"));
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
