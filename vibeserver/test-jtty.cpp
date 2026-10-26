// test-jtty.cpp — JTTY (decoders/jtty_decoder.h): the grammar against the PUBLISHED golden vectors, the FEC, our own
// transmitter round-tripped through our receiver, sensitivity vs SNR, several stations at once, and FALSE FRAMES on
// noise, on RTTY and on real FT8/FT4 recordings.
//
// ★★★ WHAT IS PINNED.
//   • the ten 34-bit vectors the protocol description publishes (jtty_source_encoding.txt "Representative 34-bit
//     vectors") — encoded by us, bit for bit, and decoded back to the same text;
//   • automatic packing's frame counts from the design note's tables (CQ K1ABC CQ = 1, WB9XYZ 599 123 = 2, 599 BRUCE = 2…);
//   • every message round-trips text → frames → tones → audio → our receiver → text;
//   • the decode rate vs SNR (2500 Hz, WSJT-X convention) of one frame at an arbitrary instant and frequency;
//   • six stations at once, overlapping in time, multi-frame messages, decoded and kept apart;
//   • NO frame from an hour of white noise, from synthetic RTTY, or from the real FT8/FT4 recordings
//     (~/VibeSDR-test-audio/ft8, when present — reported NOT RUN otherwise, never a pass);
//   • the receiver's cost (× real time) on this machine.
// ★ Silent: nothing is played. Deterministic: fixed seeds.
#include "decoders/jtty_decoder.h"
#include "vibe_decoder_host.h"

#include <mutex>
#include <thread>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace vibe::jtty;
using vibe::DecoderHost;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double uni() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
    bool haveSpare = false; double spare = 0;
    double gauss() {
        if (haveSpare) { haveSpare = false; return spare; }
        double u, v, r;
        do { u = 2 * uni() - 1; v = 2 * uni() - 1; r = u * u + v * v; } while (r >= 1 || r == 0);
        const double m = std::sqrt(-2 * std::log(r) / r);
        spare = v * m; haveSpare = true; return u * m;
    }
};

static uint64_t fromBin(const char* b) { uint64_t v = 0; for (; *b; ++b) v = (v << 1) | (uint64_t)(*b == '1'); return v; }
/** The transmitted audio of a message: all its frames back to back. */
static void transmit(const std::vector<uint64_t>& frames, double f0, float amp, std::vector<float>& out, size_t at) {
    std::vector<uint8_t> tones;
    for (uint64_t w : frames) { uint8_t t[kFrameSyms]; encodeFrame(w, t); tones.insert(tones.end(), t, t + kFrameSyms); }
    synthesise(tones.data(), (int)tones.size(), f0, 12000, amp, out, at);
}
/** Signal amplitude for an SNR in 2500 Hz with unit-variance white noise at 12 kHz. */
static float ampFor(double snrDb) { return (float)std::sqrt(2.0 * std::pow(10.0, snrDb / 10.0) * 2500.0 / 6000.0); }
static void addNoise(std::vector<float>& x, Rng& r) { for (auto& v : x) v += (float)r.gauss(); }

