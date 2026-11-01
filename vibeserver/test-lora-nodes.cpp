// test-lora-nodes.cpp — V12 THE ISM MAP's server half (2026-11-01; vibe_lora_track.h, vibe_ism_gallery.h "stations
// received"): positions parsed from MeshCore adverts and Meshtastic POSITION / NODEINFO, the node table (cap, 24 h expiry,
// newest wins), the PRIVACY RULE (a person's node leaves rounded to ~1 km and marked approximate; infrastructure exact; no
// message text near a position), and the gallery's node card (bounded, written once a minute not per packet, read back
// after a restart, background vs live, the furthest node).
//   test-lora-nodes                          the tests (fast: no DSP)
//   test-lora-nodes --seed-gallery <dir>     writes a day's "stations received" card into a gallery dir (the web test)
// Build: g++ -std=c++17 -O2 -I android/app/src/main/cpp -o test-lora-nodes vibeserver/test-lora-nodes.cpp -lpthread
#include "vibe_ism_service.h"
#include "vibe_ism_gallery.h"

#include <dirent.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace vibelora;
static int fails = 0, passes = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); (c ? passes : fails)++; }
static int countOf(const std::string& s, const std::string& w) { int n = 0; for (size_t p = 0; (p = s.find(w, p)) != std::string::npos; p += w.size()) ++n; return n; }

// ── packet builders (the encoding side of each spec, as test-lora.cpp) ───────────────────────────────────────────────
struct PbW {
    std::vector<uint8_t> b;
    void varint(uint64_t v) { while (v >= 0x80) { b.push_back(uint8_t(v | 0x80)); v >>= 7; } b.push_back(uint8_t(v)); }
    void key(int f, int wt) { varint(uint64_t(f) << 3 | uint64_t(wt)); }
    void u(int f, uint64_t v) { key(f, 0); varint(v); }
    void f32(int f, uint32_t v) { key(f, 5); for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void bytes(int f, const std::vector<uint8_t>& d) { key(f, 2); varint(d.size()); b.insert(b.end(), d.begin(), d.end()); }
    void str(int f, const std::string& s) { bytes(f, std::vector<uint8_t>(s.begin(), s.end())); }
};
static std::vector<uint8_t> mtPacket(uint32_t from, uint32_t id, int port, const std::vector<uint8_t>& body) {
    PbW d; d.u(1, uint64_t(port)); d.bytes(2, body);
    const uint8_t* k = meshtastic::defaultKey();
    const uint32_t to = 0xFFFFFFFF;
    std::vector<uint8_t> p(16);
    for (int i = 0; i < 4; ++i) { p[size_t(i)] = uint8_t(to >> (8 * i)); p[size_t(4 + i)] = uint8_t(from >> (8 * i)); p[size_t(8 + i)] = uint8_t(id >> (8 * i)); }
    p[12] = uint8_t(1 | 3 << 5);   // hop limit 1 of 3: 2 hops
    p[13] = uint8_t(meshtastic::channelHash("LongFast", k, 16));
    uint8_t nonce[16] = {0};
    for (int i = 0; i < 4; ++i) { nonce[i] = uint8_t(id >> (8 * i)); nonce[8 + i] = uint8_t(from >> (8 * i)); }
    std::vector<uint8_t> ct(d.b.size());
    crypto::aesCtr(k, nonce, d.b.data(), ct.data(), ct.size());
    p.insert(p.end(), ct.begin(), ct.end());
    return p;
}
static std::vector<uint8_t> mtPosition(uint32_t from, uint32_t id, double lat, double lon) {
    PbW pos; pos.f32(1, uint32_t(int32_t(std::lround(lat * 1e7)))); pos.f32(2, uint32_t(int32_t(std::lround(lon * 1e7)))); pos.u(3, 90);
    return mtPacket(from, id, 3, pos.b);
}
static std::vector<uint8_t> mtUser(uint32_t from, uint32_t id, const std::string& name, int role) {
    char nid[12]; std::snprintf(nid, sizeof nid, "!%08x", from);
    PbW u; u.str(1, nid); u.str(2, name); u.str(3, name.substr(0, 4)); u.u(5, 43); u.u(7, uint64_t(role));
    return mtPacket(from, id, 4, u.b);
}
/** A MeshCore advert from key `seed` (its first 4 bytes are the node's id). kind: 1 chat, 2 repeater, 3 room, 4 sensor. */
static std::vector<uint8_t> mcAdvert(int seed, const std::string& name, double lat, double lon, int kind, int hops = 3) {
    std::vector<uint8_t> p = {uint8_t(0x04 << 2 | 0x01), uint8_t(hops)};
    for (int i = 0; i < hops; ++i) p.push_back(uint8_t(0xB0 + i));
    for (int i = 0; i < 32; ++i) p.push_back(i < 4 ? uint8_t(uint32_t(seed) >> (24 - 8 * i)) : uint8_t(seed * 7 + i));   // public key (its first 4 bytes = the id)
    for (int i = 0; i < 4; ++i) p.push_back(uint8_t(0x11 * i));              // timestamp
    for (int i = 0; i < 64; ++i) p.push_back(uint8_t(i));                    // signature (not checked)
    p.push_back(uint8_t(0x80 | 0x10 | kind));
    const int32_t la = int32_t(std::lround(lat * 1e6)), lo = int32_t(std::lround(lon * 1e6));
    for (int i = 0; i < 4; ++i) p.push_back(uint8_t(uint32_t(la) >> (8 * i)));
    for (int i = 0; i < 4; ++i) p.push_back(uint8_t(uint32_t(lo) >> (8 * i)));
    p.insert(p.end(), name.begin(), name.end());
    return p;
}
static Msg mc(int seed, const std::string& name, double lat, double lon, int kind, int hops = 3, float snr = 6.5f) {
    Msg m; meshcore::parse(mcAdvert(seed, name, lat, lon, kind, hops), m); m.freqHz = 869.618e6; m.snrDb = snr; m.sf = 8; m.bw = 62.5e3; return m;
}
static Msg mt(const std::vector<uint8_t>& b) { Msg m; meshtastic::parse(b, m); m.freqHz = 869.525e6; m.snrDb = 2.5f; m.sf = 11; m.bw = 250e3; return m; }

static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/loranodes-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static std::string index(const std::string& dir) { FILE* f = std::fopen((dir + "/index.jsonl").c_str(), "r"); std::string s; if (!f) return s; char b[4096]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n); std::fclose(f); return s; }

