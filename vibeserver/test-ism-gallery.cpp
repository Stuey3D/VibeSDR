// test-ism-gallery.cpp — ISM IN THE GALLERY, below the server (V12, 2026-10-25): vibe_ism_gallery.h's daily log and
// summary card, fed through the REAL vibeism::Tracker (so what is logged is what the list counts), on a fake clock.
//
// ★★ WHAT THIS PINS (the live half — a listener's session through the real server writing the day's .txt, the gallery
//    listing ISM — is scripts/test-server-ism-gallery.mjs):
//    · THE LINE: "17:01:12  433.920  Oregon Scientific … · ch 1" — the list's readable line exactly, UTC time + MHz.
//    · NEVER A WRITE PER READING: nothing on disk until a minute has passed or 8 KB is held; then ONE append; finish()
//      writes what is held. A day's log splits into parts past kPartMaxBytes.
//    · THE FOLD: the Tracker's 3 s repeat fold reaches the file (a sensor's 3 identical frames = one line); a new value
//      inside 3 s, or the same value after 3 s, is a new line.
//    · ONE FILE PER UTC DAY PER BAND: midnight closes yesterday's log and card and starts today's; another band is
//      another file; background + a listener on one day = ONE file, marked "both".
//    · THE CARD: devices by kind, most heard, newest, the last reading of each device; rewritten at most once a minute
//      and only when something new was heard; its title fits the index's 120 bytes.
//    · A RESTART: the day's log is appended to (not a second file), the card's table read back, and new secret devices
//      numbered above the ones the day already named.
//    · ★★★ PRIVACY: TPMS / key fob / remote IDs and codes, in decimal and hex, appear in NO file and NO index line.
//    · The download name ("ISM 433.92 2026-10-25.txt") rides in the index as "dn", and is safe for a header.
//   clang++ -std=c++17 -O2 -I android/app/src/main/cpp -o /tmp/test-ism-gallery vibeserver/test-ism-gallery.cpp -lpthread
#include "vibe_ism_service.h"
#include "vibe_ism_gallery.h"
#include "vibe_bgdecode_config.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); (c ? passes : fails)++; }

static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/ismgal-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static std::vector<std::string> files(const std::string& dir) {
    std::vector<std::string> v;
    if (DIR* d = opendir(dir.c_str())) { while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n[0] != '.') v.push_back(n); } closedir(d); }
    return v;
}
static std::string everything(const std::string& dir) { std::string all; for (auto& f : files(dir)) all += slurp(dir + "/" + f) + "\n"; return all; }
static int countOf(const std::string& s, const std::string& w) { int n = 0; for (size_t p = 0; (p = s.find(w, p)) != std::string::npos; p += w.size()) ++n; return n; }

// ── readings, as the protocols hand them to the Tracker ─────────────────────────────────────────────────────────────
static vibeism::Reading oregon(double t, int ch = 1, int batt = 0) {
    vibeism::Reading r; r.kind = "weather"; r.make = "Oregon Scientific"; r.model = "THN132N"; r.pubId = "6B"; r.channel = ch;
    r.tempC = t; r.battOk = batt; r.freqHz = 433.92e6; return r;
}
static vibeism::Reading tpms(uint64_t id, double kpa) {
    vibeism::Reading r; r.kind = "tpms"; r.secret = true; r.make = "TPMS"; r.model = "type B"; r.secretId = id; r.kpa = kpa; r.tempC = 18;
    r.freqHz = 433.95e6; return r;
}
static vibeism::Reading fob(uint64_t code) {
    vibeism::Reading r; r.kind = "keyfob"; r.secret = true; r.make = "KeeLoq"; r.secretId = code; r.freqHz = 433.92e6; return r;
}
static vibeism::Reading remote(uint64_t code) {
    vibeism::Reading r; r.kind = "doorbell"; r.secret = true; r.make = "EV1527"; r.secretId = code; r.button = "A"; r.freqHz = 434.0e6; return r;
}
static vibeism::Reading wh1080() {
    vibeism::Reading r; r.kind = "weather"; r.make = "Fine Offset"; r.model = "WH1080"; r.pubId = "4F"; r.tempC = 12.5; r.hum = 90;
    r.windKmh = 3.7; r.windDir = 180; r.rainMm = 23.4; r.battOk = 1; r.freqHz = 433.85e6; return r;
}

