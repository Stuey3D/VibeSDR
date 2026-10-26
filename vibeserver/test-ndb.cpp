// test-ndb.cpp — the NDB skimmer (decoders/ndb_skimmer.h), the published-beacon matching (vibe_ndb_db.h), the service's
// report (vibe_ndb_service.h) and the gallery's day (vibe_ndb_gallery.h). Synthetic NDBs from tools/ndb-bench/ndb_synth.h
// (ICAO keying, A2A at 95 % depth / A1A), SNR as carrier over noise in 2.5 kHz. The measuring bench is scripts/test-ndb.sh.
// ★ Silent: nothing is played.
#include "decoders/ndb_skimmer.h"
#include "vibe_ndb_db.h"
#include "vibe_ndb_gallery.h"
#include "vibe_ndb_service.h"
#include "ndb_synth.h"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <vector>

using namespace vibe;
using namespace ndbsynth;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); (c ? passes : fails)++; }

/** One beacon through the bare engine; the idents confirmed. */
static std::vector<std::string> runOne(const Beacon& b, double secs, uint64_t seed, double* firstAt = nullptr) {
    Rng rng(seed);
    std::vector<cf> iq; synth({ b }, 48000, secs, rng, iq);
    ndb::Skimmer::Config c; c.rate = 48000;
    ndb::Skimmer sk(c); sk.setStartUtcMs(0);
    std::vector<std::string> got;
    sk.onIdent = [&](const ndb::BeaconView& v) { if (got.empty() && firstAt) *firstAt = sk.nowMs() / 1000.0; got.push_back(v.ident); };
    for (size_t i = 0; i < iq.size(); i += 4096) sk.feedIq(iq.data() + i, (int)std::min<size_t>(4096, iq.size() - i));
    return got;
}

