// vibe_dab_epg.h — the electronic programme guide off the air: binary Programme Information
// (ETSI TS 102 371 V3.3.1, the encoding; ETSI TS 102 818, the schema it encodes).
//
// ★★★ WHAT THIS IS. The same SPI carousel that carries the multiplex's logos can carry its
//     SCHEDULE: what is on now, what is on next, and for days ahead. The objects are told apart
//     by their MOT content type and subtype (clause 6.4.2, table 11):
//         7/0  Service Information   — the ensemble, its services, their logos  (vibe_dab_spi.h)
//         7/1  Programme Information — the schedule                            (this file)
//         7/2  Group Information     — series and groupings
//     Until now only 7/0 was read and the rest were dropped by the filter in DabService::pumpSpi.
//
// ★★★ NOTHING IN THE UK TRANSMITS THIS, AND IT IS BUILT ANYWAY. Measured 2026-09-08: 12B (BBC
//     National) carries an SPI carousel whose only document is serviceInformation, and 7D (NNDAB)
//     carries no SPI carousel at all. So this decoder has nothing to decode here and cannot be
//     verified on air from Northampton. Stuart's instruction, and the precedent he set it by:
//     "RT+ isnt broadcast in the UK but it is in the Netherlands on FM so we included it even
//     though I could never test it myself. So the full technical implementation needs to be added
//     even if its just a line in the advanced window that will not populate in the UK."
//     ★ It is therefore written against the CLAUSE, and tested against documents synthesised from
//       the spec's own tag tables (test-dab-epg.cpp) — not against a capture, because there is no
//       capture to be had. Every field below cites where it comes from, so the next person to hold
//       a Dutch or German capture can check it clause by clause rather than guessing what I meant.
//
// ★★ THE STRING TOKEN TABLE IS NOT OPTIONAL (clause 5.5). Up to 16 single-byte tags stand in for
//    repeated strings, and "the token table applies to all character data within the current
//    binary object". A decoder that ignores it does not merely miss a compression win: it renders
//    control bytes into the middle of every programme title that used one. The tags are the
//    non-printing bytes 0x01-0x08, 0x0B, 0x0C, 0x0E-0x13, chosen so they cannot collide with text.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace vibedab {

/** Element tags — TS 102 371 annex D, table D.1. Only those on the path to a schedule. */
enum : int {
    kEpgElEpg              = 0x02,   ///< top level, the EPG document
    kEpgElServiceInfo      = 0x03,   ///< top level, the SI document (vibe_dab_spi.h reads these)
    kEpgElTokenTable       = 0x04,
    kEpgElDefaultLanguage  = 0x06,
    kEpgElShortName        = 0x10,
    kEpgElMediumName       = 0x11,
    kEpgElLongName         = 0x12,
    kEpgElMediaDescription = 0x13,
    kEpgElGenre            = 0x14,
    kEpgElLocation         = 0x19,
    kEpgElShortDescription = 0x1A,
    kEpgElLongDescription  = 0x1B,
    kEpgElProgramme        = 0x1C,
    kEpgElProgrammeGroups  = 0x20,
    kEpgElSchedule         = 0x21,
    kEpgElProgrammeGroup   = 0x23,
    kEpgElScope            = 0x24,
    kEpgElServiceScope     = 0x25,
    kEpgElTime             = 0x2C,
    kEpgElBearer           = 0x2D,   ///< within location/onDemand (0x29 is the one inside service)
    kEpgElProgrammeEvent   = 0x2E,
    kEpgElRelativeTime     = 0x2F,
};

/** ★ A decoded timepoint (clause 5.4.5.2). The binary form carries UTC plus the offset to LOCAL
 *  time — the reverse of the XML form, which carries local time and the offset to UTC — "so that
 *  the format is the same as the time delivered in the FIC". It is indeed FIG 0/10's layout with
 *  the LSI bit reserved and an optional LTO byte inserted, which is why the field order below
 *  will look familiar from vibe_dab_fic.h. */
struct EpgTime {
    int mjd  = -1;      ///< Modified Julian Date, -1 when absent
    int hour = 0, minute = 0, second = 0;
    int ltoHalfHours = 0;   ///< offset to LOCAL time, signed half-hours (0 when no LTO field)
    bool valid() const { return mjd >= 0; }
    /** Minutes since midnight UTC — the form a schedule is actually sorted and searched by. */
    int utcMinutes() const { return hour * 60 + minute; }
};

