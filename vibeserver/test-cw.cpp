// test-cw.cpp — the CW decoder and skimmer (decoders/cw_skimmer.h), measured, and its plumbing (vibe_cw_run.h through
// vibe_decoder_host.h, and the gallery's CW summary in vibe_bgdecode.h).
//
// ★★★ WHAT IS PINNED. The numbers a listener and an owner rely on, on synthetic Morse whose every character is known
//     (tools/cw-bench/cw_synth.h — PARIS timing, SNR quoted in 2.5 kHz):
//   • the floor's calibration (kFloorBias) — every threshold in the engine is a multiple of it;
//   • CER at 0 dB (12–40 WPM) and −5 dB (12, 20 WPM), each the mean of three noise draws, on IQ (the skimmer) and audio (the listener);
//   • HUMAN FISTS at 0 dB (a QSO of eight overs each, tools/cw-bench/cw_synth.h fistCases): a speed ramp inside an
//     over, a step between overs (no characters lost when the signal changes lane), dash:dot 2.5 and 4, heavy and light
//     weighting, odd spacing, Farnsworth, a straight key's jitter and a bug — each against its own limit, set above what
//     the fist ITSELF costs a perfect fixed-threshold reader (the timing floor, cw_bench fists);
//   • PURE NOISE makes no signal and no spot — the skimmer runs unattended into the gallery, so a false spot is a lie
//     written down;
//   • several stations in one segment, each spotted at its own frequency, none invented;
//   • the callsign rules and the identifying patterns;
//   • a listener's "cw" and a skimmer's "cwskim" through a DecoderHost: text frames, the signals list, spots, the
//     summary tap — and the CW summary card the gallery keeps.
// ★ Silent: nothing is played. Deterministic: fixed seeds.
#include "vibe_decoder_host.h"
#include "vibe_bgdecode.h"
#include "../tools/cw-bench/cw_synth.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <thread>

using namespace vibe;
using namespace cwsynth;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

static const std::string kText =
    "CQ CQ DE G4ABC G4ABC K  THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789  "
    "UR RST 579 579 NAME JOHN QTH LONDON  TNX FER CALL  CQ TEST DL1XYZ DL1XYZ  PSE QSL VIA BURO  73 ES GL ";

struct Run { std::map<int, std::string> text; std::map<int, double> hz; std::vector<cw::Spot> spots; };
static Run run(cw::Skimmer& sk, const std::vector<cf>& iq, const std::vector<float>& re) {
    Run r;
    sk.onText = [&](int id, double hz, const std::string& t) { r.text[id] += t; r.hz[id] = hz; };
    sk.onSpot = [&](const cw::Spot& s) { r.spots.push_back(s); };
    for (size_t i = 0; i < std::max(iq.size(), re.size()); i += 512) {
        if (!iq.empty()) sk.feedIq(iq.data() + i, (int)std::min<size_t>(512, iq.size() - i));
        else sk.feedReal(re.data() + i, (int)std::min<size_t>(512, re.size() - i));
    }
    return r;
}
static std::string best(const Run& r, double hz) {
    std::string t;
    for (auto& kv : r.text) { auto h = r.hz.find(kv.first); if (h != r.hz.end() && std::fabs(h->second - hz) < 40 && kv.second.size() > t.size()) t = kv.second; }
    return t;
}
static double cerOf(bool iqIn, double wpm, double snr, uint64_t seed) {
    const double rate = iqIn ? 48000 : 12000;
    Rng rng(seed);
    Keying k; k.wpm = wpm;
    std::string sent; while (sent.size() < (size_t)(wpm * 5 * 1.1)) sent += kText;
    Tone t; t.hz = iqIn ? 3130 : 700; t.snrDb = snr; t.env = keyEnvelope(sent, k, rate, rng);
    std::vector<Tone> ts{ t }; std::vector<cf> iq; std::vector<float> re;
    synth(ts, rate, iqIn, t.env.size() / rate, rng, iq, re);
    cw::Skimmer::Config c; c.rate = rate; c.complexIn = iqIn;
    if (iqIn) { c.loHz = -20000; c.hiHz = 20000; } else { c.loHz = 150; c.hiHz = 3500; }
    cw::Skimmer sk(c);
    auto r = run(sk, iq, re);
    int e, n; cer(sent, best(r, t.hz), e, n);
    return 100.0 * e / std::max(1, (int)squash(sent).size());
}

