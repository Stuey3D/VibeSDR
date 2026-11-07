// ★★★ THE EiBi SEASON PICK AND ITS FALLBACK (NickB, 2026-10-27: "curl: (22) The requested URL
// returned error: 404"). We switch to the winter file on 25 October; EiBi had not published
// sked-b26.csv yet. The server must use sked-a26.csv, say so in words, and ask for B26 again every
// few hours — not on every wake, and never report a 404 as a broken network.
//   build: run-tests.sh (deps: eibi.cpp proc.cpp). A fake fetcher and clock; no network.
#include "eibi.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

static int fails = 0, passes = 0;
#define CHECK(what, cond) do { if (cond) passes++; else { fails++; \
    std::printf("   FAIL %s  (line %d)\n", what, __LINE__); } } while (0)
static void eqs(const char* what, const std::string& got, const std::string& want) {
    if (got == want) { passes++; return; }
    fails++; std::printf("   FAIL %s\n     got  '%s'\n     want '%s'\n", what, got.c_str(), want.c_str());
}

static std::time_t utc(int y, int m, int d, int h = 12) {
    std::tm t{}; t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = h;
    return timegm(&t);
}

// ── The fake eibispace.de: file name → body; absent = 404; `down` = no network ──
static std::map<std::string, std::string> g_site;
static bool g_down = false;
static int  g_calls = 0;
static std::string g_lastUrl;
static vseibi::Fetch fakeFetch(const std::string& url, const std::string& dest, std::string& msg) {
    g_calls++; g_lastUrl = url;
    if (g_down) { msg = "curl: (6) Could not resolve host: www.eibispace.de"; return vseibi::Fetch::Failed; }
    const std::string file = url.substr(url.rfind('/') + 1);
    auto it = g_site.find(file);
    if (it == g_site.end()) return vseibi::Fetch::NotPublished;
    FILE* f = std::fopen(dest.c_str(), "wb");
    std::fwrite(it->second.data(), 1, it->second.size(), f);
    std::fclose(f);
    return vseibi::Fetch::Ok;
}

static std::time_t g_now = 0;
static std::string g_published;
static std::string g_dir;

static std::string csvWith(const char* station, int n) {
    std::string s = "kHz:75;Time(UTC):93;Days:59;ITU:49;Station:201;Lng:49;Target:62;Remarks:135;P:35;Start:60;Stop:60;\n";
    for (int i = 0; i < n; i++)
        s += std::to_string(5000 + i * 5) + ";0000-2400;;G;" + station + ";E;Eu;;1;;\n";
    return s;
}
static void ageCache(double hours) {      // set the cache's mtime relative to the fake clock
    struct utimbuf ub; ub.actime = ub.modtime = g_now - (std::time_t)(hours * 3600);
    utime((g_dir + "/eibi.csv").c_str(), &ub);
}
static void fresh() {
    for (const char* f : {"/eibi.csv", "/eibi.season", "/eibi.tried", "/eibi.csv.tmp"}) unlink((g_dir + f).c_str());
    g_site.clear(); g_down = false; g_calls = 0; g_published.clear();
}

