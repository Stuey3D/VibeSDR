// test-fctlm-gallery — V12 beta91 TELEMETRY CHARTS: FUNcube telemetry kept in the gallery (vibe_fctlm_gallery.h).
//
// ★★ WHAT THIS PINS (frames from vibe_fctlm_tx.h's sample frames through the REAL parser, on a fake clock — FIXTURE data,
//    nothing here was heard on air):
//    §1 THE PASS CSV: one .csv item a pass — the comment head, ONE header row whose column count every row matches, every
//       channel the frame carries, "[raw]" on the uncalibrated ones and nothing on counts, the values exactly the parser's.
//       Its title ("AO-73 telemetry · pass 14:02 UTC · N frames · max el 54°") and download name ride in the index.
//    §2 NEVER A WRITE PER FRAME: nothing on disk inside the first minute; then ONE append a minute; finish() writes the rest.
//    §3 PASSES: a new AOS = a new file; no AOS known = a gap over 20 minutes; another satellite = another file.
//    §4 THE DAY'S LOG: one .txt per satellite per UTC day, a line a frame, across passes; a restart adopts it and counts on;
//       midnight starts the next day's. §4b: a background profile's pass and a listener's pick (another origin, profile and
//       dial) write ONE day log — both alive at once, no half lines, the count right, a restart finds it.
//       §4c: the SAME pass decoded by both at once (one knowing the AOS, one not; each missing frames) = ONE .csv and one
//       day log, every frame once (the union, no doubles).
//    §5 BOUNDED: a pass past 256 KB goes on in "part 2"; no file passes the cap by more than one flush.
//    §6 SERVED: a .csv resolves as text/csv.
#include "vibe_fctlm_tx.h"
#include "vibe_fctlm_gallery.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <set>
#include <string>
#include <vector>

using namespace vibe;
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { if (cond) { ++g_pass; std::printf("   ok   "); std::printf(__VA_ARGS__); std::printf("\n"); } \
                              else { ++g_fail; std::printf("   FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/fcgal-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static void rmDir(const std::string& dir) {
    if (DIR* d = opendir(dir.c_str())) { while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n != "." && n != "..") ::unlink((dir + "/" + n).c_str()); } closedir(d); }
    ::rmdir(dir.c_str());
}
static std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> v; std::stringstream ss(s); std::string l; while (std::getline(ss, l)) v.push_back(l); return v;
}
static int cols(const std::string& l) { int n = 1; for (char c : l) n += c == ','; return n; }
static int count(const std::string& s, const std::string& w) { int n = 0; for (size_t p = 0; (p = s.find(w, p)) != std::string::npos; p += w.size()) ++n; return n; }

static gallery::Meta meta() {
    gallery::Meta m; m.kind = "fctlm"; m.origin = "background"; m.freqHz = 145.935e6; m.mode = "usb"; m.decoder = "fctlm";
    m.profile = "Profile 1"; return m;
}
static vibefc::Decoded frame(int type, uint32_t seq) {
    std::vector<uint8_t> f = vibefc::tx::sampleFrame(type, seq);
    return vibefc::parse(f.data());
}
// 2026-11-22 14:02:00 UTC
static const int64_t T0 = 1795356120;

struct Clock { int64_t t = T0; };

static std::vector<std::string> itemsOf(gallery::Store& st, const std::string& ext) {
    std::vector<std::string> o;
    for (auto& n : st.names("fctlm")) if (n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0) o.push_back(n);
    return o;
}

