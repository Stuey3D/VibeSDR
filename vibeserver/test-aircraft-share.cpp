// test-aircraft-share.cpp — V12 2026-11-16 THE SHARED AIRCRAFT TABLE (vibe_aircraft_share.h): one row per aircraft from every
// radio on the machine — ADS-B, ACARS, VDL2, HFDL, ADS-C — merged by ICAO address, else registration, else flight ID; the
// sources each row was heard by; bounded (kCap rows, a fixed file); in memory without a directory; several PROCESSES writing
// at once under the lock; the datalink table's provider (vibe_avi_service.h Tracker::shareSince); the JSON the map reads.
// docs/v12/AVIATION.md §The shared table.
#include "vibe_avi_service.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace vibeavshare;

static Update adsb(uint32_t icao, const char* call, const char* reg, int64_t ms, double lat = NAN, double lon = NAN) {
    Update u; u.icao = icao; u.flight = call; u.reg = reg ? reg : ""; u.srcMask = 1 << kAdsb; u.srcMs[kAdsb] = ms; u.heardMs = ms; u.count = 3;
    if (lat == lat) { u.hasPos = true; u.lat = lat; u.lon = lon; u.posMs = ms; u.posSrc = kAdsb; u.hasAlt = true; u.altFt = 36000; u.hasTrk = true; u.trk = 90; u.hasGs = true; u.gs = 450; }
    return u;
}
static Update dl(int src, uint32_t icao, const char* reg, const char* flight, int64_t ms) {
    Update u; u.icao = icao; u.reg = reg ? reg : ""; u.flight = flight ? flight : ""; u.srcMask = uint8_t(1 << src); u.srcMs[src] = ms; u.heardMs = ms; u.count = 1;
    u.label = "H1"; u.via = src == kVdl2 ? "VDL2 136.975" : src == kAcars ? "ACARS 131.725" : "HFDL 8977 Reykjavik";
    return u;
}
static const Row* find(const std::vector<Row>& v, uint32_t icao) { for (const auto& r : v) if (r.icao == icao) return &r; return nullptr; }
static const Row* findReg(const std::vector<Row>& v, const char* reg) { for (const auto& r : v) if (regKey(r.reg) == regKey(reg)) return &r; return nullptr; }

static std::string tmpDir() {
    char t[] = "/tmp/avshareXXXXXX";
    const char* d = mkdtemp(t);
    return d ? d : "";
}

