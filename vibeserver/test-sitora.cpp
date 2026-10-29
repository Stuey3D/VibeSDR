// test-sitora.cpp — SITOR-A, the maritime ARQ teleprinter (decoders/sitor_a.h), and its plumbing (vibe_sitora_run.h
// through vibe_decoder_host.h, background decoding's table and the gallery's text run).
//
// ★★★ WHAT IS PINNED (ITU-R M.476-5, M.625-4, M.491-1; synthetic links whose every turn is known — tools/sitor-bench/
//     sitor_synth.h):
//   • the code: 35 valid words, every signal of Tables 1 and 2 as printed in the standard, and the control signals that
//     share a word with a letter (CS1 = L, CS2 = combination 32, CS3 = N, CS4 = G, CS5 = H);
//   • identities: M.491's own examples (364775427 = P E A R D B Y, 32610 = Q C X T, 1234 = X Q K M) both ways, and
//     M.625's check-sum example (Z E R);
//   • a whole link — call, identification, traffic, a change-over each way, the end — read with every repeated block
//     printed ONCE, both identities and their numbers, the called station's check-sums confirmed, the master found;
//   • the same at a weak 0 dB, the tones the other way round, and at 12 kHz;
//   • noise, a continuous SITOR-B stream, 100 Bd dot bursts (as beside the 4209.5 kHz NAVTEX) and a 50 Bd stream print
//     nothing;
//   • "sitora" through a DecoderHost: the text with each station's label, the 0x03 link states, the status JSON;
//     background decoding's row and attach message, and the gallery files it as a text run.
#include "vibe_decoder_host.h"
#include "vibe_bgdecode.h"
#include "../tools/sitor-bench/sitor_synth.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

using namespace vibe;
using namespace vibe::sitora;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

struct Run { std::string text; std::vector<int> sides; Status st; std::vector<std::string> events; };
static Run decode(const std::vector<float>& a, double rate, double centre = 1000) {
    Receiver rx(rate, centre);
    Run r; int side = -1;
    rx.onChar = [&](int s, char32_t c) { r.text += (char)c; if (s != side) { r.sides.push_back(s); side = s; } };
    rx.onEvent = [&](const std::string& e) { r.events.push_back(e); };
    for (size_t i = 0; i < a.size(); i += 960) rx.process(a.data() + i, (int)std::min<size_t>(960, a.size() - i));
    r.st = rx.status();
    return r;
}
static bool has(const Run& r, const std::string& e) { for (auto& x : r.events) if (x.find(e) != std::string::npos) return true; return false; }
static int errors(const std::string& a, const std::string& b) {   // edit distance, for a CER
    std::vector<int> p(b.size() + 1), q(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) p[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        q[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) q[j] = std::min({ p[j] + 1, q[j - 1] + 1, p[j - 1] + (a[i - 1] != b[j - 1]) });
        std::swap(p, q);
    }
    return p[b.size()];
}

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
    std::vector<int> states() { std::lock_guard<std::mutex> lk(m); std::vector<int> s; for (auto& f : bin) if (f.size() == 2 && f[0] == 0x03) s.push_back(f[1]); return s; }
    std::string lastJson(const std::string& type) { std::lock_guard<std::mutex> lk(m); for (auto it = txt.rbegin(); it != txt.rend(); ++it) if (it->find(type) != std::string::npos) return *it; return ""; }
};