/** The Tracker in front of the log, as vibeism::Service does it: only what accept() counts reaches the sink. */
struct Rig {
    vibeism::Tracker tr;
    std::shared_ptr<ismgal::DayLog> log;
    int64_t* clock;
    bool push(const vibeism::Reading& r) {
        vibeism::Heard h;
        const bool counted = tr.accept(r, double(*clock), &h);
        if (counted) log->heard(h);
        return counted;
    }
};
static ismgal::DayLog::Options opts(int64_t* clock, const char* origin = "live", const char* band = "433") {
    ismgal::DayLog::Options o;
    const vibeism::Band* b = vibeism::bandById(band);
    o.bandId = b->id; o.bandName = b->name; o.bandHz = b->centreHz; o.origin = origin;
    o.profile = std::string(origin) == "background" ? "Profile 1" : "";
    o.clock = [clock] { return *clock; };
    return o;
}
static std::string index(const std::string& dir) { return slurp(dir + "/index.jsonl"); }
static std::string itemText(gallery::Store& st, const std::string& name) { std::string b; st.readItem(name, b); return b; }

int main() {
    std::printf("test-ism-gallery — ISM's daily log and summary card in the gallery\n");
    const int64_t day0 = 20386;                                // 2025-10-25 … any day; 17:01:12 on it:
    const int64_t T0 = day0 * 86400 + 17 * 3600 + 1 * 60 + 12;

    // ── 1. the line, buffering, the fold ──────────────────────────────────────────────────────────────────────────
    {
        std::printf(" the line, the RAM buffer, the fold\n");
        const std::string dir = tmpDir("buf");
        gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
        int64_t clk = T0;
        Rig rig{ {}, std::make_shared<ismgal::DayLog>(st, opts(&clk)), &clk };
        ok(rig.push(oregon(10.5)), "the first Oregon reading is counted");
        clk += 1; ok(!rig.push(oregon(10.5)), "its repeat frame 1 s later is folded by the Tracker");
        clk += 1; ok(!rig.push(oregon(10.5)), "…and the third frame too");
        ok(files(dir).size() <= 1 /* .lock only, if any */ && index(dir).empty(), "nothing on disk yet: the reading is held in RAM");
        ok(rig.log->buffered() > 0, "…in the buffer (" + std::to_string(rig.log->buffered()) + " bytes)");
        clk += 1; rig.push(oregon(10.6));                       // a new value inside 3 s = a new reading
        clk += 5; rig.push(oregon(10.6));                       // the same value after 3 s = a new reading
        ok(rig.log->readings() == 3, "3 readings counted (10.5, 10.6, 10.6 after 5 s): " + std::to_string(rig.log->readings()));
        clk += 30; rig.log->tick();
        ok(index(dir).empty(), "35 s in: still nothing written (the minute has not passed)");
        clk += 30; rig.log->tick();
        const std::string name = rig.log->logName();
        ok(!name.empty(), "after a minute: ONE append — the day's log exists (" + name + ")");
        const std::string body = itemText(st, name);
        const std::string want = gallery::utcStamp(T0, "%H:%M:%S") + "  433.920  Oregon Scientific THN132N \xc2\xb7 10.5 \xc2\xb0" "C \xc2\xb7 battery low \xc2\xb7 ch 1\n";
        ok(want.substr(0, 8) == "17:01:12", "the clock is 17:01:12 UTC");
        ok(body.find(want) != std::string::npos, "the line exactly: \"" + want.substr(0, want.size() - 1) + "\"");
        ok(countOf(body, "Oregon Scientific THN132N") == 3, "three lines for three readings (the folded frames are not lines)");
        ok(body.rfind("ISM 433.92 MHz \xc2\xb7 every reading heard on ", 0) == 0, "the file says what it is on its first line");
        ok(index(dir).find("\"k\":\"ism\"") != std::string::npos && index(dir).find("\"o\":\"live\"") != std::string::npos, "indexed as kind ism, a listener's");
        const std::string date = gallery::utcStamp(T0, "%Y-%m-%d");
        ok(index(dir).find("\"dn\":\"ISM 433.92 " + date + ".txt\"") != std::string::npos, "the download name: \"ISM 433.92 " + date + ".txt\"");
        // the size threshold: 8 KB held is written before the minute
        const size_t before = itemText(st, name).size();
        for (int i = 0; i < 120; ++i) { clk += 4; rig.push(oregon(5.0 + i * 0.1)); }
        ok(itemText(st, name).size() > before && rig.log->buffered() < ismgal::DayLog::kFlushBytes, "8 KB held is appended at once, before the minute (" + std::to_string(itemText(st, name).size()) + " bytes on disk)");
        rig.log->finish();
        ok(rig.log->buffered() == 0 && countOf(itemText(st, name), "THN132N") == 123, "finish() writes what was held: 123 lines");
        ok(files(dir).size() == 3, "one log, one summary card and the index: " + std::to_string(files(dir).size()) + " files");
    }

    // ── 2. the card ──────────────────────────────────────────────────────────────────────────────────────────────
    {
        std::printf(" the summary card\n");
        const std::string dir = tmpDir("card");
        gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
        int64_t clk = T0;
        Rig rig{ {}, std::make_shared<ismgal::DayLog>(st, opts(&clk, "background")), &clk };
        for (int i = 0; i < 12; ++i) { rig.push(oregon(10.0 + i * 0.1)); clk += 40; }
        rig.push(wh1080()); clk += 4;
        rig.push(tpms(0x11223344, 220)); clk += 4;
        rig.push(tpms(0x99AABBCC, 230)); clk += 4;
        rig.push(fob(0x00C0FFEE)); clk += 4;
        rig.push(remote(0xA5C3E8));
        rig.log->tick();
        const std::string card = rig.log->cardName();
        ok(!card.empty(), "a card is written once 5 minutes have passed and something was heard");
        const std::string text = itemText(st, card);
        ok(text.find("6 devices: 2 weather, 2 TPMS, 1 key fob, 1 doorbell / remote") != std::string::npos,
           "devices by kind: \"6 devices: 2 weather, 2 TPMS, 1 key fob, 1 doorbell / remote\"");
        ok(text.find("Readings:     17\n") != std::string::npos, "17 readings");
        ok(text.find("Most heard:   Oregon Scientific THN132N \xc2\xb7 ch 1 (12\xc3\x97)") != std::string::npos, "most heard: Oregon Scientific THN132N · ch 1 (12×)");
        ok(text.find("Newest:       Doorbell / remote #1") != std::string::npos, "newest: Doorbell / remote #1");
        ok(text.find("Heard by background decoding (Profile 1).") != std::string::npos, "says who heard it");
        ok(text.find(" \xc2\xb7 1\xc3\x97 \xc2\xb7 TPMS \xc2\xb7 TPMS #2 \xc2\xb7 type B \xc2\xb7 33 PSI \xc2\xb7 18 \xc2\xb0" "C\n") != std::string::npos,
           "the last reading of each device: \"… · 1× · TPMS · TPMS #2 · type B · 33 PSI · 18 °C\"");
        const std::string title = rig.log->cardTitleNow();
        ok(title.size() <= 120 && title.find("6 devices: 2 weather, 2 TPMS") != std::string::npos, "the title fits 120 bytes: " + title);
        // rewritten at most once a minute, and only when something changed
        const std::string idx1 = index(dir);
        clk += 10; rig.push(oregon(9.0)); rig.log->tick();
        ok(itemText(st, card) == text, "a reading 10 s after the card was written does not rewrite it yet");
        clk += 60; rig.log->tick();
        ok(itemText(st, card) == text, "nor a minute later (the cards are rewritten at most every 5 minutes — SD wear)");
        clk += 240; rig.log->tick();
        ok(itemText(st, card) != text && itemText(st, card).find("Readings:     18\n") != std::string::npos, "5 minutes after the last write it is (18 readings), in place");
        const std::string t2 = itemText(st, card);
        clk += 600; rig.log->tick();
        ok(itemText(st, card) == t2, "nothing new heard: not rewritten");
        ok(countOf(index(dir), "\"s\":\"ismsum-") == 1, "one card in the index, however often it is rewritten");
        rig.log->finish();
    }

    // ── 3. privacy ────────────────────────────────────────────────────────────────────────────────────────────────
    {
        std::printf(" privacy\n");
        const std::string dir = tmpDir("priv");
        gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
        int64_t clk = T0;
        Rig rig{ {}, std::make_shared<ismgal::DayLog>(st, opts(&clk)), &clk };
        rig.push(tpms(0x11223344, 220)); clk += 5; rig.push(tpms(0x11223344, 221)); clk += 5;
        rig.push(fob(0x00C0FFEE)); clk += 5; rig.push(remote(0xA5C3E8)); clk += 5;
        vibeism::Reading sf; sf.kind = "suspectfob"; sf.make = "FSK"; sf.secret = true; sf.secretId = 0x7777ABCD; sf.freqHz = 434.134e6;
        rig.push(sf);
        rig.log->finish();
        const std::string all = everything(dir);
        ok(all.find("TPMS #1") != std::string::npos && all.find("Key fob #1") != std::string::npos && all.find("Suspected key fob #1") != std::string::npos
           && all.find("Doorbell / remote #1") != std::string::npos, "the secret kinds appear by their nicknames");
        bool clean = true; std::string hit;
        for (uint64_t id : { 0x11223344ull, 0x00C0FFEEull, 0xA5C3E8ull, 0x7777ABCDull }) {
            char a[32], b[32], c[32]; std::snprintf(a, sizeof a, "%llx", (unsigned long long)id); std::snprintf(b, sizeof b, "%llX", (unsigned long long)id);
            std::snprintf(c, sizeof c, "%llu", (unsigned long long)id);
            for (const char* w : { a, b, c }) if (all.find(w) != std::string::npos) { clean = false; hit = w; }
        }
        ok(clean, "no ID or code in any file or the index — decimal, hex upper and lower" + (clean ? std::string() : " (found " + hit + ")"));
    }

    // ── 4. a day, a band, two sources ─────────────────────────────────────────────────────────────────────────────
    {
        std::printf(" one file per UTC day per band\n");
        const std::string dir = tmpDir("day");
        gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
        int64_t clk = day0 * 86400 + 86400 - 30;               // 23:59:30
        Rig rig{ {}, std::make_shared<ismgal::DayLog>(st, opts(&clk)), &clk };
        rig.push(oregon(3.0));
        clk += 40; rig.push(oregon(2.9));                        // 00:00:10 the next day
        const std::string y = rig.log->logName();
        rig.log->finish();
        const std::string idx = index(dir);
        ok(countOf(idx, "\"s\":\"ismlog-433-") == 2 && countOf(idx, "\"s\":\"ismsum-433-") == 2, "midnight: two logs and two cards (one each per day)");
        ok(idx.find(gallery::utcStamp(day0 * 86400, "ismlog-433-%Y-%m-%d")) != std::string::npos
           && idx.find(gallery::utcStamp((day0 + 1) * 86400, "ismlog-433-%Y-%m-%d")) != std::string::npos, "each named by its own date");
        // background on the same day + band → the SAME file, now "both"; another band → another file
        int64_t c2 = day0 * 86400 + 86400 + 3600;
        Rig bg{ {}, std::make_shared<ismgal::DayLog>(st, opts(&c2, "background")), &c2 };
        bg.push(wh1080());
        bg.log->finish();
        const std::string idx2 = index(dir);
        ok(countOf(idx2, "\"s\":\"ismlog-433-") == 2, "background decoding on the same day writes into the listener's file — still two logs");
        ok(countOf(idx2, "\"o\":\"both\"") == 2, "…and the log and card of that day are now marked \"both\"");
        Rig b868{ {}, std::make_shared<ismgal::DayLog>(st, opts(&c2, "background", "868")), &c2 };
        vibeism::Reading lc; lc.kind = "weather"; lc.make = "LaCrosse"; lc.model = "TX29-IT"; lc.tempC = 7; lc.freqHz = 868.3e6;
        b868.push(lc); b868.log->finish();
        ok(countOf(index(dir), "\"s\":\"ismlog-868-") == 1 && index(dir).find("\"dn\":\"ISM 868 ") != std::string::npos, "another band, another file (\"ISM 868 …\")");
        (void)y;
    }

    // ── 5. a restart ─────────────────────────────────────────────────────────────────────────────────────────────
    {
        std::printf(" a restart\n");
        const std::string dir = tmpDir("restart");
        gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
        int64_t clk = T0;
        {
            Rig rig{ {}, std::make_shared<ismgal::DayLog>(st, opts(&clk)), &clk };
            for (int i = 0; i < 5; ++i) { rig.push(oregon(10.0 + i)); clk += 10; }
            rig.push(tpms(0x11223344, 220)); clk += 10; rig.push(tpms(0x55667788, 230));
            rig.log->finish();
        }
        const std::string log1 = files(dir).size() ? index(dir) : "";
        // a new process: a fresh Tracker (a fresh salt), the same store
        clk += 600;
        Rig rig{ {}, std::make_shared<ismgal::DayLog>(st, opts(&clk)), &clk };
        ok(rig.log->readings() == 7 && rig.log->devices() == 3, "the day's card is read back: 7 readings, 3 devices ("
           + std::to_string(rig.log->readings()) + ", " + std::to_string(rig.log->devices()) + ")");
        const auto floor = rig.log->nickFloor();
        ok(floor.count("tpms") && floor.at("tpms") == 2, "the day already named TPMS #1 and #2");
        for (auto& kv : floor) rig.tr.reserveNicks(kv.first, kv.second);
        rig.push(tpms(0x11223344, 222));                         // the same car — but the new salt cannot know it
        rig.push(oregon(20.0));
        rig.log->finish();
        const std::string text = itemText(st, rig.log->cardName());
        ok(text.find("TPMS #3") != std::string::npos && countOf(text, "TPMS #1 \xc2\xb7") == 1, "a TPMS after the restart is #3, never a second \"#1\"");
        ok(text.find("Readings:     9\n") != std::string::npos && text.find("Oregon Scientific THN132N \xc2\xb7 ch 1 (6\xc3\x97)") != std::string::npos,
           "the counts carry on: 9 readings, the Oregon 6×");
        ok(countOf(index(dir), "\"s\":\"ismlog-") == 1, "the log is APPENDED to after the restart, not a second file");
        ok(countOf(itemText(st, rig.log->logName()), "THN132N") == 6, "…and holds all 6 Oregon lines");
        (void)log1;
    }

    // ── 6. the part cap ──────────────────────────────────────────────────────────────────────────────────────────
    {
        std::printf(" bounded\n");
        const std::string dir = tmpDir("cap");
        gallery::Store st; st.setDir(dir); st.setLimitBytes(200ull << 20);
        int64_t clk = day0 * 86400 + 60;
        Rig rig{ {}, std::make_shared<ismgal::DayLog>(st, opts(&clk)), &clk };
        // ~5.6 MB of readings: each a new value, one a second for 22 hours — the log must split into parts
        int n = 0;
        while (n < 80000) { rig.push(oregon(-40.0 + (n % 1200) * 0.1, 1 + n % 3)); clk += 1; ++n; if (n % 4 == 0) rig.log->tick(); }
        rig.log->finish();
        const std::string idx = index(dir);
        ok(countOf(idx, "\"s\":\"ismlog-433-") >= 2, "a day past " + std::to_string(ismgal::DayLog::kPartMaxBytes >> 20) + " MB splits into parts ("
           + std::to_string(countOf(idx, "\"s\":\"ismlog-433-")) + ")");
        bool under = true;
        for (auto& f : files(dir)) { struct stat s{}; if (f.find("-ism-") != std::string::npos && stat((dir + "/" + f).c_str(), &s) == 0 && (uint64_t)s.st_size > ismgal::DayLog::kPartMaxBytes + 8192) under = false; }
        ok(under, "no part is bigger than the cap (+ one buffer)");
        ok(idx.find("(2).txt\"") != std::string::npos, "part 2 downloads as \"… (2).txt\"");
    }

    // ── 8. an ISM background profile: the config ─────────────────────────────────────────────────────────────────
    {
        std::printf(" an ISM background profile (vibe_bgdecode_config.h)\n");
        bg::Config c; std::string err;
        const std::string js = "{\"on\":true,\"waitMin\":5,\"profiles\":[{\"centre\":0,\"from\":\"00:00\",\"to\":\"00:00\",\"vfos\":[],\"ism\":\"433\"}]}";
        ok(bg::parse(js, c, err) && c.profiles.size() == 1 && c.profiles[0].isIsm() && c.profiles[0].ism == "433", "parses: profile 1 is ISM 433");
        ok(bg::toJson(c) == js, "round-trips: " + bg::toJson(c));
        ok(bg::validate(c, 2.4e6, err), "valid on an RTL at 2.4 MS/s (no centre, no VFOs needed)");
        ok(!bg::validate(c, 2.4e6, err, 0, 0, /*ismOk=*/false) && err.find("cannot tune to an ISM band") != std::string::npos, "an HF+ / Icom: refused — " + err);
        ok(!bg::validate(c, 2.4e6, err, 0, 433.92e6) && err.find("locked") != std::string::npos, "a locked radio: refused — " + err);
        ok(!bg::validate(c, 768e3, err) && err.find("0.9 MS/s") != std::string::npos, "768 kS/s: refused — " + err);
        bg::Config bad; bg::parse("{\"on\":true,\"profiles\":[{\"from\":\"00:00\",\"to\":\"00:00\",\"ism\":\"440\"}]}", bad, err);
        ok(!bg::validate(bad, 2.4e6, err) && err.find("choose an ISM band") != std::string::npos, "an unknown band: refused — " + err);
        // an ISM night + a NAVTEX day: two profiles, no overlap
        bg::Config two;
        bg::parse("{\"on\":true,\"profiles\":[{\"centre\":500000,\"from\":\"06:00\",\"to\":\"22:00\",\"vfos\":[{\"hz\":517500,\"mode\":\"usb\",\"dec\":\"navtex\"}]},"
                  "{\"from\":\"22:00\",\"to\":\"06:00\",\"ism\":\"868\"}]}", two, err);
        ok(bg::validate(two, 2.4e6, err) && bg::activeProfile(two, 23 * 60) == 1 && bg::activeProfile(two, 12 * 60) == 0, "NAVTEX by day + ISM 868 by night: valid, ISM at 23:00, NAVTEX at noon");
        bg::Config ov; bg::parse("{\"on\":true,\"profiles\":[{\"ism\":\"433\"},{\"ism\":\"868\",\"from\":\"01:00\",\"to\":\"02:00\"}]}", ov, err);
        ok(!bg::validate(ov, 2.4e6, err) && err.find("overlap") != std::string::npos, "two ISM profiles that overlap are refused and named — " + err);
        bg::Config nick; bg::parse("{\"on\":true,\"waitMin\":5,\"profiles\":[{\"centre\":500000,\"from\":\"00:00\",\"to\":\"00:00\",\"vfos\":[{\"hz\":489500,\"mode\":\"usb\",\"dec\":\"navtex\"}]}]}", nick, err);
        ok(bg::toJson(nick).find("ism") == std::string::npos, "an ordinary profile writes no \"ism\" key (older settings read back unchanged)");
    }

    // ── 7. the download name is header-safe ───────────────────────────────────────────────────────────────────────
    ok(gallery::downloadSafe("ISM 433.92 2026-10-25.txt") == "ISM 433.92 2026-10-25.txt"
       && gallery::downloadSafe("a\"b\r\nc/../d;e.txt") == "abc..de.txt" && gallery::downloadSafe("..x") == "x", "downloadSafe keeps a plain name and strips quotes, CR/LF, slashes");

    std::printf("%s: %d passed, %d failed\n", fails ? "FAILED" : "ok", passes, fails);
    return fails ? 1 : 0;
}
