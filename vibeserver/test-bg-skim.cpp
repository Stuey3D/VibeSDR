// test-bg-skim.cpp — THE SKIMMERS BY BAND (V12 beta90, Stuart 2026-11-21).
//
// Stuart: "cw skimmer and rtty skimmer should be per band rather than entering a centre frequency" and "48k should default
// to 7024 for 40m 96k 7048 etc so the bands cw allocation is fully covered".
//
//   · the table (vibe_skim_bands.h): every region's CW / RTTY / PSK segments are sane (inside the band, lo < hi, CW below
//     the digimodes), and THE RULE — centre = segment bottom + width / 2: 40 m CW R1 at 24 / 48 / 96 kHz = 7012 / 7024 /
//     7048; RTTY and PSK examples; the regions differ where their plans do (160 m, 80 m, 40 m R3);
//   · the capture-fit refusal (windowFits) and the JSON every page reads (skimBandsJson, the tunable filter);
//   · the background config: the RTTY skimmer profiles (rtty-skim-24/48/96, ext "rttyskim", IQ, USB only), skimKindOf, the
//     save (any centre inside the capture is taken — a band is not required; a window outside it is refused in words);
//   · end to end without a server: synthetic 40 m RTTY IQ (two stations in the R1 digimode segment) at 250 kS/s through
//     BgRunner → DecoderHost "rttyskim" → the RTTY summary card ("RTTY spots summary · … background decoding"), the
//     spots to PSK Reporter (ADIF RTTY) when the owner has it on;
//   · switched off on this receiver (Params::skimWhyNot): shown paused with the reason, takes no slot.
// ★ Silent: nothing is played. Bounded: 24 s of 250 kS/s IQ (≈ 48 MB), freed at the end.
#include "vibe_bgdecode.h"
#include "decoders/rtty_skimmer.h"
#include "vibe_skim_bands.h"
#include "vibe_spot_report.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
using cf32 = std::complex<float>;
namespace sb = vibe::skimbands;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); (c ? passes : fails)++; }
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/bgskim-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static double centreOf(sb::Kind k, int region, const char* band, double w) {
    const sb::Seg* s = sb::segment(k, region, band);
    return s ? sb::windowCentre(*s, w) : -1;
}

