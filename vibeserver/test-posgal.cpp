// test-posgal.cpp — ★★★ beta96 maps2: the day's SONDES and VOR stations in the gallery (vibe_posgal.h), on a fake clock.
//
// ★★ WHAT THIS PINS (the clients' reader is src/utils/storedLayers.ts galPosParse — scripts/test_stored_layers.ts parses
//    THIS test's card text, written to $POSGAL_SAMPLE when set):
//    · ONE CARD PER KIND PER UTC DAY, slot "sondes-<date>" / "vor-<date>", gallery kind "sondes" / "vormap".
//    · NEVER A WRITE PER REPORT: nothing on disk until kCardSec; then one item; a second write REPLACES it (one item).
//    · THE TABLE: 10 fields, " · " between, newest first; a name holding " · " cannot split a row; "-" where there is none.
//    · NEWEST POSITION WINS, first heard kept, the highest altitude kept.
//    · A RESTART: the day's card is found again and its rows read back (a new row joins them, nothing doubles).
//    · MIDNIGHT: yesterday's card written for the last time, today's is a new item; a row last heard yesterday stays there.
//    · BOUNDED: kMaxRows rows, the least recently heard go and are counted ("Not listed:").
//    · No position, (0, 0), or out of range: not a row.
//   clang++ -std=c++17 -O2 -I android/app/src/main/cpp -o /tmp/test-posgal vibeserver/test-posgal.cpp -lpthread
#include "vibe_posgal.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); (c ? passes : fails)++; }
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/posgal-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static std::vector<std::string> files(const std::string& dir) {
    std::vector<std::string> v;
    if (DIR* d = opendir(dir.c_str())) { while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n[0] != '.' && n != "index.jsonl") v.push_back(n); } closedir(d); }
    return v;
}
static void rmrf(const std::string& dir) { for (auto& f : files(dir)) std::remove((dir + "/" + f).c_str()); std::remove((dir + "/index.jsonl").c_str()); rmdir(dir.c_str()); }
static std::string itemText(gallery::Store& st, const std::string& name) { std::string b; st.readItem(name, b); return b; }
static int countOf(const std::string& s, const std::string& w) { int n = 0; for (size_t p = 0; (p = s.find(w, p)) != std::string::npos; p += w.size()) ++n; return n; }
static std::vector<std::string> tableRows(const std::string& body) {
    std::vector<std::string> out;
    const size_t at = body.find("Each, most recent first");
    if (at == std::string::npos) return out;
    std::istringstream in(body.substr(body.find('\n', at) + 1));
    for (std::string l; std::getline(in, l);) if (!l.empty()) out.push_back(l);
    return out;
}
static posgal::Row sonde(const std::string& serial, double lat, double lon, double alt, double top, int64_t last, int64_t first = 0) {
    posgal::Row r; r.id = serial; r.type = "RS41-SGP"; r.lat = lat; r.lon = lon; r.altM = alt; r.topM = top; r.mhz = 403.0; r.last = last; r.first = first; return r;
}

