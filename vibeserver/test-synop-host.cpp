// test-synop-host.cpp — THE SERVER'S RTTY DECODER HOST SENDS EACH WEATHER REPORT AS ITS OWN FRAME (0x08), IN ITS PLACE,
// AND THE GALLERY WRITES IT UNDER ITS REPORT (V12, 2026-10-23).
//
// ★★★ The whole server path short of the socket: DWD-shaped RTTY AUDIO (50 Bd, 450 Hz shift, ITA2 with FIGS, '=' sent as
//     the figures V) → DecoderHost (the real FskDecoder, then RttyAuto — the clients' default) → a decoder socket that
//     records every frame, and the gallery's tap writing a .txt. Pins:
//       · the 0x08 frame arrives, carries the translation (Tempelhof, the wind, the temperature), and is valid JSON-ish;
//       · it lands AFTER the 0x01 text that ended its report's line, and BEFORE the next report's text;
//       · the raw groups are untouched in the 0x01 text;
//       · NAVTEX (plain language) never gets one;
//       · the gallery's RTTY .txt has the translation inline, indented, under the raw report.
// ★ SILENT: synthetic audio into the decoder only. Real-time-paced on the host's queue, ~2 s of CPU.
#include "vibe_bgdecode.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (c) passes++; else fails++; }

struct Peer : DecoderPeer {
    std::mutex m;
    std::vector<std::vector<uint8_t>> bin;
    bool open() const override { return true; }
    void binary(const uint8_t* d, size_t n) override { std::lock_guard<std::mutex> lk(m); bin.emplace_back(d, d + n); }
    void text(const std::string&) override {}
    /** The frames in order as a readable trace: text as-is, a report as ⟦…line…⟧. */
    std::string trace(std::vector<std::string>* reports = nullptr) {
        std::lock_guard<std::mutex> lk(m); std::string o;
        for (auto& f : bin) {
            if (f.size() >= 13 && f[0] == 0x01) o.append((const char*)f.data() + 13, f.size() - 13);
            else if (f.size() >= 5 && f[0] == 0x08) {
                const uint32_t len = (uint32_t)f[1] << 24 | (uint32_t)f[2] << 16 | (uint32_t)f[3] << 8 | f[4];
                const std::string j((const char*)f.data() + 5, std::min<size_t>(len, f.size() - 5));
                if (reports) reports->push_back(j);
                o += "\xe2\x9f\xa6" + j + "\xe2\x9f\xa7";
            }
        }
        return o;
    }
};