/** A decoder socket that remembers what it was sent. */
struct Peer : DecoderPeer {
    std::mutex m; std::vector<std::vector<uint8_t>> bin; std::vector<std::string> txt;
    bool open() const override { return true; }
    void binary(const uint8_t* d, size_t n) override { std::lock_guard<std::mutex> lk(m); bin.emplace_back(d, d + n); }
    void text(const std::string& s) override { std::lock_guard<std::mutex> lk(m); txt.push_back(s); }
    std::string decoded() {
        std::lock_guard<std::mutex> lk(m); std::string o;
        for (auto& f : bin) if (f.size() >= 13 && f[0] == 0x01) o.append((const char*)f.data() + 13, f.size() - 13);
        return o;
    }
    int count(const std::string& needle) { std::lock_guard<std::mutex> lk(m); int n = 0; for (auto& t : txt) n += t.find(needle) != std::string::npos; return n; }
};
struct SpyTap : DecoderTap {
    std::mutex m; std::vector<std::string> calls; std::vector<double> rf; std::atomic<int> finished{0};
    void cwSpot(const std::string& c, double hz, float, float, const std::string&) override { std::lock_guard<std::mutex> lk(m); calls.push_back(c); rf.push_back(hz); }
    void finish(bool) override { finished++; }
};
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/cw-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static std::string slurpTxt(const std::string& dir) {
    std::string all;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string f = e->d_name;
            if (f.size() > 4 && f.compare(f.size() - 4, 4, ".txt") == 0) { std::ifstream in(dir + "/" + f); std::stringstream ss; ss << in.rdbuf(); all += ss.str() + "\n"; }
        }
        closedir(d);
    }
    return all;
}

