// ★★★ A CONFIG THAT SURVIVES A PULLED PLUG (2026-10-10, the pocket box).
//
// saveServer writes temp → fsync → keeps the replaced file as config.json.bak (hard link) → rename →
// fsync the directory. loadServerOrBackup reads the file, or the .bak if the file is missing, EMPTY
// (what a cut leaves on a filesystem without data journalling) or garbage. These are the states a
// power cut can leave behind; each must end with a readable config holding the admin password —
// the one setting whose loss locks the owner out of a box with no terminal.

#include "vibeserver_config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static void ok(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", what);
    if (!cond) failures++;
}

int main() {
    std::printf("config durability — the states a power cut leaves behind\n");
    char tmpl[] = "/tmp/vs-cfgdur-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    const std::string path = dir + "/config.json";
    std::string err;

    vsconfig::ServerConfig a; a.fullMode = true; a.adminPass = "first-pass"; a.name = "A";
    ok(vsconfig::saveServer(path, a, err), "first save");
    struct stat st{};
    ok(::stat((path + ".bak").c_str(), &st) != 0, "no backup yet (nothing was replaced)");
    ok(::stat((path + ".tmp").c_str(), &st) != 0, "no temp file left behind");
    ok(::stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "config is 0600");

    vsconfig::ServerConfig b = a; b.adminPass = "second-pass"; b.name = "B";
    ok(vsconfig::saveServer(path, b, err), "second save");
    vsconfig::ServerConfig r; bool usedBak = false;
    ok(vsconfig::loadServerOrBackup(path, r, err, usedBak) && !usedBak && r.adminPass == "second-pass",
       "normal load: the new settings, not the backup");
    vsconfig::ServerConfig bk;
    ok(vsconfig::loadServer(path + ".bak", bk, err) && bk.adminPass == "first-pass",
       "the replaced file was kept as config.json.bak (the last good copy)");

    { std::ofstream t(path, std::ios::trunc); }               // a cut that left the file EMPTY
    r = {}; usedBak = false;
    ok(vsconfig::loadServerOrBackup(path, r, err, usedBak) && usedBak && r.adminPass == "first-pass",
       "empty config.json: the backup is used — the owner can still sign in");

    { std::ofstream t(path, std::ios::trunc); t << "\x01\x02 not json at all"; }
    r = {}; usedBak = false;
    ok(vsconfig::loadServerOrBackup(path, r, err, usedBak) && usedBak, "garbage config.json: the backup is used");

    ::unlink(path.c_str());
    r = {}; usedBak = false;
    ok(vsconfig::loadServerOrBackup(path, r, err, usedBak) && usedBak, "config.json missing: the backup is used");

    ::unlink((path + ".bak").c_str());
    r = {}; usedBak = false;
    ok(!vsconfig::loadServerOrBackup(path, r, err, usedBak), "both gone: reported as unreadable (the pocket box then starts as new)");

    std::string rm = "rm -rf '" + dir + "'";
    (void)!std::system(rm.c_str());
    if (failures) { std::printf("\n\033[31m%d failed\033[0m\n", failures); return 1; }
    std::printf("\n\033[32mall passed\033[0m\n");
    return 0;
}
