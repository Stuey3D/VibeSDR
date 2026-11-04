// test-ndb-gallery-clean.cpp — THE ONE-TIME NDB CARD CLEAN-UP (vibe_ndb_gallery_clean.h, V12 2026-11-04).
//
// ★★ WHY: the skimmer confirmed all-dot / all-dash idents (EEI, ES, SE, IEE …) — data carriers read as Morse — and the
//    cards already written list them. Stuart: "can we make it scan back and remove the garbage from the list too?"
//    Run over REAL cards from the Lenovo's gallery (vibeserver/testdata-ndb-gallery — public beacon data only), it pins:
//    · moved, not deleted: every uniform-ident unlisted line leaves the list for the section at the bottom;
//    · real unlisted beacons (SND, NWI, CDF, EMW) and published ones are untouched;
//    · the counts, Furthest, Best SNR and the index TITLE are what the writer would have written for what remains;
//    · one index line appended per changed card, its other fields (slot, partial, download name, radio) kept, its order
//      in the list kept; an unchanged card is not rewritten; the marker stops a second pass;
//    · the day's writer (DayLog) restarted on a cleaned card keeps the section and does not read it back as beacons.
//  `test-ndb-gallery-clean --all <cards dir> <index.jsonl>`: the pass over a whole copied gallery, with its numbers.
#include "vibe_ndb_gallery.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/ndbclean-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static void rmTree(const std::string& d) { std::string c = "rm -rf '" + d + "'"; if (std::system(c.c_str())) {} }
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void spit(const std::string& p, const std::string& b) { std::ofstream f(p, std::ios::binary); f << b; }
static size_t countOf(const std::string& s, const std::string& n) { size_t c = 0; for (size_t at = 0; (at = s.find(n, at)) != std::string::npos; at += n.size()) c++; return c; }
static std::vector<std::string> lines(const std::string& s) { std::vector<std::string> v; std::stringstream ss(s); std::string l; while (std::getline(ss, l)) v.push_back(l); return v; }
static std::string listPart(const std::string& card) { const size_t a = card.find(ndbgal::kSetAsideHeading); return a == std::string::npos ? card : card.substr(0, a); }
static std::string asidePart(const std::string& card) { const size_t a = card.find(ndbgal::kSetAsideHeading); return a == std::string::npos ? std::string() : card.substr(a); }
static bool isGarbageLine(const std::string& l) {
    ndbgal::clean::Beacon b;
    const bool day = l.size() > 2 && l[2] == ':';
    return ndbgal::clean::garbage(l, day, b);
}
static bool hasLine(const std::string& text, const std::string& prefix) { for (auto& l : lines(text)) if (l.rfind(prefix, 0) == 0) return true; return false; }
static std::string lastIndexLine(const std::string& dir, const std::string& name) {
    std::string last; for (auto& l : lines(slurp(dir + "/index.jsonl"))) if (l.find("\"f\":\"" + name + "\"") != std::string::npos) last = l; return last;
}
static void copyDir(const std::string& from, const std::string& to) { std::string c = "cp -p '" + from + "'/* '" + to + "'/"; if (std::system(c.c_str())) {} }

static const char* kDay = "20261103-000000-ndb-362k5-3448.txt";
static const char* kHr07 = "20261103-070000-ndb-362k5-9e72.txt";
static const char* kHr01 = "20261103-010000-ndb-362k5-036e.txt";
static const char* kClean = "20261027-000000-ndb-362k5-aecd.txt";
static const char* kRoll = "20261028-105805-ndb-362k5-9bbc.txt";