/** A decoder socket that remembers what it was sent. */
struct Peer : vibe::DecoderPeer {
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

struct Heard { std::vector<Frame> frames; std::vector<std::pair<int, std::string>> messages; std::vector<Spot> spots; };
static void run(Decoder& d, const std::vector<float>& x, Heard& h) {
    d.onFrame = [&](const Frame& f) { h.frames.push_back(f); };
    d.onMessage = [&](int id, double, const std::string& t, bool complete) { if (complete) h.messages.push_back({ id, t }); };
    d.onSpot = [&](const Spot& s) { h.spots.push_back(s); };
    for (size_t i = 0; i < x.size(); i += 480) d.feed(x.data() + i, (int)std::min<size_t>(480, x.size() - i));
    std::vector<float> tail(12000 * 6, 0.0f);       // flush: let open messages time out
    d.feed(tail.data(), (int)tail.size());
}

static const char* const kCalls[] = { "K1ABC", "WB9XYZ", "JA6DEF", "G4ABC", "DL1XYZ", "VK2AB", "F5ABC", "PA3XYZ", "W1AW",
                                      "KA1ABC", "OH3FM", "M0ABC", "2E0ABC", "EA4XY", "SP9ZZZ", "N0AX" };

/** A WAV file as 12 kHz mono floats (16-bit PCM or 32-bit float; 48 kHz averaged by four). false = not there. */
static bool readWav(const std::string& path, std::vector<float>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    int fmt = 0, ch = 0, rate = 0, bits = 0; size_t p = 12, dataAt = 0, dataLen = 0;
    while (p + 8 <= b.size()) {
        uint32_t len; std::memcpy(&len, b.data() + p + 4, 4);
        if (!std::memcmp(b.data() + p, "fmt ", 4)) {
            uint16_t v; std::memcpy(&v, b.data() + p + 8, 2); fmt = v;
            std::memcpy(&v, b.data() + p + 10, 2); ch = v;
            uint32_t r; std::memcpy(&r, b.data() + p + 12, 4); rate = (int)r;
            std::memcpy(&v, b.data() + p + 22, 2); bits = v;
            if (fmt == 0xFFFE && len >= 26) { std::memcpy(&v, b.data() + p + 32, 2); fmt = v; }
        } else if (!std::memcmp(b.data() + p, "data", 4)) { dataAt = p + 8; dataLen = std::min<size_t>(len, b.size() - dataAt); break; }
        p += 8 + len + (len & 1);
    }
    if (!dataAt || ch < 1 || (rate != 12000 && rate != 48000)) return false;
    const int bps = bits / 8; const size_t n = dataLen / (size_t)(bps * ch);
    std::vector<float> mono(n);
    for (size_t i = 0; i < n; ++i) {
        const char* s = b.data() + dataAt + i * (size_t)(bps * ch);
        if (fmt == 3 && bits == 32) { float v; std::memcpy(&v, s, 4); mono[i] = v; }
        else if (fmt == 1 && bits == 16) { int16_t v; std::memcpy(&v, s, 2); mono[i] = v / 32768.0f; }
        else return false;
    }
    out.clear();
    if (rate == 12000) out = mono;
    else for (size_t i = 0; i + 4 <= n; i += 4) out.push_back(0.25f * (mono[i] + mono[i + 1] + mono[i + 2] + mono[i + 3]));
    return true;
}

int main() {
    std::printf("JTTY — decoders/jtty_decoder (%s)\n", Decoder::simdName());

    // ── 1. The published 34-bit vectors ─────────────────────────────────────────────────────────────────────────
    std::printf("\n── the published 34-bit vectors ──\n");
    struct V { const char* meaning; const char* bin; uint64_t hex; Atom a; bool eom; const char* text; };
    std::vector<V> vs;
    auto atom = [](Kind k, int sub, int role, int value, int value2, const char* text) {
        Atom a; a.kind = k; a.sub = sub; a.role = role; a.value = value; a.value2 = value2; a.text = text; return a; };
    vs.push_back({ "CQ K1ABC CQ", "0000100110111101111000110101000001", 0x026F78D41ull, atom(Kind::Call, kCq, 0, 0, 0, "K1ABC"), true, "CQ K1ABC CQ" });
    vs.push_back({ "SERIAL 123, more", "1000000000000001111011000000001000", 0x20007B008ull, atom(Kind::ExchNum, kSerial, 1, 123, 0, ""), false, "599 123" });
    vs.push_back({ "SERIAL 123, EOM", "1000000000000001111011000000001001", 0x20007B009ull, atom(Kind::ExchNum, kSerial, 1, 123, 0, ""), true, "599 123" });
    vs.push_back({ "STATE CA", "1000000000000110111010000000011001", 0x2001BA019ull, atom(Kind::ExchLoc, kStateProvince, 1, 0, 0, "CA"), true, "599 CA" });
    vs.push_back({ "ZONE_LOC3 05 NWT", "0000001011011110010000110100101001", 0x00B790D29ull, atom(Kind::ExchPair, kZoneLoc3, 0, 5, 0, "NWT"), true, "599 05 NWT" });
    vs.push_back({ "CLASS_SECTION 1D EMA", "0010000010110001011000000000101001", 0x082C58029ull, atom(Kind::ExchPair, kClassSection, 0, 1, 11, "D"), true, "1D EMA" });
    vs.push_back({ "NUM_TIME 156 1749", "1000000100111001000010110100111001", 0x204E42D39ull, atom(Kind::NumTime, 0, 1, 156, 17 * 60 + 49, ""), true, "599 156 1749" });
    vs.push_back({ "CONTROL AGN?", "0000000000000000000000000001001001", 0x000000049ull, atom(Kind::Control, 0, 0, 0, 0, ""), true, "AGN?" });
    vs.push_back({ "GRID4 FN42", "0001001010000110011000000001001001", 0x04A198049ull, atom(Kind::Grid4, 0, 0, 0, 0, "FN42"), true, "FN42" });
    vs.push_back({ "TEXT5 HELLO", "0100010011100101010101010110001101", 0x11395558Dull, atom(Kind::Text5, 0, 0, 0, 0, "HELLO"), true, "HELLO" });
    for (auto& v : vs) {
        uint64_t w = 0;
        const bool packed = packAtom(v.a, v.eom, w);
        Atom back; bool eom = false;
        const bool un = unpackAtom(v.hex, back, eom);
        ok(packed && w == v.hex && w == fromBin(v.bin) && un && eom == v.eom && render(back) == v.text,
           std::string(v.meaning) + " → " + render(back));
    }
    // validity: the sentinel, the reserved bit, reserved families and subtypes, a token with a leading zero
    { Atom a; bool e; ok(!unpackAtom(0x000000001ull, a, e), "the all-zero grammar word is invalid"); }
    { Atom a; bool e; ok(!unpackAtom(0x026F78D43ull, a, e), "reserved bit 33 set is invalid"); }
    { Atom a; bool e; uint64_t fam5 = ((((uint64_t)1 << 3) | 5u) << 2 | 2u) << 2; ok(!unpackAtom(fam5, a, e), "family 101 (PROFILED) is invalid in version 1"); }
    { Atom a; bool e; uint64_t ctl18 = ((((uint64_t)18 << 16) << 3 | 4u) << 2 | 2u) << 2; ok(!unpackAtom(ctl18, a, e), "control phrase 18 is invalid"); }
    { Atom x = atom(Kind::ExchLoc, kGenericQth, 1, 0, 0, "0AB"); uint64_t w; ok(!packAtom(x, true, w), "a three-character token with a leading zero is not canonical"); }
    { Atom x = atom(Kind::Call, kCall, 0, 0, 0, "QA1ABC"); uint64_t w; ok(!packAtom(x, true, w), "a call starting with Q is not a JTTY call"); }
    { Atom x = atom(Kind::Call, kCall, 0, 0, 0, "PJ4/K1ABC"); uint64_t w; ok(!packAtom(x, true, w), "a compound call is not a JTTY call (it goes as TEXT5)"); }
    // every TEXT5 character and every call action round-trips
    {
        bool all = true;
        for (int i = 0; i < 64; i += 5) {
            Atom t; t.kind = Kind::Text5; t.text = std::string(kAlphabet + i, std::min(5, 64 - i)); while (t.text.size() < 5) t.text += ' ';
            uint64_t w; Atom b; bool e; all = all && packAtom(t, false, w) && unpackAtom(w, b, e) && b.text == t.text;
        }
        for (auto c : kCalls)
            for (int act = 0; act <= 5; ++act) {
                Atom t = atom(Kind::Call, act, 0, 0, 0, c); uint64_t w; Atom b; bool e;
                all = all && packAtom(t, false, w) && unpackAtom(w, b, e) && b.text == c && b.sub == act;
            }
        ok(all, "the 64-character alphabet and six call actions × sixteen calls round-trip");
    }

    // ── 2. Automatic packing ────────────────────────────────────────────────────────────────────────────────────
    std::printf("\n── automatic packing (the design note's frame counts) ──\n");
    struct P { const char* text; int frames; };
    const P packs[] = { { "CQ K1ABC CQ", 1 }, { "CQ KA1ABC CQ", 1 }, { "WB9XYZ", 1 }, { "WB9XYZ TU CQ KA1ABC CQ", 2 },
                        { "WB9XYZ 599 123", 2 }, { "599 123", 1 }, { "599 MA", 1 }, { "599 FN42", 1 }, { "1D EMA", 1 },
                        { "599 001", 2 }, { "599 05", 2 }, { "599 BRUCE", 2 }, { "TU KA1ABC CQ", 1 }, { "JA6DEF AGN?", 1 },
                        { "TU NOW JA6DEF 599 102", 2 }, { "WB9XYZ NR?", 2 },
                        { "THE QUICK BROWN FOX JUMPED OVER THE LAZY DOG.", 9 } };
    for (auto& p : packs) {
        std::vector<uint64_t> fr;
        const bool good = packMessage(p.text, fr);
        std::vector<Atom> atoms;
        bool valid = good;
        for (size_t i = 0; i < fr.size(); ++i) { Atom a; bool e; valid = valid && unpackAtom(fr[i], a, e) && e == (i + 1 == fr.size()); atoms.push_back(a); }
        const std::string back = renderMessage(atoms);
        ok(valid && (int)fr.size() == p.frames && back == normalise(p.text),
           std::string("\"") + p.text + "\" → " + std::to_string(fr.size()) + " frame(s), reads back \"" + back + "\"");
    }
    { std::vector<uint64_t> fr; ok(packMessage("pj4/ka1abc 599 124 ¬", fr) && renderMessage([&] { std::vector<Atom> v; for (auto w : fr) { Atom a; bool e; unpackAtom(w, a, e); v.push_back(a); } return v; }()) == "PJ4/KA1ABC 599 124 #",
          "lower case folded, a compound call as text, an unsupported character as #"); }

    // ── 3. FEC: encode → perfect metrics → our receiver's list decoder, via clean audio ─────────────────────────────
    std::printf("\n── clean round trips (text → frames → tones → audio → text) ──\n");
    {
        const char* msgs[] = { "CQ K1ABC CQ", "WB9XYZ 599 101", "TU NOW JA6DEF 599 102", "1D EMA", "599 156 1749",
                               "HELLO WORLD THIS IS JTTY", "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?!\"#$%,&*()_'=[]{}<>|:;" };
        for (auto m : msgs) {
            std::vector<uint64_t> fr; packMessage(m, fr);
            std::vector<float> x(12000, 0.0f);
            transmit(fr, 1000.0 + 3.3, 0.5f, x, 6003);
            x.resize(x.size() + 12000, 0.0f);
            Decoder::Config c; Decoder d(c); d.setStartUtcMs(0);
            Heard h; run(d, x, h);
            const std::string got = h.messages.empty() ? "(nothing)" : h.messages.front().second;
            ok(h.messages.size() == 1 && got == normalise(m) && h.frames.size() == fr.size(),
               std::string("\"") + m + "\" (" + std::to_string(fr.size()) + " frames) → \"" + got + "\"");
        }
    }

    // ── 4. Sensitivity: one frame, arbitrary instant and frequency, white noise ─────────────────────────────────
    std::printf("\n── one frame at an arbitrary instant and frequency, white noise (SNR in 2500 Hz) ──\n");
    double rateAt[32] = {0}; int iSnr = 0;
    const double snrs[] = { -8, -12, -14, -15, -16, -17, -18, -19, -20 };
    double cpuSec = 0, audioSec = 0;
    for (double snr : snrs) {
        int good = 0, bad = 0; const int N = 40;
        for (int k = 0; k < N; ++k) {
            Rng r(1000 + k * 7 + (uint64_t)(-snr) * 1000);
            const char* call = kCalls[r.next() % 16];
            std::vector<uint64_t> fr; packMessage(std::string("CQ ") + call + " CQ", fr);
            const double f0 = 300 + r.uni() * 2400;
            const size_t start = 6000 + (size_t)(r.uni() * 12000);
            std::vector<float> x((size_t)(start + 22656 + 18000), 0.0f);
            transmit(fr, f0, ampFor(snr), x, start);
            addNoise(x, r);
            Decoder::Config c; Decoder d(c); d.setStartUtcMs(0);
            Heard h;
            const auto t0 = std::chrono::steady_clock::now();
            run(d, x, h);
            cpuSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            audioSec += (x.size() + 72000) / 12000.0;
            bool hit = false;
            for (auto& f : h.frames) {
                if (f.bits == fr[0] && std::fabs(f.audioHz - f0) < 3 && std::llabs(f.sample - (int64_t)start) < 64) hit = true;
                else { ++bad; if (std::getenv("VIBE_JTTY_DEBUG")) std::printf("      wrong: %s at %.1f (sent %.1f) sample %lld (sent %zu) bits %s agree %d rank %d\n", render(f.atom).c_str(), f.audioHz, f0, (long long)f.sample, start, f.bits == fr[0] ? "same" : "diff", f.agree, f.rank); }
            }
            good += hit;
            if (!hit && std::getenv("VIBE_JTTY_DEBUG")) std::printf("      missed: %s at %.1f Hz, sample %zu (seed %d)\n", call, f0, start, k);
        }
        rateAt[iSnr++] = (double)good / N;
        std::printf("    %+5.0f dB: %2d/%d decoded, %d wrong\n", snr, good, N, bad);
        if (bad) ok(false, "a wrong frame at " + std::to_string((int)snr) + " dB");
    }
    ok(rateAt[0] == 1.0 && rateAt[1] >= 0.975, "every frame at −8 dB, all but one at −12 dB");
    ok(rateAt[2] >= 0.85, "≥ 85 % at −14 dB");
    ok(rateAt[4] >= 0.35, "≥ 35 % at −16 dB");
    ok(rateAt[5] >= 0.1, "≥ 10 % at −17 dB");
    std::printf("    cost: %.1f s of audio in %.2f s CPU = %.0f× real time\n", audioSec, cpuSec, audioSec / cpuSec);

    // ── 5. Six stations at once, overlapping, multi-frame ───────────────────────────────────────────────────────
    std::printf("\n── six stations at once (overlapping in time, multi-frame, −12 dB each) ──\n");
    {
        struct Tx { const char* msg; double f0; double at; };
        const Tx txs[] = { { "CQ KA1ABC CQ", 400, 0.3 }, { "WB9XYZ 599 101", 600, 1.1 }, { "TU NOW JA6DEF 599 102", 820, 0.0 },
                           { "THE QUICK BROWN FOX", 1100, 2.7 }, { "1D EMA", 1500, 0.9 }, { "CQ DL1XYZ CQ", 2400, 3.3 },
                           { "599 FN42", 1500, 5.0 } };   // ★ the last follows 1D EMA on the same frequency, later
        Rng r(77);
        std::vector<float> x((size_t)(12000 * 16), 0.0f);
        for (auto& t : txs) { std::vector<uint64_t> fr; packMessage(t.msg, fr); transmit(fr, t.f0, ampFor(-12), x, (size_t)(t.at * 12000)); }
        addNoise(x, r);
        Decoder::Config c; Decoder d(c); d.setStartUtcMs(0);
        Heard h; run(d, x, h);
        int found = 0;
        for (auto& t : txs) {
            bool hit = false;
            for (auto& m : h.messages) if (m.second == t.msg) hit = true;
            found += hit;
            if (!hit) std::printf("    missed \"%s\" at %.0f Hz\n", t.msg, t.f0);
        }
        int wrong = 0;
        for (auto& m : h.messages) { bool known = false; for (auto& t : txs) if (m.second == t.msg) known = true; if (!known) { ++wrong; std::printf("    other: \"%s\"\n", m.second.c_str()); } }
        ok(found == 7 && wrong == 0, std::to_string(found) + "/7 messages decoded whole, " + std::to_string(wrong) + " other");
        const auto sv = d.signals();
        bool sorted = true; for (size_t i = 1; i < sv.size(); ++i) sorted = sorted && sv[i - 1].audioHz <= sv[i].audioHz;
        ok(sv.size() == 6 && sorted, std::to_string(sv.size()) + " signals listed, lowest frequency first");
        int cq = 0; for (auto& s : h.spots) if (s.how == "CQ") ++cq;
        ok(cq == 2, std::to_string(h.spots.size()) + " spots, " + std::to_string(cq) + " calling CQ (KA1ABC, DL1XYZ)");
    }

    // ── 6. Continuation: a long message with a weak middle ──────────────────────────────────────────────────────
    std::printf("\n── a long message at −17 dB (the next frame is tried where it must be) ──\n");
    {
        int whole[2] = { 0, 0 }; int framesGot[2] = { 0, 0 };
        const std::string msg = "HELLO FROM THE VIBESDR JTTY TEST";
        std::vector<uint64_t> fr; packMessage(msg, fr);
        for (int cont = 0; cont < 2; ++cont)
            for (int k = 0; k < 10; ++k) {
                Rng r(500 + k);
                std::vector<float> x((size_t)(12000 * 2 + fr.size() * 22656 + 24000), 0.0f);
                transmit(fr, 900 + 37 * k, ampFor(-17), x, 12000);
                addNoise(x, r);
                Decoder::Config c; c.continuation = cont != 0; Decoder d(c); d.setStartUtcMs(0);
                Heard h; run(d, x, h);
                for (auto& f : h.frames) { bool mine = false; for (auto w : fr) mine = mine || w == f.bits; framesGot[cont] += mine; }
                for (auto& m : h.messages) if (m.second == normalise(msg)) whole[cont]++;
            }
        std::printf("    %zu frames × 10: without continuation %d frames, %d whole; with %d frames, %d whole\n",
                    fr.size(), framesGot[0], whole[0], framesGot[1], whole[1]);
        ok(framesGot[1] >= framesGot[0], "continuation never loses frames");
    }

    // ── 7. False frames ─────────────────────────────────────────────────────────────────────────────────────────
    std::printf("\n── false frames ──\n");
    {
        const int minutes = std::getenv("VIBE_JTTY_NOISE_MIN") ? std::atoi(std::getenv("VIBE_JTTY_NOISE_MIN")) : 60;
        Rng r(4242);
        Decoder::Config c; Decoder d(c); d.setStartUtcMs(0);
        Heard h;
        d.onFrame = [&](const Frame& f) { h.frames.push_back(f); };
        std::vector<float> x(12000 * 10);
        const auto t0 = std::chrono::steady_clock::now();
        for (int s = 0; s < minutes * 6; ++s) { for (auto& v : x) v = (float)r.gauss(); d.feed(x.data(), (int)x.size()); }
        const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto st = d.scoreStats();
        std::printf("    %d min of white noise: %llu sync candidates, %llu continuations, %llu frames; mean score %.3f, cells above %.2g/s; %.0f× real time\n",
                    minutes, (unsigned long long)d.candidates(), (unsigned long long)d.continuations(), (unsigned long long)d.framesDecoded(),
                    st.meanScore, (double)st.above / (minutes * 60.0), minutes * 60.0 / cpu);
        for (auto& f : h.frames) std::printf("      FALSE: \"%s\" at %.0f Hz, rank %d, agree %d, snr %.1f\n", render(f.atom).c_str(), f.audioHz, f.rank, f.agree, f.snrDb);
        ok(h.frames.empty(), "no frame from " + std::to_string(minutes) + " minutes of white noise");
    }
    {   // synthetic RTTY: 45.45 Bd, 170 Hz shift, random characters, at a high level in noise — ten minutes
        Rng r(99);
        Decoder::Config c; Decoder d(c); d.setStartUtcMs(0);
        size_t nf = 0;
        d.onFrame = [&](const Frame& f) { ++nf; std::printf("      FALSE (RTTY): \"%s\"\n", render(f.atom).c_str()); };
        const double baud = 45.45, mark = 1445, space = 1275;
        double ph = 0; int bitLeft = 0; bool bit = true;
        std::vector<float> x(12000);
        for (int s = 0; s < 600; ++s) {
            for (auto& v : x) {
                if (bitLeft <= 0) { bit = (r.next() & 1) != 0; bitLeft = (int)std::lround(12000 / baud); }
                --bitLeft;
                ph += 2 * M_PI * (bit ? mark : space) / 12000; if (ph > 2 * M_PI) ph -= 2 * M_PI;
                v = (float)(3.0 * std::sin(ph) + r.gauss());
            }
            d.feed(x.data(), (int)x.size());
        }
        ok(nf == 0, "no frame from ten minutes of strong RTTY (" + std::to_string(d.candidates()) + " candidates tried)");
    }
    {   // the real FT8/FT4 recordings from Stuart's UberSDR (busy bands, many signals, 12 kHz)
        const char* home = std::getenv("HOME");
        const std::string dir = std::string(home ? home : "") + "/VibeSDR-test-audio/ft8/";
        const char* files[] = { "ft8_20m_14074000_20261025T2044.wav", "ft8_40m_7074000_20261025T2118.wav",
                                "ft8_80m_3573000_20261025T2118.wav", "ft4_40m_7047500_20261025T2118.wav" };
        int have = 0; size_t nf = 0; double sec = 0; uint64_t cands = 0;
        for (auto fn : files) {
            std::vector<float> x;
            if (!readWav(dir + fn, x)) continue;
            ++have; sec += x.size() / 12000.0;
            Decoder::Config c; Decoder d(c); d.setStartUtcMs(0);
            d.onFrame = [&](const Frame& f) { ++nf; std::printf("      FALSE (%s): \"%s\" at %.0f Hz agree %d\n", fn, render(f.atom).c_str(), f.audioHz, f.agree); };
            for (size_t i = 0; i < x.size(); i += 480) d.feed(x.data() + i, (int)std::min<size_t>(480, x.size() - i));
            cands += d.candidates();
        }
        if (!have) std::printf("    NOT RUN: no FT8 recordings in %s\n", dir.c_str());
        else ok(nf == 0, "no frame from " + std::to_string((int)(sec / 60)) + " min of real FT8/FT4 band audio (" + std::to_string(cands) + " candidates tried)");
    }

    // ── 7b. REAL JTTY: the WSJT-X sample, and real transmissions off Stuart's UberSDR ────────────────────────────
    std::printf("\n── real JTTY (~/VibeSDR-test-audio/jtty, NOT RUN when absent) ──\n");
    {
        const char* home = std::getenv("HOME");
        const std::string dir = std::string(home ? home : "") + "/VibeSDR-test-audio/jtty/";
        struct Real { const char* file; std::vector<std::string> expect; const char* what; };
        const Real reals[] = {
            { "wsjtx_sample_260807_134110.wav", { "RAN ALL NIGHT ON BAND NOISE - NO FALSE DECODES!" }, "the WSJT-X 3.2 user guide's sample recording, whole" },
            { "jtty_7090000_20261026T1653.wav", { "CQ F4ELU/QRP CQ", "CQ DK7OG JO62" },
              "40 m off air: F4ELU/QRP (PSKReporter: JTTY at 7.091370) and DK7OG calling CQ" },
            { "jtty_14090000_20261026T1706.wav", { "DL5AXI DE N4", "YSA FM08" },
              "20 m off air: N4YSA (FM08) calling DL5AXI (PSKReporter: N4YSA JTTY at 14.091014)" },
        };
        for (auto& r : reals) {
            std::vector<float> x;
            if (!readWav(dir + r.file, x)) { std::printf("    NOT RUN: %s%s\n", dir.c_str(), r.file); continue; }
            Decoder::Config c; Decoder d(c); d.setStartUtcMs(0);
            Heard h; run(d, x, h);
            bool all = true;
            for (auto& e : r.expect) {
                bool hit = false;
                for (auto& m : h.messages) if (m.second.find(e) != std::string::npos) hit = true;
                all = all && hit;
            }
            std::string got; for (auto& m : h.messages) got += " \"" + m.second.substr(0, 60) + "\"";
            ok(all, std::string(r.what) + " —" + got);
        }
    }

    // ── 8. Through a DecoderHost (what the server runs) ─────────────────────────────────────────────────────────
    std::printf("\n── \"jtty\" through a DecoderHost ──\n");
    {
        DecoderHost::Env e;
        e.dialHz = [] { return 14090000.0; };
        e.rfOfAudio = [](double a) { return 14090000.0 + a; };   // USB
        DecoderHost h(e, "s1");
        auto p = std::make_shared<Peer>(); h.addPeer(p);
        ok(h.start("jtty", "{}") == DecoderHost::Start::Ok && h.kind() == "jtty" && h.wantsAudio(), "\"jtty\" starts on audio");
        // two stations at 48 kHz: a CQ at 1000 Hz, then an exchange at 1500 Hz overlapping it
        std::vector<float> a(48000 * 8, 0.0f);
        { std::vector<uint64_t> fr; packMessage("CQ G4ABC CQ", fr); std::vector<uint8_t> t; for (auto w : fr) { uint8_t x[kFrameSyms]; encodeFrame(w, x); t.insert(t.end(), x, x + kFrameSyms); }
          synthesise(t.data(), (int)t.size(), 1000.0, 48000, 0.2f, a, 24000); }
        { std::vector<uint64_t> fr; packMessage("DL1XYZ 599 042 <HI>", fr); std::vector<uint8_t> t; for (auto w : fr) { uint8_t x[kFrameSyms]; encodeFrame(w, x); t.insert(t.end(), x, x + kFrameSyms); }
          a.resize(48000 * 16, 0.0f); synthesise(t.data(), (int)t.size(), 1500.0, 48000, 0.2f, a, 60000); }
        Rng r(3); for (auto& v : a) v += (float)(0.02 * r.gauss());
        for (size_t i = 0; i < a.size(); i += 1024) {
            h.feed(a.data() + i, (int)std::min<size_t>(1024, a.size() - i), 1, NAN);
            while (h.queued() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::vector<float> z(48000 * 3, 0.0f); h.feed(z.data(), (int)z.size(), 1, NAN);
        for (int w = 0; w < 300 && h.queued(); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        std::vector<float> z2(4800, 0.0f); h.feed(z2.data(), (int)z2.size(), 1, NAN);   // ★ a tick after a second: the list
        for (int w = 0; w < 300 && h.queued(); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::string txt = p->decoded();
        std::printf("    text: %s", txt.c_str());
        ok(txt.find("14.0910  CQ G4ABC CQ\n") != std::string::npos, "a text line per message, at its RF (dial + tone 0)");
        ok(txt.find("14.0915  DL1XYZ 599 042 <HI>\n") != std::string::npos, "the second station's, punctuation and all");
        ok(p->count("\"type\":\"jtty_spot\"") == 2 && p->count("\"callsign\":\"G4ABC\"") >= 1 && p->count("\"how\":\"CQ\"") >= 1,
           "spots: G4ABC calling CQ, DL1XYZ in a QSO");
        ok(p->count("\"type\":\"jtty_signals\"") >= 1 && p->count("\"call\":\"G4ABC\"") >= 1 && p->count("\"text\":\"DL1XYZ 599 042 <HI>\"") >= 1,
           "the signals list: each station's call and messages");
        h.shutdown();
    }

    std::printf("\n%s — %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