static void s1_s2() {
    std::printf("§1 the pass CSV · §2 never a write per frame\n");
    const std::string dir = tmpDir("pass");
    gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
    Clock ck;
    {
        bg::FcTlmGalleryTap tap(st, meta(), [&] { return ck.t; });
        vibefc::TlmCtx c; c.sat = "AO-73"; c.aos = T0 - 60; c.los = T0 + 660;
        for (int i = 0; i < 6; ++i) { ck.t = T0 + 5 * i; c.unix = (double)ck.t; c.elDeg = 10 + i; tap.satTlm(frame(1 + i % 12, 1000 + i), c); }
        CHECK(st.itemCount() == 0, "six frames in 25 s: nothing written yet (held in RAM)");
        ck.t = T0 + 61; c.unix = (double)ck.t; c.elDeg = 54.4; tap.satTlm(frame(7, 1006), c);
        CHECK(itemsOf(st, ".csv").size() == 1 && itemsOf(st, ".txt").size() == 1, "a minute on: one pass CSV and one day log, written once");
        for (int i = 7; i < 30; ++i) { ck.t = T0 + 5 * i + 30; c.unix = (double)ck.t; c.elDeg = 54.4 - i; tap.satTlm(frame(1 + i % 12, 1000 + i), c); }
        tap.finish(false);
    }
    auto csv = itemsOf(st, ".csv");
    CHECK(csv.size() == 1, "one pass, one CSV (got %zu)", csv.size());
    std::string body; st.readItem(csv.empty() ? "" : csv[0], body);
    auto L = lines(body);
    CHECK(L.size() == 3 + 1 + 30, "3 comment lines + a header + 30 rows (got %zu lines)", L.size());
    CHECK(L.size() > 3 && L[0].rfind("# FUNcube telemetry \xc2\xb7 AO-73 \xc2\xb7 145.9350 MHz USB", 0) == 0, "the head names the satellite and the dial: %s", L.empty() ? "" : L[0].c_str());
    CHECK(L.size() > 3 && L[1].find("AOS 14:01") != std::string::npos && L[1].find("LOS 14:13 UTC") != std::string::npos, "the pass's AOS and LOS: %s", L.size() > 1 ? L[1].c_str() : "");
    CHECK(L.size() > 3 && L[2].find("[raw] = sent uncalibrated") != std::string::npos, "the head says what [raw] means");
    const std::string hdr = L.size() > 3 ? L[3] : "";
    const vibefc::Decoded d0 = frame(1, 1000);
    CHECK(cols(hdr) == 7 + (int)d0.values.size(), "the header: 7 fixed columns + %zu channels (got %d)", d0.values.size(), cols(hdr));
    CHECK(hdr.rfind("utc,unix,frame,seq,mode,elevation [deg],rs fixed,", 0) == 0, "the fixed columns first");
    CHECK(hdr.find(",Battery voltage [raw],") != std::string::npos, "Battery voltage is labelled [raw] (no published calibration)");
    CHECK(hdr.find(",Reboot count,") != std::string::npos && hdr.find("Reboot count [") == std::string::npos, "a count has no bracket");
    bool same = true; for (size_t i = 4; i < L.size(); ++i) same &= cols(L[i]) == cols(hdr);
    CHECK(same, "every row has the header's column count");
    // the values: row 1 = frame(1, 1000) exactly
    if (L.size() > 4) {
        std::vector<std::string> cells; std::stringstream ss(L[4]); std::string x; while (std::getline(ss, x, ',')) cells.push_back(x);
        bool eq = cells.size() == 7 + d0.values.size();
        for (size_t i = 0; eq && i < d0.values.size(); ++i) eq &= cells[7 + i] == vibefc::num(d0.values[i].v);
        CHECK(eq, "row 1's channels are the parser's values, in order");
        CHECK(cells.size() > 5 && cells[0] == "2026-11-22T14:02:00Z" && cells[1] == std::to_string(T0) && cells[2] == "WO1" && cells[3] == "1000" && cells[5] == "10.0",
              "row 1: UTC, unix, frame, seq, elevation (%s,%s,%s,%s,%s)", cells.size() > 5 ? cells[0].c_str() : "", cells.size() > 5 ? cells[1].c_str() : "",
              cells.size() > 5 ? cells[2].c_str() : "", cells.size() > 5 ? cells[3].c_str() : "", cells.size() > 5 ? cells[5].c_str() : "");
    }
    const std::string title = csv.empty() ? "" : st.field(csv[0], "title");
    CHECK(title == "AO-73 telemetry \xc2\xb7 pass 14:02 UTC \xc2\xb7 30 frames \xc2\xb7 max el 54\xc2\xb0", "the title counts the pass: %s", title.c_str());
    const std::string dn = csv.empty() ? "" : st.field(csv[0], "dn");
    CHECK(dn == "AO-73 telemetry 2026-11-22 1402 UTC.csv", "the download name: %s", dn.c_str());
    std::string path, ctype;
    CHECK(!csv.empty() && st.resolve(csv[0], path, ctype) && ctype.rfind("text/csv", 0) == 0, "§6 a .csv is served as text/csv (%s)", ctype.c_str());
    rmDir(dir);
}

