// test-dab-hardening.cpp — what the AIR may send the DAB chain, and what it must not be able to do.
//
// ★★★ WHY THIS TEST EXISTS (audit 2026-10-03). Every case below was a memory-safety fault found by
//     reading the parsers as an attacker with a transmitter would: a sub-channel that runs off the
//     end of the CIF, an SI document nested until the stack runs out, a charset-15 label that is
//     shaped like UTF-8 but is not, maps keyed by off-air numbers that only ever grew. None of it
//     shows up on a real multiplex — which is exactly why it needs a test, because no amount of
//     listening to 12B would ever have exercised it.
#include "vibe_dab_receiver.h"
#include "vibe_dab_fic.h"
#include "vibe_dab_spi.h"
#include "vibe_dab_epg.h"
#include "vibe_dab_mot.h"
#include "vibe_dab_packet.h"
#include "vibe_dab_charset.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)

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

/** A TLV element with a 24-bit length, which every nesting depth needs. */
static std::vector<uint8_t> tl(int tag, const std::vector<uint8_t>& inner) {
    std::vector<uint8_t> o{ uint8_t(tag), 0xFF, uint8_t(inner.size() >> 16), uint8_t(inner.size() >> 8), uint8_t(inner.size()) };
    o.insert(o.end(), inner.begin(), inner.end());
    return o;
}
/** `depth` levels of `tag` inside each other, built inside-out without recursion. */
static std::vector<uint8_t> nested(int tag, int depth) {
    // ★ Written in one pass: level i (outermost 0) holds the 5-byte headers of every level inside it.
    std::vector<uint8_t> v((size_t)depth * 5);
    for (int i = 0; i < depth; ++i) {
        const size_t inner = (size_t)(depth - 1 - i) * 5;
        uint8_t* h = &v[(size_t)i * 5];
        h[0] = uint8_t(tag); h[1] = 0xFF; h[2] = uint8_t(inner >> 16); h[3] = uint8_t(inner >> 8); h[4] = uint8_t(inner);
    }
    return v;
}

static bool validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { ++i; continue; }
        int need; unsigned cp, lo;
        if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; lo = 0x80; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; lo = 0x800; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; lo = 0x10000; }
        else return false;
        if (i + (size_t)need >= s.size()) return false;
        for (int k = 1; k <= need; ++k) { const unsigned char cc = (unsigned char)s[i + k]; if ((cc & 0xC0) != 0x80) return false; cp = (cp << 6) | (cc & 0x3F); }
        if (cp < lo || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += (size_t)need + 1;
    }
    return true;
}

