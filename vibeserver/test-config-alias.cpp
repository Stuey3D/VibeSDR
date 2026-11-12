// ★★ "ALSO GIVE IT A vibesdr.net ADDRESS" (2026-11-12) — dirAlias round-trips through the config, and a server that
// never had it (every config.json written before) keeps the alias it always had.
// Checks: default on; written and read back off; an older file without the key reads ON; a patch that does not mention
// it (a live gain nudge, a page that predates the switch) leaves the owner's choice alone; dirPublicUrl beside it.

#include "vibeserver_config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

static int failures = 0, passes = 0;
static void ok(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", what);
    if (cond) passes++; else failures++;
}

int main() {
    std::printf("config — dirAlias (the vibesdr.net address for a listing at the owner's own address)\n");
    std::string err;

    vsconfig::ServerConfig d;
    ok(d.dirAlias, "default: on (what every server always had)");
    ok(vsconfig::toJson(d).find("\"dirAlias\": true") != std::string::npos, "and it is written");

    // ★ An older config.json: listed at its own address, and no dirAlias at all.
    const std::string old = R"({"configured": true, "name": "Loft", "dirList": true,
        "dirPublicUrl": "https://sdr.example.org", "radios": [{"serial":"00000001","driver":"rtlsdr","configured":true}]})";
    vsconfig::ServerConfig o;
    ok(vsconfig::fromJson(old, o, err), "an older file loads");
    ok(o.dirAlias && o.dirPublicUrl == "https://sdr.example.org", "★ without the key: alias ON, address as written");

    // ★ The owner switches it off — through the page's POST shape — and it survives save + load.
    ok(vsconfig::fromJson(R"({"dirAlias": false})", o, err) && !o.dirAlias, "a post with dirAlias:false turns it off");
    char tmpl[] = "/tmp/vs-cfgalias-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    const std::string path = dir + "/config.json";
    ok(vsconfig::saveServer(path, o, err), "saved");
    vsconfig::ServerConfig back;
    ok(vsconfig::loadServer(path, back, err), "loaded");
    ok(!back.dirAlias && back.dirPublicUrl == "https://sdr.example.org" && back.dirList, "★ off, address and listing all as saved");
    { bool kept = false; for (const auto& kv : back.unknownKeys) kept = kept || kv.first == "dirAlias";
      ok(!kept, "dirAlias is this version's own key, not a kept unknown"); }

    // ★ A patch that says nothing about it must not have an opinion about it.
    ok(vsconfig::fromJson(R"({"name": "Attic"})", back, err) && !back.dirAlias, "a patch without the key keeps it off");
    ok(vsconfig::fromJson(R"({"dirAlias": true})", back, err) && back.dirAlias, "and a post can turn it back on");

    // ★ Empty address = the tunnel, as ever: the key stays whatever it was (the server simply never sends it there).
    ok(vsconfig::fromJson(R"({"dirPublicUrl": "", "dirAlias": false})", back, err) && back.dirPublicUrl.empty() && !back.dirAlias,
       "the tunnel (empty address) keeps the stored choice for when an address is typed again");

    unlink(path.c_str()); unlink((path + ".bak").c_str()); rmdir(dir.c_str());
    std::printf(failures ? "\n%d FAILED, %d passed\n" : "\nall %d passed\n", failures ? failures : passes, passes);
    return failures ? 1 : 0;
}