static void table() {
    std::printf("── the table and THE RULE (centre = segment bottom + width / 2) ──\n");
    ok(centreOf(sb::Kind::Cw, 1, "40m", 24000) == 7012000 && centreOf(sb::Kind::Cw, 1, "40m", 48000) == 7024000
       && centreOf(sb::Kind::Cw, 1, "40m", 96000) == 7048000, "40 m CW, Region 1: 24 kHz → 7012, 48 kHz → 7024, 96 kHz → 7048 (Stuart's figures)");
    const sb::Seg* c40 = sb::segment(sb::Kind::Cw, 1, "40m");
    ok(c40 && 7024000 - 24000 <= c40->loHz && 7024000 + 24000 >= c40->hiHz, "…48 kHz covers the whole R1 CW allocation (7000–7040)");
    ok(centreOf(sb::Kind::Rtty, 1, "40m", 24000) == 7052000 && centreOf(sb::Kind::Rtty, 1, "20m", 48000) == 14094000,
       "RTTY: 40 m R1 at 24 kHz → 7052 (7040–7064), 20 m at 48 kHz → 14094 (14070–14118)");
    ok(centreOf(sb::Kind::Psk, 1, "20m", 6000) == 14073000 && centreOf(sb::Kind::Psk, 1, "80m", 12000) == 3586000,
       "PSK: 20 m at 6 kHz → 14073 (14070–14076), 80 m at 12 kHz → 3586 (3580–3592, PSK31's 3580 inside)");
    ok(centreOf(sb::Kind::Cw, 2, "160m", 24000) == 1822000 && centreOf(sb::Kind::Cw, 1, "160m", 24000) == 1822000
       && centreOf(sb::Kind::Cw, 3, "160m", 24000) == 1812000, "regions differ: 160 m CW R1/R2 from 1810, R3 from 1800");
    ok(centreOf(sb::Kind::Rtty, 2, "160m", 24000) == 1812000 && centreOf(sb::Kind::Rtty, 1, "160m", 24000) == 1850000,
       "…160 m RTTY: R2 1800–1810 (→ 1812), R1 from 1838 (→ 1850)");
    ok(centreOf(sb::Kind::Cw, 3, "80m", 48000) == 3524000 && centreOf(sb::Kind::Rtty, 3, "40m", 24000) == 7042000,
       "…R3: 80 m CW 3500–3535 (48 kHz → 3524), 40 m narrowband from 7030 (24 kHz → 7042, RTTY DX 7040 inside)");
    ok(sb::segment(sb::Kind::Cw, 1, "6m") && !sb::segment(sb::Kind::Rtty, 1, "6m") && !sb::segment(sb::Kind::Cw, 1, "60m"),
       "6 m has CW only; 60 m is left out (national channels)");
    ok(&sb::segments(sb::Kind::Cw, 0) == &sb::segments(sb::Kind::Cw, 1), "an unknown region is Region 1 (vibebands::ituRegion's default)");
    // every segment sane: lo < hi, inside 1.8–54 MHz, and on each band CW sits below the digimodes
    bool sane = true;
    for (int r = 1; r <= 3; ++r)
        for (sb::Kind k : { sb::Kind::Cw, sb::Kind::Rtty, sb::Kind::Psk })
            for (const auto& s : sb::segments(k, r)) {
                sane &= s.loHz < s.hiHz && s.loHz >= 1800000 && s.hiHz <= 54000000;
                if (k != sb::Kind::Cw) { const sb::Seg* c = sb::segment(sb::Kind::Cw, r, s.id); sane &= c != nullptr && (c->hiHz <= s.loHz || std::string(s.id) == "160m"); }
            }
    ok(sane, "every segment of every region: lo < hi, inside the HF/6 m bands, CW below the digimodes (160 m R2's digimodes sit below its CW)");
    ok(sb::bandAtCentre(sb::Kind::Cw, 1, 48000, 7024000) == c40 && !sb::bandAtCentre(sb::Kind::Cw, 1, 48000, 7020000),
       "a stored centre names its band only when it IS the rule's centre (7024 → 40 m; an owner's 7020 → none, kept as their own)");

    std::printf("── the capture-fit refusal ──\n");
    const double half = bg::halfWindowHz(250000) + 3000.0;   // what background decoding's save checks
    ok(sb::windowFits(7024000, 48000, 7000000, half), "40 m CW 48 kHz fits a 250 kS/s capture at 7.000 MHz");
    ok(!sb::windowFits(7048000, 96000, 6900000, half), "…96 kHz at 6.900 MHz does not (its top 7.096 is past the capture's guard)");
    ok(sb::windowFits(7048000, 96000, 0, 0), "an unknown capture: every band (the radio's save decides)");

    std::printf("── the JSON every page reads ──\n");
    const std::string j = sb::skimBandsJson(1);
    ok(j.rfind(",\"skimBands\":{\"region\":1,\"cw\":[{\"id\":\"160m\",\"name\":\"160 m\",\"lo\":1810000,\"hi\":1838000}", 0) == 0
       && j.find("\"rtty\":[") != std::string::npos && j.find("\"psk\":[") != std::string::npos, "shape: " + j.substr(0, 120) + "…");
    const std::string hfOnly = sb::skimBandsJson(2, [](double lo, double hi) { return lo >= 7000000 && hi <= 7300000; });
    ok(hfOnly.find("\"40m\"") != std::string::npos && hfOnly.find("\"20m\"") == std::string::npos && hfOnly.find("\"region\":2") != std::string::npos,
       "the tunable filter: a radio that reaches only 7.0–7.3 MHz lists only 40 m");
}

