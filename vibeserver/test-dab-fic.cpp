// test-dab-fic.cpp — FIB/FIG parsing into an ensemble and its station list.
#include "vibe_dab_fic.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)

/** Build a 32-byte FIB from a list of FIGs (type, payload), stamping the CRC. */
static std::vector<uint8_t> makeFib(const std::vector<std::pair<int, std::vector<uint8_t>>>& figs) {
    std::vector<uint8_t> fib(32, 0xFF);
    size_t i = 0;
    for (auto& f : figs) {
        if (i + 1 + f.second.size() > 30) break;
        fib[i++] = uint8_t((f.first << 5) | (f.second.size() & 0x1F));
        memcpy(&fib[i], f.second.data(), f.second.size());
        i += f.second.size();
    }
    const uint16_t c = uint16_t(~crc16(fib.data(), 30));
    fib[30] = uint8_t(c >> 8); fib[31] = uint8_t(c & 0xFF);
    return fib;
}

static std::vector<uint8_t> label16(const char* s) {
    std::vector<uint8_t> v(16, ' ');
    for (size_t i = 0; i < 16 && s[i]; ++i) v[i] = uint8_t(s[i]);
    return v;
}

int main() {
    printf("test-dab-fic\n");
    Ensemble e;

    // ── FIG 1/0: ensemble label ─────────────────────────────────────────────
    {
        std::vector<uint8_t> p{ 0x00, 0xC1, 0x85 };            // ext 0, EId 0xC185
        auto l = label16("BBC National");
        p.insert(p.end(), l.begin(), l.end());
        auto fib = makeFib({{1, p}});
        CHECK(parseFib(fib.data(), e), "a well-formed FIB parses");
        CHECK(e.eid == 0xC185, "ensemble id");
        CHECK(e.label == "BBC National", "ensemble label, padding trimmed");
    }

    // ── FIG 0/2: two programme services ─────────────────────────────────────
    {
        // ext 2, P/D = 0 (16-bit SIds)
        std::vector<uint8_t> p{ 0x02 };
        // SId 0xC221, 1 component: TMid 0 (audio), ASCTy 0 (DAB/MP2), SubChId 3, primary
        p.insert(p.end(), { 0xC2, 0x21, 0x01, 0x00, uint8_t((3 << 2) | 0x02) });
        // SId 0xC222, 1 component: ASCTy 63 (DAB+), SubChId 5
        p.insert(p.end(), { 0xC2, 0x22, 0x01, 0x3F, uint8_t((5 << 2) | 0x02) });
        auto fib = makeFib({{0, p}});
        CHECK(parseFib(fib.data(), e), "service organisation FIB parses");
        CHECK(e.services.size() == 2, "two services discovered");
        CHECK(e.services[0xC221].components.size() == 1, "service has one component");
        CHECK(e.services[0xC221].components[0].subChId == 3, "sub-channel id read");
        CHECK(e.services[0xC221].components[0].scType == 0, "ASCTy 0 = MPEG Layer II (the BBC)");
        CHECK(e.services[0xC222].components[0].scType == 63, "ASCTy 63 = DAB+");
        CHECK(e.services[0xC222].components[0].primary, "primary flag");
    }

    // ── FIG 1/1: service labels attach to the services already found ────────
    {
        std::vector<uint8_t> p{ 0x01, 0xC2, 0x21 };
        auto l = label16("BBC Radio 4");
        p.insert(p.end(), l.begin(), l.end());
        auto fib = makeFib({{1, p}});
        CHECK(parseFib(fib.data(), e), "label FIB parses");
        CHECK(e.services[0xC221].label == "BBC Radio 4", "label lands on the right service");
        CHECK(e.services.size() == 2, "labelling does not invent a service");
    }

    // ── FIG 0/1: sub-channel organisation, both forms ───────────────────────
    {
        std::vector<uint8_t> p{ 0x01 };
        // short form (UEP): SubChId 3, StartCU 0x010, table index 7
        p.insert(p.end(), { uint8_t((3 << 2) | 0x00), 0x10, 0x07 });
        // long form (EEP): SubChId 5, StartCU 0x020, option/level, size 84 CU
        p.insert(p.end(), { uint8_t((5 << 2) | 0x00), 0x20, uint8_t(0x80 | (2 << 2) | 0x00), 84 });
        auto fib = makeFib({{0, p}});
        CHECK(parseFib(fib.data(), e), "sub-channel FIB parses");
        CHECK(e.subChannels[3].startCu == 0x010 && !e.subChannels[3].eep, "short form = UEP");
        CHECK(e.subChannels[5].eep && e.subChannels[5].sizeCu == 84, "long form = EEP with a size");
        CHECK(e.subChannels[5].protLevel == 2, "EEP protection level");
    }

    // ── ★★★ A BAD CRC MUST CHANGE NOTHING ───────────────────────────────────
    {
        const size_t before = e.services.size();
        const std::string lbl = e.services[0xC221].label;
        std::vector<uint8_t> p{ 0x01, 0xC2, 0x21 };
        auto l = label16("CORRUPTED");
        p.insert(p.end(), l.begin(), l.end());
        auto fib = makeFib({{1, p}});
        fib[5] ^= 0x40;                                  // damage the payload, CRC now wrong
        CHECK(!parseFib(fib.data(), e), "a corrupt FIB is rejected");
        CHECK(e.services.size() == before, "and adds no service");
        CHECK(e.services[0xC221].label == lbl, "and does not overwrite a good label");
    }

    // ── repeats update rather than duplicate (the list must not flap) ───────
    {
        std::vector<uint8_t> p{ 0x01, 0xC2, 0x21 };
        auto l = label16("BBC Radio 4");
        p.insert(p.end(), l.begin(), l.end());
        auto fib = makeFib({{1, p}});
        parseFib(fib.data(), e); parseFib(fib.data(), e);
        CHECK(e.services.size() == 2, "the same label three times still yields two services");
    }

    // ── a truncated / garbage FIB must not run off the end ──────────────────
    {
        std::vector<uint8_t> fib(32, 0x00);
        fib[0] = 0x3F;                                   // type 1, length 31 — overruns
        const uint16_t c = uint16_t(~crc16(fib.data(), 30));
        fib[30] = uint8_t(c >> 8); fib[31] = uint8_t(c & 0xFF);
        Ensemble tmp;
        CHECK(parseFib(fib.data(), tmp), "CRC is valid so it is parsed");
        CHECK(tmp.empty(), "but an over-long FIG yields nothing rather than reading past the end");
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