// ── DWD-shaped RTTY: ITA2 5N1.5, LTRS/FIGS, '=' as the figures V ───────────────────────────────────────────────────
static const char* LT = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\0MXV\0";
static int figsCode(char c) {
    switch (c) {
    case '3': return 1; case '-': return 3; case '8': return 6; case '7': return 7; case '4': return 10; case '5': return 16;
    case '2': return 19; case '6': return 21; case '0': return 22; case '1': return 23; case '9': return 24; case '.': return 28;
    case '/': return 29; case '=': return 30; default: return -1;
    }
}
static std::vector<int> bits(const std::string& text) {
    std::vector<int> b;
    auto code = [&](int c) { b.push_back(-1); for (int k = 0; k < 5; k++) b.push_back((c >> k & 1) ? 1 : -1); b.push_back(1); b.push_back(2); };
    bool figs = false;
    for (int i = 0; i < 4; i++) code(31);
    for (char ch : text) {
        if (ch == ' ' || ch == '\r' || ch == '\n') { code(ch == ' ' ? 4 : ch == '\r' ? 8 : 2); continue; }
        const int f = figsCode(ch);
        if (f >= 0) { if (!figs) { code(27); figs = true; } code(f); continue; }
        int l = -1; for (int i = 1; i < 32; i++) if (LT[i] == ch) l = i;
        if (l > 0) { if (figs) { code(31); figs = false; } code(l); }
    }
    for (int i = 0; i < 6; i++) code(31);
    return b;
}
static std::vector<float> audio(const std::vector<int>& bt, double baud = 50, double centre = 1000, double shift = 450) {
    std::vector<float> s; double ph = 0, acc = 0; const double fs = 48000, spb = fs / baud;
    auto tone = [&](double f, double n) { acc += n; const long k = std::lround(acc); while ((long)s.size() < k) { ph += 2 * M_PI * f / fs; s.push_back(0.3f * (float)std::sin(ph)); } };
    tone(centre + shift / 2, fs);
    for (int v : bt) { if (v == 2) { tone(centre + shift / 2, spb / 2); continue; } tone(centre + (v > 0 ? 1 : -1) * shift / 2, spb); }
    tone(centre + shift / 2, 4 * fs);   // ★ 4 s of idle mark after: the host's 3-s "line never ended" release must fire
    return s;
}
static void feed(DecoderHost& h, const std::vector<float>& a) {
    for (size_t i = 0; i < a.size(); i += 960) {
        h.feed(&a[i], (int)std::min<size_t>(960, a.size() - i));
        // ★ Faster than real time, but the 3-s idle release is a wall-clock rule — so the idle mark at the end is fed at
        //   roughly real time (below).
        while (h.queued() > 9600) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    while (h.queued() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

static const std::string BULLETIN =
    "RYRYRYRYRYRYRYRYRYRY\r\nCQ CQ CQ DE DDK2 DDH7 DDK9\r\n"
    "ZCZC 123\r\nSMDL01 EDZW 230600\r\nAAXX 23064\r\n"
    "10384 41/96 82508 10118 20072 30115 40206 57012 8807/ 333 10173 20065=\r\n"
    "10147 31/97 72315 10092 40213=\r\nNNNN\r\n";

static void run(const char* label, const std::string& attach) {
    std::printf("── %s ──\n", label);
    gallery::Store store; const std::string dir = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/vibe-synop-" + std::to_string(getpid()) + "-" + std::to_string(std::rand());
    std::string mk = "mkdir -p '" + dir + "'"; if (std::system(mk.c_str()) != 0) { ok(false, "tmp dir"); return; }
    store.setDir(dir);
    std::shared_ptr<bg::GalleryTap> gtap;
    DecoderHost::Env env;
    env.dialHz = [] { return 4582000.0; };
    env.tap = [&](const std::string& ext, const std::string& msg) -> DecoderTapPtr {
        gallery::Meta m; m.kind = bg::galleryKindFor(ext, msg); if (m.kind.empty()) return nullptr;
        m.origin = "live"; m.freqHz = 4582000; m.mode = "usb"; m.decoder = ext;
        gtap = std::make_shared<bg::GalleryTap>(store, m);
        return gtap;
    };
    DecoderHost h(env, "s");
    auto p = std::make_shared<Peer>();
    h.addPeer(p);
    ok(h.start("fsk", attach) == DecoderHost::Start::Ok, "RTTY started");
    // ★ The bulletin twice — AUTO spends the first lines finding the signal.
    feed(h, audio(bits(BULLETIN + BULLETIN)));
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    { std::vector<float> idle(4800, 0.0f); h.feed(idle.data(), (int)idle.size()); }   // ★ one more block: the idle release runs on a block
    while (h.queued() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::vector<std::string> reps;
    const std::string tr = p->trace(&reps);
    const size_t t1 = reps.size();
    std::printf("     %zu report frames; text %zu bytes\n", t1, tr.size());
    for (auto& r : reps) std::printf("     %s\n", r.substr(0, 220).c_str());
    ok(t1 >= 2, "0x08 report frames arrive (" + std::to_string(t1) + ")");
    bool tempel = false, hamburg = false;
    for (auto& r : reps) {
        if (r.find("\"line\":\"10384 \xc2\xb7 Tempelhof, Germany \xc2\xb7 06:00 UTC \xc2\xb7 wind 250\xc2\xb0 8 kt (15 km/h) \xc2\xb7 11.8 \xc2\xb0""C \xc2\xb7 1020.6 hPa\"") != std::string::npos) tempel = true;
        if (r.find("10147 \xc2\xb7 Hamburg, Germany") != std::string::npos && r.find("230\xc2\xb0 15 kt") != std::string::npos) hamburg = true;
    }
    ok(tempel, "...Tempelhof's report translated exactly: 06:00 UTC · wind 250° 8 kt (15 km/h) · 11.8 °C · 1020.6 hPa");
    ok(hamburg, "...and Hamburg's (72315: 230° 15 kt)");
    ok(reps.empty() || reps[0].rfind("{\"type\":\"weather_report\",\"kind\":\"synop\",\"raw\":\"10384 41/96", 0) == 0 || reps[0].find("\"raw\":\"") != std::string::npos,
       "the frame body: {type:weather_report, kind, raw, line, …}");
    // In its place: the frame comes after "=\r\n" of its own report and before the next station number.
    const size_t a = tr.find("20065;\r\n\xe2\x9f\xa6");
    const size_t b = tr.find("\xe2\x9f\xa7" "10147");
    ok(a != std::string::npos && b != std::string::npos && a < b, "each frame lands at the end of ITS report's line, before the next report's text");
    ok(tr.find("10384 41/96 82508 10118 20072 30115 40206 57012 8807/ 333 10173 20065;") != std::string::npos,
       "the raw groups in the 0x01 text are untouched");
    h.stop();
    // The gallery's copy.
    std::string path, ctype;
    const std::string name = gtap ? gtap->lastName() : "";
    ok(!name.empty() && store.resolve(name, path, ctype), "the gallery kept the RTTY run (" + name + ")");
    std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); const std::string body = ss.str();
    const size_t r = body.find("20065;");
    const size_t l = body.find("    \xc2\xbb 10384 \xc2\xb7 Tempelhof, Germany");
    ok(r != std::string::npos && l != std::string::npos && l > r && body.find("10147", r) > l,
       "★ the gallery .txt has the translation INLINE, under its report, before the next");
    ok(body.find("        Visibility 4 km") != std::string::npos, "...with its details indented under it");
    if (l != std::string::npos) std::printf("%s\n", body.substr(r > 60 ? r - 60 : 0, 600).c_str());
    std::string rm = "rm -rf '" + dir + "'"; (void)!std::system(rm.c_str());
}

int main() {
    run("FskDecoder, DWD's 50 Bd / 450 Hz", "{\"type\":\"audio_extension_attach\",\"extension_name\":\"fsk\",\"center_frequency\":1000,\"shift\":450,\"baud_rate\":50,\"framing\":\"5N1.5\",\"encoding\":\"ITA2\"}");
    run("RTTY AUTO (the clients' default)", "{\"type\":\"audio_extension_attach\",\"extension_name\":\"fsk\",\"center_frequency\":1000,\"shift\":450,\"baud_rate\":50,\"framing\":\"5N1.5\",\"encoding\":\"ITA2\",\"auto\":true}");
    std::printf("── NAVTEX: plain language, never a report frame ──\n");
    {
        DecoderHost::Env env; DecoderHost h(env, "n");
        auto p = std::make_shared<Peer>(); h.addPeer(p);
        ok(h.start("navtex", "{}") == DecoderHost::Start::Ok, "NAVTEX started");
        feed(h, audio(bits("AAXX 23064\r\n10384 41/96 82508 10118=\r\n"), 100, 500, 170));
        std::vector<std::string> reps; p->trace(&reps);
        ok(reps.empty(), "no 0x08 on NAVTEX");
        h.stop();
    }
    std::printf("── %d passed, %d failed ──\n", passes, fails);
    return fails ? 1 : 0;
}
