// The handheld VTS port's golden test: replays gen_vts_golden.ts's inputs through src/vts_core.h and requires the app's
// outputs byte for byte. Then a scenario check of the bar's owner (vts::Screen) and the bar (vts::Bar) against the app's
// rules and timings (SDRScreen.tsx / VTSBar.tsx have no TS that runs outside React, so those are stated here).
// Silent: no GL, no audio.   handheld-ui/test/run_vts_golden.sh
#include "../src/vts_core.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace hh::vts;

static std::string unhex(const std::string& h) {
    if (h == "-") return "";
    std::string s;
    for (size_t i = 0; i + 1 < h.size(); i += 2) s += (char)std::stoi(h.substr(i, 2), nullptr, 16);
    return s;
}
static std::string hex(const std::string& s) {
    if (s.empty()) return "-";
    static const char* d = "0123456789abcdef";
    std::string o;
    for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 15]; }
    return o;
}
static std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> v; std::string cur;
    for (char ch : s) { if (ch == c) { v.push_back(cur); cur.clear(); } else cur += ch; }
    v.push_back(cur);
    return v;
}
static std::string num(double v) {
    // JS String(number) for the integers the stabiliser returns (due ms)
    char b[48];
    if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.0f", v);
    else std::snprintf(b, sizeof b, "%.17g", v);
    return b;
}

static int fails = 0, checks = 0;
static void expect(bool ok, const std::string& what) {
    checks++;
    if (!ok) { if (fails < 25) std::fprintf(stderr, "FAIL %s\n", what.c_str()); fails++; }
}

static int golden(const char* path) {
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "no golden file %s\n", path); return 2; }
    std::string line;
    PsStabiliser ps;
    int n = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        n++;
        const size_t bar = line.find(" | ");
        const std::string lhs = bar == std::string::npos ? line : line.substr(0, bar);
        const std::string want = bar == std::string::npos ? "" : line.substr(bar + 3);
        std::istringstream is(lhs);
        std::string op; is >> op;
        std::string got;
        auto psOut = [&](const std::string& shown, double now) {
            return hex(shown) + " " + (ps.rotating() ? "1" : "0") + " " + (ps.settled() ? "1" : "0") + " " + num(ps.nextDueIn(now));
        };
        if (op == "L") {
            std::string id, label, name, text; is >> id >> label >> name >> text;
            const auto segs = vtsLineSegments(unhex(id), label, unhex(name), unhex(text));
            std::string ss;
            static const char* kinds[] = {"id", "sep", "name", "text"};
            for (size_t i = 0; i < segs.size(); i++) { if (i) ss += ","; ss += std::string(kinds[segs[i].kind]) + ":" + hex(segs[i].s); }
            got = hex(vtsLine(unhex(id), label, unhex(name), unhex(text))) + " | " + (ss.empty() ? "-" : ss);
        } else if (op == "H") {
            double v; is >> v; got = hex(vtsHex(v));
        } else if (op == "M") {
            std::string a; is >> a;
            std::vector<std::u32string> segs;
            for (auto& h : split(a, ',')) segs.push_back(u32(unhex(h)));
            got = hex(u8(mergeSegments(segs)));
        } else if (op == "PR") {
            ps.reset(); continue;
        } else if (op == "PF") {
            std::string key, p; double now; is >> key >> p >> now;
            got = psOut(ps.feed(unhex(key), unhex(p), now), now);
        } else if (op == "PT") {
            double now; is >> now;
            got = psOut(ps.tick(now), now);
        } else if (op == "B") {
            double hz; int region; is >> hz >> region;
            const auto bands = bandsAtRegion(hz, region);
            if (bands.empty()) got = "- - -";
            else { std::string p, s; bandNotifText(bands, region, p, s); got = hex(bandKey(bands)) + " " + hex(p) + " " + hex(s); }
        } else if (op == "E") {
            int ecc; std::string pi; is >> ecc >> pi; got = hex(eccPiToIso(ecc, unhex(pi)));
        } else if (op == "C" || op == "D") {
            size_t mx; std::string s; is >> mx >> s;
            got = hex(op == "C" ? cleanKeepSpacing(unhex(s), mx) : dabSafeText(unhex(s), mx));
        } else { std::fprintf(stderr, "unknown op: %s\n", line.c_str()); fails++; continue; }
        expect(got == want, "line " + std::to_string(n) + ": " + lhs + "\n   want " + want + "\n   got  " + got);
    }
    std::fprintf(stderr, "vts golden: %d lines\n", n);
    return 0;
}

