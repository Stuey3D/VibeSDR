// test-ndb-suspect.cpp — the "Suspected …" clue for an NDB heard but not in the published list, the day/night distance
// class, the "unlikely" flag on a far listed match heard by day, and the owner's confirm / reject (vibe_ndb_suspect.h,
// vibe_ndb_db.h OwnerBeacons, vibe_ndb_service.h, vibe_ndb_gallery.h). V12 2026-11-04.
// ★ Silent: nothing is played. Synthetic IQ from tools/ndb-bench/ndb_synth.h for the service's report.
// ★ VIBE_NDB_CARDS=<folder of real gallery cards>: the unlisted idents in them run through the clue (printed, not judged).
#include "vibe_ndb_db.h"
#include "vibe_ndb_gallery.h"
#include "vibe_ndb_service.h"
#include "vibe_ndb_suspect.h"
#include "ndb_synth.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace vibe;
using namespace ndbsynth;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); (c ? passes : fails)++; }

static const double kLat = 52.29, kLon = -0.85;   // the receiver of test-ndb (Northamptonshire, IO92)
static vibendb::DayNight byDay() { vibendb::DayNight d; d.day = 40; d.night = 5; return d; }
static vibendb::DayNight nightOnly() { vibendb::DayNight d; d.night = 40; return d; }
static std::string best(const std::string& id, double khz, const vibendb::DayNight& dn, bool pos = true) {
    const auto v = vibendb::suspects(id, khz * 1000, pos, kLat, kLon, dn);
    return v.empty() ? std::string("-") : v.front().icao();
}
static std::string tmpdir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? t : "/tmp";
}

