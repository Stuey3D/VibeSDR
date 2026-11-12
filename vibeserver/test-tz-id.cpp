// ★★★ THE SERVER'S ZONE NAME (vibe_tz_id.h, 2026-11-12) — the "tz" field beside tzOffsetMin/tzAbbr.
// Kiko's pre-2019 Android box said -120 "BRST" in November; the name lets a current client correct it.
// Pure: every source is handed in, so this never depends on (or changes) the machine's real zone. Silent.
#include "vibe_tz_id.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void eq(const std::string& got, const std::string& want, const char* what) {
    const bool c = got == want;
    std::printf("  %s %s -> \"%s\"\n", c ? "ok  " : "FAIL", what, got.c_str());
    if (!c) { fails++; std::printf("       wanted \"%s\"\n", want.c_str()); }
}

int main() {
    std::printf("tz-id: choosing the zone name\n");
    // The sources, one at a time.
    eq(vsTzIdFrom("America/Sao_Paulo", "", "", ""), "America/Sao_Paulo", "TZ bare name");
    eq(vsTzIdFrom(":Europe/London", "", "", ""), "Europe/London", "TZ ':' prefix");
    eq(vsTzIdFrom("/usr/share/zoneinfo/Asia/Kolkata", "", "", ""), "Asia/Kolkata", "TZ as a zoneinfo path");
    eq(vsTzIdFrom(nullptr, "America/Sao_Paulo", "", ""), "America/Sao_Paulo", "Android persist.sys.timezone / Apple CF");
    eq(vsTzIdFrom(nullptr, "", "", "/usr/share/zoneinfo/America/Sao_Paulo"), "America/Sao_Paulo", "Linux /etc/localtime link");
    eq(vsTzIdFrom(nullptr, "", "", "../usr/share/zoneinfo/Pacific/Chatham"), "Pacific/Chatham", "relative link");
    eq(vsTzIdFrom(nullptr, "", "", "/var/db/timezone/zoneinfo/Europe/London"), "Europe/London", "macOS-style link");
    eq(vsTzIdFrom(nullptr, "", "", "/usr/share/zoneinfo/posix/America/St_Johns"), "America/St_Johns", "posix/ subtree");
    eq(vsTzIdFrom(nullptr, "", "Europe/Berlin\n", ""), "Europe/Berlin", "Debian /etc/timezone (newline trimmed)");
    eq(vsTzIdFrom(nullptr, "", "", "/usr/share/zoneinfo/Etc/GMT+3"), "Etc/GMT+3", "Etc/GMT+3 keeps its sign");
    // Precedence.
    eq(vsTzIdFrom("Asia/Tokyo", "America/Sao_Paulo", "", ""), "Asia/Tokyo", "TZ beats the platform");
    eq(vsTzIdFrom(nullptr, "", "Europe/Berlin", "/usr/share/zoneinfo/Europe/Paris"), "Europe/Paris",
       "the live symlink beats a stale /etc/timezone");
    eq(vsTzIdFrom("GMT0BST,M3.5.0/1,M10.5.0", "", "", "/usr/share/zoneinfo/Europe/London"), "Europe/London",
       "a POSIX rule in TZ is not a name — next source");
    // Refusals.
    eq(vsTzIdFrom(nullptr, "", "", "/etc/some/file"), "", "a path outside zoneinfo");
    eq(vsTzIdFrom(nullptr, "", "", "/usr/share/zoneinfo/../../etc/passwd"), "", "'..' refused");
    eq(vsTzIdFrom(nullptr, "Bad\"Quote", "", ""), "", "a JSON-breaking character refused");
    eq(vsTzIdFrom(nullptr, "", "", ""), "", "nothing says -> empty (field not sent)");
    eq(vsTzIdFrom("", "", "", ""), "", "empty TZ is not a name");
    // The real machine: just that it is either empty or plausible (CI boxes are often UTC / unset).
    const std::string live = vsZoneId();
    const bool liveOk = live.empty() || vsTzIdPlausible(live);
    std::printf("  %s this machine -> \"%s\"\n", liveOk ? "ok  " : "FAIL", live.c_str());
    if (!liveOk) fails++;
    std::printf(fails ? "tz-id: %d FAILED\n" : "tz-id: all passed\n", fails);
    return fails ? 1 : 0;
}
