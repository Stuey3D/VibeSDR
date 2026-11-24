// test-orbcomm-run — ORBCOMM end to end on a SYNTHETIC pass (no off-air data): the engine's ORBCOMM birds (the synthetic
// ORBCOMM fixture through the mirror's sats.json shape, testdata-sat/), a pass over IO92 found by SGP4, and a signal built
// from it — minor frames whose ephemeris packets carry the bird's TRUE Earth-fixed position and GPS time, on a channel
// moved by the bird's predicted Doppler — through vibeorb::Run exactly as the decoder host drives it.
//
// §1 the run: packets decode; the carrier is NAMED by its own position report (the right catalogue number); a subscriber
//    message is shown as "not decoded" with its raw bytes; housekeeping lines (downlink / uplink channels); the status
//    JSON; the packet-line rate limit.
// §2 the sky plot: one dot per 5 s, at the bird's az/el (SGP4) to within a degree, SNR-coloured; the store round trip in
//    a temporary directory (radio tags, the radio / kind filters, untagged dots pass, the bound and compaction); the JSON
//    and the PNG (written to $TMPDIR/orbcomm-skyplot-synthetic.png for a look).
// Silent. Run: scripts/run-tests.sh (VIBE_TESTS="test-orbcomm-run").
#include "vibe_orbcomm_run.h"
#include "vibe_orbcomm_tx.h"
#include "vibe_sat_engine.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) { g_pass++; } else { g_fail++; std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vibeorb;