static void s3_s4() {
    std::printf("§3 passes · §4 the day's log\n");
    const std::string dir = tmpDir("day");
    gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
    Clock ck;
    uint32_t seq = 2000;   // ★ the satellite's own counter: one a frame, never repeated (a frame's identity)
    auto run = [&](bg::FcTlmGalleryTap& tap, const char* sat, double aos, int64_t start, int n) {
        vibefc::TlmCtx c; c.sat = sat; c.aos = aos; c.los = aos > 0 ? aos + 700 : 0;
        for (int i = 0; i < n; ++i) { ck.t = start + 5 * i; c.unix = (double)ck.t; tap.satTlm(frame(1 + i % 24, seq++), c); }
    };
    {
        bg::FcTlmGalleryTap tap(st, meta(), [&] { return ck.t; });
        run(tap, "AO-73", T0, T0, 20);                     // pass 1
        run(tap, "AO-73", T0 + 6000, T0 + 6000, 15);       // pass 2: a new AOS
        tap.finish(false);
    }
    CHECK(itemsOf(st, ".csv").size() == 2, "a new AOS: a second pass file (got %zu)", itemsOf(st, ".csv").size());
    {
        bg::FcTlmGalleryTap tap(st, meta(), [&] { return ck.t; });   // a hand-tuned listener: no AOS
        run(tap, "AO-73", 0, T0 + 12000, 10);
        run(tap, "AO-73", 0, T0 + 12000 + 50 + 5 * 60, 10);           // 5 minutes on: the same pass
        run(tap, "AO-73", 0, T0 + 12000 + 50 + 5 * 60 + 50 + 25 * 60, 10);   // 25 minutes on: another
        run(tap, "SONATE-2", 0, T0 + 20000, 4);                       // another satellite
        tap.finish(false);
    }
    CHECK(itemsOf(st, ".csv").size() == 5, "no AOS: a 5-minute gap is the same pass, a 25-minute gap and another satellite are not (got %zu)", itemsOf(st, ".csv").size());
    auto txt = itemsOf(st, ".txt");
    CHECK(txt.size() == 2, "the day's log: one per satellite per day, across taps and passes (got %zu)", txt.size());
    std::string ao;
    for (auto& n : txt) if (st.field(n, "title").rfind("AO-73", 0) == 0) ao = n;
    std::string body; st.readItem(ao, body);
    CHECK(count(body, "  AO-73  ") == 65, "AO-73's log holds every frame of every pass: 65 lines (got %d)", count(body, "  AO-73  "));
    CHECK(body.find("14:02:00  AO-73  WO1 \xc2\xb7 seq 2000 \xc2\xb7 ") != std::string::npos && body.find(" raw \xc2\xb7 RS fixed 0") != std::string::npos,
          "a log line: UTC time, satellite, frame, seq, mode, battery raw, RS");
    CHECK(st.field(ao, "title") == "AO-73 telemetry log \xc2\xb7 22 Nov \xc2\xb7 65 frames", "its title counts the day: %s", st.field(ao, "title").c_str());
    // a restart: the same day's log, counted on
    {
        bg::FcTlmGalleryTap tap(st, meta(), [&] { return ck.t; });
        run(tap, "AO-73", T0 + 30000, T0 + 30000, 5);
        tap.finish(false);
        CHECK(tap.dayName() == ao || itemsOf(st, ".txt").size() == 2, "a restart appends to the day's log (no second file)");
    }
    CHECK(itemsOf(st, ".txt").size() == 2 && st.field(ao, "title") == "AO-73 telemetry log \xc2\xb7 22 Nov \xc2\xb7 70 frames", "…and counts on: %s", st.field(ao, "title").c_str());
    // midnight
    {
        bg::FcTlmGalleryTap tap(st, meta(), [&] { return ck.t; });
        const int64_t mid = (T0 / 86400 + 1) * 86400;
        run(tap, "AO-73", mid - 30, mid - 30, 12);   // 23:59:30 → 00:00:25
        tap.finish(false);
    }
    CHECK(itemsOf(st, ".txt").size() == 3, "a pass over UTC midnight: the next day's log begins (got %zu)", itemsOf(st, ".txt").size());
    CHECK(itemsOf(st, ".csv").size() == 7, "…but the pass stays ONE file (got %zu)", itemsOf(st, ".csv").size());
    rmDir(dir);
}