int main() {
    const int64_t day = 20418;                 // 2025-11-26 … any UTC day
    int64_t clock = day * 86400 + 10 * 3600;   // 10:00:00 UTC
    auto clk = [&]() { return clock; };

    std::printf("ONE CARD, NEVER A WRITE PER REPORT\n");
    const std::string dir = tmpDir("a");
    std::string sample;
    {
        gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
        posgal::DayCard c(st, { posgal::sondesSpec(), clk });
        c.seen(sonde("S1234567", 51.5, -1.25, 1200, 1200, clock - 30, clock - 600));
        c.tick();
        ok(files(dir).empty(), "nothing on disk before 5 minutes");
        clock += 120; c.seen(sonde("S1234567", 51.6, -1.20, 9000, 9000, clock)); c.tick();
        ok(files(dir).empty(), "…still nothing at 2 minutes");
        clock += 200; c.seen(sonde("T7654321", 52.1, 0.15, 300, 300, clock)); c.tick();
        ok(files(dir).size() == 1, "one item after 5 minutes");
        const std::string name = c.itemName();
        std::string b = itemText(st, name);
        auto rows = tableRows(b);
        ok(rows.size() == 2, "two sondes in the table");
        ok(rows.size() == 2 && rows[0].find(" \xc2\xb7 T7654321 \xc2\xb7 ") != std::string::npos, "newest first");
        ok(rows.size() == 2 && rows[1].find("51.6000, -1.2000") != std::string::npos, "the newest position wins");
        ok(rows.size() == 2 && rows[1].compare(12, 8, "09:50:00") == 0, "first heard kept (09:50:00)");
        ok(rows.size() == 2 && rows[1].find(" \xc2\xb7 9000 \xc2\xb7 9000 \xc2\xb7 403.000 \xc2\xb7 -") != std::string::npos, "altitude · highest · MHz · no country = \"-\"");
        ok(countOf(rows.size() ? rows[0] : "", " \xc2\xb7 ") == 9, "10 fields");
        ok(st.indexText().find("\"s\":\"sondes-") != std::string::npos && st.indexText().find("\"k\":\"sondes\"") != std::string::npos, "the slot and the kind in the index");
        ok(c.titleNow().find("Radiosondes heard") == 0 && c.titleNow().find("2 sondes") != std::string::npos && c.titleNow().find("highest 9000 m") != std::string::npos, "the title: " + c.titleNow());
        // the highest is kept when the sonde falls
        clock += 400; c.seen(sonde("S1234567", 51.7, -1.1, 2000, 2000, clock)); c.tick();
        b = itemText(st, c.itemName()); rows = tableRows(b);
        ok(files(dir).size() == 1, "a second write REPLACES the item");
        ok(rows.size() == 2 && rows[0].find(" \xc2\xb7 2000 \xc2\xb7 9000 \xc2\xb7 ") != std::string::npos, "falling: altitude 2000, highest still 9000");
        // garbage is not a row
        c.seen(sonde("NOPOS", NAN, NAN, 0, 0, clock)); c.seen(sonde("ZERO", 0, 0, 0, 0, clock)); c.seen(sonde("FAR", 95, 10, 0, 0, clock)); c.seen(sonde("", 50, 1, 0, 0, clock));
        ok(c.rows() == 2, "no position, (0, 0), out of range or no id: not a row");
        sample = b;
        c.finish();
    }
    std::printf("A RESTART\n");
    {
        gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
        clock += 60;
        posgal::DayCard c(st, { posgal::sondesSpec(), clk });
        ok(c.rows() == 2, "the day's card found again, its rows read back");
        c.seen(sonde("S1234567", 51.8, -1.0, 500, 500, clock));
        c.seen(sonde("U1111111", 50.1, 1.1, 100, 100, clock));
        c.finish();
        ok(files(dir).size() == 1, "still one item (adopted, not a second)");
        const auto rows = tableRows(itemText(st, c.itemName()));
        ok(rows.size() == 3, "three sondes — nothing doubled");
        bool top = false;
        for (auto& r : rows) if (r.find("S1234567") != std::string::npos && r.find(" \xc2\xb7 500 \xc2\xb7 9000 \xc2\xb7 ") != std::string::npos) top = true;
        ok(top, "the restored highest kept (altitude 500, highest 9000)");
    }
    std::printf("MIDNIGHT\n");
    {
        const std::string d2 = tmpDir("b");
        gallery::Store st; st.setDir(d2); st.setLimitBytes(50ull << 20);
        clock = day * 86400 + 23 * 3600 + 59 * 60;
        posgal::DayCard c(st, { posgal::vorSpec(), clk });
        posgal::Row v; v.id = "DTY/116400"; v.name = "DAVENTRY \xc2\xb7 VOR"; v.type = "vor"; v.cc = "GB"; v.lat = 52.18; v.lon = -1.114; v.mhz = 116.4; v.last = clock; v.first = clock - 3600;
        c.seen(v);
        clock += 120;   // 00:01 the next day
        c.tick();
        ok(files(d2).size() == 1, "yesterday's card written at midnight");
        posgal::Row w = v; w.id = "BNN/113750"; w.name = "BOVINGDON"; w.last = clock; w.first = clock;
        c.seen(w);
        c.finish();
        ok(files(d2).size() == 2, "today's is a new item");
        const std::string idx = st.indexText();
        ok(countOf(idx, "\"k\":\"vormap\"") == 2 && idx.find("\"s\":\"vor-") != std::string::npos, "kind vormap, slot vor-<date>");
        bool split = false;
        for (auto& f : files(d2)) { std::string b; std::ifstream in(d2 + "/" + f); std::stringstream ss; ss << in.rdbuf(); b = ss.str();
            for (auto& r : tableRows(b)) if (countOf(r, " \xc2\xb7 ") != 9) split = true;
            if (b.find("DTY/116400") != std::string::npos) ok(b.find("DAVENTRY - VOR") != std::string::npos, "a \" · \" inside a name cannot split the row"); }
        ok(!split, "every row 10 fields");
        rmrf(d2);
    }
    std::printf("BOUNDED\n");
    {
        const std::string d3 = tmpDir("c");
        gallery::Store st; st.setDir(d3); st.setLimitBytes(50ull << 20);
        clock = day * 86400 + 12 * 3600;
        posgal::DayCard c(st, { posgal::sondesSpec(), clk });
        for (int i = 0; i < 350; ++i) { char id[16]; std::snprintf(id, sizeof id, "X%05d", i); c.seen(sonde(id, 50 + i * 0.01, 1, 100, 100, clock - 350 + i)); }
        ok(c.rows() == posgal::DayCard::kMaxRows, "at most 300 rows");
        c.finish();
        const std::string b = itemText(st, c.itemName());
        ok(b.find("Not listed:   50 ") != std::string::npos, "the 50 least recently heard counted, not listed");
        ok(b.find("X00000") == std::string::npos && b.find("X00349") != std::string::npos, "the oldest went, the newest stayed");
        rmrf(d3);
    }
    if (const char* out = std::getenv("POSGAL_SAMPLE")) { std::ofstream f(out); f << sample; }
    rmrf(dir);
    std::printf("\ntest-posgal: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