int main() {
    std::printf("── the sun ──\n");
    {
        // 2026-06-21 12:00 UTC at 52.3 N 0.85 W: ≈ 61°; 00:00 UTC: ≈ −14°; 2026-11-04 10:00 UTC: ≈ 18°
        const double noon = vibendb::sunElevationDeg(kLat, kLon, 1782043200), midnight = vibendb::sunElevationDeg(kLat, kLon, 1782000000);
        const double nov = vibendb::sunElevationDeg(kLat, kLon, 1793786400);
        ok(noon > 59 && noon < 63, "midsummer noon: " + std::to_string(noon));
        ok(midnight < -12 && midnight > -16, "midsummer midnight: " + std::to_string(midnight));
        ok(nov > 14 && nov < 22, "a November morning, 10:00 UTC: " + std::to_string(nov));
        vibendb::DayNight d; for (int i = 0; i < 12; ++i) d.add(nov);
        ok(d.byDay() && !d.nightOnly() && std::string(d.words()) == "heard by day", "12 minutes in that sun = heard by day");
        vibendb::DayNight n; for (int i = 0; i < 12; ++i) n.add(midnight); n.add(2.0);   // the grey line counts as neither
        ok(n.nightOnly() && n.day == 0, "12 minutes at midnight = heard only after dark; 2° counts as neither");
    }

    std::printf("\n── the airfield-name clue ──\n");
    {
        // ★ SND / CDF / EMW are in the bundled list now (our own receptions) — suspects() never reads the list for its
        //   answer (only to boost an airfield already published with the same ident on ANOTHER frequency, and never
        //   from our receptions), so these are the clue as it would have been before we added them.
        const auto snd = vibendb::suspects("SND", 362500, true, kLat, kLon, byDay());
        ok(!snd.empty() && snd.front().icao() == "EGMC", "SND 362.5: Southend (EGMC) first, over Stansted (the N-D together) — " + (snd.empty() ? std::string("none") : snd.front().why));
        ok(best("CDF", 388.5, byDay()) == "EGFF", "CDF 388.5: Cardiff (EGFF)");
        ok(best("NWI", 342.5, byDay()) == "EGSH", "NWI 342.5: Norwich (EGSH) — its IATA code");
        ok(best("NQY", 347.0, byDay()) == "EGHQ", "NQY 347.0: Newquay (EGHQ) — its IATA code");
        ok(best("CAM", 332.5, byDay()) == "EGSC", "CAM: Cambridge (EGSC)");
        const auto cdf = vibendb::suspects("CDF", 388500, true, kLat, kLon, byDay());
        ok(!cdf.empty() && cdf.front().why.find("CDF \xe2\x80\x94 the letters of Cardiff \xc2\xb7 ") == 0 && cdf.front().why.find("heard by day") != std::string::npos,
           "the evidence in plain words: " + (cdf.empty() ? std::string() : cdf.front().why));
        const auto mld = vibendb::suspects("MLD", 400000, true, 52.36, 0.49, byDay());
        bool mild = false; for (auto& s : mld) mild |= s.icao() == "EGUN" && s.name() == "RAF Mildenhall";
        ok(mild, "MLD near Mildenhall: \"RAF Mildenhall\" (the RAF prefix kept in the name, never matched on)");
    }
    {
        // ★ absurd matches are not made
        ok(best("SND", 362.5, byDay(), false) == "-", "no receiver position: no clue");
        ok(best("E", 350, byDay()) == "-" && best("EEEEE", 350, byDay()) == "-" && best("X5", 350, byDay()) == "-",
           "one letter, five letters, a digit: no clue");
        ok(best("QXZ", 350, nightOnly()) == "-", "QXZ fits nothing");
        // a two-letter ident: never a small field, never beyond 300 km
        const auto mf = vibendb::suspects("MF", 421000, true, kLat, kLon, byDay());
        bool small = false; for (auto& s : mf) small |= s.a->size == 0;
        ok(!small, "MF: no small airfield suggested for two letters");
        const auto far = vibendb::suspects("NJ", 385000, true, kLat, kLon, nightOnly());
        bool beyond = false; for (auto& s : far) beyond |= s.km > 300;
        ok(!far.empty() && !beyond, "NJ by night: two letters are never suggested beyond 300 km (" + (far.empty() ? std::string("none") : far.front().icao()) + ")");
        ok(best("EEI", 349, byDay()) == "-" && best("IEE", 350, byDay()) == "-" && best("ES", 288, byDay()) == "-" && best("IS", 302, byDay()) == "-",
           "noise-like idents (only E I S H T: EEI, IEE, ES, IS) get no clue");
        // reach: by day 600 km; after dark 1 500 km and only a strong fit beyond 600
        bool dayFar = false; for (auto& s : vibendb::suspects("MRV", 404000, true, kLat, kLon, byDay())) dayFar |= s.km > 600;
        ok(!dayFar, "heard by day: nothing beyond 600 km");
        bool weakFar = false;
        for (const char* id : { "MRV", "WSN", "PHG", "EEI", "IEE", "MVC", "NRD" })
            for (auto& s : vibendb::suspects(id, 400000, true, kLat, kLon, nightOnly())) weakFar |= s.km > 600 && s.score < 70;
        ok(!weakFar, "after dark: beyond 600 km only a fit of 70 or more");
        // the evidence never names a stray letter or an accent's half
        bool stray = false;
        for (const char* id : { "SND", "EEI", "NWI", "PHG", "MRV" })
            for (auto& s : vibendb::suspects(id, 400000, true, kLat, kLon, nightOnly())) stray |= s.why.find(" S End") != std::string::npos || s.why.find('#') != std::string::npos;
        ok(!stray, "no \"S End\" from Land's End, no '#' from an accented name");
    }

    std::printf("\n── unlikely: a far listed match heard by day ──\n");
    {
        ok(!vibendb::unlikelyWhy(7901, byDay()).empty(), "7 901 km heard by day: unlikely (" + vibendb::unlikelyWhy(7901, byDay()) + ")");
        ok(vibendb::unlikelyWhy(7901, nightOnly()).empty(), "7 901 km heard only after dark: not flagged (sky wave)");
        ok(vibendb::unlikelyWhy(400, byDay()).empty(), "400 km by day: not flagged");
        ok(vibendb::unlikelyWhy(7901, vibendb::DayNight()).empty(), "no day/night evidence yet: not flagged");
    }

    std::printf("\n── the service's report ──\n");
    {
        vibendb::ownerBeacons().resetForTest();
        // ★ the IS / Ishurdi shape: a listed beacon thousands of km away, heard all morning. (IS itself — all dots — is
        //   read as M by the synthetic bench's speed estimate, so a three-letter far one stands in; the real IS is in
        //   the unit check above.) And NQY on 349.0 (unlisted there: our own list has Newquay's real NQY on 347.0 since 2026-11-04), in a slice centred on 342.5 kHz.
        const vibendb::NavaidRec* far = nullptr; double farKm = 0;
        for (size_t i = 0; i < vibendb::kNavaidCount && !far; ++i) {
            const auto& n = vibendb::kNavaids[i];
            if (n.hz < 355000 || n.hz > 364000 || std::strlen(n.ident) != 3 || !*(vibendb::kNavaidNames + n.name)) continue;
            double km, deg; vibendb::distBearing(kLat, kLon, n.lat, n.lon, km, deg);
            const auto m = vibendb::match(n.hz, n.ident, true, kLat, kLon);
            if (km > 4000 && m.kind == vibendb::Match::Exact && m.n.r == &n) { far = &n; farKm = km; }
        }
        ok(far != nullptr, std::string("a far listed beacon to stand in for IS: ") + (far ? far->ident : "none") + " " + std::to_string((int)farKm) + " km");
        if (!far) return 1;
        const std::string farName = vibendb::kNavaidNames + far->name;
        std::vector<Beacon> bs(2);
        bs[0].ident = far->ident; bs[0].offsetHz = far->hz - 342500.0; bs[0].snrDb = 8; bs[0].toneHz = 400;
        bs[1].ident = "NQY"; bs[1].offsetHz = 349000 - 342500; bs[1].snrDb = 8; bs[1].toneHz = 400; bs[1].periodSec = 9;
        Rng rng(11); std::vector<cf> iq; synth(bs, 48000, 90, rng, iq);
        vibendb::Service s;
        int64_t clk = 1793786400;   // 2026-11-04 10:00 UTC — the sun up at the receiver
        s.setClockForTest([&] { return clk; });
        s.start(48000, 342500, 320000, 365000, true, kLat, kLon);
        s.stop();
        s.seedDayNightForTest(far->ident, far->hz, 45, 0);   // heard all morning
        s.seedDayNightForTest("NQY", 349000, 30, 0);
        for (size_t i = 0; i < iq.size(); i += 4800) { s.feedSliceForTest(iq.data() + i, (int)std::min<size_t>(4800, iq.size() - i), 48000, 342500); clk = 1793786400 + (int64_t)(i / 48000); }
        s.buildReportForTest();
        const std::string r = s.report();
        const size_t is = r.find("\"ident\":\"" + std::string(far->ident) + "\""), nq = r.find("\"ident\":\"NQY\"");
        ok(is != std::string::npos && nq != std::string::npos, "both read");
        if (is == std::string::npos || nq == std::string::npos) std::printf("%s\n", r.substr(0, 3000).c_str());
        ok(r.find(farName) != std::string::npos && r.find("\"unlikely\":\"") != std::string::npos, "matched to " + farName + " and flagged unlikely");
        ok(r.find("\"far\":{") == std::string::npos, "…and not the furthest (nothing else listed was heard)");
        ok(r.find("\"sus\":{\"icao\":\"EGHQ\"") != std::string::npos, "NQY suspected Newquay (EGHQ) in the row");
        const size_t logAt = r.find("\"log\":[");
        ok(logAt != std::string::npos && r.find("\"sus\":{\"icao\":\"EGHQ\"", logAt) != std::string::npos && r.find("\"unlikely\":\"", logAt) != std::string::npos,
           "…and both in the heard log");

        // ★★ the owner confirms NQY = Newquay: from then on an Exact match, named, positioned, the owner's source
        const std::string dir = tmpdir("ndbsus");
        vibendb::ownerBeacons().setFile(dir + "/ndb-confirmed.json");
        std::string note;
        ok(vibendb::ownerDecide("confirm", "NQY", 349001, "EGHQ", 1793786400, note).empty(), "the owner confirms NQY = EGHQ");
        ok(note.find("CONFIRMED NQY 349.0 kHz = Cornwall Airport Newquay EGHQ") != std::string::npos && note.find("ndb-heard.json") != std::string::npos,
           "the server-log line: " + note);
        const auto m = vibendb::match(349002, "NQY", true, kLat, kLon);
        ok(m.kind == vibendb::Match::Exact && m.n.owner() && m.n.source() == "confirmed by this server's owner" && m.km > 300 && m.km < 400,
           "NQY is now an Exact match, " + std::to_string((int)m.km) + " km, \"confirmed by this server's owner\"");
        clk += 60; s.buildReportForTest();
        const std::string r2 = s.report();
        ok(r2.find("\"name\":\"Cornwall Airport Newquay\"") != std::string::npos && r2.find("\"own\":true") != std::string::npos, "the report names it, marked the owner's");
        // persist + reload
        std::ifstream f(dir + "/ndb-confirmed.json"); std::stringstream ss; ss << f.rdbuf();
        ok(ss.str().find("\"ident\":\"NQY\"") != std::string::npos && ss.str().find("\"airfield\":\"EGHQ\"") != std::string::npos, "written to ndb-confirmed.json");
        ok(std::ifstream(dir + "/ndb-confirmed.json.tmp").fail(), "…atomically (no .tmp left)");
        vibendb::ownerBeacons().resetForTest();
        ok(vibendb::match(349002, "NQY", true, kLat, kLon).kind != vibendb::Match::Exact, "forgotten in memory…");
        vibendb::ownerBeacons().setFile(dir + "/ndb-confirmed.json");
        ok(vibendb::match(349002, "NQY", true, kLat, kLon).kind == vibendb::Match::Exact, "…and read back from the file (a restart)");
        // reject: Southend is never suggested again for SND@362.5; the next airfield is
        ok(vibendb::ownerDecide("reject", "SND", 362500, "EGMC", 1793786400, note).empty() && note.find("REJECTED") != std::string::npos, "the owner rejects SND = EGMC");
        bool still = false; for (auto& x : vibendb::suspects("SND", 362500, true, kLat, kLon, byDay())) still |= x.icao() == "EGMC";
        ok(!still, "…so Southend is not suggested for SND@362.5 again");
        bool other = false; for (auto& x : vibendb::suspects("SND", 380000, true, kLat, kLon, byDay())) other |= x.icao() == "EGMC";
        ok(other, "…but still is for SND on another frequency");
        ok(vibendb::ownerDecide("forget", "SND", 362500, "", 0, note).empty(), "forget undoes it");
        // bad input is refused
        ok(!vibendb::ownerDecide("confirm", "NQY", 349000, "ZZZZ", 0, note).empty(), "an unknown airfield is refused");
        ok(!vibendb::ownerDecide("confirm", "NQY", 5000000, "EGHQ", 0, note).empty(), "a frequency outside 150–1750 kHz is refused");
        ok(!vibendb::ownerDecide("delete-all", "NQY", 349000, "EGHQ", 0, note).empty(), "an unknown action is refused");
        vibendb::ownerBeacons().resetForTest();
        std::string cmd = "rm -rf '" + dir + "'"; if (std::system(cmd.c_str()) != 0) std::printf("  (could not remove %s)\n", dir.c_str());
    }

    std::printf("\n── the gallery's lines ──\n");
    {
        const std::string dir = tmpdir("ndbsusgal");
        int64_t clk = 1793786400;
        {
            gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
            ndbgal::DayLog::Options o; o.origin = "live"; o.havePos = true; o.lat = kLat; o.lon = kLon; o.clock = [&] { return clk; };
            ndbgal::DayLog d(st, o);
            vibendb::HeardBeacon w; w.ident = "WOD"; w.hz = 351999; w.snrDb = 26; w.listed = true; w.name = "Woodley"; w.cc = "GB";
            w.lat = 51.4528; w.lon = -0.8789; w.km = 94; w.deg = 181;
            d.heard(w, clk);
            vibendb::HeardBeacon i; i.ident = "IS"; i.hz = 349970; i.snrDb = 4; i.listed = true; i.name = "Ishurdi"; i.cc = "BD";
            i.lat = 24.15; i.lon = 89.05; i.km = 7901; i.deg = 75; i.unlikely = vibendb::unlikelyWhy(7901, byDay());
            d.heard(i, clk);
            vibendb::HeardBeacon n; n.ident = "NQY"; n.hz = 349000; n.snrDb = 9;
            const auto sv = vibendb::suspects("NQY", 349000, true, kLat, kLon, byDay());
            n.susIcao = sv.front().icao(); n.susName = sv.front().name(); n.susWhy = sv.front().why; n.susKm = sv.front().km; n.susLat = sv.front().a->lat; n.susLon = sv.front().a->lon;
            d.heard(n, clk);
            clk += 120; d.tick();
            const std::string card = d.cardText();
            ok(card.find("furthest: WOD") != std::string::npos, "the furthest is WOD, not the unlikely IS");
            ok(card.find("Ishurdi (unlikely: 7901 km away yet heard by day") != std::string::npos, "IS's line says unlikely, plainly");
            ok(card.find("(not in the published list) suspected Cornwall Airport Newquay (EGHQ), unconfirmed: NQY \xe2\x80\x94 NQY is its IATA code, ") != std::string::npos,
               "NQY's line carries the clue, never as fact");
            size_t nqLine = card.find("NQY \xc2\xb7"); std::string line = card.substr(nqLine, card.find('\n', nqLine) - nqLine);
            int dots = 0; for (size_t p = 0; (p = line.find(" \xc2\xb7 ", p)) != std::string::npos; p += 3) ++dots;
            ok(dots == 6, "…without breaking the line's fields (" + std::to_string(dots) + " separators after the times)");
            const std::string hour = d.hourText(clk);
            ok(hour.find("Furthest:          WOD") != std::string::npos && hour.find("unlikely") != std::string::npos && hour.find("suspected Cornwall") != std::string::npos,
               "the hour's card: furthest WOD, IS unlikely, NQY suspected");
            const std::string png = d.mapPng();
            ok(png.size() > 1000, "the map draws (IS skipped, NQY a ring)");
            d.finish();
        }
        {
            gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
            ndbgal::DayLog::Options o; o.origin = "live"; o.havePos = true; o.lat = kLat; o.lon = kLon; o.clock = [&] { return clk + 60; };
            ndbgal::DayLog d(st, o);
            const std::string card = d.cardText();
            ok(d.beacons() == 3 && card.find("furthest: WOD") != std::string::npos && card.find("Ishurdi (unlikely:") != std::string::npos,
               "a restart reads the three back, IS still unlikely and still not the furthest");
        }
        std::string cmd = "rm -rf '" + dir + "'"; if (std::system(cmd.c_str()) != 0) std::printf("  (could not remove %s)\n", dir.c_str());
    }

    // ★ the real cards: every unlisted ident in them through the clue (by day, from this receiver) — printed for the record
    if (const char* cards = std::getenv("VIBE_NDB_CARDS")) {
        std::printf("\n── the real cards (%s) ──\n", cards);
        std::map<std::string, int> seen;   // "IDENT@kHz" → lines
        if (DIR* dd = opendir(cards)) {
            while (dirent* de = readdir(dd)) {
                const std::string fn = de->d_name; if (fn.size() < 4 || fn.substr(fn.size() - 4) != ".txt") continue;
                std::ifstream f(std::string(cards) + "/" + fn); std::string l;
                while (std::getline(f, l)) {
                    if (l.find("(not in the published list)") == std::string::npos || l.find(" min \xc2\xb7") == std::string::npos) continue;
                    char id[8] = {}; double khz = 0;
                    if (std::sscanf(l.c_str(), "%7s \xc2\xb7 %lf kHz", id, &khz) == 2) seen[std::string(id) + "@" + std::to_string((int)std::lround(khz * 2))]++;
                }
            }
            closedir(dd);
        }
        for (const auto& kv : seen) {
            if (kv.second < 3) continue;   // ★ heard in 3+ hours: the real ones, not one-off misreads
            const std::string id = kv.first.substr(0, kv.first.find('@')); const double khz = std::atoi(kv.first.c_str() + id.size() + 1) / 2.0;
            const auto m = vibendb::match(khz * 1000, id, true, kLat, kLon);
            const auto v = vibendb::suspects(id, khz * 1000, true, kLat, kLon, byDay());
            std::printf("  %-5s %6.1f kHz %3d h  %s%s\n", id.c_str(), khz, kv.second, m.kind == vibendb::Match::Exact ? "(listed now) " : "",
                        v.empty() ? "no clue" : ("suspected " + v.front().name() + " " + v.front().icao() + ": " + v.front().why).c_str());
        }
    }

    std::printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
