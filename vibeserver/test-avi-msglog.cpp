// test-avi-msglog.cpp — ★★★ beta97 hotfix the datalink MESSAGE LOG (vibe_avi_service.h Tracker::msgsJson, served at
// /vibeserver/avmsgs.json): every decoded message (two from one aircraft between reads → two), newest first with a
// sequence cursor (since=), the time window, bodies only for an asker with the right, bounded (kLogMax; a call's max, trunc).
//   c++ -std=c++17 -I../android/app/src/main/cpp test-avi-msglog.cpp -o /tmp/t && /tmp/t
#include "vibe_avi_service.h"
#include <cstdio>
#include <cstring>
static int fails = 0;
static void ok(bool c, const char* w) { std::printf("  %s %s\n", c ? "ok  " : "FAIL", w); if (!c) ++fails; }
static int count(const std::string& s, const char* k) { int n = 0; for (size_t p = 0; (p = s.find(k, p)) != std::string::npos; ++p) ++n; return n; }
int main() {
    vibeavi::Tracker t(true);
    vibeacars::Block b; b.reg = ".G-EZAB"; b.label = "H1"; b.flight = "EZY12"; b.text = "FIRST"; b.downlink = true; b.hz = 131.725e6; b.snrDb = 12;
    t.addAcars(b, 1000);
    b.text = "SECOND"; b.label = "QA";
    t.addAcars(b, 1001);
    std::string j = t.msgsJson(1002, 0, 3600, 300, true);
    ok(count(j, "\"id\":") == 2 && j.find("FIRST") != std::string::npos && j.find("SECOND") != std::string::npos, "two messages from one aircraft between reads: two entries");
    ok(j.find("\"seq\":2") != std::string::npos && j.find("\"id\":2") < j.find("\"id\":1"), "newest first, with the sequence cursor");
    std::string j2 = t.msgsJson(1002, 1, 3600, 300, true);
    ok(count(j2, "\"id\":") == 1 && j2.find("SECOND") != std::string::npos, "since=1: only what came after");
    std::string j3 = t.msgsJson(1002, 0, 3600, 300, false);
    ok(j3.find("FIRST") == std::string::npos && j3.find("\"label\":\"H1\"") != std::string::npos, "no bodies for an asker without the right (labels stay)");
    std::string j4 = t.msgsJson(5000, 0, 3600, 300, true);
    ok(count(j4, "\"id\":") == 0, "outside the window: none");
    for (int i = 0; i < 2000; ++i) { b.text = "M"; t.addAcars(b, 2000 + i * 0.01); }
    std::string j5 = t.msgsJson(2100, 0, 86400, 5000, true);
    ok(count(j5, "\"id\":") == (int)vibeavi::Tracker::kLogMax, "bounded: the newest kLogMax kept");
    std::string j6 = t.msgsJson(2100, 0, 86400, 300, true);
    ok(count(j6, "\"id\":") == 300 && j6.find("\"trunc\":true") != std::string::npos, "a call's cap: the newest 300, trunc says so");
    std::printf("server log: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