// ── the screen and the bar ──────────────────────────────────────────────────────────────────────────────────────────

struct Rig {
    Screen sc; Bar bar; Inputs in; double t = 0;
    Rig() { in.mode = "wfm"; in.vfoHz = 96.1e6; in.region = 1; in.connectedEdge = true; }
    void step(double ms) {
        // 30 frames a second, as the R36S draws
        const double end = t + ms;
        while (t < end) {
            t = std::min(end, t + 33.3);
            in.now = t;
            sc.update(in);
            const uint64_t e = bar.update(sc.notif(), t);
            if (e) { sc.timedEnd(e); bar.update(sc.notif(), t); }
            in.connectedEdge = false; in.rdsNew = false; in.noticeNew = false; in.dabNew = false; in.dabOffNew = false; in.drmNew = false; in.toneNew = false;
        }
    }
    void rds(const std::string& ps, const std::string& rt, int pi, bool flags = true) {
        in.rdsNew = true; in.rdsPs = ps; in.rdsRt = rt; in.rdsPi = pi; in.rdsEcc = 0xE1;
        in.rdsHasFlags = flags; in.rdsTp = 1; in.rdsTa = 0; in.rdsAf = 3;
    }
    std::string line() const { const auto& s = bar.shown(); return s ? vtsLine(s->id, s->idLabel, s->name, s->rt) : "<none>"; }
};