int main() {
    vibe::vibeFlushDenormals();
    std::printf("CW engine: %s kernels\n", cw::Skimmer::simdName());

    // ── the code table ──
    {
        bool all = true;
        for (const char* ch : { "A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
                                "0","1","2","3","4","5","6","7","8","9",".",",","?","/","=","+","-","(",")","@","<SK>","<AS>" }) {
            const std::string code = cw::morseCode(ch);
            unsigned idx = 1; for (char c : code) idx = idx * 2 + (c == '-');
            const char* back = cw::morseText(idx);
            all &= !code.empty() && back && std::string(back) == ch;
        }
        ok(all, "every ITU-R M.1677 character round-trips through the tree");
    }
    // ── callsigns ──
    {
        bool good = true;
        for (const char* c : { "G4ABC", "2E0ABC", "9A1A", "4X4ABC", "3DA0AB", "K1ABC/P", "EA8/G4ABC", "R1ANF", "VK2ABC", "JA1ZZ" }) good &= cw::isCallsign(c);
        ok(good, "real callsigns are calls (portable forms too)");
        bool bad = false;
        for (const char* c : { "5NN", "599", "TEST", "CQ", "TU", "EEE5E", "TU5", "A2", "ABCDEFG", "73" }) bad |= cw::isCallsign(c);
        ok(!bad, "reports, words and noise-shaped strings are not");
        auto id = [](std::vector<std::string> w, const char* call, const char* how) {
            std::string c, h; return cw::identify(w, c, h) && c == call && h == how;
        };
        ok(id({ "CQ", "CQ", "DE", "G4ABC" }, "G4ABC", "CQ"), "CQ CQ DE CALL");
        ok(id({ "CQ", "TEST", "DL1XYZ" }, "DL1XYZ", "TEST") && id({ "DL1XYZ", "TEST" }, "DL1XYZ", "TEST"), "contest CQs, call before or after TEST");
        ok(id({ "CQ", "POTA", "K1ABC" }, "K1ABC", "CQ"), "a modifier between CQ and the call");
        ok(id({ "DE", "IU", "5R", "DL" }, "IU5RDL", "DE"), "a call split by a gap read as a word space is joined");
        std::string c, h;
        ok(!cw::identify({ "UR", "RST", "599" }, c, h) && !cw::identify({ "5NN", "TU" }, c, h), "text with no call identifies nobody");
    }
    // ── ★ V12 2026-10-29: callsigns must make sense (the ITU series) ──
    {
        bool good = true;
        for (const char* c : { "A65BR", "S51ABC", "T77C", "E73A", "C31XX", "V51AS", "Z35A", "2E0ABC", "9A1A", "DB70DLR", "M0PKZ", "G4TPJ", "TK1RR" })
            good &= cw::isCallsign(c);
        ok(good, "ITU series: letter+figure blocks (A6, S5, T7, E7, C3, V5, Z3), figure+letter (2E, 9A), special calls (DB70DLR)");
        bool bad = false;
        for (const char* c : { "QE5ABC", "0A1BC", "1S1AB", "E9ABC", "O1ABC", "X2ABC", "U5ABC" }) bad |= cw::isCallsign(c);
        ok(!bad, "no Q series, no 0/1 first figure, no single letter that is not a whole-letter nation (O, U, X), no unallocated E8/E9");
    }
    // ── the floor ──
    for (int pass = 0; pass < 2; ++pass) {
        const bool cplx = pass == 0; const double rate = cplx ? 48000 : 12000;
        cw::Skimmer::Config c; c.rate = rate; c.complexIn = cplx;
        if (cplx) { c.loHz = -20000; c.hiHz = 20000; } else { c.loHz = 150; c.hiHz = 3500; }
        cw::Skimmer sk(c);
        Rng rng(5); std::vector<Tone> none; std::vector<cf> iq; std::vector<float> re;
        synth(none, rate, cplx, 60, rng, iq, re);
        run(sk, iq, re);
        const double db = 10 * std::log10(sk.noiseCalibration());
        char b[160]; std::snprintf(b, sizeof b, "the noise floor is calibrated on %s: %+.2f dB (within ±0.5)", cplx ? "IQ" : "audio", db);
        ok(std::fabs(db) <= 0.5, b);
    }
    // ── accuracy ──
    // ★ The MEAN over three noise draws: at 40 WPM and 0 dB one draw ranges 0.3–3.4 % (a handful of characters in 270),
    //   so a single draw judged the seed, not the decoder.
    auto cer3 = [](bool cplx, double w, double snr, uint64_t seed) {
        double sum = 0; for (int k = 0; k < 3; ++k) sum += cerOf(cplx, w, snr, seed + 1000 * (uint64_t)k); return sum / 3;
    };
    for (int pass = 0; pass < 2; ++pass) {
        const bool cplx = pass == 0;
        for (double w : { 12.0, 20.0, 30.0, 40.0 }) {
            const double e = cer3(cplx, w, 0, (uint64_t)w * 7 + pass);
            char b[160]; std::snprintf(b, sizeof b, "%s, %2.0f WPM at 0 dB (2.5 kHz): CER %.1f %%, mean of 3 (≤ 2.5)", cplx ? "IQ" : "audio", w, e);
            ok(e <= 2.5, b);
        }
        for (double w : { 12.0, 20.0 }) {
            const double e = cer3(cplx, w, -5, (uint64_t)w * 11 + pass);
            // ★ ≤ 6: at −5 dB, 20 WPM on IQ, six draws read 2.9–6.9 % (mean 4.7; the engine before the human-fist timing,
            //   4.1) — the limit is the spread of the noise, not of the decoder; a real regression moves the mean past it.
            char b[160]; std::snprintf(b, sizeof b, "%s, %2.0f WPM at −5 dB: CER %.1f %%, mean of 3 (≤ 6)", cplx ? "IQ" : "audio", w, e);
            ok(e <= 6.0, b);
        }
    }
    // ── human fists ──
    {
        // ★ Limits (CER %, 0 dB): the clean shapes as tight as a keyer; the jittery ones a margin over their timing floor
        //   (cw_bench fists: σ 20 % ≈ 3–4 %, σ 30 % ≈ 20–22 %, the bug 9–15 %, long uneven gaps ≈ 3.5 %).
        const std::map<std::string, double> lim = {
            { "straight key, σ 15 % per element", 3 }, { "straight key, σ 20 % per element", 10 },
            { "straight key, σ 30 % per element", 40 }, { "bug key (dots exact, dashes/gaps ±25 %)", 25 },
            { "long, uneven char/word gaps", 10 }, { "all at once: ramp, σ 15 %, 3.4:1, heavy", 5 } };
        for (const FistCase& fc : fistCases()) {
            Rng rng(92); std::string sent;
            Tone t; t.hz = 2000; t.snrDb = 0; t.env = fistEnvelope(fc, rng, sent);
            std::vector<Tone> ts{ t }; std::vector<cf> iq; std::vector<float> re;
            synth(ts, 48000, true, t.env.size() / 48000.0, rng, iq, re);
            cw::Skimmer::Config c; c.rate = 48000; c.complexIn = true; c.loHz = -20000; c.hiHz = 20000;
            cw::Skimmer sk(c);
            auto r = run(sk, iq, re);
            std::string got;   // ★ every signal near the tone: a station lost and found again is still one transmission
            for (auto& kv : r.text) if (std::fabs(r.hz[kv.first] - t.hz) < 40) got += kv.second + " ";
            int e, n; cer(sent, got, e, n);
            const double pc = 100.0 * e / std::max(1, (int)squash(sent).size());
            auto it = lim.find(fc.name); const double L = it != lim.end() ? it->second : 2.0;
            char b[200]; std::snprintf(b, sizeof b, "fist: %s — CER %.1f %% (≤ %.0f)", fc.name, pc, L);
            ok(pc <= L, b);
        }
    }
    // ── ★★ V12 2026-10-29: HUMAN-SENT MORSE (Stuart's 80 m screenshots) ──
    {
        struct H { const char* name; std::string text; Keying k; int overs; double cerMax; int mergedMax; int splitMax; };
        std::vector<H> hs;
        const std::string tonbridge = "M0PKZ DE G4TPJ R R TNX FER CALL = UR RST 599 599 = OP IS RICHARD RICHARD ES QTH IS NR TONBRIDGE TONBRIDGE = HW? + M0PKZ DE G4TPJ KN  ";
        { Keying k; k.wpm = 24; k.charGap = 3.0; k.wordGap = 3.6; k.jitter = 0.05; hs.push_back({ "the TONBRIDGE line (G4TPJ, 24 WPM, word gaps 1.2 × letter gaps)", tonbridge, k, 3, 4.0, 4, 10 }); }
        { Keying k; k.wpm = 22; k.charGap = 3.0; k.wordGap = 3.9; k.jitter = 0.06;
          hs.push_back({ "an ITALIAN QSO with word gaps 1.3 × letter gaps reads as Italian", "IZ0ABC DE IK2XYZ BUONA SERA = TUTTA LESTATE SI E STATO UN PO STRANO = QUI ANTENNA FILARE IN GIARDINO = GRAZIE DEL QSO 73 K  ", k, 3, 5.0, 8, 6 }); }
        { Keying k; k.wpm = 8; k.jitter = 0.2; k.elemGap = 1.6; k.charGap = 4.5; k.wordGap = 9;
          hs.push_back({ "a slow straight key (8 WPM, long gaps) is shown at all", kOvers[1], k, 2, 30.0, 99, 12 }); }
        for (const H& hc : hs) {
            Rng rng(500 + 10 + hc.text.size());   // ★ the +0 dB draw of cw_bench human
            std::string sent; std::vector<float> env;
            for (int o = 0; o < hc.overs; ++o) { auto e = keyEnvelope(hc.text, hc.k, 48000, rng, 1.5); env.insert(env.end(), e.begin(), e.end()); sent += hc.text; }
            Tone t; t.hz = 2000; t.snrDb = 0; t.env = env;
            std::vector<Tone> ts{ t }; std::vector<cf> iq; std::vector<float> re;
            synth(ts, 48000, true, env.size() / 48000.0, rng, iq, re);
            cw::Skimmer::Config c; c.rate = 48000; c.complexIn = true; c.loHz = -20000; c.hiHz = 20000;
            cw::Skimmer sk(c);
            auto r = run(sk, iq, re);
            std::string got; for (auto& kv : r.text) if (std::fabs(r.hz[kv.first] - t.hz) < 40) got += kv.second + " ";
            int e, n, sp, mg, rs; cer(sent, got, e, n); spaceErrors(sent, got, sp, mg, rs);
            const double pc = 100.0 * e / std::max(1, (int)squash(sent).size());
            char b[260]; std::snprintf(b, sizeof b, "%s — CER %.1f %% (≤ %.0f), words run together %d (≤ %d), broken %d (≤ %d)", hc.name, pc, hc.cerMax, mg, hc.mergedMax, sp, hc.splitMax);
            ok(!got.empty() && pc <= hc.cerMax && mg <= hc.mergedMax && sp <= hc.splitMax, b);
            if (std::string(hc.name).find("TONBRIDGE") != std::string::npos)
                ok(got.find("OP IS RICHARD RICHARD ES QTH IS NR TONBRIDGE TONBRIDGE") != std::string::npos,
                   "the Tonbridge line reads with its spaces: \"OP IS RICHARD RICHARD ES QTH IS NR TONBRIDGE TONBRIDGE\"");
        }
    }
    // ── ★ V12 2026-10-29: the speed as a range — one figure for a keyer, a range for a fist that varies ──
    {
        // ★ The range is of the LAST 24 characters — so each over here is short (≈ 45 characters) and the ramp steep, and
        //   the last over's second half is what the range must show.
        auto rangeOf = [](const Keying& k, int& lo, int& hi, float& w) {
            Rng rng(77); std::string sent; std::vector<float> env;
            for (int o = 0; o < 6; ++o) { auto e = keyEnvelope(kOvers[3], k, 48000, rng, 1.5); env.insert(env.end(), e.begin(), e.end()); }
            Tone t; t.hz = 2000; t.snrDb = 10; t.env = env;
            std::vector<Tone> ts{ t }; std::vector<cf> iq; std::vector<float> re;
            synth(ts, 48000, true, env.size() / 48000.0, rng, iq, re);
            cw::Skimmer::Config c; c.rate = 48000; c.complexIn = true; c.loHz = -20000; c.hiHz = 20000;
            cw::Skimmer sk(c);
            run(sk, iq, re);
            lo = hi = 0; w = 0;
            for (auto& v : sk.signals()) if (std::fabs(v.offsetHz - 2000) < 40) { lo = v.wpmLo; hi = v.wpmHi; w = v.wpm; return; }
        };
        int lo, hi; float w;
        { Keying k; k.wpm = 24; rangeOf(k, lo, hi, w);
          char b[160]; std::snprintf(b, sizeof b, "a keyer at 24 WPM shows ONE figure (%d–%d, %.0f)", lo, hi, w);
          ok(lo == hi && lo >= 22 && lo <= 26, b); }
        { Keying k; k.wpm = 14; k.wpmEnd = 28; rangeOf(k, lo, hi, w);
          char b[160]; std::snprintf(b, sizeof b, "a fist ramping 14→28 WPM in each over shows a RANGE (%d–%d) around the figure (%.0f)", lo, hi, w);
          ok(lo < hi && lo <= (int)std::lround(w) && hi >= (int)std::lround(w) && hi - lo >= 3, b); }
    }
    // ── pure noise ──
    {
        cw::Skimmer::Config c; c.rate = 96000; c.complexIn = true; c.loHz = -45000; c.hiHz = 45000;
        cw::Skimmer sk(c);
        Rng rng(99); size_t text = 0; size_t spots = 0;
        for (int t = 0; t < 30; ++t) {
            std::vector<Tone> none; std::vector<cf> iq; std::vector<float> re;
            synth(none, 96000, true, 10, rng, iq, re);
            auto r = run(sk, iq, re);
            for (auto& kv : r.text) text += kv.second.size();
            spots += r.spots.size();
        }
        ok(text == 0 && spots == 0 && sk.signals(true).empty(), "5 minutes of pure noise across 96 kHz: no signal, no text, no spot ("
           + std::to_string(text) + " chars, " + std::to_string(spots) + " spots)");
    }
    // ── several stations ──
    {
        const char* calls[] = { "G4ABC", "DL1XYZ", "EA3QP", "K1ABC", "JA1ZZ", "OH2BH" };
        const double hz[] = { -9000, -5300, -1200, 2600, 6100, 9900 };
        Rng rng(4242); std::vector<Tone> ts;
        for (int i = 0; i < 6; ++i) {
            Keying k; k.wpm = 16 + 4 * i; k.riseMs = (i % 2) ? 0 : 4;
            std::string s; for (int r = 0; r < 4; ++r) s += std::string("CQ CQ DE ") + calls[i] + " " + calls[i] + " K  ";
            Tone t; t.hz = hz[i]; t.snrDb = -2 + 2 * i; t.startSec = rng.uni() * 3; t.env = keyEnvelope(s, k, 48000, rng);
            ts.push_back(t);
        }
        double secs = 0; for (auto& t : ts) secs = std::max(secs, t.startSec + t.env.size() / 48000.0);
        std::vector<cf> iq; std::vector<float> re; synth(ts, 48000, true, secs, rng, iq, re);
        cw::Skimmer::Config c; c.rate = 48000; c.complexIn = true; c.loHz = -20000; c.hiHz = 20000;
        cw::Skimmer sk(c);
        auto r = run(sk, iq, re);
        int found = 0, wrong = 0;
        for (int i = 0; i < 6; ++i) for (auto& s : r.spots) if (s.call == calls[i] && std::fabs(s.offsetHz - hz[i]) < 30) { ++found; break; }
        for (auto& s : r.spots) { bool real = false; for (auto c2 : calls) real |= s.call == c2; wrong += !real; }
        ok(found == 6, "six stations in one 48 kHz segment, each spotted at its own frequency (" + std::to_string(found) + "/6)");
        ok(wrong == 0, "…and no call invented (" + std::to_string(wrong) + " false)");
    }
    // ── through a DecoderHost: a listener's "cw" on 48 kHz audio ──
    {
        DecoderHost::Env e;
        e.dialHz = [] { return 7030000.0; };
        e.rfOfAudio = [](double a) { return 7030000.0 + a - 600.0; };   // this server's CW: the dial is the 600 Hz tone
        auto tap = std::make_shared<SpyTap>();
        e.cwSpotTap = [tap](bool) -> DecoderTapPtr { return tap; };
        DecoderHost h(e, "s1");
        auto p = std::make_shared<Peer>(); h.addPeer(p);
        ok(h.start("cw", "{}") == DecoderHost::Start::Ok && h.kind() == "cw" && h.wantsAudio() && !h.wantsIq(), "\"cw\" starts on audio");
        Rng rng(31); Keying k; k.wpm = 22;
        std::string s; for (int i = 0; i < 4; ++i) s += "CQ CQ DE G4ABC G4ABC K  ";
        Tone t; t.hz = 600; t.snrDb = 6; t.env = keyEnvelope(s, k, 48000, rng);
        std::vector<Tone> ts{ t }; std::vector<cf> iq; std::vector<float> au;
        synth(ts, 48000, false, t.env.size() / 48000.0, rng, iq, au, 0.3);
        for (size_t i = 0; i < au.size(); i += 1024) {
            h.feed(au.data() + i, (int)std::min<size_t>(1024, au.size() - i), 1, NAN);
            while (h.queued() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (int w = 0; w < 200 && h.queued(); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::string got = p->decoded();
        ok(got.find("G4ABC G4ABC") != std::string::npos, "the primary signal's text arrives as 0x01 frames (\"" + got.substr(0, 60) + "\")");
        ok(p->count("\"type\":\"cw_signals\"") > 0 && p->count("\"skimmer\":false") > 0, "the signals list arrives, marked not a skimmer");
        ok(p->count("\"type\":\"cw_spot\"") >= 1 && p->count("\"callsign\":\"G4ABC\"") >= 1, "G4ABC is spotted on the socket");
        bool rfOk = false; { std::lock_guard<std::mutex> lk(tap->m); for (size_t i = 0; i < tap->calls.size(); ++i) rfOk |= tap->calls[i] == "G4ABC" && std::fabs(tap->rf[i] - 7030000.0) < 40; }
        ok(rfOk, "…and the summary tap gets it at its RF frequency (dial + tone − 600)");
        h.stop();
        ok(tap->finished.load() >= 1 && !h.wantsAudio(), "stopping finishes the summary and stops the audio");
        h.shutdown();
    }
    // ── through a DecoderHost: a skimmer's "cwskim" on IQ ──
    {
        DecoderHost::Env e;
        DecoderHost h(e, "s2");
        auto p = std::make_shared<Peer>(); h.addPeer(p);
        const int before = DecoderHost::skimmersRunning().load();
        ok(h.start("cwskim", "{\"width\":24000}") == DecoderHost::Start::Ok && h.wantsIq() && !h.wantsAudio(), "\"cwskim\" starts on IQ, not audio");
        ok(DecoderHost::skimmersRunning().load() == before + 1 && std::fabs(h.iqWidthHz() - 24000) < 1, "the process counts it, and its width is the one asked for");
        Rng rng(77); std::vector<Tone> ts;
        const char* calls[] = { "EA3QP", "OK1RR" }; const double hz[] = { -4000, 5200 };
        for (int i = 0; i < 2; ++i) {
            Keying k; k.wpm = 20 + 6 * i; std::string s; for (int r = 0; r < 4; ++r) s += std::string("CQ TEST ") + calls[i] + " " + calls[i] + " TEST  ";
            Tone t; t.hz = hz[i]; t.snrDb = 6; t.env = keyEnvelope(s, k, 32000, rng); ts.push_back(t);
        }
        double secs = 0; for (auto& t : ts) secs = std::max(secs, t.env.size() / 32000.0);
        std::vector<cf> iq; std::vector<float> re; synth(ts, 32000, true, secs, rng, iq, re);
        for (size_t i = 0; i < iq.size(); i += 2048) {
            h.feedIq(iq.data() + i, (int)std::min<size_t>(2048, iq.size() - i), 32000, 14030000.0);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        for (int w = 0; w < 300 && p->count("OK1RR") == 0; ++w) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ok(p->count("\"callsign\":\"EA3QP\"") >= 1 && p->count("\"callsign\":\"OK1RR\"") >= 1, "both stations spotted from IQ");
        ok(p->count("\"frequency\":14026000") >= 1 && p->count("\"frequency\":14035200") >= 1, "each at centre + offset");
        ok(p->count("\"how\":\"TEST\"") >= 1, "a contest CQ is spotted as TEST");
        ok(p->decoded().empty(), "a skimmer sends no 0x01 text (its text is in the list)");
        h.stop();
        ok(DecoderHost::skimmersRunning().load() == before && !h.wantsIq(), "stopped: no longer counted, no longer wants IQ");
        h.shutdown();
    }
    // ── the gallery's CW summary ──
    {
        gallery::Store st; const std::string dir = tmpDir("gal"); st.setDir(dir);
        gallery::Meta m; m.kind = "cw"; m.origin = "background"; m.freqHz = 7025000; m.mode = "usb"; m.decoder = "cwskim"; m.profile = "Profile 1";
        int64_t now = 1761400000;   // a fixed clock
        {
            bg::Ft8SummaryTap t(st, m, NAN, NAN, [&] { return now; });
            for (int i = 0; i < 5; ++i) { t.cwSpot("DL1ABC", 7012300, 24, 12, "CQ"); now += 20; }
            t.cwSpot("G4ABC", 7025400, 18, 8, "DE"); t.cwSpot("JA1ZZ", 7012800, 30, 3, "CQ");
            now += 70; t.tick();
            t.finish(false);
        }
        const std::string all = slurpTxt(dir);
        ok(all.find("CW spots summary") != std::string::npos, "a CW summary card is written");
        ok(all.find("Spots heard:       7 from 3 stations") != std::string::npos, "it counts spots and stations");
        ok(all.find("Most heard:        DL1ABC (5 spots)") != std::string::npos, "the most-heard call");
        ok(all.find("Busiest:           7012") != std::string::npos, "the busiest kHz");
        ok(all.find("Japan") != std::string::npos && all.find("Germany") != std::string::npos, "stations by country");
        ok(all.find("FT8") == std::string::npos && all.find("FT4") == std::string::npos, "nothing in it says FT8");
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
