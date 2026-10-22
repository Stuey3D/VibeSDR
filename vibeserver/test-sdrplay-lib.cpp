// ★★★ WHICH RSP DRIVER, AND WHOSE SERVICE (vibe_sdrplay_lib.h, 2026-10-22) — both answers, on a scratch
// filesystem, so the "a system SDRplay stack wins" rule is proved both ways without touching the real one.
// This Mac HAS SDRplay 3.15.1 installed; the test never reads /Library, /opt or anything outside its root.
//   build: c++ -std=c++17 -I android/app/src/main/cpp -o /tmp/tsl vibeserver/test-sdrplay-lib.cpp
#include "vibe_sdrplay_lib.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

static int fails = 0, passes = 0;
static void ok(bool cond, const char* what) {
    if (cond) { ++passes; printf("   ok   %s\n", what); }
    else      { ++fails;  printf("   FAIL %s\n", what); }
}

static void mkdirs(const std::string& p) {
    std::string cur;
    for (size_t i = 1; i <= p.size(); i++) {
        if (i == p.size() || p[i] == '/') { cur = p.substr(0, i); ::mkdir(cur.c_str(), 0755); }
    }
}
static void touch(const std::string& p) {
    mkdirs(p.substr(0, p.rfind('/')));
    if (FILE* f = std::fopen(p.c_str(), "w")) std::fclose(f);
}