/** ★ The pass over a whole copied gallery: its numbers, and every moved line checked. */
static int all(const std::string& cards, const std::string& index) {
    const std::string d = tmpDir("all");
    copyDir(cards, d);
    spit(d + "/index.jsonl", slurp(index));
    // before: every card's list lines
    std::map<std::string, std::string> before;
    { DIR* dd = opendir(d.c_str()); while (dirent* e = dd ? readdir(dd) : nullptr) { std::string n = e->d_name; if (n.size() > 4 && n.substr(n.size() - 4) == ".txt") before[n] = slurp(d + "/" + n); } if (dd) closedir(dd); }
    gallery::Store s; s.setDir(d);
    const ndbgal::CleanStats st = ndbgal::cleanUniformIdents(s);
    std::printf("cards read %zu · changed %zu · lines moved %zu · busy %zu · failed %zu · distinct idents %zu\n", st.cards, st.changed, st.lines, st.skipped, st.failed, st.idents.size());
    std::printf("idents moved:"); for (auto& kv : st.idents) std::printf(" %s×%zu", kv.first.c_str(), kv.second); std::printf("\n");
    size_t bad = 0, lost = 0, movedSeen = 0;
    for (auto& kv : before) {
        const std::string after = slurp(d + "/" + kv.first);
        if (after == kv.second) continue;
        // every line that left the list is in the section, uniform and unlisted; nothing else changed in the list but counts
        const std::string a = asidePart(after);
        for (auto& l : lines(listPart(kv.second))) {
            if (listPart(after).find(l + "\n") != std::string::npos) continue;
            if (isGarbageLine(l)) { movedSeen++; if (a.find(l + "\n") == std::string::npos) { lost++; std::printf("  LOST %s: %s\n", kv.first.c_str(), l.c_str()); } }
            else if (l.find(" \xc2\xb7 ") != std::string::npos && l.find(" kHz") != std::string::npos) { bad++; std::printf("  MIXED/LISTED MOVED %s: %s\n", kv.first.c_str(), l.c_str()); }
        }
        for (auto& l : lines(a)) if (l != ndbgal::kSetAsideHeading && !l.empty() && !isGarbageLine(l)) { bad++; std::printf("  NOT GARBAGE in section %s: %s\n", kv.first.c_str(), l.c_str()); }
    }
    for (auto& kv : st.idents) if (!vibendb::uniformIdent(kv.first)) { bad++; std::printf("  a mixed ident moved: %s\n", kv.first.c_str()); }
    std::printf("checked: %zu moved lines found in their sections, %zu lost, %zu wrongly moved\n", movedSeen, lost, bad);
    // a second pass changes nothing
    const ndbgal::CleanStats again = ndbgal::cleanUniformIdents(s, /*force=*/true);
    std::printf("a second (forced) pass: %zu changed\n", again.changed);
    if (const char* keep = std::getenv("NDBCLEAN_KEEP")) { std::printf("kept in %s\n", d.c_str()); (void)keep; } else rmTree(d);
    return bad || lost || again.changed ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--all") return all(argv[2], argv[3]);
    const std::string fix = argc >= 2 ? argv[1] : "vibeserver/testdata-ndb-gallery";
    std::printf("test-ndb-gallery-clean — all-dot / all-dash unlisted idents set aside on the cards already written\n");

    std::printf("── the ident rule ──\n");
    ok(vibendb::uniformIdent("EEI") && vibendb::uniformIdent("ES") && vibendb::uniformIdent("EHE") && vibendb::uniformIdent("E") && vibendb::uniformIdent("5"),
       "only dots (E I S H 5): EEI, ES, EHE, E, 5");
    ok(vibendb::uniformIdent("T") && vibendb::uniformIdent("MO") && vibendb::uniformIdent("OTT0"), "only dashes (T M O 0): T, MO, OTT0");
    ok(!vibendb::uniformIdent("SND") && !vibendb::uniformIdent("EMW") && !vibendb::uniformIdent("NWI") && !vibendb::uniformIdent("CDF")
       && !vibendb::uniformIdent("ET") && !vibendb::uniformIdent(""), "mixed (SND, EMW, NWI, CDF, ET) and empty: not uniform");

    {
        ndbgal::clean::Beacon b;
        const char* D = " \xc2\xb7 ";
        const std::string withKm = std::string("IS    ") + D + "349.965 kHz" + D + "heard 8 min" + D + "best 0 dB" + D + "7901 km" + D + "75\xc2\xb0 ENE" + D + "BD" + D + "(not in the published list)";
        const std::string noName = std::string("EEI   ") + D + "290.378 kHz" + D + "heard 2 min" + D + "best 1 dB" + D + "-" + D + "-" + D + "-" + D + "(published, no name)";
        const std::string unlisted = std::string("EEI   ") + D + "290.378 kHz" + D + "heard 2 min" + D + "best 1 dB" + D + "-" + D + "-" + D + "-" + D + "(not in the published list)";
        ok(!ndbgal::clean::garbage(withKm, false, b), "a uniform ident WITH a distance (it matched the list) is never moved, whatever its station says");
        ok(!ndbgal::clean::garbage(noName, false, b), "\"(published, no name)\" is listed: never moved");
        ok(ndbgal::clean::garbage(unlisted, false, b), "a uniform ident, no distance, not in the list: moved");
    }
    const std::string d = tmpDir("fix");
    copyDir(fix, d);
    {   // ★ the rolling card as it was live: two of the 07:00 hour's data-carrier lines in its list (the copy has none)
        std::string r = slurp(d + "/" + kRoll);
        const std::string at = "EMW    \xc2\xb7 392.996";
        r.insert(r.find(at), "ES     \xc2\xb7 349.136 kHz \xc2\xb7 heard 7 min \xc2\xb7 best 12 dB \xc2\xb7 - \xc2\xb7 - \xc2\xb7 - \xc2\xb7 (not in the published list)\n"
                             "SE     \xc2\xb7 349.182 kHz \xc2\xb7 heard 2 min \xc2\xb7 best 3 dB \xc2\xb7 - \xc2\xb7 - \xc2\xb7 - \xc2\xb7 (not in the published list)\n");
        const size_t bh = r.find("Beacons heard:     3"); r.replace(bh, 20, "Beacons heard:     5");
        spit(d + "/" + kRoll, r);
    }
    const std::string day0 = slurp(d + "/" + kDay), h07 = slurp(d + "/" + kHr07), h01 = slurp(d + "/" + kHr01), clean0 = slurp(d + "/" + kClean);
    struct stat cleanSt{}; ::stat((d + "/" + kClean).c_str(), &cleanSt);
    const size_t idx0 = lines(slurp(d + "/index.jsonl")).size();
    gallery::Store s; s.setDir(d); s.setRadioLabel("SDRplay RSP1A");
    const std::vector<std::string> order0 = s.names("ndb");

    std::printf("── the pass ──\n");
    const ndbgal::CleanStats st = ndbgal::cleanUniformIdents(s);
    ok(st.ran && st.cards == 5 && st.changed == 4 && st.skipped == 0 && st.failed == 0, "5 cards read, 4 changed (the day, two hours, the rolling card), none busy");
    struct stat mk{};
    ok(::stat((d + "/" + ndbgal::kCleanMarker).c_str(), &mk) == 0, "the marker is written");
    ok(s.names("ndb") == order0, "the cards keep their names and their order in the list");

    std::printf("── the day's log (128 beacons, 55 of them data carriers) ──\n");
    const std::string day1 = slurp(d + "/" + kDay);
    size_t garb0 = 0; for (auto& l : lines(day0)) garb0 += isGarbageLine(l);
    size_t inList = 0, inAside = 0; for (auto& l : lines(listPart(day1))) inList += isGarbageLine(l); for (auto& l : lines(asidePart(day1))) inAside += isGarbageLine(l);
    ok(garb0 == 55 && inList == 0 && inAside == 55, "all 55 uniform unlisted lines moved to the section, none left in the list");
    bool allThere = true; for (auto& l : lines(day0)) if (isGarbageLine(l) && asidePart(day1).find(l + "\n") == std::string::npos) allThere = false;
    ok(allThere, "each moved line is in the section verbatim (nothing deleted)");
    ok(hasLine(listPart(day1), "23:58:55 \xc2\xb7 01:01:58 \xc2\xb7 NWI") && listPart(day1).find(" \xc2\xb7 CDF \xc2\xb7 ") != std::string::npos
       && listPart(day1).find(" \xc2\xb7 SND \xc2\xb7 ") != std::string::npos && listPart(day1).find(" \xc2\xb7 EMW \xc2\xb7 ") != std::string::npos,
       "real unlisted beacons NWI, CDF, SND, EMW stay in the list");
    ok(listPart(day1).find(" \xc2\xb7 IS \xc2\xb7 349.965 kHz") == std::string::npos || listPart(day1).find("Ishurdi") != std::string::npos,
       "a published match (IS · Ishurdi) is not judged by its ident");
    ok(day1.find("\n73 beacons identified \xc2\xb7 furthest: PIS Pisarovina 1425 km\n") != std::string::npos, "\"73 beacons identified · furthest: PIS Pisarovina 1425 km\"");
    const std::string dl = lastIndexLine(d, kDay);
    ok(dl.find("\"title\":\"NDB \xc2\xb7 2026-11-03 \xc2\xb7 73 beacons heard \xc2\xb7 furthest PIS 1425 km\"") != std::string::npos, "the index title: NDB · 2026-11-03 · 73 beacons heard · furthest PIS 1425 km");
    ok(dl.find("\"s\":\"ndbsum-2026-11-03\"") != std::string::npos && dl.find("\"dn\":\"NDB 2026-11-03 heard.txt\"") != std::string::npos
       && dl.find("\"t\":1793664000") != std::string::npos && dl.find("\"r\":\"SDRplay RSP1A\"") != std::string::npos && dl.size() > 8 && dl.substr(dl.size() - 7) == "\"re\":1}",
       "its new index line keeps slot, download name, time and radio, and stands where it was (re:1)");
    ok(day1.substr(day1.size() - 1) == "\n" && day1.find(std::string("\n\n") + ndbgal::kSetAsideHeading + "\n") != std::string::npos, "the section is at the bottom, after a blank line");

    std::printf("── an hourly card whose best SNR was a data carrier (EIE 21 dB) ──\n");
    const std::string a07 = slurp(d + "/" + kHr07);
    ok(a07.find("Beacons heard:     13\n") != std::string::npos, "22 → 13 beacons heard");
    ok(a07.find("Furthest:          BRI Bristol \xe2\x80\x94 164 km SW\n") != std::string::npos, "Furthest BRI unchanged");
    ok(a07.find("Best SNR:          BHX 19 dB\n") != std::string::npos, "Best SNR re-chosen: BHX 19 dB");
    ok(listPart(a07).find("NWI    \xc2\xb7 342.500") != std::string::npos && listPart(a07).find("MVC    \xc2\xb7") != std::string::npos
       && listPart(a07).find("CAM    \xc2\xb7 331.002") != std::string::npos, "NWI, MVC and the unlisted CAM line stay");
    const std::string l07 = lastIndexLine(d, kHr07);
    ok(l07.find("\"title\":\"NDB 07:00\xe2\x80\x93" "07:44 UTC \xc2\xb7 13 beacons \xc2\xb7 furthest BRI 164 km \xc2\xb7 best BHX 19 dB\"") != std::string::npos,
       "the index title: NDB 07:00–07:44 UTC · 13 beacons · furthest BRI 164 km · best BHX 19 dB");
    ok(l07.find("\"partial\":true") != std::string::npos && l07.find("\"dn\":\"NDB 2026-11-03 0700 UTC.txt\"") != std::string::npos, "…still partial, same download name");
    const std::string a01 = slurp(d + "/" + kHr01);
    size_t n01 = 0; for (auto& l : lines(h01)) n01 += isGarbageLine(l);
    char want01[64]; std::snprintf(want01, sizeof want01, "Beacons heard:     %zu\n", (size_t)18 - n01);
    ok(a01.find(want01) != std::string::npos && a01.find("Best SNR:          ES") == std::string::npos && a01.find("Furthest:          CRE Cres") != std::string::npos,
       "the 01:00 hour: " + std::to_string(18 - n01) + " beacons, best no longer ES, furthest CRE kept");
    const std::string l01 = lastIndexLine(d, kHr01);
    ok(l01.find("\"title\":\"NDB 01:00\xe2\x80\x93" "02:00 UTC \xc2\xb7 " + std::to_string(18 - n01) + " beacons \xc2\xb7 furthest CRE 1389 km \xc2\xb7 best ") != std::string::npos
       && l01.find("best ES ") == std::string::npos, "…and its title");

    std::printf("── the rolling last-10 card ──\n");
    const std::string ar = slurp(d + "/" + kRoll);
    ok(ar.find("Beacons heard:     3\n") != std::string::npos && asidePart(ar).find("ES     \xc2\xb7 349.136") != std::string::npos && listPart(ar).find("EMW    \xc2\xb7 392.996") != std::string::npos,
       "ES and SE set aside, EMW kept, 3 beacons");
    const std::string lr = lastIndexLine(d, kRoll);
    ok(lr.find("\"title\":\"NDB last 10 min \xc2\xb7 3 beacons \xc2\xb7 furthest BHX 62 km \xc2\xb7 best BHX 13 dB\"") != std::string::npos && lr.find("\"s\":\"ndblast10\"") != std::string::npos,
       "its title keeps \"NDB last 10 min\", its slot is kept");

    std::printf("── what does not change ──\n");
    struct stat cleanSt2{}; ::stat((d + "/" + kClean).c_str(), &cleanSt2);
    ok(slurp(d + "/" + kClean) == clean0 && cleanSt2.st_ino == cleanSt.st_ino && cleanSt2.st_mtime == cleanSt.st_mtime, "a card with nothing to move is not rewritten");
    const size_t idx1 = lines(slurp(d + "/index.jsonl")).size();
    ok(idx1 == idx0 + 4, "one index line appended per changed card (4)");
    const ndbgal::CleanStats st2 = ndbgal::cleanUniformIdents(s);
    ok(!st2.ran && lines(slurp(d + "/index.jsonl")).size() == idx1, "the marker: a second start does nothing");
    const ndbgal::CleanStats st3 = ndbgal::cleanUniformIdents(s, /*force=*/true);
    ok(st3.ran && st3.changed == 0 && slurp(d + "/" + kDay) == day1, "forced again: idempotent, nothing changes");
    ok(h07 != a07, "(the 07:00 card did change)");

    std::printf("── a card held open by a live writer ──\n");
    {
        const std::string d2 = tmpDir("open"); copyDir(fix, d2);
        gallery::Store s2; s2.setDir(d2); s2.setRadioLabel("SDRplay RSP1A");
        gallery::Meta m; m.kind = "ndb"; m.origin = "background"; m.freqHz = 362500; m.decoder = "ndb"; m.profile = "Profile 1"; m.slot = "ndbsum-2026-11-03";
        const std::string held = s2.adoptSlot(m, true);
        const ndbgal::CleanStats o = ndbgal::cleanUniformIdents(s2);
        struct stat mk2{};
        ok(held == kDay && o.skipped == 1 && slurp(d2 + "/" + kDay) == day0 && ::stat((d2 + "/" + ndbgal::kCleanMarker).c_str(), &mk2) != 0,
           "skipped, untouched, and no marker (the next start finishes it)");
        s2.close(held);
        const ndbgal::CleanStats o2 = ndbgal::cleanUniformIdents(s2);
        ok(o2.ran && o2.changed == 1 && ::stat((d2 + "/" + ndbgal::kCleanMarker).c_str(), &mk2) == 0, "the next pass cleans it and marks done");
        rmTree(d2);
    }

    std::printf("── the day's writer restarted on the cleaned card ──\n");
    {
        int64_t clk = 1793664000 + 20 * 3600;   // 2026-11-03 20:00 UTC
        ndbgal::DayLog::Options o; o.origin = "background"; o.profile = "Profile 1"; o.clock = [&] { return clk; };
        auto log = std::make_unique<ndbgal::DayLog>(s, o);
        ok(log->cardName() == kDay && log->beacons() == 73, "it adopts the day's card and reads back 73 beacons, not 128");
        const std::string t = log->cardText();
        size_t li = 0, as = 0; for (auto& l : lines(listPart(t))) li += isGarbageLine(l); for (auto& l : lines(asidePart(t))) as += isGarbageLine(l);
        ok(li == 0 && as == 55, "its card keeps the 55 set-aside lines in the section, none back in the list");
        vibendb::HeardBeacon hb; hb.ident = "BHX"; hb.hz = 406000; hb.snrDb = 30; hb.listed = true; hb.name = "Birmingham"; hb.cc = "GB"; hb.km = 62; hb.deg = 285;
        log->heard(hb, clk);
        clk += 120; log->tick(); log.reset();
        const std::string w = slurp(d + "/" + kDay);
        size_t wa = 0; for (auto& l : lines(asidePart(w))) wa += isGarbageLine(l);
        ok(countOf(w, ndbgal::kSetAsideHeading) == 1 && wa == 55 && w.find("\n73 beacons identified") != std::string::npos, "rewritten by the writer: one section, 55 lines, 73 beacons");
    }
    rmTree(d);
    std::printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