/** One programme (element 0x1C) or programme event (0x2E) in a schedule. */
struct EpgProgramme {
    uint32_t shortId = 0;          ///< attribute 0x81, a 24-bit unsigned integer (clause 5.4.4)
    std::string id;                ///< attribute 0x80, the CRID
    std::string name;              ///< the longest of shortName/mediumName/longName that was sent
    std::string description;       ///< the longer of short/longDescription
    EpgTime start;                 ///< location → time, attribute 0x80
    int durationSec = 0;           ///< location → time, attribute 0x81 (16-bit seconds, 5.4.5.3)
    bool isEvent = false;          ///< a programmeEvent nested inside a programme
};

/** The schedule for one service. `sid` comes from the scope's serviceScope bearer id. */
struct EpgSchedule {
    int ecc = -1; uint16_t eid = 0; uint32_t sid = 0;
    EpgTime scopeStart, scopeStop;
    std::vector<EpgProgramme> programmes;
};

class EpgDocument {
public:
    /** Parse one binary Programme Information object (MOT content type 7, subtype 1). */
    static std::vector<EpgSchedule> parse(const uint8_t* d, size_t n) {
        EpgDocument doc;
        std::vector<EpgSchedule> out;
        if (!d || n < 2) return out;
        int tag; size_t p = 0, len;
        // The document is one top-level element; anything that is not `epg` is not ours.
        if (!readTl(d, n, p, tag, len) || tag != kEpgElEpg) return out;
        doc.walkEpg(d + p, len, out);
        return out;
    }

    /** ★ Exposed for the tests: tag/length decoding is where a TLV parser goes wrong first. */
    static bool readTl(const uint8_t* d, size_t n, size_t& p, int& tag, size_t& len) {
        if (p + 2 > n) return false;
        tag = d[p];
        size_t l = d[p + 1];
        p += 2;
        /* Clause 5.2: 0xFE introduces a 16-bit length, 0xFF a 24-bit one. Lengths 0x00-0xFD are
         * the length itself. */
        if (l == 0xFE) { if (p + 2 > n) return false; l = (size_t(d[p]) << 8) | d[p + 1]; p += 2; }
        else if (l == 0xFF) {
            if (p + 3 > n) return false;
            l = (size_t(d[p]) << 16) | (size_t(d[p + 1]) << 8) | d[p + 2]; p += 3;
        }
        if (p + l > n) return false;         // ★ a length past the end is a corrupt object, not a clue
        len = l;
        return true;
    }

    /** ★★★ The timepoint of clause 5.4.5.2. Sizes are fixed by the two flags and nothing else:
     *      no LTO + short UTC = 4 bytes    LTO + short UTC = 5
     *      no LTO + long  UTC = 6          LTO + long  UTC = 7
     *  ★ Bit layout, MSB first from byte 0: Rfa(1) Date(17) Rfa(1) LTO flag(1) UTC flag(1), then
     *    the LTO byte if the LTO flag is set — Rfa(2) Sign(1) Half-hours(5) — then UTC: short form
     *    is hours(5) minutes(6); long form adds seconds(6) and milliseconds(10). */
    static EpgTime readTime(const uint8_t* v, size_t len) {
        EpgTime t;
        if (len < 4) return t;
        const uint32_t w = (uint32_t(v[0]) << 24) | (uint32_t(v[1]) << 16)
                         | (uint32_t(v[2]) << 8)  |  uint32_t(v[3]);
        const int mjd     = int((w >> 14) & 0x1FFFF);      // bits 1..17
        const bool ltoF   = ((w >> 12) & 1) != 0;          // bit 19
        const bool utcLong= ((w >> 11) & 1) != 0;          // bit 20
        size_t need = 4 + (ltoF ? 1 : 0) + (utcLong ? 2 : 0);
        if (len < need) return t;
        size_t bit = 21;                                    // where the variable part begins
        const uint8_t* q = v;
        auto bits = [&](size_t start, size_t count) -> uint32_t {
            uint32_t r = 0;
            for (size_t i = 0; i < count; ++i) {
                const size_t b = start + i;
                r = (r << 1) | ((q[b >> 3] >> (7 - (b & 7))) & 1);
            }
            return r;
        };
        if (ltoF) {
            const uint32_t lto = bits(bit, 8);
            const int sign = (lto >> 5) & 1;                // Rfa(2) Sign(1) Half-hours(5)
            const int half = int(lto & 0x1F);
            t.ltoHalfHours = sign ? -half : half;
            bit += 8;
        }
        t.hour   = int(bits(bit, 5)); bit += 5;
        t.minute = int(bits(bit, 6)); bit += 6;
        if (utcLong) { t.second = int(bits(bit, 6)); bit += 6; /* then 10 bits of milliseconds */ }
        t.mjd = mjd;
        return t;
    }

private:
    /* ── the string token table (clause 5.5) ──────────────────────────────────────────────── */
    std::string tokens_[256];
    bool        hasToken_[256] = { false };
    /** ★ Events found while reading one programme. A member rather than a return value because
     *  readProgramme recurses into itself for them, and the caller wants one flat list. */
    std::vector<EpgProgramme> events_;