static std::string readFile(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static const double kIoLat = 52.270833, kIoLon = -0.875;
static const double kT0 = 1795609729.166;   // the fixtures' reference time (test-sat-labels)

int main() {
    std::printf("§1 a synthetic ORBCOMM pass through the decoder\n");
    vibe::sat::Engine eng;
    std::string err;
    if (eng.loadSats(readFile("vibeserver/testdata-sat/sats-orbcomm-SYNTHETIC-2026-11-22.json"), &err) < 0) { std::printf("fixture: %s\n", err.c_str()); return 1; }
    eng.setObserver(kIoLat, kIoLon);
    std::vector<Bird> birds;
    for (auto& e : eng.orbcommElements()) { Bird b; b.norad = e.norad; b.name = e.name; b.ok = b.sgp.init(e.el); birds.push_back(std::move(b)); }
    CHECK(birds.size() == 8, "8 ORBCOMM birds with elements (%zu)", birds.size());
    vibe::sat::Observer ob(kIoLat, kIoLon);
    // the first moment a bird is ≥ 25° up
    double tStart = 0; size_t bi = 0;
    for (double t = kT0; t < kT0 + 36 * 3600 && !tStart; t += 20)
        for (size_t i = 0; i < birds.size(); i++) { auto L = vibe::sat::look(birds[i].sgp, ob, t); if (L.ok && L.elDeg > 25) { tStart = t; bi = i; break; } }
    CHECK(tStart > 0, "a bird ≥ 25° over IO92");
    if (!tStart) return 1;
    Bird& bird = birds[bi];
    std::printf("   %s (%lld) from %.0f\n", bird.name.c_str(), bird.norad, tStart);

    // the signal: 40 s of minor frames, channel 100 (137.250 MHz), centre 137.300 MHz, the bird's Doppler
    const int seconds = 40, ch = 100, scId = 0x2B;
    const double fs = 250000, centre = 137.27e6;
    std::vector<uint8_t> bytes;
    for (int f = 0; f < seconds; f++) {
        const double t = tStart + f;
        std::vector<std::vector<uint8_t>> body;
        if (f % 4 == 1) {
            double re[3]; ecefKm(bird.sgp, t + 0.1, re);
            double re2[3]; ecefKm(bird.sgp, t + 1.1, re2);
            const double pos[3] = { re[0] * 1000, re[1] * 1000, re[2] * 1000 }, vel[3] = { (re2[0] - re[0]) * 1000, (re2[1] - re[1]) * 1000, (re2[2] - re[2]) * 1000 };
            const double gps = t + 0.1 - 315964800.0 + kGpsLeap;
            body.push_back(tx::ephemerisPacket(scId, pos, vel, int(gps / 604800), int(std::fmod(gps, 604800))));
        }
        body.push_back(tx::packet(kDownlink, { 0x30, uint8_t(ch), 0x00, 0x05, 0x64, 0x00, 0x14, 0x1D, 0x01 }));
        body.push_back(tx::packet(kUplink, { 0x10, 0x54, 0x35, 0xF2, 0x23 }));
        if (f % 3 == 0) body.push_back(tx::packet(kMessage, { 0x30, 0x01, 0x82, 0xBD, 0x60, 0xC4, 0x88, 0x6B, uint8_t(f) }));
        auto fr = tx::minorFrame(scId, ch, f, body);
        bytes.insert(bytes.end(), fr.begin(), fr.end());
    }
    const auto L0 = vibe::sat::look(bird.sgp, ob, tStart), L1 = vibe::sat::look(bird.sgp, ob, tStart + seconds);
    const double d0 = L0.dopplerHz(downlinkHz(ch)), d1 = L1.dopplerHz(downlinkHz(ch));
    tx::Signal s; s.fs = fs; s.offsetHz = downlinkHz(ch) - centre + d0; s.slopeHzS = (d1 - d0) / seconds; s.esn0Db = 16; s.clockPpm = 20; s.seed = 5;
    const auto x = tx::modulate(bytes, s);
    std::printf("   Doppler %.0f → %.0f Hz, el %.1f° → %.1f°\n", d0, d1, L0.elDeg, L1.elDeg);

    double simNow = tStart;
    std::vector<vibe::skyplot::Dot> dots;
    std::vector<std::string> lines, jsons;
    Run::Opts o;
    o.widthHz = 120000;
    o.now = [&] { return simNow; };
    o.birds = [&] { return birds; };
    o.observer = [&](vibe::sat::Observer& v) { v = ob; return true; };
    o.pickNorad = [&] { return bird.norad; };
    o.satInRange = [] { return 1; };
    o.text = [&](const std::string& l) { lines.push_back(l); };
    o.json = [&](const std::string& j) { jsons.push_back(j); };
    o.dots = [&](const std::vector<vibe::skyplot::Dot>& d) { dots.insert(dots.end(), d.begin(), d.end()); };
    std::string pngTitle; int pictures = 0;
    o.skyPng = [&](std::string& title) { title = "ORBCOMM SKY PLOT TEST"; std::vector<vibe::skyplot::Dot> v(dots.rbegin(), dots.rend()); return vibe::skyplot::png(v, title); };
    o.picture = [&](const std::string&, const std::string& png, int, int, bool) { if (!png.empty()) pictures++; };
    {
        Run run(o);
        const int B = 5000;
        for (size_t i = 0; i < x.size(); i += B) {
            const int n = int(std::min<size_t>(B, x.size() - i));
            simNow = tStart + double(i + size_t(n)) / fs;
            run.feedIq(&x[i], n, fs, centre);
        }
        run.finish(false);
        std::printf("   good %llu, subscriber messages %llu, lines %zu, status/pkt JSONs %zu, dots %zu\n", (unsigned long long)run.good(),
                    (unsigned long long)run.messages(), lines.size(), jsons.size(), dots.size());
        CHECK(run.good() > 1500, "packets decode (%llu)", (unsigned long long)run.good());
        CHECK(run.goodBySat().count(bird.norad) && run.goodBySat().at(bird.norad) > 1000, "the packets are attributed to %s", bird.name.c_str());
        CHECK(run.messages() >= 8, "subscriber messages counted (%llu)", (unsigned long long)run.messages());
    }
    CHECK(pictures == 1, "the pass's sky plot picture (%d)", pictures);
    bool named = false, msgLine = false, downLine = false, upLine = false, ephLine = false;
    for (auto& j : jsons) {
        if (j.find("\"type\":\"orbcomm_status\"") != std::string::npos && j.find("\"how\":\"position\"") != std::string::npos
            && j.find("\"norad\":" + std::to_string(bird.norad)) != std::string::npos) named = true;
    }
    for (auto& l : lines) {
        if (l.find("subscriber message \xe2\x80\x94 not decoded") != std::string::npos && l.find("1A 30 01 82 BD") != std::string::npos) msgLine = true;
        if (l.find("downlink channels") != std::string::npos && l.find("137.2500") != std::string::npos) downLine = true;
        if (l.find("uplink channels 149.4125, 149.4375 MHz") != std::string::npos) upLine = true;
        if (l.find("position ") != std::string::npos && l.find("km up") != std::string::npos) ephLine = true;
    }
    CHECK(named, "status: the carrier is named by its own position report (%s)", bird.name.c_str());
    CHECK(msgLine, "a subscriber message: 'not decoded' + its raw bytes");
    CHECK(downLine && upLine && ephLine, "housekeeping lines: downlink %d uplink %d position %d", downLine, upLine, ephLine);
    size_t upLines = 0; for (auto& l : lines) if (l.find("uplink channels") != std::string::npos) upLines++;
    CHECK(upLines <= 2, "an unchanged uplink packet is not repeated every frame (%zu lines)", upLines);
    for (auto& l : lines) if (l.find("subscriber") != std::string::npos && l.find("1A") == std::string::npos) { CHECK(false, "a message line without its raw bytes: %s", l.c_str()); break; }

    std::printf("§2 the sky plot\n");
    CHECK(dots.size() >= 5 && dots.size() <= 10, "one dot a satellite per 5 s (%zu for %d s)", dots.size(), seconds);
    double worst = 0;
    for (auto& d : dots) {
        const auto L = vibe::sat::look(bird.sgp, ob, d.t + 2.5);
        double daz = std::fabs(L.azDeg - d.azDeg); if (daz > 180) daz = 360 - daz;
        worst = std::max(worst, std::max(daz * std::cos(L.elDeg * M_PI / 180), std::fabs(L.elDeg - d.elDeg)));
        CHECK(d.norad == uint32_t(bird.norad) && d.kind == vibe::skyplot::kOrbcomm && d.snr > 5 && d.snr < 30 && d.count > 10, "a dot's fields (snr %d count %u)", d.snr, d.count);
    }
    CHECK(worst < 1.0, "each dot at the bird's az/el (worst %.2f°)", worst);

    if (dots.empty()) { std::printf("test-orbcomm-run: %d passed, %d failed\n", g_pass, g_fail); return 1; }
    // the store, in a temporary directory
    char tmpl[] = "/tmp/vibe-skyplot-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "a temporary directory");
    if (dir) {
        vibe::skyplot::Store st; st.setDir(dir);
        vibe::logradio::setMine("a1b2c3d4");
        CHECK(st.add(dots), "dots stored");
        vibe::logradio::setMine("0f0e0d0c");
        std::vector<vibe::skyplot::Dot> other = dots; for (auto& d : other) d.radio.clear();
        other[0].snr = vibe::skyplot::kNothing;
        CHECK(st.add(other), "a second radio's dots");
        vibe::logradio::setMine("");
        std::vector<vibe::skyplot::Dot> untagged(1, dots[0]); untagged[0].radio.clear();
        st.add(untagged);
        vibe::skyplot::Store::Filter f; f.kind = vibe::skyplot::kOrbcomm;
        std::map<std::string, uint64_t> radios;
        auto all = st.read(f, 100000, &radios);
        CHECK(all.size() == dots.size() * 2 + 1, "all radios: %zu", all.size());
        CHECK(radios["a1b2c3d4"] == dots.size() && radios["0f0e0d0c"] == dots.size() && radios[""] == 1, "per-radio counts");
        f.radio = "a1b2c3d4";
        auto one = st.read(f);
        CHECK(one.size() == dots.size() + 1, "one radio's view (+ the untagged dot): %zu", one.size());
        f.radio.clear(); f.kind = vibe::skyplot::kMeteor;
        CHECK(st.read(f).empty(), "another kind: none");
        f.kind = vibe::skyplot::kOrbcomm;
        auto back = st.read(f);
        bool same = !back.empty() && std::fabs(back.back().azDeg - dots.front().azDeg) < 0.06 && std::fabs(back.back().elDeg - dots.front().elDeg) < 0.06;
        CHECK(same, "az/el survive the file (0.1°)");
        const std::string j = vibe::skyplot::json(all, radios, "orbcomm", false);
        CHECK(j.find("\"radioIds\":[") != std::string::npos && j.find("null") != std::string::npos && j.find("\"kind\":\"orbcomm\"") != std::string::npos, "the JSON: radio ids, a grey dot");
        // the bound: past kMaxDots the oldest go (compaction to kKeepDots)
        std::vector<vibe::skyplot::Dot> many(vibe::skyplot::kMaxDots, dots[0]);
        for (size_t i = 0; i < many.size(); i++) many[i].t = uint32_t(1000 + i);
        st.add(many);
        f = {};
        auto after = st.read(f, 1000000);
        CHECK(after.size() == vibe::skyplot::kKeepDots && after.front().t == uint32_t(1000 + vibe::skyplot::kMaxDots - 1), "bounded: %zu kept, newest first", after.size());
        std::string cmd = std::string("rm -rf ") + dir; if (std::system(cmd.c_str()) != 0) {}
    }
    // the picture
    std::vector<vibe::skyplot::Dot> v(dots.rbegin(), dots.rend());
    vibe::skyplot::Dot grey = dots[0]; grey.snr = vibe::skyplot::kNothing; grey.azDeg = 200; grey.elDeg = 15; v.push_back(grey);
    const std::string png = vibe::skyplot::png(v, "ORBCOMM SKY PLOT - SYNTHETIC PASS");
    CHECK(png.size() > 1000 && png.compare(1, 3, "PNG") == 0, "a PNG (%zu bytes)", png.size());
    const char* td = std::getenv("TMPDIR");
    const std::string out = std::string(td && *td ? td : "/tmp") + "/orbcomm-skyplot-synthetic.png";
    { std::ofstream f(out, std::ios::binary); f << png; }
    std::printf("   picture: %s\n", out.c_str());

    std::printf("test-orbcomm-run: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