int main() {
    std::printf("── the code (M.476 Tables 1 and 2, M.625 Table 2)\n");
    {
        int n = 0; for (int v = 0; v < 128; v++) n += valid((uint8_t)v);
        ok(n == 35, "35 valid words (four B, three Y)");
        ok(letterSignal(U'A') == sig("BBBYYYB") && figureOf(sig("BBBYYYB")) == U'-' && letterSignal(U'Z') == sig("BBYYYBB") && figureOf(sig("BBYYYBB")) == U'+',
           "A = BBBYYYB (figure '-'), Z = BBYYYBB (figure '+'), as the table prints them");
        ok(figureOf(sig("YBYYBBB")) == U'?' && letterOf(kCr) == 0 && letterOf(kLf) == 0 && kSpace == sig("YYBBBYB"), "B's figure is '?'; CR, LF, space as printed");
        ok(kCs1 == letterSignal(U'L') && kCs2 == kNoInfo && kCs3 == letterSignal(U'N') && kCs4 == letterSignal(U'G') && kCs5 == letterSignal(U'H'),
           "CS1 = L, CS2 = combination 32, CS3 = N, CS4 = G, CS5 = H (the IRS's words share the letters' code)");
        ok(kAlpha == 0x0f && kBeta == 0x33 && kRq == 0x66 && valid(kAlpha) && valid(kBeta) && valid(kRq), "α, β and RQ (the NAVTEX receiver's 0x0F / 0x33 / 0x66)");
        int all = 0; for (int v = 0; v < 128; v++) if (valid((uint8_t)v) && (letterOf((uint8_t)v) || v == kCr || v == kLf || v == kSpace || v == kLtrs || v == kFigs || v == kNoInfo || v == kAlpha || v == kBeta || v == kRq)) all++;
        ok(all == 35, "every valid word is a traffic signal or a service signal");
    }
    std::printf("── identities (M.491-1, M.625 §2.5)\n");
    {
        ok(identitySignals("364775427") == "PEARDBY" && identityNumber("PEARDBY") == "364775427", "364775427 = P E A R D B Y (M.491 Annex II's example), both ways");
        ok(checksums("PEARDBY") == "ZER", "its check-sums are Z E R (M.625 §2.5's example)");
        ok(identitySignals("32610") == "QCXT" && identityNumber("QCXT") == "32610", "32610 = Q C X T (Annex I's example), both ways");
        ok(identitySignals("1234") == "XQKM" && identityNumber("XQKM") == "1234", "a 4-digit 1234 = X Q K M (Table II)");
        bool round = true;
        for (int f = 0; f < 10; f++) { const std::string n = std::to_string(f) + "7305"; if (identityNumber(identitySignals(n)) != n) round = false; }
        ok(round, "every first digit's pattern of set-T places reads back (Table I)");
        ok(identityNumber("PEARDBJ").empty() && identityNumber("VVVVV").empty(), "J is not an identification signal; five signals are no identity");
    }
    using namespace sitorsynth;
    const std::string t1 = "QTH 43.10N 009.25E ETA LA SPEZIA 0600Z\nPLEASE ADVISE BERTH (MV TEST)", t2 = "BERTH 4 CONFIRMED, PILOT AT 0530Z", t3 = "THANKS QRU";
    const Plan L = makeLink("002320204", "364775427", t1, t2, t3, 4, 7);
    std::printf("── a whole link, 15 dB (%zu turns, %d blocks asked for again)\n", L.turns.size(), L.irsRepeats);
    {
        const auto a = render(L, 48000, 1000, 15, -6, 1);
        if (const char* dump = std::getenv("SITOR_DUMP")) {   // the link as a WAV, for tools/sitor-bench/sitor_wav
            FILE* f = std::fopen(dump, "wb");
            auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
            std::fwrite("RIFF", 1, 4, f); w32(36 + (uint32_t)a.size() * 2); std::fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(1); w32(48000); w32(96000); w16(2); w16(16);
            std::fwrite("data", 1, 4, f); w32((uint32_t)a.size() * 2);
            for (float v : a) { const int16_t q = (int16_t)std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767); std::fwrite(&q, 2, 1, f); }
            std::fclose(f);
        }
        const Run r = decode(a, 48000);
        ok(r.text == L.printed, "the text of both stations, every repeated block printed once");
        if (r.text != L.printed) std::printf("     got  «%s»\n     want «%s»\n", r.text.c_str(), L.printed.c_str());
        ok(r.sides.size() == 3 && r.sides[0] == r.sides[2] && r.sides[0] != r.sides[1], "three passages, master–slave–master");
        const int m = r.st.master;
        ok(m >= 0 && r.st.numbers[m] == "364775427" && r.st.ids[m] == "PEARDBY" && r.st.numbers[1 - m] == "002320204",
           "the caller (the master) is 364775427, the called station 002320204");
        ok(r.st.idChecked, "the called station's check-sums match its identity");
        ok(r.sides.size() >= 1 && r.sides[0] == m, "the first passage is the master's");
        ok(r.st.repeats >= (unsigned)L.irsRepeats, "repeats counted (" + std::to_string(r.st.repeats) + ")");
        ok(r.st.link == Link::Ended || r.st.link == Link::NoSignal || r.st.link == Link::Searching, "after the end the receiver lets the link go (ended, or searching again)");
        ok(has(r, "end of communication") && has(r, "over (+?)") && has(r, "call to "), "the call, the change-over and the end are reported");
    }
    std::printf("── weak and other ways round\n");
    {
        const auto a = render(L, 48000, 1000, 0, 0, 2);
        const Run r = decode(a, 48000);
        const int e = errors(r.text, L.printed);
        ok(e * 100 <= 5 * (int)L.printed.size(), "0 dB, both stations equal: CER " + std::to_string(100.0 * e / L.printed.size()).substr(0, 4) + " % (≤ 5 %)");
        const auto b = render(L, 48000, 1000, 15, -6, 3, 0, 12, 30, 0.3, true);
        ok(decode(b, 48000).text == L.printed, "the tones the other way round (LSB): the same text");
        const auto c = render(L, 12000, 1500, 15, -6, 4, 25, -30);
        ok(decode(c, 12000, 1500).text == L.printed, "12 kHz audio, tones at 1500 Hz, +25 / -30 Hz off: the same text");
        const Plan L4 = makeLink("32610", "", t1, t2, t3, 3, 0);
        const Run r4 = decode(render(L4, 48000, 1000, 15, 0, 5), 48000);
        ok(r4.text == L4.printed && r4.st.master >= 0 && r4.st.numbers[1 - r4.st.master] == "32610", "a 4-signal call (selcall 32610, no identification), a third of the blocks repeated");
    }
    std::printf("── what is NOT SITOR-A prints nothing\n");
    {
        std::mt19937 g(9); std::normal_distribution<double> N(0, 0.1);
        std::vector<float> z(48000 * 60); for (auto& v : z) v = (float)N(g);
        ok(decode(z, 48000).text.empty(), "a minute of noise");
        // SITOR-B / NAVTEX: an endless stream of valid words, no gaps — never a 450 ms cycle
        std::vector<uint8_t> words; std::uniform_int_distribution<int> U(0, 127);
        while (words.size() < 60 * 100 / 7) { const int v = U(g); if (valid((uint8_t)v)) words.push_back((uint8_t)v); }
        Plan fec; fec.turns.push_back({ true, words });
        ok(decode(render(fec, 48000, 1000, 20, 0, 6), 48000).text.empty(), "a minute of SITOR-B-like FEC (valid words, no gaps)");
        // 50 Bd FSK (each 100 Bd bit doubled): RTTY-like
        std::vector<uint8_t> r50;
        for (int k = 0; k < 60 * 100 / 7; k++) { uint8_t w = 0; for (int b = 0; b < 7; b++) if (((k * 7 + b) / 2 + k) % 3) w |= (uint8_t)(1 << b); r50.push_back(w); }
        // ★ the 100 Bd dot bursts heard beside the 4209.5 kHz NAVTEX (2026-10-29): alternate bits, 1.5 s every 2.5 s —
        //   every other 7-bit word reads as a valid one; at a lower acquisition bar they locked and printed garbage
        {
            std::vector<float> d(48000 * 60, 0.0f); double ph = 0;
            for (size_t i = 0; i < d.size(); i++) {
                const double t = i / 48000.0, in = std::fmod(t, 2.5);
                if (in < 1.5) { const bool b = ((int)(in * 100)) & 1; ph += 2 * M_PI * (1000 + (b ? 85 : -85)) / 48000; d[i] = (float)(0.25 * std::cos(ph)); }
                d[i] += (float)(0.25 * 0.707 * 0.02 * N(g));
            }
            ok(decode(d, 48000).text.empty(), "a minute of 100 Bd dot bursts (1.5 s every 2.5 s, as on 4209.5 kHz)");
        }
        Plan rt; rt.turns.push_back({ true, r50 });
        ok(decode(render(rt, 48000, 1000, 20, 0, 7), 48000).text.empty(), "a minute of 50 Bd FSK");
    }
    std::printf("── \"sitora\" through a DecoderHost\n");
    {
        DecoderHost::Env e;
        e.dialHz = [] { return 4208500.0; };
        e.rfOfAudio = [](double a) { return 4208500.0 + a; };
        DecoderHost h(e, "s1");
        auto p = std::make_shared<Peer>(); h.addPeer(p);
        ok(h.start("sitora", "{\"center_frequency\":1000}") == DecoderHost::Start::Ok && h.kind() == "sitora" && h.wantsAudio(), "\"sitora\" starts on audio");
        const auto a = render(L, 48000, 1000, 15, -6, 8);
        for (size_t i = 0; i < a.size(); i += 1024) {
            h.feed(a.data() + i, (int)std::min<size_t>(1024, a.size() - i), 1, NAN);
            while (h.queued() > 48000) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (int w = 0; w < 300 && h.queued(); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const std::string txt = p->decoded();
        ok(txt.find("] QTH 43.10N 009.25E ETA LA SPEZIA") != std::string::npos && txt.find("PEARDBY] ") != std::string::npos,
           "the text, its passage labelled with the sender's identity");
        ok(txt.find("[call to VVOTVTM (002320204)]") != std::string::npos && txt.find("[end of communication]") != std::string::npos, "the call and the end are lines in [ ]");
        const auto st = p->states();
        ok(std::find(st.begin(), st.end(), 2) != st.end() && std::find(st.begin(), st.end(), 4) != st.end() && std::find(st.begin(), st.end(), 7) != st.end(),
           "0x03 link states: phasing (2), traffic (4), ended (7)");
        const std::string js = p->lastJson("\"type\":\"sitora_status\"");
        ok(js.find("\"numbers\":[") != std::string::npos && js.find("364775427") != std::string::npos && js.find("\"idChecked\":true") != std::string::npos,
           "the status JSON carries the identities and the check");
        h.shutdown();
    }
    std::printf("── background decoding and the gallery\n");
    {
        const bg::DecoderDef* d = bg::decoderDef("sitora");
        ok(d && bg::decoderFitsDemod("sitora", "usb") && bg::decoderFitsDemod("sitora", "lsb") && !bg::decoderFitsDemod("sitora", "nfm"), "SITOR-A on USB and LSB");
        bg::Vfo f; f.hz = 4208500; f.mode = "usb"; f.dec = "sitora";
        ok(bg::attachMsg(f) == "{\"center_frequency\":1000}" && std::fabs(bg::signalHz(f) - 4209500) < 1, "its attach message, and a USB VFO at 4208.5 listens at 4209.5 kHz");
        ok(bg::galleryKindFor("sitora", "{}") == "sitora", "galleryKindFor(\"sitora\") files it as SITOR-A");
        gallery::Store gs; gallery::Meta m; m.kind = "sitora"; m.freqHz = 4209500; m.mode = "usb";
        ok(bg::makeGalleryTap(gs, m) != nullptr, "kind \"sitora\" has a gallery tap (a text run)");
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
