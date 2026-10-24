// test-adsb-data — V12 ADS-B aircraft data: our inflater, the builder, the reader, the enrichment the decoder
// service sends, the owner's logo folders (path traversal) and the logo zip installer (zip slip, bombs).
//
// ★ Fixtures: vibeserver/testdata-adsb/ (scripts/make-adsb-test-fixtures.py — tiny archives in the real upstream
//   layouts, carrying the real licence texts). With VIBE_ADSB_RAW=<dir holding standing-data-main.tar.gz and
//   indexedDB.zip> it also builds the REAL downloads and checks known aircraft and callsigns (scripts/build-adsb-
//   data.sh sets it).
#include "vibe_adsb_service.h"
#include "vibe_adsb_data_update.h"
#include "vibe_adsb_images_install.h"
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace vibeadsbdata;

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static const std::string kFix = "vibeserver/testdata-adsb/";
static bool has(const std::string& j, const std::string& frag) { return j.find(frag) != std::string::npos; }

static std::string tmpDir(const char* tag) {
    char b[256]; std::snprintf(b, sizeof b, "%s/vibe-adsb-test-%s-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", tag, int(getpid()));
    rmTree(b);
    ::mkdir(b, 0755);
    return b;
}
static bool exists(const std::string& p) { struct stat sb{}; return ::stat(p.c_str(), &sb) == 0; }

static void testInflate() {
    std::printf("inflate (RFC 1951, our own)\n");
    const std::string plain = slurp(kFix + "raw-plain.bin");
    CHECK(plain.size() > 40000, "fixture missing — run scripts/make-adsb-test-fixtures.py");
    for (int lvl : {0, 1, 6, 9}) {
        const std::string c = slurp(kFix + "raw-deflate-l" + std::to_string(lvl) + ".bin");
        std::string out;
        vibeinflate::Inflater inf;
        const std::string e = inf.run((const uint8_t*)c.data(), c.size(), [&](const uint8_t* b, size_t n) { out.append((const char*)b, n); return true; });
        CHECK(e.empty() && out == plain, "zlib level %d: '%s', %zu of %zu bytes", lvl, e.c_str(), out.size(), plain.size());
    }
    // truncation and garbage are errors, never a crash or an endless loop
    const std::string c = slurp(kFix + "raw-deflate-l9.bin");
    std::string out;
    vibeinflate::Inflater inf;
    CHECK(!inf.run((const uint8_t*)c.data(), c.size() / 2, [&](const uint8_t* b, size_t n) { out.append((const char*)b, n); return true; }).empty(), "a truncated stream must fail");
    std::string junk(4096, '\0'); for (size_t i = 0; i < junk.size(); ++i) junk[i] = char((i * 2654435761u) >> 13);
    vibeinflate::Inflater j2; (void)j2.run((const uint8_t*)junk.data(), junk.size(), [](const uint8_t*, size_t) { return true; });
    CHECK(vibeinflate::crc32(0, (const uint8_t*)"123456789", 9) == 0xCBF43926u, "CRC-32 check value");
    // gzip + tar (the VRS layout, with GitHub's pax global header)
    const std::string tgz = slurp(kFix + "mini-standing-data.tar.gz");
    vibeinflate::TarReader tar;
    std::vector<std::string> names; std::string commit;
    tar.want = [](const std::string&) { return true; };
    tar.file = [&](const std::string& n, const std::string&) { names.push_back(n); };
    tar.global = [&](const std::string& k, const std::string& v) { if (k == "comment") commit = v; };
    const std::string ge = vibeinflate::gunzip((const uint8_t*)tgz.data(), tgz.size(), [&](const uint8_t* b, size_t n) { return tar.feed(b, n); });
    CHECK(ge.empty(), "gunzip: %s", ge.c_str());
    CHECK(commit == "fixture0123456789", "pax global comment = '%s'", commit.c_str());
    CHECK(names.size() >= 10 && names[0] == "standing-data-main/LICENSE", "tar members: %zu, first '%s'", names.size(), names.empty() ? "" : names[0].c_str());
    std::string bad = tgz; bad[bad.size() - 6] ^= 0x55;     // the trailer's CRC
    CHECK(!vibeinflate::gunzip((const uint8_t*)bad.data(), bad.size(), [](const uint8_t*, size_t) { return true; }).empty(), "a bad gzip CRC must fail");
    // zips from three writers: Python deflated, Python stored, Info-ZIP
    for (const char* z : {"images-deflated.zip", "images-stored.zip", "images-infozip.zip"}) {
        const std::string zb = slurp(kFix + z);
        std::vector<vibeinflate::ZipEntry> list;
        const std::string e = vibeinflate::zipList((const uint8_t*)zb.data(), zb.size(), list);
        CHECK(e.empty() && !list.empty(), "%s: %s", z, e.c_str());
        for (const auto& en : list) {
            std::string body;
            const std::string re = vibeinflate::zipRead((const uint8_t*)zb.data(), zb.size(), en, 1 << 20, body);
            CHECK(re.empty() || en.dir(), "%s %s: %s", z, en.name.c_str(), re.c_str());
        }
    }
}