    /** ★ Only these 16 bytes may be token tags (clause 5.5.2); 0x00, 0x09, 0x0A and 0x0D are
     *  excluded so that a token can never be confused with whitespace a decoder might trim. */
    static bool tokenTagAllowed(int t) {
        switch (t) {
            case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
            case 0x08: case 0x0B: case 0x0C: case 0x0E: case 0x0F: case 0x10: case 0x11:
            case 0x12: case 0x13: return true;
            default: return false;
        }
    }

    void readTokenTable(const uint8_t* d, size_t n) {
        size_t p = 0;
        while (p + 2 <= n) {
            const int tag = d[p];
            const size_t len = d[p + 1];
            p += 2;
            if (p + len > n) return;                        // truncated table: keep what parsed
            /* ★ "Token strings shall never include references to other tokens", so a token's own
             *  bytes are taken literally and expansion cannot recurse — which is also what stops a
             *  malicious or corrupt table from expanding without bound. */
            if (tokenTagAllowed(tag)) {
                tokens_[tag].assign(reinterpret_cast<const char*>(d + p), len);
                hasToken_[tag] = true;
            }
            p += len;
        }
    }

    /** Replace every token tag in a string with its table entry. Strings are UTF-8 (clause 5.4.2);
     *  a token tag is a byte below 0x20, which UTF-8 can never use as a continuation byte, so the
     *  substitution cannot split a character. */
    std::string expand(const uint8_t* v, size_t len) const {
        std::string s;
        s.reserve(len + 16);
        for (size_t i = 0; i < len; ++i) {
            const uint8_t c = v[i];
            if (c < 0x20 && hasToken_[c]) s += tokens_[c];
            else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;  // ★ never emit a raw control byte
            else s += char(c);
        }
        return s;
    }

    /* ── the document ─────────────────────────────────────────────────────────────────────── */

    /** The `epg` element: attributes, then the token table and default language, then schedules.
     *  ★ The token table "shall occur after the attributes and before any elements", so a single
     *    forward pass sees it before anything that uses it. */
    void walkEpg(const uint8_t* d, size_t n, std::vector<EpgSchedule>& out) {
        size_t p = 0; int tag; size_t len;
        while (readTl(d, n, p, tag, len)) {
            const uint8_t* v = d + p;
            if (tag == kEpgElTokenTable)          readTokenTable(v, len);
            else if (tag == kEpgElSchedule)       walkSchedule(v, len, out);
            else if (tag == kEpgElProgrammeGroups) { /* GI: series groupings, not a schedule */ }
            p += len;
        }
    }

    void walkSchedule(const uint8_t* d, size_t n, std::vector<EpgSchedule>& out) {
        EpgSchedule sch;
        size_t p = 0; int tag; size_t len;
        while (readTl(d, n, p, tag, len)) {
            const uint8_t* v = d + p;
            if (tag == kEpgElScope)          readScope(v, len, sch);
            else if (tag == kEpgElProgramme) {
                events_.clear();
                sch.programmes.push_back(readProgramme(v, len, sch, false));
                /* ★ The events follow their parent, so "now" is still found by scanning forward. */
                for (auto& ev : events_) sch.programmes.push_back(std::move(ev));
                events_.clear();
            }
            p += len;
        }
        if (!sch.programmes.empty() || sch.sid) out.push_back(std::move(sch));
    }

    /** scope: startTime 0x80, stopTime 0x81, and a serviceScope child naming the service. */
    void readScope(const uint8_t* d, size_t n, EpgSchedule& sch) {
        size_t p = 0; int tag; size_t len;
        while (readTl(d, n, p, tag, len)) {
            const uint8_t* v = d + p;
            if (tag == 0x80)                    sch.scopeStart = readTime(v, len);
            else if (tag == 0x81)               sch.scopeStop  = readTime(v, len);
            else if (tag == kEpgElServiceScope) readServiceScope(v, len, sch);
            p += len;
        }
    }

    void readServiceScope(const uint8_t* d, size_t n, EpgSchedule& sch) {
        size_t p = 0; int tag; size_t len;
        while (readTl(d, n, p, tag, len)) {
            if (tag == 0x80) readBearerId(d + p, len, sch.ecc, sch.eid, sch.sid);
            p += len;
        }
    }

