// ★★ V12 beta96 SHARE ADS-B WITH FLIGHT TRACKERS — RadioConfig::adsbFeed (one JSON blob per radio, as dxScan): OFF by
// default (empty), round-trips through the config file, an older file reads it empty, another radio keeps none, an
// oversized blob is dropped rather than half-applied, and what is stored parses with vibe_beast.h.
#include "vibeserver_config.h"
#include "vibe_beast.h"
#include "vibe_feeds.h"
#include "vibe_radio_caps.h"

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
    // ★★ beta96 (Stuart 11-26) the destinations are SERVER-level: a radio's blob is merged into adsbShare and cleared
    ok(vsconfig::fromJson(vsconfig::toJson(d), back, err) && back.radios.size() == 2, "written and read back");
    ok(back.radios.size() == 2 && back.radios[0].adsbFeed.empty() && back.radios[1].adsbFeed.empty(), "the radio's own blob is migrated away (cleared)");
    vibe::beast::Config c;
    ok(vibe::beast::parseConfig(back.adsbShare, c, e) && c.serve && c.push && c.feeds.size() == 1 && c.feeds[0].host == "feed.adsb.lol"
       && c.feeds[0].uuid.size() == 36 && c.port == 30005, "…into the server-level hub: serve, push, the destination and its UUID");
    vsconfig::ServerConfig again;
    ok(vsconfig::fromJson(vsconfig::toJson(back), again, err) && again.adsbShare == back.adsbShare, "the migration is idempotent (a second read changes nothing)");
    // two radios with overlapping destinations: merged once each
    vsconfig::ServerConfig two; two.configured = true;
    vsconfig::RadioConfig t1 = a, t2 = b;
    t1.adsbFeed = R"({"push":true,"feeds":[{"host":"feed.adsb.lol","port":30004},{"host":"feed.map.flights","port":30004,"uuid":"0f3c3a2b-1234-4abc-9def-0123456789ab"}]})";
    t2.adsbFeed = R"({"serve":true,"port":30105,"open":true,"push":true,"feeds":[{"host":"FEED.ADSB.LOL","port":30004},{"host":"feed.adsb.fi","port":30004,"on":false}]})";
    two.radios = {t1, t2};
    vsconfig::ServerConfig tb;
    vibe::beast::Config tc;
    ok(vsconfig::fromJson(vsconfig::toJson(two), tb, err) && vibe::beast::parseConfig(tb.adsbShare, tc, e) && tc.feeds.size() == 3,
       "two radios' destinations merge, the duplicate (any case) kept once");
    ok(tc.serve && tc.port == 30105 && tc.open && tc.push && !tc.feeds[2].on, "…serve from the radio that served (its port, any address), push, a switched-off one stays off");
    const std::string old = R"({"configured": true, "name": "Loft", "radios": [{"serial":"00000001","driver":"rtlsdr","configured":true}]})";
    vsconfig::ServerConfig o;
    ok(vsconfig::fromJson(old, o, err) && o.radios.size() == 1 && o.radios[0].adsbFeed.empty() && o.adsbShare.empty() && o.aircraftFeeds.empty(), "an older file: everything off");
    const std::string huge = std::string(R"({"configured": true, "radios": [{"serial":"1","driver":"rtlsdr","configured":true,"adsbFeed":")")
                           + std::string(5000, 'x') + R"("}]})";
    vsconfig::ServerConfig h;
    ok(vsconfig::fromJson(huge, h, err) && h.radios.size() == 1 && h.radios[0].adsbFeed.empty(), "an oversized blob is dropped, never half-applied");
    // ★★ beta96 FEEDS ARE NOT RADIOS: a Beast feed radio from b96-adsbfeed becomes a server-level source, its entry dropped
    vsconfig::ServerConfig bs; bs.configured = true;
    vsconfig::RadioConfig br; br.serial = "beast-1"; br.driver = "beast"; br.configured = true; br.beastIn = "192.168.86.12:30005"; br.label = "PiAware";
    vsconfig::RadioConfig rtl = b;
    bs.radios = {rtl, br};
    vsconfig::ServerConfig bb;
    std::vector<vibe::feeds::SourceCfg> src;
    ok(vsconfig::fromJson(vsconfig::toJson(bs), bb, err) && bb.radios.size() == 1 && bb.radios[0].serial == "00000002", "the Beast feed radio's entry is dropped; the real radio stays");
    ok(vibe::feeds::parseSources(bb.aircraftFeeds, src, e) && src.size() == 1 && src[0].fmt == "beast" && src[0].dir == "connect"
       && src[0].host == "192.168.86.12" && src[0].port == 30005 && src[0].label == "adsb" && src[0].name == "PiAware",
       "…and is a server-level source: Beast, connect 192.168.86.12:30005, ADS-B, named PiAware");
    vsconfig::ServerConfig bb2;
    ok(vsconfig::fromJson(vsconfig::toJson(bb), bb2, err) && bb2.aircraftFeeds == bb.aircraftFeeds && bb2.radios.size() == 1, "read again: unchanged");
    // a source that is already there is not added twice
    vsconfig::ServerConfig dup = bs; dup.aircraftFeeds = R"([{"fmt":"beast","dir":"connect","host":"192.168.86.12","port":30005}])";
    vsconfig::ServerConfig db;
    ok(vsconfig::fromJson(vsconfig::toJson(dup), db, err) && vibe::feeds::parseSources(db.aircraftFeeds, src, e) && src.size() == 1, "a feed radio already listed as a source is not added twice");
    // Stuart's two sources survive a round trip
    vsconfig::ServerConfig st; st.configured = true; st.radios = {b};
    st.aircraftFeeds = R"([{"fmt":"beast","dir":"connect","host":"192.168.86.12","port":30005,"label":"adsb","name":"PiAware"},{"fmt":"sbs","dir":"listen","port":32010,"label":"hfdl","name":"UberSDR HFDL"}])";
    vsconfig::ServerConfig sb;
    ok(vsconfig::fromJson(vsconfig::toJson(st), sb, err) && sb.aircraftFeeds == st.aircraftFeeds, "server-level sources round-trip exactly");
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