static void testBlocks() {
    std::printf("country blocks (Annex 10, cross-checked against VRS)\n");
    CHECK(std::string(stateFor(0x400000)) == "GB" && std::string(stateFor(0x43FFFF)) == "GB", "UK block");
    CHECK(std::string(stateFor(0xA12345)) == "US", "US");
    CHECK(std::string(stateFor(0x3C6ABC)) == "DE", "Germany");
    CHECK(std::string(stateFor(0x4D2400)) == "MT", "Malta (amended block)");
    CHECK(stateFor(0xF00001) == nullptr, "ICAO's own block has no state");
    int n = 0; const StateBlock* t = stateBlocks(n);
    for (int i = 1; i < n; ++i) CHECK(t[i].first > t[i - 1].last, "blocks sorted, no overlap at %06x", t[i].first);
    // ★★ every civil VRS block of 1 024+ addresses agrees with ours (from the STARTER, which is VRS's table)
    size_t sn = 0; const uint8_t* sp = starterVrs(sn);
    Vad v;
    CHECK(sp && v.open(sp, sn), "the starter compiled in is a valid data file");
    const Vad::Sec b = v.sec(fourcc("BLKS"));
    int compared = 0, disagree = 0;
    for (size_t k = 0; k < b.count(kBlkRec); ++k) {
        const uint8_t* r = b.p + k * kBlkRec;
        const uint32_t f = rd32(r), l = rd32(r + 4);
        const std::string iso = unkey(r + 8, 2);
        if (r[10] || l - f + 1 < 1024 || l - f >= 0x400000 || iso == "ZZ" || iso.empty()) continue;
        // ★ known oddities of the VRS table: blocks that do not end on an Annex 10 boundary (SM, MN), and ICAO's own (XA)
        if (iso == "SM" || iso == "MN" || iso == "XA") continue;
        ++compared;
        const char* a = stateFor(f); const char* z = stateFor(l);
        if (!a || !z || iso != a || iso != z) { ++disagree; std::printf("    VRS %06x-%06x %s, ours %s/%s\n", f, l, iso.c_str(), a ? a : "-", z ? z : "-"); }
    }
    CHECK(compared > 150 && disagree == 0, "compared %d VRS blocks, %d disagree", compared, disagree);
}