static void scenarios() {
    {   // RDS: PI alone, then the name, then RadioText — one held bar, updated in place
        Rig r; r.step(500);
        expect(!r.bar.shown(), "nothing to say → no bar");
        r.rds("", "", 0xC202); r.step(100);
        expect(r.line() == "PI: C202", "PI alone holds the bar: " + r.line());
        expect(r.bar.shown()->badge == "RDS" && r.bar.shown()->hold, "RDS badge, held");
        r.step(300);
        expect(std::fabs(r.bar.opacity() - 1) < 1e-9, "faded in within 180 ms");
        r.rds("BBC R2  ", "", 0xC202); r.step(100);
        expect(r.line() == "PI: C202 / BBC R2", "PS joins: " + r.line());
        expect(std::fabs(r.bar.opacity() - 1) < 1e-9, "a held update is IN PLACE (no re-fade)");
        r.rds("BBC R2", "Steve Wright", 0xC202); r.step(100);
        expect(r.line() == "PI: C202 / BBC R2: Steve Wright", "RadioText joins: " + r.line());
        expect(r.bar.shown()->iso == "GB", "ECC E1 + PI C → GB: " + r.bar.shown()->iso);
        expect(r.bar.shown()->annunciators && r.bar.shown()->tp && !r.bar.shown()->ta && r.bar.shown()->af, "TP · TA · AF");
        r.step(60000);
        expect(r.bar.shown().has_value(), "held: never times out");
        // the station goes (a retune out of FM): the bar fades in 300 ms
        r.rds("", "", -1, false); r.step(100);
        expect(r.bar.shown().has_value() && r.bar.opacity() < 1, "clearing fades");
        r.step(400);
        expect(!r.bar.shown(), "…and is gone");
    }
    {   // a notice owns the bar; RDS waits for it, then lands
        Rig r; r.step(100);
        r.in.noticeNew = true; r.in.noticeText = "the server owner has reserved the notch filters"; r.step(100);
        expect(r.bar.shown() && r.bar.shown()->kind == Notif::Notice, "notice shown");
        r.rds("RADIO 1", "", 0xC201); r.step(2000);
        expect(r.bar.shown()->kind == Notif::Notice, "RDS defers to the notice");
        r.in.noticeNew = true; r.in.noticeText = "the antenna switch is not answering just now"; r.step(100);
        expect(r.bar.shown()->name == "the server owner has reserved the notch filters", "a second notice queues");
        r.step(5000);   // 7.2 s in: the first has gone, the second presents
        expect(r.bar.shown() && r.bar.shown()->name == "the antenna switch is not answering just now", "the queue moves on: " + r.line());
        r.step(7200);
        expect(r.bar.shown() && r.bar.shown()->kind == Notif::StationOn && r.line() == "PI: C201 / RADIO 1", "then the station: " + r.line());
    }
    {   // same words twice = once
        Rig r; r.step(100);
        uint64_t a = r.sc.show("X", 7000), b = r.sc.show("X", 7000);
        expect(a == b, "same words → same notice");
        r.sc.show("Y", 7000); r.sc.show("Z", 7000); r.sc.show("W", 7000); r.sc.show("V", 7000);   // queue ≤ 3, oldest dropped
        r.step(7100); expect(r.bar.shown() && r.bar.shown()->name == "Z", "Y dropped (queue of 3): " + r.line());
    }
    {   // withdraw by key, showing or waiting
        Rig r; r.step(100);
        const uint64_t k = r.sc.show("gain at minimum", 30000); r.step(500);
        r.sc.withdraw(k); r.step(400);
        expect(!r.bar.shown(), "withdrawn → gone");
    }
    {   // band crossing: not on the first look, yes on a crossing; 8 s
        Rig r; r.in.mode = "am"; r.in.vfoHz = 909000; r.step(1000);
        expect(!r.bar.shown(), "the first check only latches the band");
        r.in.vfoHz = 7100000; r.step(400);
        expect(r.bar.shown() && r.bar.shown()->kind == Notif::Band, "crossing → band notif");
        expect(r.bar.shown()->name == "BAND: 7 MHz" "\xE2\x80\x93" "7.200 MHz \xC2\xB7 40m Ham Band", "band text: " + r.bar.shown()->name);
        r.step(7500); expect(r.bar.shown().has_value(), "still up at 7.9 s");
        r.step(900); expect(!r.bar.shown(), "gone after 8 s + 300 ms fade");
    }
    {   // the slide: a held line pauses 1.2 s, 16 ms a pixel, pauses 1.5 s, starts again
        Rig r; r.rds("RADIO 1", "A very long radiotext that will not fit in the window", 0xC201); r.step(100);
        const double t0 = r.t;
        const double textW = 900, areaW = 400;    // dist 500 → 8 s
        expect(r.bar.slideX(textW, areaW) == 0, "slide starts at 0");
        r.step(1100); expect(r.bar.slideX(textW, areaW) == 0, "still paused at 1.1 s");
        r.step(4000); { const double x = r.bar.slideX(textW, areaW), dt = r.t - t0; expect(std::fabs(x - -(dt - 1200) / 16) < 5, "16 ms / px (± a frame of the mount): " + num(x)); }
        r.step(5000); expect(r.bar.slideX(textW, areaW) == -500, "parked at the end");
        r.step(1450); expect(r.bar.slideX(textW, areaW) == 0, "back to the start after the 1.5 s pause (period 10.7 s)");
        // the same words again (a BER-only message) do not restart it; new words do
        r.step(1500); const double before = r.bar.slideX(textW, areaW);
        r.rds("RADIO 1", "A very long radiotext that will not fit in the window", 0xC201); r.step(33);
        expect(r.bar.slideX(textW, areaW) <= before, "same words: the scroll goes on");
        r.rds("RADIO 1", "Another long radiotext that will not fit in the window either", 0xC201); r.step(33);
        expect(r.bar.slideX(textW, areaW) == 0, "new words: from the start");
    }
    {   // a timed line: 0.9 s pause, one pass at 16 ms / px (fitted into the notice's time), then parks
        Rig r; r.step(100);
        r.sc.show("x", 7000); r.step(33);
        const double t0 = r.t;
        r.step(800); expect(r.bar.slideX(600, 400) == 0, "timed: paused 0.9 s");
        r.step(1700); { const double x = r.bar.slideX(600, 400); const double dur = std::max(1500.0, std::min(200 * 16.0, 7000 - 2500.0));
                        expect(std::fabs(x - -200 * (r.t - t0 - 900) / dur) < 0.01, "timed pass: " + num(x)); }
        r.step(3000); expect(r.bar.slideX(600, 400) == -200, "parked at the end");
    }
    {   // DAB: SId / service / DLS, the DAB+ mark; leaving DAB takes it away
        Rig r; r.in.mode = "dab"; r.in.vfoHz = 225648000; r.step(100);
        r.in.dabNew = true; r.in.dabSvcLabel = "Heart"; r.in.dabSvcSid = 0xC0A7; r.in.dabSvcCodec = "DAB+"; r.in.dabSvcDls = "Now: Adele"; r.step(100);
        expect(r.line() == "SId: C0A7 / Heart: Now: Adele", "DAB line: " + r.line());
        expect(r.bar.shown()->dab && r.bar.shown()->dabPlus, "DAB+ mark");
        r.in.dabOffNew = true; r.in.mode = "wfm"; r.step(600);
        expect(!r.bar.shown(), "leaving DAB clears it");
    }
    {   // DRM: SId / SDC label: text · audio, the country, no mark; an unlocked report and leaving DRM take it away
        Rig r; r.in.mode = "drm"; r.in.vfoHz = 5990000; r.step(100);
        r.in.drmNew = true; r.in.drmLive = true; r.in.drmLabel = "TDF DRM"; r.in.drmSid = 0x1000; r.in.drmCountry = "FR";
        r.in.drmText = "Diffusion DRM"; r.in.drmAudio = "AAC \xc2\xb7 12 kHz | 8 kbit/s"; r.step(100);
        expect(r.line() == "SId: 1000 / TDF DRM: Diffusion DRM  \xc2\xb7  AAC \xc2\xb7 12 kHz | 8 kbit/s", "DRM line: " + r.line());
        expect(r.bar.shown()->iso == "FR" && r.bar.shown()->badge.empty() && !r.bar.shown()->dab, "DRM country, no mark");
        r.in.drmNew = true; r.in.drmLive = false; r.step(600);
        expect(!r.bar.shown(), "an unlocked DRM report clears it");
        r.in.drmNew = true; r.in.drmLive = true; r.step(100);
        expect(r.bar.shown().has_value(), "DRM back on relock");
        r.in.mode = "usb"; r.step(600);
        expect(!r.bar.shown(), "leaving DRM clears it");
    }
    {   // the tone, NFM only
        Rig r; r.in.mode = "nfm"; r.in.vfoHz = 145500000; r.step(100);
        r.in.toneNew = true; r.in.toneLabel = "CTCSS 88.5 Hz"; r.step(100);
        expect(r.line() == "CTCSS 88.5 Hz", "tone line: " + r.line());
        r.in.mode = "am"; r.step(500);
        expect(!r.bar.shown(), "another mode drops it");
    }
    {   // the PS stabiliser in the line: a marquee PS is one name, the RadioText kept with it
        Rig r;
        const char* rot[3] = {"UMUARAMA", "ALINE", "RADIO"};
        for (int i = 0; i < 12; i++) { r.rds(rot[i % 3], "", 0x9301); r.step(1500); }
        expect(r.bar.shown() && r.bar.shown()->name == "UMUARAMA ALINE RADIO", "rotation → one stable name: " + (r.bar.shown() ? r.bar.shown()->name : ""));
    }
    {   // RTL automation, once per connection; gain at minimum once per connection
        Rig r; r.in.driver = "rtl"; r.in.vibeAgc = true; r.in.hasTunerBw = true; r.in.tunerBwAuto = true; r.step(100);
        expect(r.bar.shown() && r.bar.shown()->name == "AGC: On  |  IF Filter: Auto", "RTL explanation: " + r.line());
        r.step(9000); expect(!r.bar.shown(), "8 s");
        r.step(20000); expect(!r.bar.shown(), "not again");
        Rig g; g.in.driver = "rtl"; g.in.gains = {0, 9, 14}; g.in.gainNow = 0; g.step(100);
        expect(g.bar.shown() && g.bar.shown()->name.rfind("This receiver's gain is at minimum", 0) == 0, "gain-min notice");
        g.in.gainNow = 9; g.step(500); expect(!g.bar.shown(), "withdrawn when the gain moves");
        g.in.gainNow = 0; g.step(500); expect(!g.bar.shown(), "once per connection");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s vts_golden.txt\n", argv[0]); return 2; }
    if (golden(argv[1])) return 2;
    scenarios();
    std::fprintf(stderr, "vts: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
