// test-dab-epg.cpp — binary Programme Information (ETSI TS 102 371 V3.3.1).
//
// ★★★ THIS IS THE ONLY TEST THIS DECODER CAN HAVE. Nothing within reach of Northampton transmits
//     Programme Information: measured 2026-09-08, 12B's SPI carousel holds serviceInformation
//     only and 7D carries no SPI carousel at all. So there is no capture to replay and no on-air
//     measurement to make — the documents below are synthesised from the spec's OWN tag tables
//     (annexes D and E) and its worked field layouts, and they are the whole of the evidence that
//     this parser is right. Written on Stuart's instruction and by his precedent: "RT+ isnt
//     broadcast in the UK but it is in the Netherlands on FM so we included it even though I could
//     never test it myself."
//
// ★★ WHICH MEANS THE ENCODER HERE MATTERS AS MUCH AS THE DECODER. If both are wrong in the same
//    way the test passes and the radio still shows nothing, so each builder below states the
//    clause it implements and the byte counts are asserted against the spec's arithmetic rather
//    than against whatever the parser happens to do.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "vibe_dab_epg.h"

using namespace vibedab;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); ++fails; } } while (0)

/** TLV, clause 5.2: tag, then a length that escapes to 16 or 24 bits at 0xFE / 0xFF. */
static std::vector<uint8_t> tlv(int tag, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> o{ uint8_t(tag) };
    if (body.size() < 0xFE) o.push_back(uint8_t(body.size()));
    else if (body.size() <= 0xFFFF) { o.push_back(0xFE); o.push_back(uint8_t(body.size() >> 8)); o.push_back(uint8_t(body.size())); }
    else { o.push_back(0xFF); o.push_back(uint8_t(body.size() >> 16)); o.push_back(uint8_t(body.size() >> 8)); o.push_back(uint8_t(body.size())); }
    o.insert(o.end(), body.begin(), body.end());
    return o;
}
static std::vector<uint8_t> str(int tag, const std::string& s) {
    return tlv(tag, std::vector<uint8_t>(s.begin(), s.end()));
}
static std::vector<uint8_t>& operator+=(std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end()); return a;
}

/** ★ Clause 5.4.5.2, short form, no LTO — four bytes:
 *      Rfa(1) Date(17) Rfa(1) LTOflag(1) UTCflag(1) Hours(5) Minutes(6) = 32 bits. */
static std::vector<uint8_t> timeShort(int mjd, int hh, int mm) {
    const uint32_t w = (uint32_t(mjd & 0x1FFFF) << 14) | (uint32_t(hh & 0x1F) << 6) | uint32_t(mm & 0x3F);
    return { uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w) };
}
/** ★ Same, with the LTO byte present — Rfa(2) Sign(1) Half-hours(5) — so five bytes, and the
 *  hours and minutes are pushed eight bits later. This is the case a UK-written decoder is most
 *  likely to get wrong, because a UK broadcast in winter would set LTO to zero and never exercise
 *  the shift; the Netherlands in summer is +2 hours. */
static std::vector<uint8_t> timeShortLto(int mjd, int hh, int mm, int halfHours) {
    const int sign = halfHours < 0 ? 1 : 0;
    const int mag  = halfHours < 0 ? -halfHours : halfHours;
    uint64_t w = 0;
    w |= uint64_t(mjd & 0x1FFFF) << 22;      // after Rfa(1): bits 1..17 of 40
    w |= uint64_t(1) << 20;                  // LTO flag (bit 19)
    w |= uint64_t((sign << 5) | (mag & 0x1F)) << 11;
    w |= uint64_t(hh & 0x1F) << 6;
    w |= uint64_t(mm & 0x3F);
    return { uint8_t(w >> 32), uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w) };
}
/** Clause 5.4.5.3: every duration is a 16-bit count of seconds. */
static std::vector<uint8_t> dur(int seconds) {
    return tlv(0x81, { uint8_t(seconds >> 8), uint8_t(seconds) });
}
/** Clause 5.4.5.1.2, dab: domain — flags/SCIdS, ECC, EId, then a 16- or 32-bit SId. */
static std::vector<uint8_t> bearer16(int ecc, uint16_t eid, uint16_t sid) {
    return { 0x00, uint8_t(ecc), uint8_t(eid >> 8), uint8_t(eid), uint8_t(sid >> 8), uint8_t(sid) };
}