static void testCallsigns() {
    std::printf("callsign normalisation (VRS routes README)\n");
    auto iata = [](const std::string& c) { return c == "U2" ? std::string("EZY") : std::string(); };
    auto norm = [&](const char* in) { std::string c, n; return normaliseCallsign(in, c, n, iata) ? c + n : std::string("-"); };
    CHECK(norm("EZY1200") == "EZY1200", "EZY1200");
    CHECK(norm("EZY0001") == "EZY1", "EZY0001 → %s", norm("EZY0001").c_str());
    CHECK(norm("EZY0000") == "EZY0", "EZY0000 → %s", norm("EZY0000").c_str());
    CHECK(norm("EZY00AB") == "EZY0AB", "EZY00AB → %s", norm("EZY00AB").c_str());
    CHECK(norm("U21234") == "EZY1234", "U21234 → %s", norm("U21234").c_str());
    CHECK(norm("BAW12A ") == "BAW12A", "trailing space");
    CHECK(norm("GABCD") == "-", "a registration is no flight");
    CHECK(norm("BAW12345") == "-", "five-digit number refused");
    CHECK(norm("BAW1A2") == "-", "letter before digit refused");
    CHECK(packCallsign("BAW1") < packCallsign("BAW10") && packCallsign("BAW10") < packCallsign("BAW2"), "packed keys sort like strings");
}