int main() {
    std::printf("NDB skimmer (%s kernels)\n", ndb::Skimmer::simdName());

    std::printf("\n── the vote ──\n");
    {
        ndb::IdentVote v;
        ok(!v.add("WOD", 0) && !v.add("WOD", 10000), "two readings confirm nothing");
        ok(v.add("WOD", 20000) && v.ident() == "WOD", "three agreeing readings confirm WOD");
        ndb::IdentVote n;
        for (int i = 0; i < 12; ++i) n.add(i % 2 ? "EE" : "E", i * 10000);
        ok(!n.confirmed(), "noise-like readings (E, EE) never confirm");
        ndb::IdentVote m;
        m.add("MD", 0); m.add("MOD", 10000); m.add("WOD", 20000); m.add("MOD", 30000);
        ok(!m.confirmed(), "MOD, WOD, MOD (2 of 3) is not yet a consensus");
        ndb::IdentVote f;
        f.setFoldHint("WOD");
        f.add("MOD", 0); f.add("MOD", 10000); f.add("MOD", 20000); f.add("MOD", 30000);
        ok(!f.confirmed(), "a fold that reads WOD vetoes four single readings of MOD");
        ndb::IdentVote g;
        ok(g.addFolded({ "BHX", "BHX", "BHX" }, 0) && g.ident() == "BHX", "three disjoint folded groups agreeing confirm");
        ndb::IdentVote h;
        ok(!h.addFolded({ "BHX", "BUX" }, 0), "two groups that disagree do not");
    }

    std::printf("\n── one beacon ──\n");
    for (double snr : { 10.0, -5.0 }) {
        for (double tone : { 1020.0, 400.0, 0.0 }) {
            Beacon b; b.ident = "WOD"; b.offsetHz = 3000.3; b.snrDb = snr; b.toneHz = tone;
            double at = 0;
            const auto got = runOne(b, 120, 7 + (uint64_t)tone, &at);
            char w[160]; std::snprintf(w, sizeof w, "%+.0f dB, %s: WOD confirmed (in %.0f s), nothing else", snr,
                                       tone > 0 ? (tone > 700 ? "1020 Hz A2A" : "400 Hz A2A") : "A1A", at);
            ok(got.size() == 1 && got[0] == "WOD", w);
        }
    }
    {
        Beacon b; b.ident = "BHX"; b.offsetHz = -1863; b.snrDb = -10; b.toneHz = 400; b.periodSec = 10;
        const auto got = runOne(b, 240, 11);
        ok(got.size() == 1 && got[0] == "BHX", "-10 dB 400 Hz: BHX confirmed by the fold, nothing else");
    }
    for (double wpm : { 5.0, 12.0 }) {
        Beacon b; b.ident = "EME"; b.offsetHz = 1234; b.snrDb = 0; b.toneHz = 1020; b.wpm = wpm; b.periodSec = 14;
        const auto got = runOne(b, 150, 21);
        ok(got.size() == 1 && got[0] == "EME", std::to_string((int)wpm) + " WPM: EME confirmed");
    }

    std::printf("\n── pure noise ──\n");
    {
        ndb::Skimmer::Config c; c.rate = 48000;
        ndb::Skimmer sk(c);
        int idents = 0, carriers = 0;
        sk.onIdent = [&](const ndb::BeaconView&) { ++idents; };
        for (int m = 0; m < 5; ++m) {
            Rng rng(500 + m);
            std::vector<cf> iq; synth({}, 48000, 60, rng, iq);
            for (size_t i = 0; i < iq.size(); i += 4096) sk.feedIq(iq.data() + i, (int)std::min<size_t>(4096, iq.size() - i));
            carriers = std::max(carriers, sk.carriers());
        }
        ok(idents == 0 && carriers == 0, "5 minutes of noise: no carrier, no ident (" + std::to_string(carriers) + ", " + std::to_string(idents) + ")");
        ok(std::fabs(10 * std::log10(sk.noiseCalibration())) < 0.3, "the floor is calibrated on noise (within 0.3 dB)");
    }

    std::printf("\n── the published list ──\n");
    {
        ok(vibendb::kNavaidCount > 4000, "the table holds " + std::to_string(vibendb::kNavaidCount) + " NDBs");
        const double lat = 52.29, lon = -0.85;   // ★ near Northampton
        const auto m = vibendb::match(351999, "WOD", true, lat, lon);
        ok(m.kind == vibendb::Match::Exact && m.n.name() == "Woodley" && m.n.cc() == "GB" && m.km > 80 && m.km < 110,
           "WOD at 351.999 kHz is Woodley, GB, " + std::to_string((int)m.km) + " km");
        ok(m.deg > 160 && m.deg < 200, "…and lies to the south (" + std::to_string((int)m.deg) + "°)");
        const auto nr = vibendb::match(351999, "WOT", true, lat, lon);
        ok(nr.kind == vibendb::Match::Near && nr.n.ident() == "WOD", "WOT on 352 kHz is ≈ WOD (one letter off)");
        const auto cand = vibendb::match(352000, "", true, lat, lon);
        ok(cand.kind == vibendb::Match::Candidate && !cand.others.empty(), "no ident: the nearest on the frequency is a candidate, the rest ranked");
        const auto un = vibendb::match(353499, "EME", true, lat, lon);
        ok(un.kind != vibendb::Match::Exact, "EME (heard, not in the list) is never matched to another beacon");
        const auto f = vibendb::nearest(lat, lon, 5);
        ok(f.size() == 5 && f[0].km <= f[4].km, "Find NDB: nearest first");
        ok(!vibendb::nearest(lat, lon, 10, "woodley").empty(), "Find NDB: by name");
    }

    std::printf("\n── the service: report, match, harmonic ──\n");
    {
        // WOD at 352.0 and CAM at 333.0 kHz in a 48 kHz slice centred on 342.5 kHz
        std::vector<Beacon> bs(2);
        bs[0].ident = "WOD"; bs[0].offsetHz = 352000 - 342500; bs[0].snrDb = 5; bs[0].toneHz = 400;
        bs[1].ident = "CAM"; bs[1].offsetHz = 333000 - 342500; bs[1].snrDb = 0; bs[1].toneHz = 400; bs[1].periodSec = 9;
        Rng rng(3); std::vector<cf> iq; synth(bs, 48000, 90, rng, iq);
        vibendb::Service s;
        int64_t clk = 1793000000;
        s.setClockForTest([&] { return clk; });
        s.start(48000, 342500, 320000, 365000, true, 52.29, -0.85);
        s.stop();   // ★ the tap thread is not wanted: the slice is fed directly
        for (size_t i = 0; i < iq.size(); i += 4800) { s.feedSliceForTest(iq.data() + i, (int)std::min<size_t>(4800, iq.size() - i), 48000, 342500); clk = 1793000000 + (int64_t)(i / 48000); }
        s.buildReportForTest();
        const std::string r = s.report();
        ok(r.rfind("{\"type\":\"ndb\"", 0) == 0, "a report");
        ok(r.find("\"ident\":\"WOD\"") != std::string::npos && r.find("\"name\":\"Woodley\"") != std::string::npos, "WOD named Woodley");
        ok(r.find("\"ident\":\"CAM\"") != std::string::npos && r.find("\"name\":\"Cambridge\"") != std::string::npos, "CAM named Cambridge");
        ok(r.find("\"k\":\"match\"") != std::string::npos && r.find("\"identified\":2") != std::string::npos, "two identified, both matched");
        const std::string st = s.stats();
        ok(st.find("\"ident\":\"WOD\"") != std::string::npos, "the stats carry WOD's SNR history");
        const std::string fd = s.find("", 20);
        ok(fd.find("\"heard\":true") != std::string::npos, "Find NDB marks the beacons heard");
    }
    {
        // a fundamental at 40 kHz and its 2nd harmonic at 80 kHz (both keyed with the same ident)
        std::vector<Beacon> bs(2);
        bs[0].ident = "KS"; bs[0].offsetHz = -20000; bs[0].snrDb = 15; bs[0].toneHz = 1020;
        bs[1] = bs[0]; bs[1].offsetHz = 20000; bs[1].snrDb = 0;
        Rng rng(4); std::vector<cf> iq; synth(bs, 48000, 60, rng, iq);
        vibendb::Service s;
        s.start(48000, 60000, 37000, 83000, false, 0, 0); s.stop();
        for (size_t i = 0; i < iq.size(); i += 4800) s.feedSliceForTest(iq.data() + i, (int)std::min<size_t>(4800, iq.size() - i), 48000, 60000);
        s.buildReportForTest();
        ok(s.report().find("\"harm\":{") != std::string::npos, "a carrier at twice another's frequency is flagged as its harmonic");
    }

    std::printf("\n── the gallery's day ──\n");
    {
        char t[256]; std::snprintf(t, sizeof t, "%s/ndbgal-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
        const std::string dir = mkdtemp(t) ? t : "/tmp";
        int64_t clk = 1793000000;
        {
            gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
            ndbgal::DayLog::Options o; o.origin = "live"; o.havePos = true; o.lat = 52.29; o.lon = -0.85; o.clock = [&] { return clk; };
            ndbgal::DayLog d(st, o);
            vibendb::HeardBeacon h; h.ident = "WOD"; h.hz = 351999; h.snrDb = 26; h.listed = true; h.name = "Woodley"; h.cc = "GB";
            h.lat = 51.4528; h.lon = -0.8789; h.km = 94; h.deg = 181;
            d.heard(h, clk);
            vibendb::HeardBeacon e; e.ident = "EME"; e.hz = 353499; e.snrDb = 23;
            d.heard(e, clk);
            const std::string png = d.mapPng();
            ok(png.size() > 1000 && png.compare(1, 3, "PNG") == 0, "the day's map is a PNG (" + std::to_string(png.size()) + " bytes)");
            clk += 120; d.tick();
            const std::string card = d.cardText();
            ok(card.find("WOD") != std::string::npos && card.find("Woodley") != std::string::npos && card.find("94 km") != std::string::npos,
               "the card lists WOD, Woodley, 94 km");
            ok(card.find("EME") != std::string::npos && card.find("not in the published list") != std::string::npos, "…and EME, unlisted");
            d.finish();
            ok(!d.cardName().empty() && !d.mapName().empty(), "the card and the map are in the gallery");
        }
        {
            gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
            ndbgal::DayLog::Options o; o.origin = "live"; o.clock = [&] { return clk + 60; };
            ndbgal::DayLog d(st, o);
            ok(d.beacons() == 2, "a restart reads the day's two beacons back");
        }
        std::string cmd = "rm -rf '" + dir + "'"; if (std::system(cmd.c_str()) != 0) std::printf("  (could not remove %s)\n", dir.c_str());
    }

    std::printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