/** A captured Sink: what the service would hand the gallery. */
struct FixSink : vibeism::Sink {
    std::vector<vibeism::NodeFix> fixes;
    void heard(const vibeism::Heard&) override {}
    void node(const vibeism::NodeFix& f) override { fixes.push_back(f); }
};
static void feed(MeshTable& t, const Msg& m, double at, vibeism::Sink* s) {
    std::vector<Event> ev; std::vector<Fix> fx;
    t.accept(m, at, &ev, &fx);
    for (auto& f : fx) {
        vibeism::NodeFix n; n.net = f.net; n.type = f.type; n.name = f.name; n.node = f.node; n.lat = f.lat; n.lon = f.lon; n.approx = f.approx;
        n.hops = f.hops; n.snr = f.snr; n.freqHz = f.freqHz; s->node(n);
    }
}

/** The web test's fixture: a day of nodes heard, some in the background, some live. */
static int seedGallery(const std::string& dir) {
    vibe::gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
    int64_t clk = vibe::gallery::nowUtc() - 3600;
    auto opts = [&](const char* origin) {
        vibe::ismgal::DayLog::Options o; o.bandId = "868"; o.bandName = "868 MHz SRD"; o.bandHz = 868.6e6; o.origin = origin;
        o.profile = std::string(origin) == "background" ? "Profile 1" : ""; o.clock = [&clk] { return clk; }; o.rxLat = 52.2405; o.rxLon = -0.9027;
        return o;
    };
    struct N { int seed; const char* name; double lat, lon; int kind, hops; };
    const N bg[] = {{1, "Benson", 51.6223, -1.1028, 2, 7}, {2, "M0SWM AL3 7", 51.7990, -0.3967, 2, 8}, {3, "MeshCore Repeater", 52.2060, -0.9080, 2, 3},
                    {4, "Moulton Rptr", 52.2900, -0.8400, 2, 1}, {5, "Daventry Room", 52.2570, -1.1610, 3, 2}, {6, "Brixworth WX", 52.3330, -0.9050, 4, 2},
                    {7, "Alice T-Deck", 52.24137, -0.89211, 1, 1}, {8, "Kettering Hill", 52.3990, -0.7260, 2, 4}};
    {
        vibe::ismgal::DayLog lg(st, opts("background"));
        MeshTable t;
        for (int r = 0; r < 3; ++r) for (const N& n : bg) { feed(t, mc(n.seed, n.name, n.lat, n.lon, n.kind, n.hops, 4.0f + n.seed), double(clk), &lg); clk += 37; }
        lg.finish();
    }
    clk += 120;
    {
        vibe::ismgal::DayLog lg(st, opts("live"));
        MeshTable t;
        feed(t, mc(9, "Wellingborough Rptr", 52.3020, -0.6940, 2, 2), double(clk), &lg);
        feed(t, mt(mtUser(0x1a2b3c4d, 0x2001, "Northampton Base", 2)), double(clk), &lg);
        feed(t, mt(mtPosition(0x1a2b3c4d, 0x2002, 52.2400, -0.8950)), double(clk), &lg);
        feed(t, mt(mtPosition(0x55667788, 0x2003, 52.21871, -0.94420)), double(clk), &lg);   // a handheld: no NodeInfo heard → a person
        feed(t, mc(1, "Benson", 51.6223, -1.1028, 2, 7), double(clk), &lg);                   // heard live too → "both"
        lg.finish();
    }
    std::printf("seeded %s\n", dir.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--seed-gallery") == 0) return seedGallery(argv[2]);
    std::printf("test-lora-nodes — the ISM map's nodes: parsing, the table, privacy, the gallery's card\n");

    std::printf("── positions parsed ──\n");
    {
        const Msg r = mc(1, "Benson", 51.6223, -1.1028, 2, 7);
        ok(r.what == "advert" && r.longName == "Benson" && r.role == "repeater" && std::fabs(r.lat - 51.6223) < 1e-6 && std::fabs(r.lon + 1.1028) < 1e-6 && r.pathLen == 7,
           "MeshCore repeater advert: name, type, 51.6223 -1.1028, 7 hops");
        ok(mc(2, "Room", 52.1, -0.5, 3).role == "room server" && mc(3, "WX", 52.1, -0.5, 4).role == "sensor" && mc(4, "Me", 52.1, -0.5, 1).role == "chat node",
           "MeshCore room server / sensor / chat node kinds");
        const Msg z = mc(5, "Nowhere", 0, 0, 2);
        ok(z.what == "advert" && !(z.lat == z.lat), "a 0,0 advert has no position (no fix, never Null Island)");
        const Msg p = mt(mtPosition(0x1a2b3c4d, 0x10, 52.2400, -0.8950));
        ok(p.what == "position" && std::fabs(p.lat - 52.24) < 1e-6 && std::fabs(p.lon + 0.895) < 1e-6 && p.node == "!1a2b3c4d", "Meshtastic POSITION on the default channel");
        const Msg u = mt(mtUser(0x1a2b3c4d, 0x11, "Northampton Base", 2));
        ok(u.what == "nodeinfo" && u.longName == "Northampton Base" && u.role == "router", "Meshtastic NODEINFO: name and role (router)");
        ok(std::string(nodeType("meshcore", "repeater")) == "repeater" && std::string(nodeType("meshcore", "room server")) == "room"
           && std::string(nodeType("meshcore", "chat node")) == "companion" && std::string(nodeType("meshtastic", "router (late)")) == "repeater"
           && std::string(nodeType("meshtastic", "client base")) == "companion" && std::string(nodeType("meshtastic", "")) == "companion"
           && std::string(nodeType("meshtastic", "tracker")) == "companion",
           "node types: repeaters and rooms are infrastructure; a client base, a tracker and an unknown role are people");
    }

    std::printf("── the PRIVACY rule on what leaves the table ──\n");
    {
        MeshTable t; FixSink s;
        feed(t, mc(1, "Benson", 51.6223, -1.1028, 2, 7), 0, &s);
        feed(t, mc(7, "Alice T-Deck", 52.24137, -0.89211, 1, 1), 1, &s);
        feed(t, mt(mtPosition(0x55667788, 0x20, 52.21871, -0.94420)), 2, &s);
        std::string all; for (auto& r : t.rowsJson(10)) all += r.second + "\n";
        ok(all.find("\"lat\":51.622300") != std::string::npos && all.find("\"lon\":-1.102800") != std::string::npos, "a repeater: its advertised position, exactly");
        ok(all.find("\"lat\":52.24,\"lon\":-0.89,\"approx\":1") != std::string::npos, "a companion: rounded to 52.24, -0.89 and marked approx");
        ok(all.find("52.2413") == std::string::npos && all.find("0.8921") == std::string::npos, "…and its exact figures appear nowhere in the JSON (nor the line)");
        ok(all.find("\"lat\":52.22,\"lon\":-0.94,\"approx\":1") != std::string::npos && all.find("52.2187") == std::string::npos,
           "a Meshtastic node whose role is not known yet is a person: rounded");
        ok(all.find("52.24, -0.89 approx.") != std::string::npos, "the list's line says the rounded position and \"approx.\"");
        ok(all.find("\"ntype\":\"repeater\"") != std::string::npos && all.find("\"ntype\":\"companion\"") != std::string::npos, "the type travels as ntype");
        ok(s.fixes.size() == 3, "three fixes for the gallery: " + std::to_string(s.fixes.size()));
        bool fxOk = s.fixes.size() == 3 && !s.fixes[0].approx && s.fixes[1].approx && std::fabs(s.fixes[1].lat - 52.24) < 1e-9 && s.fixes[2].approx;
        ok(fxOk, "the gallery's fixes carry the published positions (repeater exact, people rounded)");
        // a NODEINFO saying "router" later promotes the Meshtastic node to infrastructure — its advertised position
        feed(t, mt(mtUser(0x55667788, 0x21, "Hilltop Router", 2)), 3, &s);
        all.clear(); for (auto& r : t.rowsJson(10)) all += r.second;
        ok(all.find("\"lat\":52.218710") != std::string::npos, "a role heard later (router) publishes the advertised position");
        // a public message from a positioned node: its text is in the list, never in a fix
        Msg tx; tx.proto = "meshtastic"; tx.node = "!55667788"; tx.what = "text"; tx.text = "SECRET-ISH CHAT"; tx.channel = "LongFast"; tx.freqHz = 869.525e6;
        FixSink s2; feed(t, tx, 4, &s2);
        ok(s2.fixes.size() == 1 && s2.fixes[0].name == "Hilltop Router", "a message from a positioned node counts as a hearing (the node's fix) …");
        ok(sizeof(vibeism::NodeFix) > 0 && s2.fixes[0].name.find("SECRET") == std::string::npos, "… and no field of a fix carries its text");
    }

    std::printf("── the node table: cap, 24 h expiry, newest wins ──\n");
    {
        MeshTable t;
        for (int i = 0; i < 450; ++i) t.accept(mc(1000 + i, "Node " + std::to_string(i), 52.0 + i * 1e-3, -1.0, 2), double(i));
        ok(t.size() == MeshTable::kMaxRows, "450 nodes → the table holds " + std::to_string(t.size()) + " (kMaxRows 400)");
        std::string all; for (auto& r : t.rowsJson(500)) all += r.second;
        ok(all.find("\"long\":\"Node 0\"") == std::string::npos && all.find("\"long\":\"Node 49\"") == std::string::npos
           && all.find("\"long\":\"Node 50\"") != std::string::npos && all.find("\"long\":\"Node 449\"") != std::string::npos,
           "the 50 least recently heard went; the newest stayed");
        MeshTable e;
        e.accept(mc(1, "Benson", 51.6223, -1.1028, 2), 0);
        e.accept(mc(2, "Other", 51.7, -1.0, 2), 23 * 3600.0);
        ok(e.size() == 2, "a node heard 23 h ago is still on the map (MeshCore repeaters advertise every 12 h)");
        e.accept(mc(3, "Third", 51.8, -1.0, 2), 24 * 3600.0 + 5);
        ok(e.size() == 2 && e.rowsJson(24 * 3600.0 + 5).size() == 2, "past 24 h it expires");
        std::string ej; for (auto& r : e.rowsJson(24 * 3600.0 + 5)) ej += r.second;
        ok(ej.find("Benson") == std::string::npos, "…Benson was the one that went");
        MeshTable n;
        n.accept(mc(1, "Benson", 51.6223, -1.1028, 2, 7), 0);
        n.accept(mc(1, "Benson Hill", 51.6300, -1.1100, 2, 4), 60);
        std::string nj; for (auto& r : n.rowsJson(60)) nj += r.second;
        ok(n.size() == 1 && nj.find("\"long\":\"Benson Hill\"") != std::string::npos && nj.find("\"lat\":51.630000") != std::string::npos
           && nj.find("\"hops\":4") != std::string::npos && nj.find("\"count\":2") != std::string::npos,
           "the same node again: one row, the newest name, position and hops, heard 2 times");
    }

    std::printf("── the gallery's \"stations received\" card ──\n");
    {
        const std::string dir = tmpDir("card");
        vibe::gallery::Store st; st.setDir(dir); st.setLimitBytes(50ull << 20);
        const int64_t T0 = 20393LL * 86400 + 9 * 3600;
        int64_t clk = T0;
        auto opts = [&](const char* origin) {
            vibe::ismgal::DayLog::Options o; o.bandId = "868"; o.bandName = "868 MHz SRD"; o.bandHz = 868.6e6; o.origin = origin;
            o.profile = std::string(origin) == "background" ? "Profile 1" : ""; o.clock = [&clk] { return clk; }; o.rxLat = 52.2405; o.rxLon = -0.9027;
            return o;
        };
        {
            auto lg = std::make_shared<vibe::ismgal::DayLog>(st, opts("background"));
            MeshTable t;
            feed(t, mc(1, "Benson", 51.6223, -1.1028, 2, 7), double(clk), lg.get());
            feed(t, mc(7, "Alice T-Deck", 52.24137, -0.89211, 1, 1), double(clk), lg.get());
            for (int i = 0; i < 20; ++i) { clk += 1; feed(t, mc(1, "Benson", 51.6223, -1.1028, 2, 7), double(clk), lg.get()); }
            lg->tick();
            ok(lg->nodesName().empty() && index(dir).empty(), "22 packets in 20 s: nothing written yet (never a write per packet)");
            clk += 60; lg->tick();
            ok(!lg->nodesName().empty(), "after a minute: the card is written (" + lg->nodesName() + ")");
            const std::string body = lg->nodesText();
            ok(body.find("21\xc3\x97 \xc2\xb7 background \xc2\xb7 MeshCore \xc2\xb7 repeater \xc2\xb7 Benson \xc2\xb7 ") != std::string::npos
               && body.find("51.622300, -1.102800") != std::string::npos, "Benson: heard 21×, in the background, at its advertised position");
            ok(body.find("52.24, -0.89 approx") != std::string::npos && body.find("52.2413") == std::string::npos, "Alice (a companion): rounded and approx");
            ok(lg->nodesTitleNow().find("LoRa / MeshCore 868 MHz") == 0 && lg->nodesTitleNow().find("2 nodes heard") != std::string::npos
               && lg->nodesTitleNow().find("1 repeater") != std::string::npos && lg->nodesTitleNow().find("furthest 7") != std::string::npos,
               "the title: " + lg->nodesTitleNow());
            ok(index(dir).find("\"s\":\"ismnodes-868-") != std::string::npos, "indexed with the slot the clients draw MAP for (ismnodes-)");
            lg->finish();
        }
        // a listener arrives: a new DayLog (live) adopts the day's card and carries on
        clk += 300;
        {
            auto lg = std::make_shared<vibe::ismgal::DayLog>(st, opts("live"));
            ok(lg->nodes() == 2, "a listener arrives: the day's 2 nodes are read back from the card");
            MeshTable t;
            feed(t, mc(1, "Benson", 51.6223, -1.1028, 2, 7), double(clk), lg.get());
            feed(t, mc(9, "Wellingborough Rptr", 52.3020, -0.6940, 2, 2), double(clk), lg.get());
            lg->finish();
            const std::string body = lg->nodesText();
            ok(body.find("22\xc3\x97 \xc2\xb7 both \xc2\xb7 MeshCore \xc2\xb7 repeater \xc2\xb7 Benson") != std::string::npos, "Benson: 22×, heard both in the background and live");
            ok(body.find("live \xc2\xb7 MeshCore \xc2\xb7 repeater \xc2\xb7 Wellingborough Rptr") != std::string::npos, "a new node heard live is marked live");
            ok(body.find("Heard by background decoding and listeners") != std::string::npos, "the card says both");
            ok(countOf(index(dir), "ismnodes-868-") >= 1 && lg->nodes() == 3, "one card, three nodes");
        }
        // a restart: the companion stays rounded even if a file said otherwise; cap
        {
            auto lg = std::make_shared<vibe::ismgal::DayLog>(st, opts("background"));
            ok(lg->nodes() == 3 && lg->nodesText().find("52.24, -0.89 approx") != std::string::npos, "after a restart: 3 nodes, the companion still approx");
            MeshTable t;
            for (int i = 0; i < 520; ++i) { clk += 1; feed(t, mc(2000 + i, "N" + std::to_string(i), 52.0 + i * 1e-3, -1.0, 2), double(clk), lg.get()); }
            ok(lg->nodes() == vibe::ismgal::DayLog::kMaxNodes, "523 nodes heard → the card keeps " + std::to_string(lg->nodes()) + " (kMaxNodes)");
            lg->finish();
            ok(lg->nodesText().find("Not listed:   23") != std::string::npos, "…and says how many it dropped");
        }
    }

    std::printf("%d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