static void testBuildAndLookup(const std::string& dir) {
    std::printf("builder + reader on the fixtures\n");
    const std::string tgz = slurp(kFix + "mini-standing-data.tar.gz"), zip = slurp(kFix + "mini-mictronics.zip");
    BuildResult v = buildVrs((const uint8_t*)tgz.data(), tgz.size(), false, {{"retrieved", "2026-10-24"}});
    CHECK(v.error.empty(), "VRS: %s", v.error.c_str());
    CHECK(v.routes == 3 && v.airports == 5 && v.airlines == 5 && v.blocks == 4 && v.overlay == 2, "VRS counts %zu %zu %zu %zu %zu", v.routes, v.airports, v.airlines, v.blocks, v.overlay);
    CHECK(v.version == "fixture0123456789", "commit '%s'", v.version.c_str());
    BuildResult m = buildMic((const uint8_t*)zip.data(), zip.size(), {});
    CHECK(m.error.empty(), "Mictronics: %s", m.error.c_str());
    CHECK(m.aircraft == 1205 && m.operators == 2 && m.types == 3, "Mictronics counts %zu %zu %zu", m.aircraft, m.operators, m.types);
    std::string err;
    CHECK(writeAtomic(dir + "/vrs.vad", v.bytes, err) && writeAtomic(dir + "/mic.vad", m.bytes, err), "write: %s", err.c_str());
    // ★★ a changed licence is REFUSED: flip one byte of the LICENSE inside the archive by rebuilding with a bad pin
    {
        const std::string e = checkLicence("CC0 1.0 Universal but different", kVrsLicenceFnv, kVrsLicencePhrase, "VRS");
        CHECK(!e.empty() && has(e, "CHANGED"), "licence change detected: %s", e.c_str());
        std::string z2 = zip;
        const size_t at = z2.find("Attribution License.");      // the comment, not the deflated LICENSE
        if (at != std::string::npos) z2.replace(at, 11, "Xttribution");
        BuildResult m2 = buildMic((const uint8_t*)z2.data(), z2.size(), {});
        CHECK(!m2.licenceError.empty(), "an archive that no longer says ODC-By is held");
    }
    Db& db = Db::get();
    db.setDir(dir);
    db.refresh(true);
    Info i;
    db.lookupHex(0x4CA2D6, i);
    db.lookupCallsign("RYR8CK", i.r, 0x4CA2D6, i);
    CHECK(i.r == "EI-DLJ" && i.t == "B738" && i.desc == "BOEING 737-800" && i.wtc == "M" && i.eng == "2J" && i.species == "L", "EI-DLJ: %s", i.json().c_str());
    CHECK(i.country == "IE" && i.countryName == "Ireland" && !i.mil, "Ireland: %s", i.json().c_str());
    CHECK(i.op == "RYR" && i.opName == "Ryanair" && i.route.empty(), "Ryanair, no route: %s", i.json().c_str());
    Info b;
    db.lookupHex(0x400F01, b);
    db.lookupCallsign("BAW001", b.r, 0x400F01, b);
    CHECK(b.r == "G-XLEA" && b.t == "A388" && b.country == "GB", "G-XLEA: %s", b.json().c_str());
    CHECK(b.route.size() == 2 && b.route[0].code == "EGLL" && b.route[1].iata == "JFK" && b.route[1].name == "John F Kennedy International Airport",
          "BAW001 → BAW1 EGLL-KJFK: %s", b.json().c_str());
    Db::judge(b, 55.0, -30.0);        // mid-Atlantic, on the great circle
    CHECK(b.plausible == 1, "on the route: %s", b.json().c_str());
    Db::judge(b, -33.9, 151.2);       // Sydney
    CHECK(b.plausible == 0 && has(b.json(), "\"plausible\":false"), "far off the route: %s", b.json().c_str());
    Info c;
    db.lookupHex(0x400100, c);
    db.lookupCallsign("BAW12A", c.r, 0x400100, c);
    CHECK(c.route.size() == 3 && has(c.json(), "\"stops\":[\"EGKK\",\"EHAM\",\"EGSS\"]"), "three-stop route: %s", c.json().c_str());
    Db::judge(c, 52.1, 2.5);          // over the North Sea, EHAM → EGSS
    CHECK(c.leg == 1 && has(c.json(), "\"from\":\"EHAM\",\"to\":\"EGSS\""), "the leg it is on: %s", c.json().c_str());
    Info e;
    db.lookupHex(0x123456, e);
    db.lookupCallsign("U21", e.r, 0x123456, e);     // IATA U2 → EZY1, via the fixture's airlines (U2 is unique)
    CHECK(e.op == "EZY" && e.route.size() == 2 && e.route[0].code == "EHAM", "U21 → EZY1: %s", e.json().c_str());
    // military: Mictronics flag, and VRS's sub-block (UK 43C000–43FFFF), and a territory (Cayman inside the UK)
    Info mil; db.lookupHex(0xAE1234, mil);
    CHECK(mil.mil && mil.country == "US" && mil.r == "03-3123", "C-17: %s", mil.json().c_str());
    Info raf; db.lookupHex(0x43C123, raf);
    CHECK(raf.mil && raf.country == "GB", "RAF block: %s", raf.json().c_str());
    Info ky; db.lookupHex(0x400050, ky);
    CHECK(ky.country == "KY" && ky.countryName == "Cayman Islands" && !ky.mil, "Cayman: %s", ky.json().c_str());
    // ★★ privacy: a private operator's name never comes out — "Private"
    Info pv; db.lookupHex(0x400ABC, pv); db.lookupCallsign("GPRIV", pv.r, 0x400ABC, pv);
    CHECK(pv.opName == "Private" && !has(pv.json(), "Bloggs") && pv.r == "G-PRIV", "private: %s", pv.json().c_str());
    Info co; db.lookupHex(0x400ABD, co); db.lookupCallsign("GCOMP", co.r, 0x400ABD, co);
    CHECK(co.opName == "Acme Charter Ltd" && co.r == "G-COMP", "company operator from the VRS overlay: %s", co.json().c_str());
    // JSON escaping of awkward strings (a quote in a registration)
    Info q; db.lookupHex(0x4CA2D7, q);
    CHECK(has(q.json(), "\"r\":\"EI-\\\\\\\"Q\\\"\""), "escaped: %s", q.json().c_str());
    // the reader survives garbage
    Vad g; std::string ge;
    CHECK(!g.open((const uint8_t*)"VADB\x01\0\0\0", 8, &ge), "short file refused");
    std::string corrupt = v.bytes; std::memset(&corrupt[32 + 4], 0xFF, 4);   // first section's offset → past the end
    CHECK(!g.open((const uint8_t*)corrupt.data(), corrupt.size(), &ge), "a section past the end is refused");
}