int main() {
    printf("test-dab-epg\n");

    // ── the timepoint, on its own, because everything else hangs off it ──────────────────
    {
        const auto t4 = timeShort(60000, 14, 30);
        CHECK(t4.size() == 4, "short form with no LTO is four bytes (clause 5.4.5.2)");
        EpgTime a = EpgDocument::readTime(t4.data(), t4.size());
        CHECK(a.valid() && a.mjd == 60000, "MJD round-trips");
        CHECK(a.hour == 14 && a.minute == 30, "hours and minutes round-trip");
        CHECK(a.ltoHalfHours == 0, "no LTO field means local time is UTC");
        CHECK(a.utcMinutes() == 14 * 60 + 30, "minutes since midnight");

        /* ★★★ THE LTO BYTE SHIFTS EVERYTHING AFTER IT. Decode it as though it were absent and the
         *  hours and minutes are read out of the middle of the LTO field — a schedule that is
         *  plausible, wrong, and wrong only outside the UK's winter. */
        const auto t5 = timeShortLto(60000, 14, 30, +4);          // +2 hours, Amsterdam in summer
        CHECK(t5.size() == 5, "the LTO byte makes it five");
        EpgTime b = EpgDocument::readTime(t5.data(), t5.size());
        CHECK(b.valid() && b.mjd == 60000, "★ MJD survives the LTO byte");
        CHECK(b.hour == 14 && b.minute == 30, "★ hours and minutes are read AFTER the LTO byte");
        CHECK(b.ltoHalfHours == 4, "★ +2 hours, as half-hours");

        EpgTime c = EpgDocument::readTime(timeShortLto(60000, 9, 5, -5).data(), 5);
        CHECK(c.ltoHalfHours == -5 && c.hour == 9 && c.minute == 5, "★ a negative LTO keeps its sign");

        // Too short to hold what its own flags promise: nothing, rather than a guess.
        const uint8_t truncated[4] = { 0x00, 0x00, 0x10, 0x00 };   // LTO flag set, only 4 bytes
        CHECK(!EpgDocument::readTime(truncated, 4).valid(), "★ a truncated timepoint is not decoded");
        CHECK(!EpgDocument::readTime(t4.data(), 3).valid(), "three bytes is never a timepoint");
    }

    // ── a whole schedule, as a Dutch or German multiplex would send one ──────────────────
    {
        /* epg
         *   tokenTable: 0x01 = "BBC Radio ", 0x02 = " with "
         *   schedule
         *     scope startTime, stopTime, serviceScope(bearer E1 C185 C221)
         *     programme id/shortId, mediumName, mediaDescription(shortDescription), location(time)
         *     programme ... containing a programmeEvent
         */
        std::vector<uint8_t> tokens;
        tokens += std::vector<uint8_t>{ 0x01, 10 }; { const char* s = "BBC Radio "; tokens.insert(tokens.end(), s, s + 10); }
        tokens += std::vector<uint8_t>{ 0x02, 6  }; { const char* s = " with ";     tokens.insert(tokens.end(), s, s + 6);  }

        std::vector<uint8_t> scope;
        scope += tlv(0x80, timeShort(60000, 0, 0));
        scope += tlv(0x81, timeShort(60001, 0, 0));
        scope += tlv(kEpgElServiceScope, tlv(0x80, bearer16(0xE1, 0xC185, 0xC221)));

        std::vector<uint8_t> p1;
        p1 += str(0x80, "crid://bbc.co.uk/p1");
        p1 += tlv(0x81, { 0x00, 0x12, 0x34 });                       // shortId, 24-bit
        /* ★ The name uses BOTH tokens: "\x01" "1" "\x02" "Greg James" -> "BBC Radio 1 with Greg
         *  James". A decoder that ignores the table renders two control bytes into the title. */
        p1 += str(kEpgElMediumName, "\x01" "1" "\x02" "Greg James");
        p1 += tlv(kEpgElMediaDescription, str(kEpgElShortDescription, "The best new music"));
        { std::vector<uint8_t> loc, tm; tm += tlv(0x80, timeShortLto(60000, 6, 30, 2)); tm += dur(10800);
          loc += tlv(kEpgElTime, tm); p1 += tlv(kEpgElLocation, loc); }

        std::vector<uint8_t> ev;
        ev += str(kEpgElShortName, "News");
        { std::vector<uint8_t> loc, tm; tm += tlv(0x80, timeShort(60000, 10, 0)); tm += dur(300);
          loc += tlv(kEpgElTime, tm); ev += tlv(kEpgElLocation, loc); }

        std::vector<uint8_t> p2;
        p2 += str(kEpgElLongName, "\x01" "2 Breakfast");
        { std::vector<uint8_t> loc, tm; tm += tlv(0x80, timeShort(60000, 9, 30)); tm += dur(9000);
          loc += tlv(kEpgElTime, tm); p2 += tlv(kEpgElLocation, loc); }
        p2 += tlv(kEpgElProgrammeEvent, ev);

        std::vector<uint8_t> sched;
        sched += tlv(kEpgElScope, scope);
        sched += tlv(kEpgElProgramme, p1);
        sched += tlv(kEpgElProgramme, p2);

        std::vector<uint8_t> body;
        body += tlv(kEpgElTokenTable, tokens);
        body += tlv(kEpgElSchedule, sched);
        const std::vector<uint8_t> doc = tlv(kEpgElEpg, body);

        auto out = EpgDocument::parse(doc.data(), doc.size());
        CHECK(out.size() == 1, "one schedule");
        if (out.size() == 1) {
            const EpgSchedule& s = out[0];
            CHECK(s.sid == 0xC221 && s.ecc == 0xE1 && s.eid == 0xC185,
                  "★ the schedule is bound to a service by the serviceScope bearer id");
            CHECK(s.scopeStart.valid() && s.scopeStart.mjd == 60000, "scope start");
            CHECK(s.scopeStop.valid() && s.scopeStop.mjd == 60001, "scope stop");
            CHECK(s.programmes.size() == 3, "★ two programmes and the event inside the second");
            if (s.programmes.size() == 3) {
                CHECK(s.programmes[0].name == "BBC Radio 1 with Greg James",
                      "★★★ BOTH tokens expanded — the table applies to all character data (5.5.1)");
                CHECK(s.programmes[0].shortId == 0x1234, "shortId is a 24-bit integer (5.4.4)");
                CHECK(s.programmes[0].id == "crid://bbc.co.uk/p1", "the CRID");
                CHECK(s.programmes[0].description == "The best new music", "the short description");
                CHECK(s.programmes[0].start.hour == 6 && s.programmes[0].start.minute == 30,
                      "★ the start time, read past its LTO byte");
                CHECK(s.programmes[0].start.ltoHalfHours == 2, "+1 hour local offset");
                CHECK(s.programmes[0].durationSec == 10800, "three hours, in seconds");
                CHECK(!s.programmes[0].isEvent, "a programme is not an event");
                CHECK(s.programmes[1].name == "BBC Radio 2 Breakfast", "★ longName is used, token expanded");
                CHECK(s.programmes[2].name == "News" && s.programmes[2].isEvent,
                      "★ the programmeEvent follows its parent and is marked as an event");
                CHECK(s.programmes[2].durationSec == 300, "the event's own duration");
            }
        }
    }

    // ── the name a broadcaster actually chose ───────────────────────────────────────────
    {
        /* ★ TS 102 818 lets a broadcaster send any subset of short/medium/longName. A receiver
         *  that insists on one of them draws blank rows for everyone who chose another. */
        auto build = [](const std::vector<uint8_t>& nameEl) {
            std::vector<uint8_t> pr = nameEl, sch, body;
            sch += tlv(kEpgElProgramme, pr);
            body += tlv(kEpgElSchedule, sch);
            return tlv(kEpgElEpg, body);
        };
        auto nameOf = [&](const std::vector<uint8_t>& d) {
            auto o = EpgDocument::parse(d.data(), d.size());
            return (o.size() == 1 && o[0].programmes.size() == 1) ? o[0].programmes[0].name : std::string("<none>");
        };
        CHECK(nameOf(build(str(kEpgElShortName,  "Short")))  == "Short",  "shortName alone is used");
        CHECK(nameOf(build(str(kEpgElMediumName, "Medium"))) == "Medium", "mediumName alone is used");
        CHECK(nameOf(build(str(kEpgElLongName,   "Long")))   == "Long",   "longName alone is used");
        std::vector<uint8_t> all = str(kEpgElShortName, "Short");
        all += str(kEpgElMediumName, "Medium");
        all += str(kEpgElLongName, "Long");
        CHECK(nameOf(build(all)) == "Long", "★ the longest name that was sent wins");
    }

    // ── what must NOT happen to a corrupt object ────────────────────────────────────────
    {
        /* ★★★ The same guarantee the dynamic label has to give: whatever arrives, nothing that
         *  reaches the stats JSON may be invalid UTF-8 or carry a raw control byte. A programme
         *  title goes to the browser exactly as a DL Plus tag does, and the RDS lesson was that a
         *  string which breaks JSON.parse takes the pane with it. */
        auto ctrlFree = [](const std::string& s) {
            for (char c : s) if (uint8_t(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') return false;
            return true;
        };
        // A title full of token tags for which NO token was defined.
        std::vector<uint8_t> pr = str(kEpgElMediumName, std::string("\x01\x02\x03" "Real\x0B\x0C", 9));
        std::vector<uint8_t> sch, body;
        sch += tlv(kEpgElProgramme, pr);
        body += tlv(kEpgElSchedule, sch);
        auto d = tlv(kEpgElEpg, body);
        auto o = EpgDocument::parse(d.data(), d.size());
        CHECK(o.size() == 1 && o[0].programmes.size() == 1, "the programme still parses");
        if (o.size() == 1 && o[0].programmes.size() == 1) {
            CHECK(ctrlFree(o[0].programmes[0].name), "★★★ undefined token tags are dropped, never emitted raw");
            CHECK(o[0].programmes[0].name == "Real", "★ and what is left is the real text");
        }

        // A length that runs past the end of the object.
        std::vector<uint8_t> bad{ uint8_t(kEpgElEpg), 0x40, uint8_t(kEpgElSchedule), 0x40, 0x00 };
        auto o2 = EpgDocument::parse(bad.data(), bad.size());
        CHECK(o2.empty(), "★ a length past the end yields nothing, not a fragment");

        // Random bytes, at length, must neither crash nor invent schedules.
        uint32_t rng = 0xC0FFEEu;
        auto next = [&rng]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        size_t made = 0;
        for (int i = 0; i < 40000; ++i) {
            std::vector<uint8_t> junk(4 + next() % 60);
            for (auto& b : junk) b = uint8_t(next());
            if (i & 1) junk[0] = uint8_t(kEpgElEpg);     // half of them look like a document
            auto r = EpgDocument::parse(junk.data(), junk.size());
            for (const auto& s : r) {
                made += s.programmes.size();
                for (const auto& p : s.programmes) {
                    if (!ctrlFree(p.name) || !ctrlFree(p.description)) { CHECK(false, "★★★ a corrupt object produced an unsendable string"); i = 40000; break; }
                }
            }
        }
        CHECK(fails == 0 || true, "fuzz completed");
        printf("  (fuzz: 40000 corrupt objects, %zu programmes invented and all of them printable)\n", made);
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
