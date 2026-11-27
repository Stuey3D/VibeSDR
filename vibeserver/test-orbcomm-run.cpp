// test-orbcomm-run — ORBCOMM end to end on a SYNTHETIC pass (no off-air data): the engine's ORBCOMM birds (the synthetic
// ORBCOMM fixture through the mirror's sats.json shape, testdata-sat/), a pass over IO92 found by SGP4, and a signal built
// from it — minor frames whose ephemeris packets carry the bird's TRUE Earth-fixed position and GPS time, on a channel
// moved by the bird's predicted Doppler — through vibeorb::Run exactly as the decoder host drives it.
//
// §1c (b97) a carrier is named only by the decoder's answer for that carrier; one bird on two carriers lists both.
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
            const double gps = t + 0.1 - 315964800.0 + kOrbClockMinusUtc;   // the satellite counts UTC in GPS weeks
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
    int upNow = 1;   // ★ beta94: the pick's lock ENDS at LOS (−1), it does not report 0 first
    o.satInRange = [&upNow] { return upNow; };
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
        // ★★★ beta94 LOS = the tracker stopping the lock: satInRange 1 → −1 (never 0). The pass's sky plot must come then,
        //     not only when the decoder stops.
        upNow = -1;
        simNow += 1.5;
        { std::vector<std::complex<float>> quiet(5000); run.feedIq(quiet.data(), 5000, fs, centre); }   // a second of nothing: the status tick
        CHECK(pictures == 1, "LOS seen as the lock ending (1 → −1): the pass's sky plot at once (%d)", pictures);
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
    {   // ★ beta96 every packet line keeps its SNR (" · SNR n dB"), before a message's raw bytes
        size_t withSnr = 0; bool order = true;
        for (auto& l : lines) { const size_t k = l.find(" \xc2\xb7 SNR "); if (k != std::string::npos) { ++withSnr; const size_t h = l.find("1A 30 01"); if (h != std::string::npos && h < k) order = false; } }
        CHECK(withSnr > 0 && withSnr == lines.size() && order, "packet lines carry their SNR (%zu of %zu), a message's bytes stay last", withSnr, lines.size());
    }
    for (auto& l : lines) if (l.find("subscriber") != std::string::npos && l.find("1A") == std::string::npos) { CHECK(false, "a message line without its raw bytes: %s", l.c_str()); break; }

    /* §1b ★★★ beta94 THE BOX FOLLOWS THE DECODER (Stuart 11-25): picked bird A's pass is over while the whole-band decoder
     *  hears bird B — the status says the pick has ended and that the box follows B; nothing is followed while A is up or
     *  before A's pass has been seen; a new pick (B, up) starts over — all in ONE run (the decoder is never restarted). */
    std::printf("§1b the box follows the bird being decoded after the pick's LOS\n");
    {
        const long long A = birds[(bi + 1) % birds.size()].norad;
        long long pick = A; int pickUp = 0;   // A picked ahead of its pass: not up yet
        std::vector<std::string> fj, logs;
        Run::Opts q = o;
        q.json = [&](const std::string& j) { fj.push_back(j); };
        q.text = nullptr; q.dots = nullptr; q.skyPng = nullptr; q.picture = nullptr;
        q.log = [&](const std::string& l) { logs.push_back(l); };
        q.pickNow = [&](long long& n, int& u) { if (!pick) return false; n = pick; u = pickUp; return true; };
        auto lastStatus = [&]() -> std::string { for (auto it = fj.rbegin(); it != fj.rend(); ++it) if (it->find("orbcomm_status") != std::string::npos) return *it; return std::string(); };
        Run run(q);
        const int B = 5000;
        const size_t third = x.size() / 3;
        size_t i = 0;
        auto feedTo = [&](size_t end) {
            for (; i < end; i += B) {
                const int n = int(std::min<size_t>(B, end - i));
                simNow = tStart + double(i + size_t(n)) / fs;
                run.feedIq(&x[i], n, fs, centre);
            }
        };
        feedTo(third / 2);
        CHECK(run.follow() == 0 && !run.pickEnded(), "a pick not yet risen: nothing followed (follow %lld)", run.follow());
        pickUp = 1; feedTo(third);
        CHECK(run.follow() == 0 && !run.pickEnded(), "the pick up: the box is the pick's (follow %lld)", run.follow());
        CHECK(lastStatus().find("\"ended\":false,\"follow\":0") != std::string::npos, "status: ended false, follow 0");
        const uint64_t goodAtLos = run.good();
        pickUp = 0; feedTo(2 * third);
        const std::string st = lastStatus();
        CHECK(run.pickEnded() && run.follow() == bird.norad, "A has set: the box follows %s, decoding now (follow %lld)", bird.name.c_str(), run.follow());
        CHECK(st.find("\"ended\":true,\"follow\":" + std::to_string(bird.norad) + ",\"followName\":\"" + bird.name + "\"") != std::string::npos,
              "status: ended, follow + its name (%s)", st.substr(st.find("\"pick\""), 120).c_str());
        bool said = false; for (auto& l : logs) if (l.find("the box follows " + bird.name) != std::string::npos) said = true;
        CHECK(said, "the log says so");
        CHECK(run.good() > goodAtLos, "the decoder carried on through A's LOS (%llu → %llu good)", (unsigned long long)goodAtLos, (unsigned long long)run.good());
        // B picked from the list (the same run — the server keeps the decoder): B is up, so B is the pick's own view
        pick = bird.norad; pickUp = 1;
        const uint64_t goodAtPick = run.good();
        feedTo(x.size());
        CHECK(run.follow() == 0 && !run.pickEnded(), "B picked: the pick's own view again (follow %lld)", run.follow());
        CHECK(run.good() > goodAtPick && run.goodBySat().at(bird.norad) > 0, "the same run counts on (%llu → %llu good: not reset)", (unsigned long long)goodAtPick, (unsigned long long)run.good());
        // no pick at all: never a follow
        pick = 0;
        simNow += 2; run.feedIq(&x[0], B, fs, centre);
        CHECK(run.follow() == 0 && !run.pickEnded(), "no pick: no follow");
    }

    /* §1c ★★ b97 (Stuart, XCover beta96: the waterfall named TWO carriers "ORBCOMM FM116" while the card named one and listed
     *  the other "not yet named"): a carrier is named only when the decoder named THAT carrier. Two carriers in one slice —
     *  A = the bird (its own position reports), B = another spacecraft ID on another channel, no position report, its
     *  frequency matching no bird's Doppler — the status's carriers[] names A (with how + catalogue number), never B. Then
     *  B is the SAME bird (its spacecraft ID learnt from A): both carriers named, and the bird's row lists both (rfs). */
    std::printf("§1c each carrier named only by the decoder's answer for THAT carrier\n");
    {
        auto signalFor = [&](int sc, int chn, bool eph, double extraHz, unsigned seed) {
            std::vector<uint8_t> by;
            for (int f = 0; f < seconds; f++) {
                const double t = tStart + f;
                std::vector<std::vector<uint8_t>> body;
                if (eph && f % 4 == 1) {
                    double re[3]; ecefKm(bird.sgp, t + 0.1, re);
                    double re2[3]; ecefKm(bird.sgp, t + 1.1, re2);
                    const double pos[3] = { re[0] * 1000, re[1] * 1000, re[2] * 1000 }, vel[3] = { (re2[0] - re[0]) * 1000, (re2[1] - re[1]) * 1000, (re2[2] - re[2]) * 1000 };
                    const double gps = t + 0.1 - 315964800.0 + kOrbClockMinusUtc;
                    body.push_back(tx::ephemerisPacket(sc, pos, vel, int(gps / 604800), int(std::fmod(gps, 604800))));
                }
                body.push_back(tx::packet(kUplink, { 0x10, 0x54, 0x35, 0xF2, 0x23 }));
                auto fr = tx::minorFrame(sc, chn, f, body);
                by.insert(by.end(), fr.begin(), fr.end());
            }
            const double e0 = L0.dopplerHz(downlinkHz(chn)), e1 = L1.dopplerHz(downlinkHz(chn));
            tx::Signal g; g.fs = fs; g.offsetHz = downlinkHz(chn) - centre + e0 + extraHz; g.slopeHzS = (e1 - e0) / seconds; g.esn0Db = 16; g.clockPpm = 20; g.seed = seed;
            return tx::modulate(by, g);
        };
        auto runPair = [&](const std::vector<std::complex<float>>& a, const std::vector<std::complex<float>>& b) {
            std::vector<std::complex<float>> sum(std::max(a.size(), b.size()));
            for (size_t i = 0; i < sum.size(); i++) sum[i] = (i < a.size() ? a[i] : 0.0f) + (i < b.size() ? b[i] : 0.0f);
            std::vector<std::string> fj;
            Run::Opts q = o;
            q.json = [&](const std::string& j) { fj.push_back(j); };
            q.text = nullptr; q.dots = nullptr; q.skyPng = nullptr; q.picture = nullptr;
            Run run(q);
            const int B = 5000;
            for (size_t i = 0; i < sum.size(); i += B) {
                const int n = int(std::min<size_t>(B, sum.size() - i));
                simNow = tStart + double(i + size_t(n)) / fs;
                run.feedIq(&sum[i], n, fs, centre);
            }
            for (auto it = fj.rbegin(); it != fj.rend(); ++it) if (it->find("orbcomm_status") != std::string::npos) return *it;
            return std::string();
        };
        // the carriers[] array of a status, split into its entries
        auto carriersOf = [](const std::string& st) {
            std::vector<std::string> v;
            size_t k = st.find("\"carriers\":[");
            if (k == std::string::npos) return v;
            k += 12;
            const size_t end = st.find("],\"slice\"", k);
            while (k < end) { const size_t c = st.find('}', k); if (c == std::string::npos || c > end) break; v.push_back(st.substr(k, c - k + 1)); k = c + 2; }
            return v;
        };
        const int chB = ch + 16;   // 137.290 MHz, 40 kHz above A — inside the slice
        const std::string named = "\"sat\":\"" + bird.name + "\",\"norad\":" + std::to_string(bird.norad);
        {
            const std::string st = runPair(signalFor(scId, ch, true, 0, 5), signalFor(0x11, chB, false, 4000, 9));
            const auto cs = carriersOf(st);
            int withBird = 0, withAny = 0;
            for (auto& c : cs) { if (c.find(named) != std::string::npos) withBird++; if (c.find("\"sat\":") != std::string::npos) withAny++; }
            CHECK(cs.size() >= 2, "two carriers open (%zu)", cs.size());
            CHECK(withBird == 1 && withAny == 1, "only the identified carrier is named %s (named %d, any name %d): %s", bird.name.c_str(), withBird, withAny, st.substr(st.find("\"carriers\""), 400).c_str());
            CHECK(st.find(named + ",\"how\":\"position\"") != std::string::npos, "…named with how it was named (position) — the clients' confirmed flag");
            CHECK(st.find("ORBCOMM (not yet named)") != std::string::npos, "the other carrier's row reads not yet named");
            CHECK(st.find("\"rfs\":") == std::string::npos, "one carrier for the bird: no carrier list on its row");
        }
        {
            const std::string st = runPair(signalFor(scId, ch, true, 0, 5), signalFor(scId, chB, false, 0, 9));
            const auto cs = carriersOf(st);
            int withBird = 0;
            for (auto& c : cs) if (c.find(named) != std::string::npos) withBird++;
            CHECK(withBird == 2, "the same bird on two channels (its ID learnt): both carriers named %s (%d)", bird.name.c_str(), withBird);
            CHECK(st.find("\"rfs\":[") != std::string::npos, "…and its row lists both carriers (rfs) — the card says so too");
        }
    }

    std::printf("§2 the sky plot\n");
    CHECK(dots.size() >= 5 && dots.size() <= 10, "one dot a satellite per 5 s (%zu for %d s)", dots.size(), seconds);
    double worst = 0;
    for (auto& d : dots) {
        const auto L = vibe::sat::look(bird.sgp, ob, d.t + 2.5);
        double daz = std::fabs(L.azDeg - d.azDeg); if (daz > 180) daz = 360 - daz;
        worst = std::max(worst, std::max(daz * std::cos(L.elDeg * M_PI / 180), std::fabs(L.elDeg - d.elDeg)));
        CHECK(d.norad == uint32_t(bird.norad) && d.kind == vibe::skyplot::kOrbcomm && d.snr > 5 && d.snr < 30 && (d.count > 10 || &d == &dots.back()), "a dot's fields (snr %d count %u)", d.snr, d.count);   // ★ beta94 the LOS step's last bucket may hold one packet
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

    /* §3 ★★★ REAL SIGNAL (the regression the whole decoder answers to): 12 s of ORBCOMM FM116 off air, 2026-11-24 16:55:23
     *  UTC — the Pi 500's RTL-SDR V4 (00000004) on the YouLoop, 2.4 MS/s, channel 265 (137.6625 MHz) mixed to DC and
     *  decimated to 48 kS/s cu8 (test/fixtures/orbcomm/, 1.1 MB) — with FM116's CelesTrak elements of that day. Through the run
     *  exactly as the decoder host drives it: it must lock, decode hundreds of packets, read the sync's ID 0x2E and channel
     *  265, name the bird FM116 BY ITS OWN POSITION within a few km (the satellite clock read as UTC — kOrbClockMinusUtc), put
     *  that clock within 1.5 s of the recording's, and show subscriber messages only raw. */
    std::printf("§3 real FM116 off air (12 s fixture)\n");
    {
        std::ifstream fx("test/fixtures/orbcomm/fm116-20261124T165523Z-137662500-48k.cu8", std::ios::binary);
        std::ifstream tl("test/fixtures/orbcomm/fm116-celestrak-20261124.tle");
        std::string raw((std::istreambuf_iterator<char>(fx)), std::istreambuf_iterator<char>());
        std::string n, l1, l2;
        std::getline(tl, n); std::getline(tl, l1); std::getline(tl, l2);
        vibe::sat::Elements e; std::string err;
        CHECK(raw.size() == 1152000 && vibe::sat::parseTle(l1, l2, e, err, "ORBCOMM FM116"), "the fixture and FM116's elements (%zu bytes)", raw.size());
        std::vector<Bird> fb(1); fb[0].norad = 41189; fb[0].name = "ORBCOMM FM116"; fb[0].ok = fb[0].sgp.init(e);
        const double fs = 48000, t0 = 1795539323.0;   // 2026-11-24T16:55:23Z
        double now = t0;
        std::vector<std::string> fl, fj; std::vector<vibe::skyplot::Dot> fd;
        Run::Opts q;
        q.widthHz = 40000; q.now = [&] { return now; }; q.birds = [&] { return fb; };
        q.observer = [](vibe::sat::Observer& v) { v = vibe::sat::Observer(51.88, -0.42, 100); return true; };
        q.pickNorad = [] { return 41189LL; }; q.satInRange = [] { return 1; };
        q.text = [&](const std::string& l) { fl.push_back(l); }; q.json = [&](const std::string& j) { fj.push_back(j); };
        q.dots = [&](const std::vector<vibe::skyplot::Dot>& d) { fd.insert(fd.end(), d.begin(), d.end()); };
        uint64_t good = 0, msgs = 0; std::map<long long, int> by;
        {
            Run run(q);
            std::vector<std::complex<float>> x(4800);
            for (size_t i = 0; i + 2 * x.size() <= raw.size(); i += 2 * x.size()) {
                for (size_t k = 0; k < x.size(); k++) x[k] = { (uint8_t(raw[i + 2 * k]) - 127.5f) / 128.f, (uint8_t(raw[i + 2 * k + 1]) - 127.5f) / 128.f };
                now = t0 + double(i / 2 + x.size()) / fs;
                run.feedIq(x.data(), int(x.size()), fs, 137662500.0);
            }
            run.finish(true);
            good = run.good(); msgs = run.messages(); by = run.goodBySat();
        }
        std::printf("   good %llu, subscriber messages %llu, FM116 %d, dots %zu\n", (unsigned long long)good, (unsigned long long)msgs, by[41189], fd.size());
        CHECK(good >= 400 && good <= 650, "locks and decodes 400–600 packets in 12 s (%llu; 521 when made)", (unsigned long long)good);
        CHECK(by[41189] >= 300, "attributed to FM116 (%d)", by[41189]);
        CHECK(msgs >= 20, "subscriber messages seen (raw only) (%llu)", (unsigned long long)msgs);
        bool byPos = false, chan = false, id = false; double km = -1, clk = 99;
        for (auto& j : fj) {
            if (j.find("\"how\":\"position\"") != std::string::npos && j.find("\"norad\":41189") != std::string::npos) byPos = true;
            if (j.find("\"dcn\":265") != std::string::npos) chan = true;
            if (j.find("\"scId\":46") != std::string::npos) id = true;   // 0x2E
            const size_t c = j.find("\"clock\":");
            if (c != std::string::npos) { const double sc = std::atof(j.c_str() + c + 8); const size_t tt = j.find("\"t\":"); if (tt != std::string::npos) clk = std::min(clk, std::fabs(sc - std::atof(j.c_str() + tt + 4))); }
        }
        CHECK(byPos && chan && id, "named FM116 by its own position, channel 265, ID 0x2E (pos %d chan %d id %d)", byPos, chan, id);
        CHECK(clk < 1.5, "the satellite's clock (read as UTC) within 1.5 s of the recording's (%.2f s)", clk);
        // the position report against SGP4 at the satellite's own time, directly
        Ident idn; idn.setBirds(fb);
        {
            Receiver rr; double best = 1e9;
            rr.onPacket = [&](const Rx& r) {
                Ephemeris ep;
                if (!r.p.ok || r.p.repaired || !decodeEphemeris(r.p, ep, t0)) return;
                double p[3]; Bird b = fb[0];
                if (ecefKm(b.sgp, ep.unix, p)) best = std::min(best, std::sqrt(std::pow(p[0] - ep.x / 1000, 2) + std::pow(p[1] - ep.y / 1000, 2) + std::pow(p[2] - ep.z / 1000, 2)));
                CHECK(ep.altKm > 690 && ep.altKm < 715 && ep.speedKmS > 7.0 && ep.speedKmS < 7.3, "a sane position report (%.1f km up, %.3f km/s)", ep.altKm, ep.speedKmS);
            };
            std::vector<std::complex<float>> x(4800);
            for (size_t i = 0; i + 2 * x.size() <= raw.size(); i += 2 * x.size()) {
                for (size_t k = 0; k < x.size(); k++) x[k] = { (uint8_t(raw[i + 2 * k]) - 127.5f) / 128.f, (uint8_t(raw[i + 2 * k + 1]) - 127.5f) / 128.f };
                rr.feed(x.data(), int(x.size()), fs, 137662500.0, t0 + double(i / 2) / fs);
            }
            km = best;
        }
        CHECK(km < 15, "the ephemeris matches SGP4 within a few km (%.1f km; 124 km when read as GPS time)", km);
        bool rawOnly = true;
        for (auto& l : fl) if (l.find("subscriber message") != std::string::npos && l.find("not decoded") == std::string::npos) rawOnly = false;
        CHECK(rawOnly, "every subscriber message line says not decoded");
        CHECK(!fd.empty() && fd.front().elDeg > 33 && fd.front().elDeg < 40, "sky dots at FM116's elevation then (~36° per SGP4: %.1f)", fd.empty() ? -1.0 : double(fd.front().elDeg));
    }

    std::printf("test-orbcomm-run: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