static void config() {
    std::printf("── the background config ──\n");
    const bg::DecoderDef* d = bg::decoderDef("rtty-skim-48");
    ok(d && std::string(d->ext) == "rttyskim" && d->widthHz == 48000 && bg::decoderDef("rtty-skim-24") && bg::decoderDef("rtty-skim-96"),
       "RTTY skimmer profiles rtty-skim-24/48/96 on the decoder host's \"rttyskim\" (its listener widths)");
    ok(bg::decoderFitsDemod("rtty-skim-48", "usb") && !bg::decoderFitsDemod("rtty-skim-48", "lsb"), "USB row only (fed IQ — no demodulator runs)");
    bg::Vfo v{7064000, "usb", "rtty-skim-48"};
    ok(bg::isSkimmer(v) && bg::passbandHz(v) == 48000 && bg::attachMsg(v) == "{\"width\":48000}", "a skimmer: its whole 48 kHz segment, attached with its width");
    ok(bg::skimKindOf("cw-skim-96") == "cw" && bg::skimKindOf("rtty-skim-24") == "rtty" && bg::skimKindOf("psk-skim-6") == "psk" && bg::skimKindOf("navtex").empty(),
       "skimKindOf: cw / rtty / psk, '' for any other decoder");
    bg::Config c; std::string err;
    ok(bg::parse(R"({"on":true,"profiles":[{"centre":7000000,"vfos":[{"hz":7024000,"mode":"usb","dec":"cw-skim-48"},{"hz":7052000,"mode":"usb","dec":"rtty-skim-24"},{"hz":7043000,"mode":"usb","dec":"psk-skim-6"}]}]})", c, err)
       && bg::toJson(c).find("\"dec\":\"rtty-skim-24\"") != std::string::npos, "parses and writes back (the stored value is still a centre frequency)");
    ok(bg::validate(c, 250000, err), "40 m CW 48 / RTTY 24 / PSK 6 by band at 7.000 MHz, 250 kS/s: valid (" + err + ")");
    bg::Config own = c; own.profiles[0].vfos[0].hz = 7031500;
    ok(bg::validate(own, 250000, err), "an owner's own centre (7031.5) inside the capture: taken — no band required");
    bg::Config far = c; far.profiles[0].vfos[1].hz = 7090000; far.profiles[0].vfos[1].dec = "rtty-skim-96";
    ok(!bg::validate(far, 250000, err) && err.find("VFO 2") != std::string::npos && err.find("Move the centre") != std::string::npos,
       "a 96 kHz RTTY window at 7090 past the capture's guard: refused, in words — " + err);
}

// ── synthetic RTTY (ITA2, 45.45 Bd, 170 Hz, 1.5 stop bits, phase-continuous FSK) ──
static void addRtty(std::vector<cf32>& x, double fs, double offHz, double startSec, const std::string& text, double amp, bool reverse = false) {
    std::vector<uint8_t> hb;   // half-bit units
    auto frame = [&](int code) { hb.push_back(0); hb.push_back(0); for (int b = 0; b < 5; ++b) { const uint8_t v = (code >> b) & 1; hb.push_back(v); hb.push_back(v); } for (int k = 0; k < 3; ++k) hb.push_back(1); };
    for (int k = 0; k < 20; ++k) hb.push_back(1);
    bool figs = false; frame(31);
    for (char ch : text) { bool f = figs; const int code = rttysk::ita2Code(ch, f); if (code < 0) continue; if (f != figs) { frame(f ? 27 : 31); figs = f; } frame(code); }
    const double hbSec = 1.0 / (2 * 45.45);
    double ph = 0, t = 0;
    for (size_t i = (size_t)(startSec * fs); i < x.size(); ++i) {
        int h = (int)(t / hbSec);
        if (h >= (int)hb.size()) { t -= hb.size() * hbSec; h = 0; }
        const bool mark = hb[(size_t)h] != 0;
        ph += 2 * M_PI * (offHz + ((mark != reverse) ? 85.0 : -85.0)) / fs;
        if (ph > M_PI) ph -= 2 * M_PI;
        x[i] += cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
        t += 1.0 / fs;
    }
}