static void s4b() {
    std::printf("§4b ONE day log for every writer (Stuart 11-22): a background profile's pass + a listener's pick\n");
    const std::string dir = tmpDir("two");
    gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
    Clock ck;
    gallery::Meta live = meta(); live.origin = "live"; live.profile = ""; live.freqHz = 145.9338e6;   // another dial, no profile
    int bgN = 0, lvN = 0;
    {
        // both alive at once, both holding frames before either has written (they must not make two files)
        bg::FcTlmGalleryTap bgTap(st, meta(), [&] { return ck.t; });
        bg::FcTlmGalleryTap lvTap(st, live, [&] { return ck.t; });
        vibefc::TlmCtx c; c.sat = "AO-73"; c.aos = (double)T0; c.los = (double)T0 + 700;
        for (int i = 0; i < 60; ++i) {   // 5 minutes, alternate frames, flushes interleaving every minute
            ck.t = T0 + 5 * i; c.unix = (double)ck.t;
            if (i % 2) { bgTap.satTlm(frame(1 + i % 24, 5000 + i), c); ++bgN; } else { lvTap.satTlm(frame(1 + i % 24, 6000 + i), c); ++lvN; }
        }
        bgTap.finish(false);
        CHECK(bgTap.dayName() == lvTap.dayName() || lvTap.dayName().empty() || bgTap.dayName().empty(), "both taps hold the same day item");
        lvTap.finish(false);
    }
    auto txt = itemsOf(st, ".txt");
    CHECK(txt.size() == 1, "ONE day log for both writers, whatever their profile, origin or dial (got %zu)", txt.size());
    std::string body; if (!txt.empty()) st.readItem(txt[0], body);
    auto L = lines(body);
    int frames = 0, bad = 0, fromBg = 0, fromLv = 0;
    for (size_t i = 3; i < L.size(); ++i) {
        const std::string& l = L[i];
        const bool ok = l.size() > 20 && l[2] == ':' && l[5] == ':' && l.compare(8, 9, "  AO-73  ") == 0 && l.find("RS fixed 0") != std::string::npos;
        if (ok) { ++frames; if (l.find("seq 5") != std::string::npos) ++fromBg; if (l.find("seq 6") != std::string::npos) ++fromLv; } else ++bad;
    }
    CHECK(frames == 60 && fromBg == bgN && fromLv == lvN, "every frame of both, once (%d = %d background + %d listener)", frames, fromBg, fromLv);
    CHECK(bad == 0, "no half or merged lines (%d malformed)", bad);
    const std::string title = txt.empty() ? "" : st.field(txt[0], "title");
    CHECK(title == "AO-73 telemetry log \xc2\xb7 22 Nov \xc2\xb7 60 frames", "the title counts both writers' frames: %s", title.c_str());
    CHECK(itemsOf(st, ".csv").size() == 1, "…and the pass they share is ONE CSV (got %zu)", itemsOf(st, ".csv").size());
    { auto c = itemsOf(st, ".csv"); std::string b; if (!c.empty()) st.readItem(c[0], b);
      int rows = 0; for (auto& l : lines(b)) rows += l.rfind("2026-", 0) == 0;
      CHECK(rows == 60 && (c.empty() ? "" : st.field(c[0], "title")).find("60 frames") != std::string::npos, "with both writers' 60 frames (%d rows)", rows); }
    // a restart (a listener, later) finds the same day's log and counts on
    {
        bg::FcTlmGalleryTap tap(st, live, [&] { return ck.t; });
        vibefc::TlmCtx c; c.sat = "AO-73"; c.aos = (double)T0 + 6000;
        for (int i = 0; i < 4; ++i) { ck.t = T0 + 6000 + 5 * i; c.unix = (double)ck.t; tap.satTlm(frame(1, 7000 + i), c); }
        tap.finish(false);
    }
    txt = itemsOf(st, ".txt");
    CHECK(txt.size() == 1 && st.field(txt[0], "title") == "AO-73 telemetry log \xc2\xb7 22 Nov \xc2\xb7 64 frames",
          "a restart appends to the same day and counts on (%s)", txt.empty() ? "" : st.field(txt[0], "title").c_str());
    rmDir(dir);
}