int main() {
    char tmpl[] = "/tmp/vs-eibi-test-XXXXXX";
    g_dir = mkdtemp(tmpl);
    vseibi::setDir(g_dir);
    vseibi::setFetcher(fakeFetch);
    vseibi::setClock([] { return g_now; });
    vseibi::setPublisher([](const std::string& j) { g_published = j; });

    std::printf("── season arithmetic ──\n");
    eqs("27 Oct 2026 is B26",      vseibi::seasonCodeAt(utc(2026, 10, 27)), "b26");
    eqs("24 Oct 2026 is A26",      vseibi::seasonCodeAt(utc(2026, 10, 24)), "a26");
    eqs("15 Jan 2027 is still B26", vseibi::seasonCodeAt(utc(2027, 1, 15)), "b26");
    eqs("24 Mar 2027 is still B26", vseibi::seasonCodeAt(utc(2027, 3, 24)), "b26");
    eqs("25 Mar 2027 is A27",      vseibi::seasonCodeAt(utc(2027, 3, 25)), "a27");
    eqs("1 Jan 2100 is B99",       vseibi::seasonCodeAt(utc(2100, 1, 1)), "b99");
    eqs("before b26 is a26",       vseibi::previousSeason("b26"), "a26");
    eqs("before a27 is b26",       vseibi::previousSeason("a27"), "b26");
    eqs("before a00 is b99",       vseibi::previousSeason("a00"), "b99");
    eqs("label b26",               vseibi::seasonLabel("b26"), "B26 (winter)");
    eqs("label a26",               vseibi::seasonLabel("a26"), "A26 (summer)");

    std::printf("── B26 not published, no cache at all: falls back to A26 ──\n");
    fresh();
    g_now = utc(2026, 10, 27, 21);
    g_site["sked-a26.csv"] = csvWith("Summer Radio", 3);
    CHECK("needs a refresh with no cache", vseibi::needsRefresh());
    std::string err;
    int n = vseibi::refresh(err);
    CHECK("got A26's 3 entries", n == 3);
    eqs("no error for an unpublished season", err, "");
    CHECK("published to /stations", g_published.find("Summer Radio") != std::string::npos);
    eqs("the owner is told in words", vseibi::note(),
        "EiBi hasn't published the B26 (winter) schedule yet — using A26 until it appears.");
    CHECK("asked for B26 then A26", g_calls == 2 && g_lastUrl == "http://www.eibispace.de/dx/sked-a26.csv");
    CHECK("plain http (the site's certificate is expired)", g_lastUrl.rfind("http://", 0) == 0);
    CHECK("no temp file left behind", access((g_dir + "/eibi.csv.tmp").c_str(), F_OK) != 0);

    std::printf("── …and does not re-ask on every wake ──\n");
    g_now += 3600;
    CHECK("1 h later: no refresh", !vseibi::needsRefresh());
    g_now += 4 * 3600;
    CHECK("5 h later: no refresh", !vseibi::needsRefresh());
    g_now += 2 * 3600;
    CHECK("7 h later: ask again", vseibi::needsRefresh());

    std::printf("── still not there: keeps the fresh A26 cache without fetching it again ──\n");
    ageCache(7);
    g_calls = 0;
    n = vseibi::refresh(err);
    CHECK("still A26's entries", n == 3);
    CHECK("only B26 was asked for", g_calls == 1);
    CHECK("note still says so", !vseibi::note().empty());

    std::printf("── B26 appears: switches, note gone ──\n");
    g_now += 7 * 3600;
    CHECK("due again", vseibi::needsRefresh());
    g_site["sked-b26.csv"] = csvWith("Winter Radio", 5);
    n = vseibi::refresh(err);
    CHECK("got B26's 5 entries", n == 5);
    eqs("no error", err, "");
    eqs("no note once current", vseibi::note(), "");
    CHECK("winter list published", g_published.find("Winter Radio") != std::string::npos);
    CHECK("tried marker cleared", access((g_dir + "/eibi.tried").c_str(), F_OK) != 0);
    ageCache(1);
    CHECK("current and fresh: no refresh", !vseibi::needsRefresh());
    ageCache(21);
    CHECK("current but a day old: refresh", vseibi::needsRefresh());

    std::printf("── a REAL network failure is reported as one, and keeps the cache ──\n");
    g_down = true;
    n = vseibi::refresh(err);
    CHECK("returns 0 from refresh", n == 0);
    CHECK("says it could not reach the site", err.find("couldn't reach eibispace.de") == 0);
    CHECK("carries curl's own reason", err.find("Could not resolve host") != std::string::npos);
    CHECK("the cache is still there", vseibi::loadFromCache() == 5);

    std::printf("── unpublished AND offline for the fallback, with no cache ──\n");
    fresh();
    g_now = utc(2026, 10, 27, 21);
    // B26 404s; then the network drops before A26.
    int calls = 0;
    vseibi::setFetcher([&](const std::string& u, const std::string& d, std::string& m) {
        if (++calls == 1) return vseibi::Fetch::NotPublished;
        g_down = true; return fakeFetch(u, d, m);
    });
    n = vseibi::refresh(err);
    CHECK("nothing to serve", n == 0);
    CHECK("names the unpublished season", err.find("B26 (winter)") != std::string::npos);
    CHECK("and the network failure", err.find("couldn't reach eibispace.de") != std::string::npos);
    vseibi::setFetcher(fakeFetch);
    g_down = false;

    std::printf("── a 200 with no entries (an HTML page) counts as not published ──\n");
    fresh();
    g_site["sked-b26.csv"] = "<html><body>Not Found</body></html>";
    g_site["sked-a26.csv"] = csvWith("Summer Radio", 2);
    n = vseibi::refresh(err);
    CHECK("fell back to A26", n == 2);
    CHECK("note given", vseibi::note().find("using A26") != std::string::npos);

    std::printf("── an old cache from a server before this fix (season file names only) ──\n");
    fresh();
    {
        FILE* f = std::fopen((g_dir + "/eibi.csv").c_str(), "wb");
        const std::string c = csvWith("Old Summer", 4); std::fwrite(c.data(), 1, c.size(), f); std::fclose(f);
        f = std::fopen((g_dir + "/eibi.season").c_str(), "wb"); std::fputs("sked-a26.csv", f); std::fclose(f);
    }
    ageCache(2);
    CHECK("season turned over: refresh", vseibi::needsRefresh());
    eqs("no note before anyone asked", vseibi::note(), "");
    n = vseibi::refresh(err);
    CHECK("kept the fresh A26 cache (no download)", n == 4 && g_calls == 1);
    eqs("no error", err, "");
    CHECK("note now given", !vseibi::note().empty());

    std::printf("── January: B26 is the CURRENT season, A26 its fallback ──\n");
    fresh();
    g_now = utc(2027, 1, 10);
    g_site["sked-b26.csv"] = csvWith("Winter Radio", 6);
    n = vseibi::refresh(err);
    CHECK("January asks for B26 (the year it started)", n == 6 && g_lastUrl.find("sked-b26.csv") != std::string::npos);

    std::printf("── March rollover: A27 unpublished → B26 ──\n");
    fresh();
    g_now = utc(2027, 3, 26);
    g_site["sked-b26.csv"] = csvWith("Winter Radio", 6);
    n = vseibi::refresh(err);
    CHECK("fell back to B26", n == 6);
    eqs("March note", vseibi::note(),
        "EiBi hasn't published the A27 (summer) schedule yet — using B26 until it appears.");

    std::printf("── V12 R36S IMPORT EIBI: a file the owner put on the card (no internet) ──\n");
    eqs("season of sked-b26.csv",        vseibi::seasonOfName("sked-b26.csv"), "b26");
    eqs("season of a browser's rename",  vseibi::seasonOfName("/card/EiBi/SKED-A26 (1).csv"), "a26");
    eqs("no season in bookmarks.csv",    vseibi::seasonOfName("bookmarks.csv"), "");
    eqs("sked-a261.csv is not a season", vseibi::seasonOfName("sked-a261.csv"), "");
    {
        fresh();
        g_now = utc(2026, 11, 6, 12);
        const std::string card = g_dir + "/sked-b26.csv";
        auto put = [&](const std::string& path, const std::string& body) {
            FILE* f = std::fopen(path.c_str(), "wb"); std::fwrite(body.data(), 1, body.size(), f); std::fclose(f);
        };
        put(card, csvWith("Card Radio", 7));
        std::string season;
        n = vseibi::importFile(card, err, &season);
        CHECK("imported 7 entries", n == 7);
        eqs("no error", err, "");
        eqs("its season, in words", season, "B26 (winter)");
        CHECK("published to /stations", g_published.find("Card Radio") != std::string::npos);
        CHECK("the comment's separator is UTF-8 once (not \"Â·\")", g_published.find("0000-2400 \xC2\xB7 E") != std::string::npos
                                                                && g_published.find("\xC3\x82") == std::string::npos);
        CHECK("installed as THE cache (what the radio loads at start)", vseibi::loadFromCache() == 7);
        CHECK("loadedCount agrees, no re-parse needed", vseibi::loadedCount() == 7);
        FILE* sf = std::fopen((g_dir + "/eibi.season").c_str(), "rb");
        char sb[32] = {0}; if (sf) { std::fread(sb, 1, sizeof sb - 1, sf); std::fclose(sf); }
        eqs("season marker set from the name", sb, "sked-b26.csv");
        CHECK("no staging files left", access((g_dir + "/eibi.csv.import").c_str(), F_OK) != 0
                                     && access((g_dir + "/eibi.season.tmp").c_str(), F_OK) != 0);
        CHECK("the card's copy is left alone", access(card.c_str(), F_OK) == 0);
        CHECK("current season, fresh: no download attempt", !vseibi::needsRefresh());

        // ★ a wrong file must never replace a working schedule
        const std::string bad = g_dir + "/sked-a27.csv";
        put(bad, "<html><body>EiBi</body></html>");
        g_published.clear();
        n = vseibi::importFile(bad, err, &season);
        CHECK("an HTML page is refused", n == 0 && err.find("no stations") == 0);
        CHECK("nothing published", g_published.empty());
        CHECK("the B26 cache is untouched", vseibi::loadFromCache() == 7);
        n = vseibi::importFile(g_dir + "/missing.csv", err, &season);
        CHECK("a missing file is refused", n == 0 && !err.empty());
        {
            FILE* f = std::fopen(bad.c_str(), "wb");
            const std::string row(1024, 'x');
            for (int i = 0; i < 9 * 1024; i++) std::fwrite(row.data(), 1, row.size(), f);   // 9 MB
            std::fclose(f);
        }
        n = vseibi::importFile(bad, err, &season);
        CHECK("over 8 MB is refused", n == 0 && err.find("8 MB") != std::string::npos);
        CHECK("…and the cache still holds B26", vseibi::loadFromCache() == 7);
        unlink(bad.c_str());

        // ★ a schedule whose name carries no season: installed, marker cleared (a networked server would still ask daily)
        const std::string plain = g_dir + "/schedule.csv";
        put(plain, csvWith("Plain Radio", 2));
        n = vseibi::importFile(plain, err, &season);
        CHECK("a season-less name still imports", n == 2 && season.empty());
        CHECK("season marker cleared", access((g_dir + "/eibi.season").c_str(), F_OK) != 0);
        unlink(plain.c_str());

        // ★ the previous season while EiBi has not published the current one: the "not yet" marker stays meaningful
        g_now = utc(2026, 10, 27, 21);
        {
            FILE* f = std::fopen((g_dir + "/eibi.tried").c_str(), "wb");
            std::fprintf(f, "sked-b26.csv %lld", (long long)g_now); std::fclose(f);
        }
        const std::string a26 = g_dir + "/sked-a26.csv";
        put(a26, csvWith("Summer Radio", 3));
        n = vseibi::importFile(a26, err, &season);
        CHECK("A26 imported", n == 3 && season == "A26 (summer)");
        eqs("note: using A26 until B26 appears", vseibi::note(),
            "EiBi hasn't published the B26 (winter) schedule yet — using A26 until it appears.");
        // and then the owner brings B26 itself: the marker for B26 goes
        put(card, csvWith("Card Radio", 7));
        n = vseibi::importFile(card, err, &season);
        CHECK("B26 imported over A26", n == 7);
        CHECK("tried marker for B26 cleared", access((g_dir + "/eibi.tried").c_str(), F_OK) != 0);
        eqs("no note", vseibi::note(), "");
        unlink(a26.c_str());
        unlink(card.c_str());
    }

    fresh();
    rmdir(g_dir.c_str());
    std::printf(fails ? "\n%d FAILURES (%d passed)\n" : "\nall %d passed\n", fails ? fails : passes, passes);
    return fails ? 1 : 0;
}