    /** ★ bearerURI, dab: domain (clause 5.4.5.1.2) — the same field vibe_dab_spi.h reads for a
     *  service's logo, and it must stay the same shape in both:
     *      byte 0: Rfa(3) ens(1)? sidFlag(1) SCIdS(4) — bit 0x10 says the SId is 32-bit
     *      byte 1: ECC     bytes 2-3: EId     then the SId, 16 or 32 bits. */
    static void readBearerId(const uint8_t* v, size_t len, int& ecc, uint16_t& eid, uint32_t& sid) {
        if (len < 6) return;
        const bool sid32 = (v[0] & 0x10) != 0;
        ecc = v[1];
        eid = uint16_t((v[2] << 8) | v[3]);
        if (sid32) { if (len >= 8) sid = (uint32_t(v[4]) << 24) | (uint32_t(v[5]) << 16) | (uint32_t(v[6]) << 8) | v[7]; }
        else       { sid = (uint32_t(v[4]) << 8) | v[5]; }
    }

    EpgProgramme readProgramme(const uint8_t* d, size_t n, const EpgSchedule& sch, bool isEvent) {
        EpgProgramme pr;
        pr.isEvent = isEvent;
        size_t p = 0; int tag; size_t len;
        std::string shortName, mediumName, longName, shortDesc, longDesc;
        while (readTl(d, n, p, tag, len)) {
            const uint8_t* v = d + p;
            switch (tag) {
                case 0x80: pr.id = expand(v, len); break;                       // id (CRID)
                case 0x81:                                                      // shortId, 24-bit
                    if (len >= 3) pr.shortId = (uint32_t(v[0]) << 16) | (uint32_t(v[1]) << 8) | v[2];
                    break;
                case kEpgElShortName:  shortName  = expand(v, len); break;
                case kEpgElMediumName: mediumName = expand(v, len); break;
                case kEpgElLongName:   longName   = expand(v, len); break;
                case kEpgElLocation:   readLocation(v, len, pr);    break;
                case kEpgElMediaDescription: readMediaDescription(v, len, shortDesc, longDesc); break;
                /* ★ A programmeEvent is a segment INSIDE a programme — the individual items of a
                 *  magazine show, or the tracks of a concert. Collected flat and marked, because
                 *  a listener reading "what is on" wants the same list either way, and a tree
                 *  would only be a tree for the handful of broadcasters that use them. */
                case kEpgElProgrammeEvent: events_.push_back(readProgramme(v, len, sch, true)); break;
                default: break;
            }
            p += len;
        }
        /* ★ The LONGEST name that was sent. A basic-profile document may carry any subset of the
         *  three, and a receiver that insists on one of them shows blank rows for a station that
         *  chose a different one (TS 102 818 leaves the choice to the broadcaster). */
        pr.name = !longName.empty() ? longName : !mediumName.empty() ? mediumName : shortName;
        pr.description = !longDesc.empty() ? longDesc : shortDesc;
        (void)sch;
        return pr;
    }

    void readMediaDescription(const uint8_t* d, size_t n, std::string& shortDesc, std::string& longDesc) {
        size_t p = 0; int tag; size_t len;
        while (readTl(d, n, p, tag, len)) {
            const uint8_t* v = d + p;
            if (tag == kEpgElShortDescription && shortDesc.empty()) shortDesc = expand(v, len);
            else if (tag == kEpgElLongDescription && longDesc.empty()) longDesc = expand(v, len);
            p += len;
        }
    }

    /** location → time (0x2C): attribute 0x80 the start, 0x81 the duration in seconds. */
    void readLocation(const uint8_t* d, size_t n, EpgProgramme& pr) {
        size_t p = 0; int tag; size_t len;
        while (readTl(d, n, p, tag, len)) {
            const uint8_t* v = d + p;
            if (tag == kEpgElTime || tag == kEpgElRelativeTime) {
                size_t q = 0; int t2; size_t l2;
                while (readTl(v, len, q, t2, l2)) {
                    if (t2 == 0x80 && tag == kEpgElTime) pr.start = readTime(v + q, l2);
                    /* Clause 5.4.5.3: every duration is a 16-bit unsigned count of seconds. */
                    else if (t2 == 0x81 && l2 >= 2) pr.durationSec = (int(v[q]) << 8) | v[q + 1];
                    q += l2;
                }
            }
            p += len;
        }
    }
};

}  // namespace vibedab