static void testService(const std::string& dir) {
    std::printf("enrichment in the decoder service (adsb_info)\n");
    Db::get().setDir(dir); Db::get().refresh(true);
    vibeadsb::Service s;
    std::vector<vibeadsb::Service::Seen> snap = {{0x4CA2D6, false, "", false, 0, 0}, {0x400F01, false, "BAW1", true, 55, -30}, {0x7FFFFF, true, "", false, 0, 0}};
    s.enrich(snap, 1.0);
    const std::string d1 = s.takeInfoDelta();
    CHECK(has(d1, "\"type\":\"adsb_info\"") && has(d1, "\"4ca2d6\":{\"r\":\"EI-DLJ\"") && has(d1, "\"400f01\":") && has(d1, "\"plausible\":true"), "first delta: %s", d1.c_str());
    CHECK(!has(d1, "7fffff"), "an anonymous address is not enriched");
    CHECK(s.takeInfoDelta().empty(), "nothing changed → nothing sent");
    s.enrich(snap, 2.0);
    CHECK(s.takeInfoDelta().empty(), "the same second again sends nothing");
    snap[0].callsign = "RYR8CK";      // a callsign arrives
    s.enrich(snap, 3.0);
    const std::string d2 = s.takeInfoDelta();
    CHECK(has(d2, "\"4ca2d6\":") && has(d2, "\"op\":\"RYR\"") && !has(d2, "400f01"), "only the changed aircraft: %s", d2.c_str());
    const std::string all = s.infoAll();
    CHECK(has(all, "\"full\":1") && has(all, "4ca2d6") && has(all, "400f01"), "a joiner gets everything: %s", all.c_str());
    snap.erase(snap.begin());
    s.enrich(snap, 4.0);
    CHECK(!has(s.infoAll(), "4ca2d6"), "an aircraft that left is forgotten");
}

