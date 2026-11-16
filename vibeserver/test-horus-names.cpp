// test-horus-names.cpp — V12 balloons: Horus payload IDs → callsigns (vibe_horus_names.h, docs/v12/BALLOONS.md).
// The list's own format (Project Horus's payload_id_list.txt: '#' comments, "ID, CALLSIGN"), the bounds, the fallback
// "HORUS-<id>", and the fetch rule: only when a name is asked for, AT MOST ONCE A DAY across restarts, a failure counts as
// the day's try, a bad or oversized download never replaces a good copy. A fake transport and a fake clock; no network.
//   build: c++ -std=c++17 -I android/app/src/main/cpp -o /tmp/t vibeserver/test-horus-names.cpp -lpthread && /tmp/t
#include "vibe_horus_names.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

static int passes = 0, fails = 0;
static void ok(bool c, const std::string& what) { (c ? passes : fails)++; std::printf("  %s %s\n", c ? "ok  " : "FAIL", what.c_str()); }

static const char* kSample =
    "# Horus Binary Payload ID List\n"
    "# Payload IDs 0 through 255 are reserved for 'v1' payloads (22 byte packets).\n"
    "#\n"
    "0, 4FSKTEST\n"
    "1, HORUSBINARY\n"
    "2, VK5BRL\r\n"
    "   256, 4FSKTEST-V2\n"
    "31418, HORUSGEIGER\n"
    "42069, KI1TEN\n"
    "70000, TOOBIG\n"
    "x12, BADID\n"
    "77, has space\n"
    "78, <script>\n"
    "79, AVERYVERYVERYLONGCALLSIGNNAME\n"
    "80\n"
    "\n";

int main() {
    std::printf("── the list's format ──\n");
    {
        std::map<int, std::string> m;
        const size_t n = vibehorus::parseList(kSample, m);
        ok(n == 6 && m.size() == 6, "six good lines of fourteen (" + std::to_string(m.size()) + ")");
        ok(m[0] == "4FSKTEST" && m[2] == "VK5BRL" && m[256] == "4FSKTEST-V2" && m[42069] == "KI1TEN", "v1 and v2 IDs, CRLF and leading spaces");
        ok(!m.count(70000) && !m.count(77) && !m.count(78) && !m.count(79) && !m.count(80), "out of range, spaces, markup, too long, no name: skipped");
        std::string big; for (int i = 0; i < 70000; ++i) big += std::to_string(i) + ", P" + std::to_string(i) + "\n";
        std::map<int, std::string> b; vibehorus::parseList(big, b);
        ok(b.size() <= vibehorus::kMaxIds, "never more than kMaxIds names (" + std::to_string(b.size()) + ")");
    }

    char tmpl[] = "/tmp/vs-horus-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    int64_t now = 1'800'000'000;
    int calls = 0; int status = 200; std::string body = kSample;
    auto fake = [&](const std::string& url, const std::string& dest) {
        ++calls;
        ok(url == vibehorus::kListUrl, "the URL is Project Horus's list");
        if (status == 200) { std::ofstream f(dest, std::ios::binary); f << body; }
        return status;
    };

    std::printf("── fetched only when needed, at most once a day ──\n");
    {
        vibehorus::Names n; n.inlineRefresh = true; n.clock = [&] { return now; };
        n.setDir(dir); n.setFetcher(fake);
        ok(calls == 0, "nothing downloaded at start (a server that never hears Horus never fetches)");
        ok(n.display(2) == "VK5BRL" && calls == 1, "the first name asked for downloads the list: 2 → VK5BRL");
        ok(n.display(31418) == "HORUSGEIGER" && n.display(5) == "HORUS-5", "a known v2 ID by name; an unknown one HORUS-5");
        now += 3600; n.name(1);
        ok(calls == 1, "an hour later: no second download");
        now += 86400; n.name(1);
        ok(calls == 2, "a day later: refreshed");
    }
    std::printf("── across a restart, failures, bad downloads ──\n");
    {
        vibehorus::Names n; n.inlineRefresh = true; n.clock = [&] { return now + 60; };
        n.setDir(dir); n.setFetcher(fake);
        ok(n.size() == 6, "a restart reads the copy on disk (" + std::to_string(n.size()) + " names)");
        n.name(1);
        ok(calls == 2, "…and does not download again the same day");
    }
    {
        now += 2 * 86400;
        vibehorus::Names n; n.inlineRefresh = true; n.clock = [&] { return now; };
        n.setDir(dir); n.setFetcher(fake);
        status = 0;
        ok(n.display(2) == "VK5BRL" && calls == 3, "the network down: the old copy still names 2");
        n.name(2); ok(calls == 3, "a failed try counts as the day's try");
        now += 86400 + 1; status = 200; body = std::string(vibehorus::kMaxBytes + 10, '#');
        n.name(2);
        ok(calls == 4 && n.display(2) == "VK5BRL", "an oversized download is refused; the good copy stays");
        now += 86400 + 1; body = "<html>not the list</html>\n";
        n.name(2);
        ok(calls == 5 && n.display(2) == "VK5BRL", "a download that is not the list (no lines parse) never replaces it");
        now += 86400 + 1; body = std::string(kSample) + "12345, NEWBIE\n";
        ok(n.display(12345) == "NEWBIE" && calls == 6, "a good new list is taken: a new payload's name");
    }
    {
        vibehorus::Names n; n.inlineRefresh = true;
        ok(n.display(7) == "HORUS-7" && calls == 6, "no directory and no transport (iOS, a box with no downloader): HORUS-<id>, no fetch");
    }
    std::system(("rm -rf '" + dir + "'").c_str());
    std::printf("horus names: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
