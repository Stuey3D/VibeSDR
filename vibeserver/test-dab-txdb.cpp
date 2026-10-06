// test-dab-txdb.cpp — the DAB transmitter directory: UK TII lookups, Switzerland's by-block
// licensed sites, whose record each row is, and the owner's own FMLIST-layout list (2026-10-06).
#include "vibe_dab_txdb_e1.h"
#include "vibe_dab_txdb_ch.h"
#include "vibe_dab_txdb_cz.h"
#include "vibe_dab_txdb_nl.h"
#include <cstdio>
#include <string>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)

static DabTxDb makeDb() {
    DabTxDb db;
    db.addBuiltin(0xE1, kDabTx_E1, kDabTx_E1_n);
    db.addBuiltin(0xE1, kDabTx_CH, kDabTx_CH_n);
    db.addBuiltin(0xE2, kDabTx_CZ, kDabTx_CZ_n);
    db.addBuiltin(0xE3, kDabTx_NL, kDabTx_NL_n);
    return db;
}

int main() {
    printf("test-dab-txdb\n");
    const double NaN = std::nan("");
    // Zürich, and Coventry: two receivers either side of the shared ECC E1
    const double zLat = 47.3769, zLon = 8.5417, cLat = 52.4068, cLon = -1.5197;
    {
        DabTxDb db = makeDb();
        // ── the UK is unchanged: a TII hit on Digital One's Wrotham (11D, C181, 01/07) ──
        DabTxMatch m = db.lookup(0xE1, 0xC181, 1, 7, NaN, NaN);
        CHECK(m.found && m.site == "Wrotham", "UK TII lookup still names Wrotham");
        // ── a Swiss row has no TII: no code may ever resolve against it ──
        m = db.lookup(0xE1, 0x4001, 0, 0, zLat, zLon);
        CHECK(!m.found, "a 0/0 'code' never matches a by-block row");

        // ── SRG D01 on 12C, heard in Zürich: Zürichberg, nearest first, BAKOM's record ──
        auto s = db.sitesFor(0xE1, 0x4FFE, zLat, zLon, 4, "12C");
        CHECK(!s.empty(), "Swiss 12C ensemble gets licensed sites");
        CHECK(!s.empty() && s[0].site == "Zuerich Zuerichberg", "nearest 12C site to the centre of Zürich is Zürichberg (Uetliberg is further out)");
        CHECK(!s.empty() && s[0].src == kDabSrcBakom, "…and it is BAKOM's record");
        CHECK(!s.empty() && s[0].km >= 0 && s[0].km < 10, "…a few km away");
        CHECK(!s.empty() && s[0].subId == 0, "…with no TII code");
        CHECK(std::string(dabTxSourceLabel(s.empty() ? 0 : s[0].src)) == "BAKOM record", "label reads BAKOM record");
        for (size_t i = 1; i < s.size(); ++i) CHECK(s[i - 1].km <= s[i].km, "sorted nearest first");

        // ── without the block, a Swiss ensemble gets nothing rather than every site in the country ──
        CHECK(db.sitesFor(0xE1, 0x4FFE, zLat, zLon, 4, nullptr).empty(), "no block → no by-block rows");
        CHECK(db.sitesFor(0xE1, 0x4FFE, zLat, zLon, 4, "").empty(), "empty block → no by-block rows");
        // ── a block Switzerland does not use for DAB gives nothing ──
        CHECK(db.sitesFor(0xE1, 0x4FFE, zLat, zLon, 4, "13F").empty(), "unused block → none");

        // ── a UK ensemble on 12C must not pick up Swiss sites (same ECC, different country nibble) ──
        auto uk = db.sitesFor(0xE1, 0xC184, 53.48, -2.24, 8, "12C");
        bool anySwiss = false;
        for (const auto& x : uk) if (x.src != kDabSrcOfcom) anySwiss = true;
        CHECK(!anySwiss, "UK 12C ensemble lists only Ofcom rows"); CHECK(!uk.empty(), "…and does list them");
        // ── a UK ensemble lists Ofcom's record, labelled as such ──
        auto d1 = db.sitesFor(0xE1, 0xC181, cLat, cLon, 4, "11D");
        CHECK(!d1.empty() && d1[0].src == kDabSrcOfcom, "Digital One lists Ofcom rows");
        CHECK(std::string(dabTxSourceLabel(kDabSrcOfcom)) == "Ofcom record", "label reads Ofcom record");
        // ── another country's ECC never sees Swiss or UK rows ──
        CHECK(db.sitesFor(0xE0, 0x4FFE, zLat, zLon, 4, "12C").empty(), "ECC E0 (e.g. Germany) gets none of E1's rows");
    }
    // ── Czechia: ČTÚ publishes EIds — Multiplex A (0x2005) on 12C, heard in Prague ──
    {
        DabTxDb db = makeDb();
        auto s = db.sitesFor(0xE2, 0x2005, 50.08, 14.42, 4, "12C");
        CHECK(!s.empty() && s[0].src == kDabSrcCtu, "Czech multiplex A lists ČTÚ rows");
        CHECK(std::string(dabTxSourceLabel(kDabSrcCtu)) == "\xC4\x8CT\xC3\x9A record", "label reads ČTÚ record");
        CHECK(!s.empty() && s[0].area == "Vys\xC3\xADlac\xC3\xAD s\xC3\xAD\xC5\xA5 A", "area is the multiplex name");
        // the same EId on another block is not this multiplex (ČTÚ's placeholder EIds)
        CHECK(db.sitesFor(0xE2, 0x2005, 50.08, 14.42, 4, "5A").empty(), "EId + wrong block → none");
        // block unknown → the EId alone still answers
        CHECK(!db.sitesFor(0xE2, 0x2005, 50.08, 14.42, 4, nullptr).empty(), "EId with no block known still lists sites");
        // 0x211B is filed against several regional networks: only the one on our block
        auto r = db.sitesFor(0xE2, 0x211B, 50.0, 15.0, 8, "8B");
        bool onlyR1 = !r.empty();
        for (const auto& x : r) if (x.area.size() < 3 || x.area.compare(x.area.size() - 3, 3, " R1") != 0) onlyR1 = false;
        CHECK(onlyR1, "0x211B on 8B lists regional network R1 only");
        CHECK(!db.lookup(0xE2, 0x2005, 1, 1, NaN, NaN).found, "no TII in ČTÚ's data → no TII hit");
        for (size_t i = 0; i < kDabTx_CZ_n; ++i)
            if (kDabTx_CZ[i].src != kDabSrcCtu || (kDabTx_CZ[i].eid >> 12) != 0x2) { CHECK(false, "CZ rows: ČTÚ, country 2"); break; }
    }
    // ── the Netherlands: RDI publishes site + block — a Dutch ensemble on 11C, heard in Amsterdam ──
    {
        DabTxDb db = makeDb();
        auto s = db.sitesFor(0xE3, 0x8FFF, 52.37, 4.90, 4, "11C");
        CHECK(!s.empty() && s[0].src == kDabSrcRdi && s[0].km < 40, "Dutch 11C lists a nearby RDI site");
        CHECK(std::string(dabTxSourceLabel(kDabSrcRdi)) == "RDI record", "label reads RDI record");
        CHECK(db.sitesFor(0xE3, 0xE0FF, 52.37, 4.90, 4, "11C").empty(), "another country nibble on ECC E3 gets none");
        for (size_t i = 0; i < kDabTx_NL_n; ++i)
            if (kDabTx_NL[i].src != kDabSrcRdi || !(kDabTx_NL[i].flags & kDabTxByBlock)) { CHECK(false, "NL rows: RDI, by-block"); break; }
    }
    // ── every compiled-in Swiss row is well formed ──
    {
        bool ok = true;
        for (size_t i = 0; i < kDabTx_CH_n; ++i) {
            const DabTx& r = kDabTx_CH[i];
            if ((r.eid >> 12) != 0x4 || !(r.flags & kDabTxByBlock) || r.src != kDabSrcBakom || r.subId != 0
                || r.lat < 45.5f || r.lat > 48.1f || r.lon < 5.7f || r.lon > 10.8f || !r.block || !r.block[0] || !r.site[0]) {
                printf("  bad row %zu: %s %s\n", i, r.site, r.block); ok = false;
            }
        }
        CHECK(ok, "Swiss rows: country 4, by-block, BAKOM, inside Switzerland");
        CHECK(kDabTx_CH_n > 300, "Swiss table is not empty or truncated");
        for (size_t i = 0; i < kDabTx_E1_n; ++i)
            if (kDabTx_E1[i].src != kDabSrcOfcom || kDabTx_E1[i].flags) { CHECK(false, "UK rows default to Ofcom, EId-matched"); break; }
    }
    // ── the owner's own list, FMLIST / AbracaDABra layout ──
    {
        using R = DabTxDb::DabTxListRow;
        R r = DabTxDb::parseTxListLine("123;DE;5C;DR Deutschland;10BC;1203;Berlin Alexanderplatz;52.5208;13.4094;34;368;V;178.352;10\n");
        CHECK(r.ok, "a full FMLIST row parses");
        CHECK(r.ecc == 0xE0 && r.eid == 0x10BC, "country DE → ECC E0, EId hex");
        CHECK(r.mainId == 12 && r.subId == 3, "TII 1203 is main 12, sub 03 (decimal)");
        CHECK(r.site == "Berlin Alexanderplatz" && r.label == "DR Deutschland" && r.block == "5C", "names and block");
        CHECK(!DabTxDb::parseTxListLine("1;DE;5C;X;10BC;;Nowhere;52.5;13.4;0;0;V;178;1").ok, "no TII → skipped");
        CHECK(!DabTxDb::parseTxListLine("1;DE;5C;X;10BC;1200;Nowhere;52.5;13.4;0;0;V;178;1").ok, "sub 00 → skipped");
        CHECK(!DabTxDb::parseTxListLine("1;DE;5C;X;10BC;1203;Nowhere;abc;13.4;0;0;V;178;1").ok, "bad latitude → skipped");
        CHECK(!DabTxDb::parseTxListLine("1;DE;5C;X;10BC;1203").ok, "short line → skipped");
        R u = DabTxDb::parseTxListLine("1; XX ;5C;X;10BC;1203;Somewhere;52.5;13.4;0;0;V;178;1");
        CHECK(u.ok && u.ecc == -1, "unknown country → matches on EId alone");

        const char* path = "/tmp/vibe-test-dab-tx-list.csv";
        FILE* f = std::fopen(path, "w");
        std::fputs("id;country;channel;label;eid;tii;location;lat;lon;alt;height;pol;freq;power\n", f);
        std::fputs("1;DE;5C;DR Deutschland;10BC;1203;Berlin Alexanderplatz;52.5208;13.4094;34;368;V;178.352;10\n", f);
        std::fputs("2;DE;5C;DR Deutschland;10BC;1203;Somewhere Far;48.1;11.5;0;0;V;178.352;10\n", f);
        std::fputs("3;XX;5C;Mystery;ABCD;0101;Unknown Land;10;10;0;0;V;178.352;1\n", f);
        std::fclose(f);
        DabTxDb db = makeDb();
        CHECK(db.loadTxListFile(path) == 3, "three rows loaded");
        DabTxMatch m = db.lookup(0xE0, 0x10BC, 12, 3, 52.4, 13.3);
        CHECK(m.found && m.site == "Berlin Alexanderplatz", "owner's list names the TII, nearest of the reused code");
        m = db.lookup(0xE0, 0x10BC, 12, 3, NaN, NaN);
        CHECK(m.found && m.ambiguous, "reused code without a position is ambiguous");
        m = db.lookup(0xE7, 0xABCD, 1, 1, NaN, NaN);
        CHECK(m.found && m.site == "Unknown Land", "unknown-country row matches any ECC on its EId");
        auto own = db.sitesFor(0xE0, 0x10BC, 52.4, 13.3, 4, "5C");
        CHECK(!own.empty() && own[0].src == kDabSrcUser, "licensed sites from the owner's list say so");
        CHECK(std::string(dabTxSourceLabel(kDabSrcUser)) == "your list", "label reads your list");
        CHECK(db.loadTxListFile("/tmp/vibe-test-no-such-file.csv") == 0, "a missing file is no rows, not an error");
        std::remove(path);
    }
    printf(fails ? "  %d FAILED\n" : "  all passed\n", fails);
    return fails ? 1 : 0;
}