static void testImages(const std::string& dir) {
    std::printf("owner logos: the index, traversal, the zip installer\n");
    Images& im = Db::images();
    im.setDir(dir);
    CHECK(!Images::validKey("../etc/passwd") && !Images::validKey("a/b") && !Images::validKey("..") && !Images::validKey("%2e%2e") &&
          !Images::validKey("") && !Images::validKey(std::string(41, 'A')) && !Images::validKey("BAW.png") && !Images::validKey("a\\b"),
          "keys with path characters are refused");
    CHECK(Images::validKey("RYR-B738") && Images::validKey("G-ABCD") && Images::validKey("4CA2D6"), "VRS names are keys");
    // install the ordinary pack (deflated) and check what landed where
    const std::string pack = slurp(kFix + "images-deflated.zip");
    ImageInstall r = installImageZip((const uint8_t*)pack.data(), pack.size(), -1, dir);
    CHECK(r.error.empty() && r.logos == 2 && r.silhouettes == 2, "pack: %s", r.summary().c_str());
    CHECK(r.skipped == 2 && has(r.summary(), "not the image their name says"), "skips counted with reasons (the __MACOSX twin not among them): %s", r.summary().c_str());
    std::string path, mime;
    CHECK(im.find(0, "baw", &path, &mime) && mime == "image/png" && path == dir + "/OperatorFlags/BAW.png", "BAW logo (any case): %s", path.c_str());
    CHECK(im.find(0, "RYR-B738", nullptr, &mime) && mime == "image/bmp", "RYR-B738 bmp");
    CHECK(im.find(1, "A388", nullptr, &mime) && mime == "image/svg+xml", "A388 silhouette svg");
    CHECK(!im.find(0, "FAKE") && !exists(dir + "/OperatorFlags/fake.png"), "a fake png is not installed");
    CHECK(!im.find(0, "../OperatorFlags/BAW"), "a traversal key finds nothing");
    // stored + Info-ZIP add to it (the pack ADDS)
    const std::string inz = slurp(kFix + "images-infozip.zip");
    r = installImageZip((const uint8_t*)inz.data(), inz.size(), -1, dir);
    CHECK(r.error.empty() && r.silhouettes == 2 && im.find(1, "C17") && im.find(1, "B738"), "Info-ZIP pack added: %s", r.summary().c_str());
    // a flat zip: asks for a choice, then installs where told
    const std::string flat = slurp(kFix + "images-flat.zip");
    r = installImageZip((const uint8_t*)flat.data(), flat.size(), -1, dir);
    CHECK(r.logos == 0 && r.skipped == 2 && has(r.summary(), "choose which"), "flat without a choice: %s", r.summary().c_str());
    r = installImageZip((const uint8_t*)flat.data(), flat.size(), 0, dir);
    CHECK(r.logos == 2 && im.find(0, "G-ABCD", nullptr, &mime) && mime == "image/gif", "flat as logos: %s", r.summary().c_str());
    // ★★★ zip slip: nothing outside the folders, ever
    const std::string slip = slurp(kFix + "images-slip.zip");
    r = installImageZip((const uint8_t*)slip.data(), slip.size(), 0, dir + "/inner");
    CHECK(r.logos == 1 && r.skipped == 5, "slip: %s", r.summary().c_str());
    CHECK(!exists(dir + "/evil.png") && !exists(dir + "/evil2.png") && !exists("/abs/evil.png") && !exists(dir + "/inner/evil.png") &&
          !exists(dir + "/inner/evil2.png") && exists(dir + "/inner/OperatorFlags/ok.png"),
          "nothing escaped the folder");
    // ★★ a bomb (50 MB behind a directory that claims 1 KB) stops at the stated size; nothing installed
    const std::string bomb = slurp(kFix + "images-bomb.zip");
    r = installImageZip((const uint8_t*)bomb.data(), bomb.size(), 0, dir + "/bomb");
    CHECK(!r.error.empty() && !exists(dir + "/bomb/OperatorFlags/BOMB.png"), "bomb: %s", r.summary().c_str());
    const std::string enc = slurp(kFix + "images-encrypted.zip");
    r = installImageZip((const uint8_t*)enc.data(), enc.size(), 0, dir + "/enc");
    CHECK(r.logos == 0 && has(r.summary(), "encrypted"), "encrypted: %s", r.summary().c_str());
    CHECK(!installImageZip((const uint8_t*)"PK\5\6", 4, 0, dir).error.empty(), "garbage refused");
    // ★★ A REAL VRS PACK IS BIG: Stuart's OperatorFlags.zip from a Mac is 30 398 entries (~15 200 flags + a __MACOSX
    //    twin of each) — the old 20 000 cap refused it whole. Built here, stored: 15 200 flags + 15 200 twins.
    {
        std::string zb, cd; uint16_t count = 0;
        auto u16 = [](std::string& o, uint32_t v) { o += char(v & 255); o += char((v >> 8) & 255); };
        auto u32 = [&](std::string& o, uint32_t v) { u16(o, v & 0xFFFF); u16(o, v >> 16); };
        auto add = [&](const std::string& name, const std::string& body) {
            const uint32_t crc = vibeinflate::crc32(0, (const uint8_t*)body.data(), body.size()), at = uint32_t(zb.size());
            u32(zb, 0x04034b50); u16(zb, 10); u16(zb, 0); u16(zb, 0); u32(zb, 0); u32(zb, crc);
            u32(zb, uint32_t(body.size())); u32(zb, uint32_t(body.size())); u16(zb, uint32_t(name.size())); u16(zb, 0);
            zb += name; zb += body;
            u32(cd, 0x02014b50); u16(cd, 20); u16(cd, 10); u16(cd, 0); u16(cd, 0); u32(cd, 0); u32(cd, crc);
            u32(cd, uint32_t(body.size())); u32(cd, uint32_t(body.size())); u16(cd, uint32_t(name.size()));
            u16(cd, 0); u16(cd, 0); u16(cd, 0); u16(cd, 0); u32(cd, 0); u32(cd, at); cd += name;
            ++count;
        };
        for (int i = 0; i < 15200; ++i) {
            const std::string key = "OP" + std::to_string(i);
            add("OperatorFlags/" + key + ".bmp", std::string("BM") + std::string(30, char(i)));
            add("__MACOSX/OperatorFlags/._" + key + ".bmp", std::string(40, 'x'));
        }
        const uint32_t cdAt = uint32_t(zb.size());
        zb += cd;
        u32(zb, 0x06054b50); u16(zb, 0); u16(zb, 0); u16(zb, count); u16(zb, count);
        u32(zb, uint32_t(cd.size())); u32(zb, cdAt); u16(zb, 0);
        r = installImageZip((const uint8_t*)zb.data(), zb.size(), -1, dir + "/big");
        CHECK(r.error.empty() && r.logos == 15200 && r.skipped == 0, "a 30 400-entry Mac pack installs, its twins unmentioned: %s",
              r.summary().c_str());
    }
    // the enrichment names an installed logo/silhouette, in VRS's order (the index back on the real folder)
    im.setDir(dir);
    Info i; i.r = "EI-DLJ"; i.op = "RYR"; i.t = "B738";
    Db::resolveImages("4CA2D6", i);
    CHECK(i.logo == "RYR-B738" && i.sil == "B738", "VRS order: logo %s sil %s", i.logo.c_str(), i.sil.c_str());
    // ★★ one airframe's livery: operator, type and registration run together, as the pack names it (EWG-A320D-ABDQ)
    if (FILE* f = std::fopen((dir + "/OperatorFlags/RYR-B738EI-DLJ.bmp").c_str(), "wb")) { std::fputs("BM", f); std::fclose(f); }
    im.setDir(dir);
    Db::resolveImages("4CA2D6", i);
    CHECK(i.logo == "RYR-B738EI-DLJ", "the airframe's own livery first: %s", i.logo.c_str());
    CHECK(classifyMember("GeneralAviation/G-CIGH.bmp") == 0, "the GeneralAviation pack installs with the logos (by registration)");
    // Remove all
    std::string err;
    CHECK(removeImages(dir, 0, err) && !im.find(0, "BAW") && im.find(1, "B738"), "remove all logos (silhouettes stay): %s", err.c_str());
}

