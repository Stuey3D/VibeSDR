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

    // ── ★★★ P/D IS BIT 5, NOT BIT 7. A C/N=1 FIG 0/2 describes the NEXT configuration ──────
    {
        Ensemble t;
        // C/N = 1 (0x80), P/D = 0, ext 2: one 16-bit service. Must be IGNORED (not current).
        std::vector<uint8_t> p{ 0x82, 0xC3, 0x33, 0x01, 0x00, uint8_t((7 << 2) | 0x02) };
        auto fib = makeFib({{0, p}});
        CHECK(parseFib(fib.data(), t), "a next-configuration FIB parses");
        CHECK(t.services.empty(), "but C/N = 1 must not write into the CURRENT service map");
        // P/D = 1 (0x20), ext 2: one 32-bit data service SId 0xE1C33344 with a packet component.
        std::vector<uint8_t> d{ 0x22, 0xE1, 0xC3, 0x33, 0x44, 0x01, 0xC0 | 0x01, 0x23 | 0x02 };
        fib = makeFib({{0, d}});
        CHECK(parseFib(fib.data(), t), "a data-service FIB parses");
        CHECK(t.services.count(0xE1C33344) == 1, "P/D = 1 reads a 32-bit SId");
        CHECK(t.services[0xE1C33344].isData, "and marks it as a data service");
        CHECK(t.services[0xE1C33344].components.size() == 1
              && t.services[0xE1C33344].components[0].tmid == 3, "packet-mode component, TMId 3");
    }

    // ── FIG 0/0: EId, CIF count, change flags ───────────────────────────────
    {
        Ensemble t;
        // EId 0xCE15, change flags 0, Al 0, CIF count hi 7, lo 123 -> 7*250+123 = 1873
        std::vector<uint8_t> p{ 0x00, 0xCE, 0x15, 0x07, 123 };
        auto fib = makeFib({{0, p}});
        parseFib(fib.data(), t);
        CHECK(t.eid == 0xCE15, "FIG 0/0 EId");
        CHECK(t.cifCount == 1873, "FIG 0/0 CIF count = hi*250 + lo");
        CHECK(t.changeFlags == 0 && t.occurrenceChange == -1, "no change signalled");
        std::vector<uint8_t> q{ 0x00, 0xCE, 0x15, uint8_t(0xC0 | 0x20 | 0x02), 5, 200 };
        fib = makeFib({{0, q}});
        parseFib(fib.data(), t);
        CHECK(t.changeFlags == 3 && t.alarm && t.occurrenceChange == 200, "change flags, alarm, occurrence change");
    }

    // ── FIG 0/7: service count and the MCI-complete rule ────────────────────
    {
        Ensemble t;
        std::vector<uint8_t> p7{ 0x07, uint8_t((2 << 2) | 0x01), 0x2A };   // 2 services, count 0x12A
        parseFib(makeFib({{0, p7}}).data(), t);
        CHECK(t.serviceCount == 2 && t.configCount == 0x12A, "FIG 0/7 services and count");
        CHECK(!t.mciComplete(), "MCI incomplete before the services arrive");
        std::vector<uint8_t> p2{ 0x02, 0xC2, 0x21, 0x01, 0x00, uint8_t((3 << 2) | 0x02),
                                       0xC2, 0x22, 0x01, 0x3F, uint8_t((5 << 2) | 0x02) };
        parseFib(makeFib({{0, p2}}).data(), t);
        std::vector<uint8_t> p1{ 0x01, uint8_t(3 << 2), 0x10, 0x05,      // subch 3, UEP index 5
                                       uint8_t(5 << 2), 0x60, uint8_t(0x80 | (0 << 4) | (1 << 2) | 0x00), 84 };
        parseFib(makeFib({{0, p1}}).data(), t);
        CHECK(!t.mciComplete(), "still incomplete without labels");
        std::vector<uint8_t> l1{ 0x01, 0xC2, 0x21 }; auto a = label16("BBC Radio 4"); l1.insert(l1.end(), a.begin(), a.end());
        std::vector<uint8_t> l2{ 0x01, 0xC2, 0x22 }; auto c = label16("Radio X");     l2.insert(l2.end(), c.begin(), c.end());
        parseFib(makeFib({{1, l1}}).data(), t);
        CHECK(!t.mciComplete(), "one of two labelled: incomplete");
        parseFib(makeFib({{1, l2}}).data(), t);
        CHECK(t.mciComplete(), "both labelled with sub-channels: complete");
        CHECK(t.services[0xC221].complete(t.subChannels) && t.services[0xC222].complete(t.subChannels), "per-service completeness");
    }

    // ── FIG 0/8 + 0/13: SCIdS binds the slideshow to the component ──────────
    {
        Ensemble t;
        std::vector<uint8_t> p2{ 0x02, 0xC2, 0x21, 0x01, 0x00, uint8_t((3 << 2) | 0x02) };
        parseFib(makeFib({{0, p2}}).data(), t);
        // 0/8: SId C221, ext flag 0, SCIdS 0, short form SubChId 3
        std::vector<uint8_t> p8{ 0x08, 0xC2, 0x21, 0x00, 0x03 };
        parseFib(makeFib({{0, p8}}).data(), t);
        CHECK(t.services[0xC221].components[0].scids == 0, "FIG 0/8 binds SCIdS 0 to sub-channel 3");
        // 0/13: SId C221, SCIdS 0, 1 app: type 0x002 (MOT SlideShow), len 2, X-PAD AppTy 12, DSCTy 60
        std::vector<uint8_t> p13{ 0x0D, 0xC2, 0x21, 0x01, uint8_t(0x002 >> 3), uint8_t(((0x002 & 7) << 5) | 2), 12, 60 };
        parseFib(makeFib({{0, p13}}).data(), t);
        CHECK(t.services[0xC221].hasSlideshow(), "FIG 0/13 user app 0x002 = slideshow");
        CHECK(t.services[0xC221].components[0].apps[0].xpadApp == 12, "X-PAD application type read");
        // 0/2 repeats every frame: the binding must survive it
        parseFib(makeFib({{0, p2}}).data(), t);
        CHECK(t.services[0xC221].hasSlideshow() && t.services[0xC221].components[0].scids == 0,
              "a repeated FIG 0/2 keeps SCIdS and user apps");
    }

    // ── FIG 0/9 ECC + LTO, FIG 0/17 PTy ────────────────────────────────────
    {
        Ensemble t;
        std::vector<uint8_t> p2{ 0x02, 0xC2, 0x21, 0x01, 0x00, uint8_t((3 << 2) | 0x02) };
        parseFib(makeFib({{0, p2}}).data(), t);
        std::vector<uint8_t> p9{ 0x09, uint8_t(0x20 | 0x02), 0xE1, 0x01 };   // LTO -1.0 h, ECC E1
        parseFib(makeFib({{0, p9}}).data(), t);
        CHECK(t.ecc == 0xE1 && t.ltoHalfHours == -2, "FIG 0/9 ECC and signed LTO");
        std::vector<uint8_t> p17{ 0x11, 0xC2, 0x21, 0x00, 0x0A };            // PTy 10 (Pop)
        parseFib(makeFib({{0, p17}}).data(), t);
        CHECK(t.services[0xC221].pty == 10, "FIG 0/17 programme type");
    }

    // ── EBU Latin and the short label ───────────────────────────────────────
    {
        Ensemble t;
        std::vector<uint8_t> p{ 0x01, 0xC2, 0x21 };
        std::vector<uint8_t> l(16, 0x00);
        const char* base = "Radio Cymru";
        for (size_t i = 0; base[i]; ++i) l[i] = uint8_t(base[i]);
        l[11] = ' '; l[12] = 0xAA;                       // 0xAA = pound sign in EBU Latin
        p.insert(p.end(), l.begin(), l.end());
        p.push_back(0xF8); p.push_back(0x00);            // flags: first five characters
        parseFib(makeFib({{1, p}}).data(), t);
        CHECK(t.services[0xC221].label == "Radio Cymru \xC2\xA3", "EBU Latin 0xAA -> U+00A3 as UTF-8, 0x00 padding trimmed");
        CHECK(t.services[0xC221].shortLabel == "Radio", "short label from the character flag field");
        CHECK(dabTextToUtf8((const uint8_t*)"\xE1\x8F\xA9", 3) == "\xC3\x85\xC5\xB8\xE2\x82\xAC", "0xE1 is A-ring, not a-acute; 0xA9 is the euro");
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