int main() {
    using namespace vibe::sdrplaylib;
    char tmpl[] = "/tmp/vibe-sdrplay-lib-XXXXXX";
    const char* d = ::mkdtemp(tmpl);
    if (!d) { printf("mkdtemp failed\n"); return 1; }
    const std::string root = d;

    printf("\nmacOS: is a system SDRplay stack installed?\n");
    ok(!macSystemStack(root), "an empty disk: no — the app runs its own service");
    touch(root + "/Library/SDRplayAPI/3.15.1/bin/sdrplay_apiService");
    ok(macSystemStack(root), "SDRplay's versioned service binary: yes");
    std::system(("rm -rf '" + root + "/Library'").c_str());
    touch(root + "/Library/LaunchDaemons/com.sdrplay.service.plist");
    ok(macSystemStack(root), "only the launch daemon plist (a half-uninstalled stack still starts at boot): yes");
    std::system(("rm -rf '" + root + "/Library'").c_str());
    touch(root + "/Library/SDRplayAPI/3.15.1/lib/libsdrplay_api.so.3.15");
    ok(!macSystemStack(root), "a library with no service and no daemon: no (a library is not a service)");

    printf("\nLinux: the same fact as vibeserver-rsp.service's ConditionPathExists\n");
    ok(!linuxSystemStack(root), "no /opt/sdrplay_api: no — vibeserver-rsp runs");
    touch(root + "/opt/sdrplay_api/sdrplay_apiService");
    ok(linuxSystemStack(root), "/opt/sdrplay_api/sdrplay_apiService (official or sdrplay-api-open): yes");
    ok(std::string(kLinuxSystemService) == "/opt/sdrplay_api/sdrplay_apiService",
       "the path matches the unit file's ConditionPathExists=! (checked by hand there)");

    printf("\nThe supervisor: run our service only with a bundled one and no system stack\n");
    ok(macRunBundledService(true, false), "bundled service, no system stack: run it");
    ok(!macRunBundledService(true, true), "bundled service, SDRplay installed: stay out of its way");
    ok(!macRunBundledService(false, false), "no bundled service (an older app): nothing to run");

    printf("\nLibrary order\n");
    const std::vector<std::string> sys = {"/Library/SDRplayAPI/3.15.1/lib/libsdrplay_api.so.3.15",
                                          "/usr/local/lib/libsdrplay_api.so.3"};
    const std::string ours = "/Applications/VibeServer.app/Contents/Frameworks/libsdrplay_api.so.3";
    auto a = candidateOrder(false, ours, sys);
    ok(a.size() == 3 && a.front() == ours, "no system stack: the bundled library first");
    auto b = candidateOrder(true, ours, sys);
    ok(b.size() == 3 && b.front() == sys[0] && b.back() == ours, "system stack: theirs first, ours the fallback");
    auto c = candidateOrder(false, "", sys);
    ok(c == sys, "no bundled library (command-line build): the system list unchanged");

    printf("\nOur own bundle, found from the executable\n");
    ok(bundleContentsOf("/Applications/VibeServer.app/Contents/MacOS/VibeServer") == "/Applications/VibeServer.app/Contents",
       "Simple mode (the app itself)");
    ok(bundleContentsOf("/Users/x/Desktop/VibeServer.app/Contents/MacOS/vibeserver-engine") == "/Users/x/Desktop/VibeServer.app/Contents",
       "Full mode (the engine), dragged anywhere");
    ok(bundleContentsOf("/Users/x/VibeSDR/vibeserver/build/vibeserver").empty(), "a build-tree binary: no bundle");
    ok(bundleContentsOf("/A/Contents/MacOS/x/Y.app/Contents/MacOS/VibeServer") == "/A/Contents/MacOS/x/Y.app/Contents",
       "the LAST Contents/MacOS wins");
    ok(bundleContentsOf("/A.app/Contents/MacOS/sub/tool").empty(), "MacOS/<dir>/exe is not a bundle executable");

    printf("\nThe in-bundle check that replaces root ownership for our own library\n");
    const std::string ct = "/Applications/VibeServer.app/Contents";
    ok(insideBundleFrameworks(ct + "/Frameworks/libsdrplay_api.so.3.15", ct), "the real file in our Frameworks: accepted");
    ok(!insideBundleFrameworks("/Users/x/Library/libsdrplay_api.so.3.15", ct), "a user-writable path: refused");
    ok(!insideBundleFrameworks("/Applications/VibeServer.app/Contents/Frameworks2/libsdrplay_api.so", ct),
       "a sibling that merely starts with the same letters: refused");
    ok(!insideBundleFrameworks(ct + "/Frameworks/../../../evil.so", ct), "a path with .. in it: refused");
    ok(!insideBundleFrameworks(ct + "/Frameworks/sub/lib.so", ct), "a subdirectory of Frameworks: refused");
    ok(!insideBundleFrameworks(ct + "/Frameworks/", ct), "the directory itself: refused");
    ok(!insideBundleFrameworks(ct + "/Frameworks/x.so", ""), "no bundle at all: refused");

    printf("\nAntenna names: exact first, then a short name that means exactly one port\n");
    const std::vector<std::string> duo = {"Tuner 1 50\u03a9", "Tuner 1 Hi-Z", "Tuner 2 50\u03a9"};
    const std::vector<std::string> rsp2 = {"A", "B", "Hi-Z"}, dx = {"A", "B", "C"};
    ok(resolvePort(rsp2, "Hi-Z") == "Hi-Z", "RSP2: Hi-Z is Hi-Z");
    ok(resolvePort(duo, "Hi-Z") == "Tuner 1 Hi-Z", "Duo: an RSP2-style rule 'Hi-Z' finds Tuner 1 Hi-Z");
    ok(resolvePort(duo, "Tuner 2") == "Tuner 2 50\u03a9", "Duo: 'Tuner 2' is its only socket");
    ok(resolvePort(duo, "Tuner 1").empty(), "Duo: 'Tuner 1' is two ports — ambiguous, ignored");
    ok(resolvePort(duo, "50\u03a9").empty(), "Duo: '50Ω' is two ports — ambiguous, ignored");
    ok(resolvePort(duo, "Tuner 1 Hi-Z") == "Tuner 1 Hi-Z", "Duo: the full name, exactly");
    ok(resolvePort(dx, "C") == "C" && resolvePort(dx, "D").empty(), "dx: C is C, D is nothing");
    ok(resolvePort(dx, "Hi-Z").empty(), "dx: no Hi-Z port, so a Hi-Z rule changes nothing");
    ok(resolvePort({}, "A").empty(), "a single-socket radio: nothing to choose");
    ok(resolvePort(duo, "Z").empty() && resolvePort(duo, "").empty(), "not a word-boundary match, or empty: nothing");

    printf("\nAndroid: the RSP needs API 26 (ASharedMemory_create)\n");
    ok(!androidRspSupported(21), "API 21 (Lite's floor, a Fire 7): not offered");
    ok(!androidRspSupported(25), "API 25: not offered");
    ok(androidRspSupported(26), "API 26 (Android 8.0): offered");
    ok(androidRspSupported(36), "API 36: offered");

    std::system(("rm -rf '" + root + "'").c_str());
    printf("\n%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