static void testUpdater(const std::string& dir) {
    std::printf("the weekly update (a fake transport)\n");
    Updater& u = Updater::get();
    u.setDir(dir);
    int calls = 0; bool give304 = false;
    u.setFetcher([&](const std::string& url, const std::string& dest, const std::string& inm, const std::string&) {
        ++calls;
        FetchResult r;
        if (give304 && !inm.empty()) { r.status = 304; return r; }
        const std::string src = url.find("standing") != std::string::npos ? kFix + "mini-standing-data.tar.gz" : kFix + "mini-mictronics.zip";
        std::string err;
        writeAtomic(dest, slurp(src), err);
        r.status = 200; r.etag = "\"e-" + std::to_string(calls) + "\"";
        return r;
    });
    std::string sum = u.runOnce(true);
    CHECK(calls == 2 && exists(dir + "/vrs.vad") && exists(dir + "/mic.vad") && !exists(dir + "/vrs.download"), "first run: %s", sum.c_str());
    State s; s.load(dir + "/state.txt");
    CHECK(s.get("vrs.etag") == "\"e-1\"" && s.get("mic.status") == "updated" && s.num("next") > int64_t(time(nullptr)) + 86000, "state: next %lld", (long long)s.num("next"));
    give304 = true;
    sum = u.runOnce(true);
    s.load(dir + "/state.txt");
    CHECK(s.get("vrs.status") == "unchanged" && s.get("mic.status") == "unchanged", "conditional: %s", sum.c_str());
    // ★★ a failure keeps the old file and backs off
    const std::string before = slurp(dir + "/mic.vad");
    u.setFetcher([&](const std::string&, const std::string& dest, const std::string&, const std::string&) {
        FetchResult r; std::string err; writeAtomic(dest, "this is not a zip", err); r.status = 200; return r; });
    sum = u.runOnce(true);
    s.load(dir + "/state.txt");
    CHECK(slurp(dir + "/mic.vad") == before && has(s.get("mic.status"), "failed") && s.num("fails") == 1 &&
          s.num("next") <= int64_t(time(nullptr)) + 6 * 3600 + 5, "a bad download changes nothing: %s", s.get("mic.status").c_str());
    u.setUpdatesOn(false);
    CHECK(!u.updatesOn() && has(u.runOnce(false), "switched off"), "the owner's switch");
    u.setUpdatesOn(true);
    const std::string st = u.statusJson();
    CHECK(has(st, "\"updates\":true") && has(st, "\"aircraft\":1205") && has(st, "ODC-By 1.0") && has(st, "CC0 1.0"), "status: %s", st.c_str());
}