static void endToEnd() {
    std::printf("\n── end to end: a background RTTY skimmer on 40 m (R1, 48 kHz → 7064) on synthetic IQ, 250 kS/s ──\n");
    // ★ PSK Reporter on (no flush happens in this test: the batch is minutes away and nothing is started)
    spotreport::Config rc; rc.on = true; rc.call = "M0TST"; rc.pskHost = "127.0.0.1"; rc.pskPort = 9;
    spotreport::Reporter::shared().configure(rc);
    gallery::Store s; s.setDir(tmpDir("rtty"));
    FileSlots slots("", [] { return 4; });
    const double fs = 250000;
    const double win = centreOf(sb::Kind::Rtty, 1, "40m", 48000);
    const double centre = win;   // the profile's RF centre on the window
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 0; p.sampleRate = fs; p.centreHz = centre; p.fftSize = 8192; p.slots = &slots; p.store = &s;
    p.lat = 52.29; p.lon = -0.85;
    p.profile.centreHz = centre;
    p.profile.vfos = { bg::Vfo{win, "usb", "rtty-skim-48"} };
    p.rttySkimWorkers = [] { return 2; };
    p.log = [](const std::string&) {};
    r.start(p);
    ok(r.decodersRunning() == 1, "the RTTY skimmer VFO took its decoder slot and runs");
    const double secs = 24.0;
    std::vector<cf32> cap((size_t)(secs * fs));
    std::mt19937 g(5); std::normal_distribution<float> n(0.0f, std::sqrt(0.5f));
    for (auto& v : cap) v = cf32(n(g), n(g));
    auto amp = [&](double snrDb) { return std::sqrt(std::pow(10.0, snrDb / 10) * 2500.0 / fs); };
    // two stations in the R1 40 m digimode segment: 7043.0 (normal) and 7046.0 (reverse)
    addRtty(cap, fs, 7043000 - centre, 1.0, "CQ CQ CQ DE G4ABC G4ABC G4ABC PSE K\nCQ TEST G4ABC G4ABC TEST\n", amp(16));
    addRtty(cap, fs, 7046000 - centre, 1.5, "CQ TEST DL1XYZ DL1XYZ TEST\nDL1XYZ 599 001 001 TU\n", amp(16), true);
    const size_t chunk = 16384;
    for (size_t o = 0; o < cap.size(); o += chunk) {
        const int k = (int)std::min(chunk, cap.size() - o);
        r.feedIq(&cap[o], k, 1793000000.0 + (double)(o + (size_t)k) / fs);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));   // ★ paced (≈ 3× real time): the runner drops past 0.5 s
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const std::string status = r.statusJson();
    const uint64_t dropped = r.droppedSamples();
    r.stop();
    std::vector<cf32>().swap(cap);
    ok(slots.inUse() == 0, "stopped — the slot is back");
    ok(status.find("\"dec\":\"rtty-skim-48\"") != std::string::npos && status.find("\"spots\":") != std::string::npos, "the admin line: the VFO and its live spot count — " + status.substr(0, 300));
    ok(dropped == 0, "no IQ dropped at this pace");
    const std::string list = s.listJson("rttyskim", 20);
    const std::string first = gallery::lineStr(list, "f");
    std::string path, ct, body;
    if (!first.empty() && s.resolve(first, path, ct)) {
        FILE* f = std::fopen(path.c_str(), "r"); char b[512]; size_t k;
        while (f && (k = std::fread(b, 1, sizeof b, f)) > 0) body.append(b, k);
        if (f) std::fclose(f);
    }
    ok(body.find("RTTY spots summary") != std::string::npos && body.find("background decoding (Profile 1)") != std::string::npos
       && body.find("the skimmer's centre") != std::string::npos, "the gallery holds the RTTY summary card, labelled background, named by the skimmer's centre");
    ok(body.find("2 from 2 stations") != std::string::npos && body.find("England") != std::string::npos && body.find("Germany") != std::string::npos,
       "both stations heard — G4ABC (England, normal) and DL1XYZ (Germany, reverse)");
    std::printf("%s\n", body.substr(0, 800).c_str());
    ok(spotreport::Reporter::shared().pendingCount() >= 2, "their spots reached PSK Reporter as RTTY (" + std::to_string(spotreport::Reporter::shared().pendingCount()) + " waiting for the batch)");
    rc.on = false; spotreport::Reporter::shared().configure(rc);
}

static void switchedOff() {
    std::printf("\n── switched off on this receiver ──\n");
    gallery::Store s; s.setDir(tmpDir("off"));
    FileSlots slots("", [] { return 4; });
    bg::BgRunner r;
    bg::BgRunner::Params p;
    p.profileIdx = 0; p.sampleRate = 250000; p.centreHz = 7000000; p.fftSize = 8192; p.slots = &slots; p.store = &s;
    p.profile.centreHz = 7000000;
    p.profile.vfos = { bg::Vfo{7052000, "usb", "rtty-skim-24"}, bg::Vfo{7024000, "usb", "cw-skim-48"} };
    p.skimWhyNot = [](const std::string& k) { return k == "rtty" ? std::string("the server owner has switched RTTY off on this receiver") : std::string(); };
    p.log = [](const std::string&) {};
    r.start(p);
    const std::string st = r.statusJson();
    ok(r.decodersRunning() == 1 && slots.inUse() == 1, "the RTTY skimmer takes no slot; the CW skimmer beside it runs");
    ok(st.find("switched RTTY off") != std::string::npos, "…and the RTTY row says why — " + st.substr(0, 300));
    r.stop();
}

int main() {
    table();
    config();
    switchedOff();
    endToEnd();
    std::printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