static void s4c() {
    std::printf("§4c THE SAME PASS FROM TWO SOURCES (Stuart: \"only one chart needed\"): one file, each frame once\n");
    const std::string dir = tmpDir("same");
    gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
    Clock ck;
    gallery::Meta live = meta(); live.origin = "live"; live.profile = ""; live.freqHz = 145.9338e6;
    int heard = 0;   // frames at least one of them decoded (i = 31 neither did)
    for (int i = 0; i < 40; ++i) heard += (i % 7 != 3) || (i % 5 != 1);
    {
        bg::FcTlmGalleryTap bgTap(st, meta(), [&] { return ck.t; });
        bg::FcTlmGalleryTap lvTap(st, live, [&] { return ck.t; });
        vibefc::TlmCtx cb; cb.sat = "AO-73"; cb.aos = (double)T0; cb.los = (double)T0 + 700;
        vibefc::TlmCtx cl = cb; cl.aos = 0; cl.los = 0;   // ★ the listener's lock does not know the AOS: joins by time
        for (int i = 0; i < 40; ++i) {
            ck.t = T0 + 5 * i;
            const vibefc::Decoded d = frame(1 + i % 24, 8000 + i);
            cb.unix = (double)ck.t; cl.unix = (double)ck.t + (i % 3 == 0 ? 1 : 0);   // a second apart sometimes
            if (i % 7 != 3) bgTap.satTlm(d, cb);     // ★ each misses a few the other heard
            if (i % 5 != 1) lvTap.satTlm(d, cl);
        }
        lvTap.finish(false);
        bgTap.finish(false);
    }
    auto csv = itemsOf(st, ".csv");
    CHECK(csv.size() == 1, "ONE pass file for the two decoders (got %zu)", csv.size());
    std::string b; if (!csv.empty()) st.readItem(csv[0], b);
    std::set<std::string> seqs; int rows = 0;
    for (auto& l : lines(b)) if (l.rfind("2026-", 0) == 0) { ++rows; std::vector<std::string> c; std::stringstream ss(l); std::string x; while (std::getline(ss, x, ',')) c.push_back(x); if (c.size() > 3) seqs.insert(c[3]); }
    CHECK(rows == heard && (int)seqs.size() == heard, "each frame once — the union of both (%d), no doubles (%d rows, %zu frames)", heard, rows, seqs.size());
    CHECK(!csv.empty() && st.field(csv[0], "title").find(std::to_string(heard) + " frames") != std::string::npos, "the title counts them: %s", csv.empty() ? "" : st.field(csv[0], "title").c_str());
    auto txt = itemsOf(st, ".txt");
    std::string body; if (!txt.empty()) st.readItem(txt[0], body);
    CHECK(txt.size() == 1 && count(body, "  AO-73  ") == heard, "the day log: one, each frame once (%d lines)", count(body, "  AO-73  "));
    rmDir(dir);
}