int main(int argc, char** argv) {
    // ★ the multi-process writer (§5) re-runs this binary as a child: `--child dir n src`
    if (argc == 5 && !std::strcmp(argv[1], "--child")) {
        Table t; t.setDir(argv[2]);
        const int n = std::atoi(argv[3]), src = std::atoi(argv[4]);
        const int64_t base = wallMs();
        for (int round = 0; round < 20; ++round) {
            std::vector<Update> ups;
            for (int i = 0; i < n; ++i) {
                Update u = src == kAdsb ? adsb(0x400000u + (uint32_t)i, "", "", base + round, 50 + i * 0.01, -1 - i * 0.01)
                                        : dl(src, 0x400000u + (uint32_t)i, "", "", base + round);
                ups.push_back(u);
            }
            t.write(ups, {}, base + round);
        }
        return 0;
    }
    std::printf("test-aircraft-share\n");
    const int64_t T = 1800000000000LL;   // a fixed wall clock (ms)

    // ── 1. merge by ICAO address: ADS-B (address + database registration) and VDL2 (address + ACARS registration) ──
    {
        std::printf("── 1. ADS-B and VDL2 on one aircraft (address) ──\n");
        std::vector<Row> rows(kCap);
        mergeInto(rows, { adsb(0x4CA97B, "RYR12AB", "EI-EST", T, 53.3, -2.2) }, T);
        mergeInto(rows, { dl(kVdl2, 0x4CA97B, "EI-EST", "FR1234", T + 5000) }, T + 5000);
        int used = 0; for (auto& r : rows) used += r.used;
        const Row* r = find(rows, 0x4CA97B);
        CHECK(used == 1 && r, "one row (got %d)", used);
        CHECK(r && r->srcMask == ((1 << kAdsb) | (1 << kVdl2)), "both sources: mask %d", r ? r->srcMask : -1);
        CHECK(r && r->reg == "EI-EST" && r->flight == "FR1234", "the newest flight (VDL2's) and the registration: %s %s", r ? r->reg.c_str() : "", r ? r->flight.c_str() : "");
        CHECK(r && (r->flags & 1) && r->posSrc == kAdsb && std::fabs(r->lat - 53.3) < 1e-6 && r->altFt == 36000, "ADS-B's position kept (VDL2 had none)");
        CHECK(r && r->srcMs[kAdsb] == T && r->srcMs[kVdl2] == T + 5000 && r->lastMs == T + 5000, "when each source heard it");
        CHECK(r && r->label == "H1" && r->via == "VDL2 136.975" && r->count == 4, "label, via, count %u", r ? r->count : 0);
    }
    // ── 2. ACARS first (registration only), then ADS-B (address + registration): one row, adopted ──
    {
        std::printf("── 2. ACARS (registration) then ADS-B (address + registration) ──\n");
        std::vector<Row> rows(kCap);
        mergeInto(rows, { dl(kAcars, 0, ".G-EZWK", "U22416", T) }, T);
        mergeInto(rows, { adsb(0x406B90, "EZY45KL", "G-EZWK", T + 1000, 51.5, -0.4) }, T + 1000);
        int used = 0; for (auto& r : rows) used += r.used;
        const Row* r = find(rows, 0x406B90);
        CHECK(used == 1 && r && r->srcMask == ((1 << kAdsb) | (1 << kAcars)), "one row with both: used %d mask %d", used, r ? r->srcMask : -1);
        CHECK(r && regKey(r->reg) == "GEZWK", "the registration's key ignores dots and hyphens (%s)", r ? r->reg.c_str() : "");
    }
    // ── 3. two rows that turn out to be one: HFDL logon (address) + an ACARS-only row, joined by a VDL2 frame with both ──
    {
        std::printf("── 3. two rows absorbed into one ──\n");
        std::vector<Row> rows(kCap);
        mergeInto(rows, { dl(kAcars, 0, "D-AIJH", "LH400", T) }, T);
        Update h = dl(kHfdl, 0x3C6548, "", "DLH400", T + 1000);
        h.hasPos = true; h.lat = 60.1; h.lon = -20.5; h.posMs = T + 1000; h.posSrc = kAdsc; h.srcMask |= 1 << kAdsc; h.srcMs[kAdsc] = T + 1000;
        mergeInto(rows, { h }, T + 1000);
        int used = 0; for (auto& r : rows) used += r.used;
        CHECK(used == 2, "two rows before the join (got %d)", used);
        mergeInto(rows, { dl(kVdl2, 0x3C6548, "D-AIJH", "", T + 2000) }, T + 2000);
        used = 0; for (auto& r : rows) used += r.used;
        const Row* r = find(rows, 0x3C6548);
        CHECK(used == 1, "one row after the join (got %d)", used);
        CHECK(r && r->srcMask == ((1 << kAcars) | (1 << kHfdl) | (1 << kVdl2) | (1 << kAdsc)), "ACARS + HFDL + VDL2 + ADS-C: mask %d", r ? r->srcMask : -1);
        CHECK(r && (r->flags & 1) && r->posSrc == kAdsc && std::fabs(r->lat - 60.1) < 1e-6, "the ADS-C position survives the join");
        CHECK(r && r->reg == "D-AIJH", "the registration from the ACARS row");
    }
    // ── 4. never across two addresses: same flight / registration, different ICAO = two rows ──
    {
        std::printf("── 4. no merge across two addresses ──\n");
        std::vector<Row> rows(kCap);
        mergeInto(rows, { dl(kHfdl, 0x111111, "", "ABC123", T), dl(kHfdl, 0x222222, "", "ABC123", T + 1) }, T + 1);
        mergeInto(rows, { adsb(0x333333, "X", "N123AB", T), adsb(0x444444, "Y", "N123AB", T + 1) }, T + 1);
        int used = 0; for (auto& r : rows) used += r.used;
        CHECK(used == 4, "four rows (got %d)", used);
        // flight alone joins a flight-only row
        std::vector<Row> r2(kCap);
        mergeInto(r2, { dl(kHfdl, 0, "", "BAW117", T) }, T);
        mergeInto(r2, { dl(kHfdl, 0x400F11, "", "BAW117", T + 9) }, T + 9);
        used = 0; for (auto& r : r2) used += r.used;
        CHECK(used == 1 && find(r2, 0x400F11), "a flight-only row adopts the logon's address (rows %d)", used);
    }
    // ── 5. the newest position wins, with its source ──
    {
        std::printf("── 5. the newest position ──\n");
        std::vector<Row> rows(kCap);
        mergeInto(rows, { adsb(0x4CA000, "A", "", T, 52.0, 1.0) }, T);
        Update late = dl(kVdl2, 0x4CA000, "", "", T + 60000); late.hasPos = true; late.lat = 54.0; late.lon = 2.0; late.posMs = T + 60000; late.posSrc = kVdl2; late.posTime = "21:20:00";
        mergeInto(rows, { late }, T + 60000);
        const Row* r = find(rows, 0x4CA000);
        CHECK(r && r->posSrc == kVdl2 && std::fabs(r->lat - 54.0) < 1e-6 && r->posTime == "21:20:00" && !(r->flags & 4), "a later datalink fix replaces ADS-B's (no track carried over)");
        Update old = adsb(0x4CA000, "A", "", T + 30000, 50.0, 0.0);
        mergeInto(rows, { old }, T + 60001);
        r = find(rows, 0x4CA000);
        CHECK(r && std::fabs(r->lat - 54.0) < 1e-6, "an OLDER fix arriving later does not");
    }
    // ── 6. bounded: expiry after an hour, eviction of the longest unheard when full ──
    {
        std::printf("── 6. bounded ──\n");
        std::vector<Row> rows(kCap);
        std::vector<Update> ups;
        for (uint32_t i = 0; i < 1500; ++i) ups.push_back(adsb(0x500000 + i, "", "", T + i));
        mergeInto(rows, ups, T + 1500);
        int used = 0; for (auto& r : rows) used += r.used;
        CHECK(used == (int)kCap, "1500 aircraft → %d rows (the cap)", used);
        CHECK(!find(rows, 0x500000) && find(rows, 0x500000 + 1499), "the longest unheard gave its slot up");
        mergeInto(rows, {}, T + 1500 + (kKeepSec + 1) * 1000);
        used = 0; for (auto& r : rows) used += r.used;
        CHECK(used == 0, "an hour unheard frees every slot (left %d)", used);
    }
    // ── 7. the file: exact size, read back, corrupt → re-made; memory without a directory ──
    {
        std::printf("── 7. the file and the memory table ──\n");
        const std::string d = tmpDir();
        Table t; t.setDir(d);
        t.write({ adsb(0x4CA97B, "RYR12AB", "EI-EST", T, 53.3, -2.2) }, { { 3, T, 8977 } }, T);
        t.write({ dl(kVdl2, 0x4CA97B, "EI-EST", "FR1234", T + 1000) }, {}, T + 1000);
        struct stat st{};
        CHECK(::stat((d + "/" + kFile).c_str(), &st) == 0 && (uint64_t)st.st_size == kFileBytes, "the file is exactly %llu bytes (got %lld)",
              (unsigned long long)kFileBytes, (long long)st.st_size);
        std::vector<GsHeard> gs;
        auto v = t.read(T + 2000, 0, &gs);
        CHECK(v.size() == 1 && v[0].srcMask == ((1 << kAdsb) | (1 << kVdl2)), "read back: one row, both sources");
        CHECK(gs.size() == 1 && gs[0].id == 3 && gs[0].khz == 8977, "the ground station heard");
        CHECK(t.read(T + 2000, T + 1500).empty(), "?since leaves out what was heard before it");
        { FILE* f = std::fopen((d + "/" + kFile).c_str(), "r+b"); if (f) { std::fwrite("garbage!", 1, 8, f); std::fclose(f); } }
        CHECK(t.read(T + 2000, 0).empty(), "a corrupt file reads as empty");
        CHECK(::stat((d + "/" + kFile).c_str(), &st) == 0 && (uint64_t)st.st_size == kFileBytes, "…and is re-made at its size");
        t.reset(); ::rmdir(d.c_str());
        Table m;
        m.write({ adsb(0x4CA97B, "A", "", T, 1, 1) }, {}, T);
        CHECK(m.read(T + 1, 0).size() == 1, "no directory: the same table in memory");
    }
    // ── 8. four PROCESSES writing at once (two ADS-B radios, VDL2, HFDL — 200 aircraft each), under the lock ──
    {
        std::printf("── 8. four processes at once ──\n");
        const std::string d = tmpDir();
        const int srcs[4] = { kAdsb, kVdl2, kHfdl, kAcars };
        pid_t kids[4];
        for (int k = 0; k < 4; ++k) {
            kids[k] = fork();
            if (kids[k] == 0) { char n[8], s[8]; std::snprintf(n, sizeof n, "200"); std::snprintf(s, sizeof s, "%d", srcs[k]);
                                execl(argv[0], argv[0], "--child", d.c_str(), n, s, (char*)nullptr); _exit(127); }
        }
        int okKids = 0;
        for (int k = 0; k < 4; ++k) { int st = 0; waitpid(kids[k], &st, 0); okKids += WIFEXITED(st) && WEXITSTATUS(st) == 0; }
        Table t; t.setDir(d);
        auto v = t.read(wallMs(), 0);
        int all = 0;
        for (const auto& r : v) all += r.srcMask == ((1 << kAdsb) | (1 << kVdl2) | (1 << kHfdl) | (1 << kAcars));
        CHECK(okKids == 4, "the four writers finished (%d)", okKids);
        CHECK(v.size() == 200 && all == 200, "200 aircraft, every one with all four sources (rows %zu, complete %d)", v.size(), all);
        int counts = 0; for (const auto& r : v) counts += r.count == 20 * 3 + 20 * 3;
        CHECK(counts == 200, "every write counted (ADS-B 60 + 3 × 20 datalink each): %d", counts);
        t.reset(); ::rmdir(d.c_str());
    }
    // ── 9. the datalink table's provider: VDL2 + ACARS + HFDL through vibe_avi_service.h's Tracker ──
    {
        std::printf("── 9. the datalink table → updates ──\n");
        vibeavi::Tracker tr(true);
        const double now = T / 1000.0;
        vibevdl2::Frame f; f.src.type = 1; f.src.addr = 0x4CA97B; f.hz = 136.975e6; f.hasAcars = true; f.acarsCrcOk = true;
        f.acars.reg = "EI-EST"; f.acars.flight = "FR1234"; f.acars.label = "H1"; f.acars.text = "SECRET BODY"; f.acars.downlink = true;
        tr.addVdl2(f, now);
        vibeacars::Block b; b.reg = "G-EZWK"; b.flight = "U22416"; b.label = "QP"; b.text = "EGLLLEMD1015"; b.hz = 131.725e6; b.downlink = true;
        tr.addAcars(b, now + 1);
        vibehfdl::Message m; m.downlink = true; m.dstId = 3; m.icao = 0x406D4D; m.flight = "M38506"; m.lat = 51.0; m.lon = 3.3; m.hz = 8977e3;
        tr.addHfdl(m, now + 2);
        std::vector<Update> ups; std::vector<GsHeard> gs;
        tr.shareSince(0, ups, gs);
        CHECK(ups.size() == 3, "three rows → three updates (%zu)", ups.size());
        bool v = false, a = false, h = false, body = false;
        for (const auto& u : ups) {
            if (u.icao == 0x4CA97B && u.srcMask == (1 << kVdl2) && u.reg == "EI-EST" && u.via == "VDL2 136.975") v = true;
            if (u.reg == "G-EZWK" && u.srcMask == (1 << kAcars)) a = true;
            if (u.icao == 0x406D4D && u.srcMask == (1 << kHfdl) && u.hasPos && u.posSrc == kHfdl) h = true;
            if (u.label.find("SECRET") != std::string::npos || u.app.find("SECRET") != std::string::npos || u.via.find("SECRET") != std::string::npos) body = true;
        }
        CHECK(v && a && h, "VDL2 %d, ACARS %d, HFDL %d", v, a, h);
        CHECK(!body, "★ never a message body (even with bodies on)");
        CHECK(gs.size() == 1 && gs[0].id == 3, "the ground station heard");
        std::vector<Update> again; std::vector<GsHeard> g2;
        tr.shareSince(T + 5000, again, g2);
        CHECK(again.empty() && g2.empty(), "nothing new since: nothing handed over");
    }
    // ── 10. the JSON ──
    {
        std::printf("── 10. the JSON ──\n");
        std::vector<Row> rows(kCap);
        mergeInto(rows, { adsb(0x4CA97B, "RYR12AB", "EI-EST", T, 53.3, -2.2), dl(kVdl2, 0x4CA97B, "EI-EST", "FR1234", T + 1000) }, T + 1000);
        Update q = dl(kAcars, 0, "G-\"QUOTE", "", T); mergeInto(rows, { q }, T + 1000);
        std::vector<Row> used; for (auto& r : rows) if (r.used) used.push_back(r);
        const std::string j = toJson(used, { { 3, T, 8977 } }, T + 2000, [](const std::string& l) { return std::string(vibeavi::labelName(l)); },
            [](int id, std::string& n, double& la, double& lo) { for (const auto& g : vibeavi::groundStations()) if (g.id == id) { n = g.name; la = g.lat; lo = g.lon; return true; } return false; });
        CHECK(j.find("\"key\":\"4CA97B\"") != std::string::npos && j.find("\"src\":[\"ADS-B\",\"VDL2\"]") != std::string::npos, "key and sources: %s", j.substr(0, 400).c_str());
        CHECK(j.find("\"srcAge\":{\"ADS-B\":2,\"VDL2\":1}") != std::string::npos, "the age of each source");
        CHECK(j.find("\"posSrc\":\"ADS-B\"") != std::string::npos && j.find("\"alt\":36000") != std::string::npos && j.find("\"trk\":90") != std::string::npos, "position fields");
        CHECK(j.find("\"labelName\":\"message to/from crew or ops\"") != std::string::npos, "the label's plain name");
        CHECK(j.find("\"name\":\"Reykjavik\"") != std::string::npos && j.find("\"khz\":8977") != std::string::npos, "the station");
        CHECK(j.find("G-\"QUOTE") == std::string::npos && j.find("G-\\\"QUOTE") != std::string::npos, "a quote reaches the JSON escaped, never raw");
        CHECK(j.find("\"key\":\"r:G\\\"QUOTE\"") != std::string::npos || j.find("\"key\":\"r:") != std::string::npos, "a registration-only row keyed r:");
    }
    // ── 11. beta88 ADS-B ONLY WHILE HEARD (Stuart, the Pi 500 in ACARS mode: "there should be 0 then") ──
    {
        std::printf("── 11. ADS-B counted only while heard; the radio's live stamp ──\n");
        const std::string d = tmpDir();
        Table t; t.setDir(d);
        int64_t live = -7;
        t.read(T, 0, nullptr, &live);
        CHECK(live == 0, "a new file: no radio has decoded ADS-B (%lld)", (long long)live);
        // 400F11 ADS-B now; 406B90 ADS-B 5 min ago (still kept: the hour); 4CA97B ADS-B 5 min ago + VDL2 now
        t.write({ adsb(0x406B90, "EZY45KL", "", T - 300000, 52.1, -1.1), adsb(0x4CA97B, "RYR12AB", "EI-EST", T - 300000, 53.3, -2.2) }, {}, T - 300000);
        t.write({ adsb(0x400F11, "BAW117", "", T, 52.4, -1.6), dl(kVdl2, 0x4CA97B, "EI-EST", "FR1234", T) }, {}, T, true);
        std::vector<GsHeard> gs;
        auto v = t.read(T + 1000, 0, &gs, &live);
        CHECK(v.size() == 3, "the hour keeps all three rows (%zu)", v.size());
        CHECK(live == T && adsbLiveNow(live, T + 1000), "the writer's ADS-B stamp is in the header (%lld)", (long long)live);
        int now = 0; for (const auto& r : v) now += adsbHeardNow(r, live, T + 1000);
        CHECK(now == 1, "ADS-B heard now: only 400F11 (%d)", now);
        std::string j = toJson(v, gs, T + 1000, nullptr, nullptr, live, false);
        CHECK(j.find("\"adsbLive\":1,\"adsbLast\":1,\"adsbNow\":1,\"adsbWin\":60") != std::string::npos, "the JSON's ADS-B state: %s", j.substr(0, 120).c_str());
        // the radio leaves ADS-B (no more stamps): after kAdsbLiveSec no radio is decoding it — the count is 0
        const int64_t later = T + (kAdsbLiveSec + 2) * 1000;
        t.write({ dl(kVdl2, 0x4CA97B, "EI-EST", "FR1234", later) }, {}, later, false);
        v = t.read(later, 0, &gs, &live);
        CHECK(live == T && !adsbLiveNow(live, later), "no stamp for %llds: no radio decoding ADS-B", (long long)kAdsbLiveSec + 2);
        now = 0; for (const auto& r : v) now += adsbHeardNow(r, live, later);
        CHECK(now == 0, "…and ADS-B heard now is 0, though 400F11 was heard %llds ago (%d)", (long long)kAdsbLiveSec + 2, now);
        j = toJson(v, gs, later, nullptr, nullptr, live, true);
        CHECK(j.find("\"adsbLive\":0,\"adsbLast\":12,\"adsbNow\":0") != std::string::npos && j.find("\"adsbOff\":1") != std::string::npos, "the JSON: %s", j.substr(0, 140).c_str());
        CHECK(toJson(v, gs, later).find("adsbLive") == std::string::npos, "no state given: no ADS-B fields (as before)");
        // the Sharer stamps the header with no aircraft at all while its ADS-B decoder runs
        CHECK(t.write({}, {}, later + 5000, true) == 0, "a stamp-only write changes no row");
        t.read(later + 5000, 0, nullptr, &live);
        CHECK(live == later + 5000, "…but stamps the header (%lld)", (long long)live);
        t.reset(); ::rmdir(d.c_str());
        Table m; int64_t ml = -1;
        m.write({}, {}, T, true); m.read(T, 0, nullptr, &ml);
        CHECK(ml == T, "the memory table (the app's one radio) carries the stamp too");
    }
    std::printf("test-aircraft-share: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
