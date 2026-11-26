// ★★ V12 beta96 SHARE ADS-B WITH FLIGHT TRACKERS — RadioConfig::adsbFeed (one JSON blob per radio, as dxScan): OFF by
// default (empty), round-trips through the config file, an older file reads it empty, another radio keeps none, an
// oversized blob is dropped rather than half-applied, and what is stored parses with vibe_beast.h.
#include "vibeserver_config.h"
#include "vibe_beast.h"

#include <cstdio>
#include <string>

static int failures = 0, passes = 0;
static void ok(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "\033[32mok\033[0m  " : "\033[31mFAIL\033[0m", what);
    if (cond) passes++; else failures++;
}

int main() {
    std::printf("config — share ADS-B with flight trackers (adsbFeed)\n");
    vsconfig::RadioConfig r0;
    ok(r0.adsbFeed.empty(), "default: empty (off)");
    vibe::beast::Config none; std::string e;
    ok(vibe::beast::parseConfig(r0.adsbFeed, none, e) && !none.any(), "…which the engine reads as everything off");
    const std::string blob = R"({"serve":true,"port":30005,"open":false,"push":true,"feeds":[{"host":"feed.adsb.lol","port":30004,"on":true,"uuid":"0f3c3a2b-1234-4abc-9def-0123456789ab"}]})";
    vsconfig::ServerConfig d;
    d.configured = true;
    vsconfig::RadioConfig a; a.serial = "00000001"; a.driver = "rtlsdr"; a.configured = true; a.adsbFeed = blob;
    vsconfig::RadioConfig b; b.serial = "00000002"; b.driver = "rtlsdr"; b.configured = true;
    d.radios = {a, b};
    vsconfig::ServerConfig back; std::string err;
    ok(vsconfig::fromJson(vsconfig::toJson(d), back, err) && back.radios.size() == 2, "written and read back");
    ok(back.radios.size() == 2 && back.radios[0].adsbFeed == blob, "the radio's blob survives exactly");
    ok(back.radios.size() == 2 && back.radios[1].adsbFeed.empty(), "another radio keeps none");
    vibe::beast::Config c;
    ok(back.radios.size() == 2 && vibe::beast::parseConfig(back.radios[0].adsbFeed, c, e) && c.serve && c.push && c.feeds.size() == 1
       && c.feeds[0].uuid.size() == 36, "and the engine reads it");
    const std::string old = R"({"configured": true, "name": "Loft", "radios": [{"serial":"00000001","driver":"rtlsdr","configured":true}]})";
    vsconfig::ServerConfig o;
    ok(vsconfig::fromJson(old, o, err) && o.radios.size() == 1 && o.radios[0].adsbFeed.empty(), "an older file: empty (off)");
    const std::string huge = std::string(R"({"configured": true, "radios": [{"serial":"1","driver":"rtlsdr","configured":true,"adsbFeed":")")
                           + std::string(5000, 'x') + R"("}]})";
    vsconfig::ServerConfig h;
    ok(vsconfig::fromJson(huge, h, err) && h.radios.size() == 1 && h.radios[0].adsbFeed.empty(), "an oversized blob is dropped, never half-applied");
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