static void testReal() {
    const char* raw = getenv("VIBE_ADSB_RAW");
    if (!raw || !*raw) { std::printf("real data: not run (set VIBE_ADSB_RAW to the downloads' folder)\n"); return; }
    std::printf("the REAL downloads (%s)\n", raw);
    const std::string dir = tmpDir("real");
    Mapped tg, zp;
    CHECK(tg.open(std::string(raw) + "/standing-data-main.tar.gz") && zp.open(std::string(raw) + "/indexedDB.zip"), "the downloads are there");
    BuildResult v = buildVrs(tg.data(), tg.size(), false, {});
    BuildResult m = buildMic(zp.data(), zp.size(), {});
    CHECK(v.error.empty() && m.error.empty(), "build: %s %s", v.error.c_str(), m.error.c_str());
    CHECK(v.routes > 500000 && v.airports > 3000 && m.aircraft > 400000 && m.operators > 5000, "sizes %zu %zu %zu %zu", v.routes, v.airports, m.aircraft, m.operators);
    std::string err;
    writeAtomic(dir + "/vrs.vad", v.bytes, err); writeAtomic(dir + "/mic.vad", m.bytes, err);
    std::printf("  vrs.vad %zu bytes, mic.vad %zu bytes\n", v.bytes.size(), m.bytes.size());
    Db& db = Db::get(); db.setDir(dir); db.refresh(true);
    Info a; db.lookupHex(0x4CA2D6, a); db.lookupCallsign("RYR8CK", a.r, 0x4CA2D6, a);
    CHECK(a.r == "EI-DLJ" && a.t == "B738" && a.op == "RYR" && a.country == "IE", "EI-DLJ RYR8CK: %s", a.json().c_str());
    Info b; db.lookupHex(0x400F01, b); db.lookupCallsign("BAW1", b.r, 0x400F01, b);
    CHECK(b.route.size() == 2 && b.route[0].code == "EGLL" && b.route[1].code == "KJFK", "BAW1 EGLL-KJFK: %s", b.json().c_str());
    Info c; db.lookupHex(0xAE1234, c);
    CHECK(c.mil && c.country == "US", "US military block: %s", c.json().c_str());
    Info d; db.lookupHex(0x43C123, d);
    CHECK(d.mil && d.country == "GB", "UK military block: %s", d.json().c_str());
    rmTree(dir);
}

int main() {
    std::printf("test-adsb-data\n");
    testInflate();
    testBlocks();
    testCallsigns();
    const std::string d = tmpDir("db");
    testBuildAndLookup(d);
    testService(d);
    const std::string im = tmpDir("img");
    testImages(im);
    const std::string up = tmpDir("upd");
    testUpdater(up);
    testReal();
    rmTree(d); rmTree(im); rmTree(up);
    std::printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