static void s5() {
    std::printf("§5 bounded\n");
    const std::string dir = tmpDir("cap");
    gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
    Clock ck;
    {
        bg::FcTlmGalleryTap tap(st, meta(), [&] { return ck.t; });
        vibefc::TlmCtx c; c.sat = "AO-73"; c.aos = T0; c.los = T0 + 99999;
        for (int i = 0; i < 1500; ++i) { ck.t = T0 + 5 * i; c.unix = (double)ck.t; tap.satTlm(frame(1 + i % 24, 3000 + i), c); }
        tap.finish(false);
    }
    auto csv = itemsOf(st, ".csv");
    bool capped = true, part2 = false;
    for (auto& n : csv) {
        capped &= st.sizeOf(n) <= bg::fctlmlog::kPassMaxBytes + bg::fctlmlog::kFlushBytes + 4096;
        part2 |= st.field(n, "title").find("part 2") != std::string::npos;
    }
    CHECK(csv.size() >= 2 && part2, "a 2-hour 'pass' goes on in part 2 (%zu files)", csv.size());
    CHECK(capped, "no pass file passes 256 KB by more than one flush");
    rmDir(dir);
}

/** ★ `--write-fixture <dir>`: three passes of SYNTHETIC readings (the sample frame with its analogue values swung along a
 *  pass — a battery sagging, panels warming) written exactly as the server writes them, for the clients' chart tests
 *  (scripts/fixtures/fctlm/). Not on-air data. */
static int writeFixture(const std::string& out) {
    const std::string dir = tmpDir("fix");
    gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
    Clock ck;
    {
        bg::FcTlmGalleryTap tap(st, meta(), [&] { return ck.t; });
        for (int p = 0; p < 3; ++p) {
            const int64_t aos = T0 + p * 6000;
            vibefc::TlmCtx c; c.sat = "AO-73"; c.aos = (double)aos; c.los = (double)aos + 660;
            for (int i = 0; i < 24; ++i) {
                if (p == 1 && i >= 10 && i < 16) continue;   // a fade: six frames lost mid-pass
                ck.t = aos + 30 + 5 * i; c.unix = (double)ck.t; c.elDeg = 5 + 40 * std::sin(3.14159 * i / 23.0);
                vibefc::Decoded d = frame(1 + i % 24, 4000 + p * 100 + i);
                for (size_t k = 0; k < d.values.size(); ++k)
                    if (d.values[k].raw) d.values[k].v = std::round(d.values[k].v + (k % 5 + 1) * 3 * std::sin(0.25 * i + k) - p * (k % 3));
                tap.satTlm(d, c);
            }
        }
        tap.finish(false);
    }
    ::mkdir(out.c_str(), 0755);
    int n = 0;
    for (auto& f : st.names("fctlm")) {
        std::string body; st.readItem(f, body);
        const bool csv = f.size() > 4 && f.compare(f.size() - 4, 4, ".csv") == 0;
        const std::string name = csv ? "pass-" + std::to_string(++n) + ".csv" : "day.txt";
        std::ofstream o(out + "/" + name, std::ios::binary);
        if (!(o << body)) { std::printf("could not write %s/%s (does the folder exist?)\n", out.c_str(), name.c_str()); rmDir(dir); return 1; }
        std::printf("wrote %s/%s · %s\n", out.c_str(), name.c_str(), st.field(f, "title").c_str());
    }
    rmDir(dir);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--write-fixture") return writeFixture(argv[2]);
    std::printf("test-fctlm-gallery — FUNcube telemetry in the gallery (fixture frames, fake clock)\n");
    s1_s2();
    s3_s4();
    s4b();
    s4c();
    s5();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