int main() {
    printf("test-dab-hardening\n");

    // ── 1. FIG 0/1: a sub-channel must lie inside the 864-CU CIF ─────────────────────────────
    {
        Ensemble e;
        // long form, EEP-A level 3: id 1 at CU 800, 100 CUs — runs 36 CUs off the end
        std::vector<uint8_t> p{ 0x01, uint8_t((1 << 2) | (800 >> 8)), uint8_t(800 & 0xFF), uint8_t(0x80 | (2 << 2) | (100 >> 8)), uint8_t(100) };
        // and id 2 at CU 0, 96 CUs — fine
        p.insert(p.end(), { uint8_t((2 << 2) | 0), 0x00, uint8_t(0x80 | (2 << 2) | 0), 96 });
        // short form (UEP) id 3 starting at CU 1000 — past the CIF before its size is even known
        p.insert(p.end(), { uint8_t((3 << 2) | (1000 >> 8)), uint8_t(1000 & 0xFF), 0x08 });
        auto fib = makeFib({{0, p}});
        CHECK(parseFib(fib.data(), e), "the FIB parses");
        CHECK(!e.subChannels.count(1), "★★★ a sub-channel running past CU 864 is refused");
        CHECK(e.subChannels.count(2) == 1, "★ a sub-channel inside the CIF is kept");
        CHECK(!e.subChannels.count(3), "★★ a short-form start past the CIF is refused");

        // profileFor: the UEP size comes from table 8, so it is checked again there
        SubChannel sc; sc.eep = false; sc.protLevel = 63; sc.startCu = 860;   // row 63: 416 CUs
        EepProfile ep; UepProfile up; int db, br, cb;
        CHECK(!DabReceiver::profileFor(sc, ep, up, db, br, cb), "★★★ a UEP sub-channel that overruns the CIF has no profile");
        sc.startCu = 0;
        CHECK(DabReceiver::profileFor(sc, ep, up, db, br, cb), "★ the same row at CU 0 decodes");
        SubChannel ee; ee.eep = true; ee.option = 0; ee.protLevel = 2; ee.sizeCu = 96; ee.startCu = 800;
        CHECK(!DabReceiver::profileFor(ee, ep, up, db, br, cb), "★★ an EEP sub-channel that overruns the CIF has no profile");
        ee.startCu = 768;
        CHECK(DabReceiver::profileFor(ee, ep, up, db, br, cb), "★ ending exactly at CU 864 is allowed");
    }

    // ── 11. FIC maps are capped ─────────────────────────────────────────────────────────────
    {
        Ensemble e;
        for (uint32_t sid = 0; sid < 1000; ++sid) {
            std::vector<uint8_t> p{ 0x01, uint8_t(sid >> 8), uint8_t(sid) };   // FIG 1/1
            for (int k = 0; k < 16; ++k) p.push_back('A');
            parseFib(makeFib({{1, p}}).data(), e);
        }
        CHECK(e.services.size() == kFicMaxServices, "★★ services stop at the cap, however many SIds the air invents");
    }

    // ── 2. SPI / EPG: nesting is capped, so a deep document cannot exhaust the stack ─────────
    {
        const auto deep = nested(0x28, 200000);         // 200k nested <service>: ~1 MB
        const auto out = SpiDocument::parse(deep.data(), deep.size());
        CHECK(out.size() <= 16, "★★★ a 200 000-deep SI document returns instead of overflowing the stack");
        // a real shape still parses: serviceInformation > services > service > bearer(id)
        std::vector<uint8_t> bearer{ 0x80, 6, 0x00, 0xE1, 0xC1, 0x85, 0xC2, 0x21 };
        auto svc = tl(0x28, tl(0x29, bearer));
        auto doc = tl(0x03, tl(0x26, svc));
        const auto real = SpiDocument::parse(doc.data(), doc.size());
        CHECK(real.size() == 1 && real[0].sid == 0xC221, "★ a normally nested SI document still yields its service");

        // EPG: epg > schedule > programme > programmeEvent > programmeEvent > ...
        auto ev = nested(kEpgElProgrammeEvent, 200000);
        auto epg = tl(kEpgElEpg, tl(kEpgElSchedule, tl(kEpgElProgramme, ev)));
        const auto sch = EpgDocument::parse(epg.data(), epg.size());
        size_t n = 0; for (auto& s : sch) n += s.programmes.size();
        CHECK(n <= 32, "★★★ a 200 000-deep programmeEvent chain returns instead of overflowing the stack");
    }

    // ── 10. charset 15 rejects what is shaped like UTF-8 but is not ────────────────────────
    {
        const uint8_t overlong[] = { 'A', 0xC0, 0x80, 'B' };
        const uint8_t surrogate[] = { 'A', 0xED, 0xA0, 0x80 };
        const uint8_t tooBig[] = { 0xF4, 0x90, 0x80, 0x80 };
        const uint8_t good[] = { 'B', 'a', 'y', 'e', 'r', 'n', ' ', 0xC3, 0xBC };
        const std::string a = dabTextToUtf8(overlong, sizeof overlong, kCharsetUtf8);
        const std::string b = dabTextToUtf8(surrogate, sizeof surrogate, kCharsetUtf8);
        const std::string c = dabTextToUtf8(tooBig, sizeof tooBig, kCharsetUtf8);
        const std::string d = dabTextToUtf8(good, sizeof good, kCharsetUtf8);
        CHECK(validUtf8(a) && a.find('\xC0') == std::string::npos, "★★ an overlong NUL is not passed through as UTF-8");
        CHECK(validUtf8(b) && b.find("\xED\xA0\x80") == std::string::npos, "★★ a UTF-16 surrogate is not passed through");
        CHECK(validUtf8(c) && c.find("\xF4\x90") == std::string::npos, "★★ a code point past U+10FFFF is not passed through");
        CHECK(d == "Bayern \xC3\xBC", "★ real UTF-8 survives untouched");
    }

    // ── 11. MOT carousel: finished bodies are capped in total, injected objects refused past it ─
    {
        MotCarousel car;
        CHECK(!car.inject("huge.bin", 7, 0, std::vector<uint8_t>(MotCarousel::kMaxObjectBytes + 1, 1)),
              "★★ an object over the per-object cap is refused (and cacheLoad then deletes the file)");
        for (int i = 0; i < 40; ++i)
            car.inject("logo" + std::to_string(i) + ".png", 2, 3, std::vector<uint8_t>(MotCarousel::kMaxObjectBytes, 1));
        CHECK(car.completeCount() <= MotCarousel::kMaxCompleteBytes / MotCarousel::kMaxObjectBytes,
              "★★★ 40 MB of objects leaves at most the 16 MB cap held");
        CHECK(car.find("logo39.png") != nullptr, "★ the newest object is the one kept");
    }

    // ── 11. packet mode: a data group that never ends cannot grow without limit ────────────────
    {
        PacketAssembler pa; pa.setAddress(5);
        size_t groups = 0; pa.setSink([&](const uint8_t*, size_t) { ++groups; });
        std::vector<uint8_t> fr;
        for (int k = 0; k < 400; ++k) {                   // 400 x 91 bytes = 36 kB, never `last`
            std::vector<uint8_t> pk(96, 0x55);
            const bool first = (k == 0);
            pk[0] = uint8_t((3 << 6) | ((k & 3) << 4) | (first ? 0x08 : 0) | (5 >> 8));
            pk[1] = 5; pk[2] = 91;
            const uint16_t c = motCrc16(pk.data(), 94);
            pk[94] = uint8_t(c >> 8); pk[95] = uint8_t(c);
            fr.insert(fr.end(), pk.begin(), pk.end());
        }
        pa.feedFrame(fr.data(), fr.size());
        CHECK(pa.packets() == 400, "the packets themselves are valid");
        CHECK(pa.lost() >= 1 && groups == 0, "★★ an unterminated data group is abandoned at the byte cap");
    }

    if (fails) { printf("  %d FAILED\n", fails); return 1; }
    printf("  all passed\n");
    return 0;
}
