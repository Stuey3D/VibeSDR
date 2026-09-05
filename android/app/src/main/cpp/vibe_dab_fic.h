// vibe_dab_fic.h — FIB / FIG parsing: the ensemble, its services and their labels.
//
// This is the layer that produces THE STATION LIST, and it does so with no audio decoded at all —
// which is why it is the honest milestone to build towards: if services with correct names and
// labels appear, everything from the null symbol up through Viterbi is demonstrably working.
//
// Structure (EN 300 401 clause 5.2.2): the FIC carries FIBs of 32 bytes — 30 of payload plus a
// CRC. Each FIB packs FIGs: one header byte (3-bit type, 5-bit length) then that many bytes.
// "FIGs shall not be split between FIBs", which is what makes a FIB independently parseable and
// therefore what makes a single CRC failure cost only that FIB.
//
// What we parse, and why these first:
//   FIG 0/1  sub-channel organisation  — where a service sits in the mux, and its protection
//   FIG 0/2  service organisation      — the services and their components
//   FIG 1/0  ensemble label            — the multiplex name
//   FIG 1/1  programme service label   — the station name
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "vibe_dab_fec.h"

namespace vibedab {

struct SubChannel {
    int  id        = -1;
    int  startCu   = 0;     ///< where it begins in the CIF
    int  sizeCu    = 0;
    bool eep       = true;  ///< equal (true) or unequal (false) error protection
    int  protLevel = 0;     ///< EEP protection level 1-4 (stored 0-3), or the UEP table index
    /** ★★★ EEP OPTION: 0 = EEP-A, 1 = EEP-B. A SEPARATE FIELD because it is a separate field on
     *  air, and folding it into protLevel is what broke 11D — see the parse in FIG 0/1. */
    int  option    = 0;
};

struct ServiceComponent {
    int  subChId  = -1;
    int  scType   = 0;      ///< audio type: 0 = MPEG Layer II (DAB), 63 = DAB+ (AAC)
    bool primary  = false;
};

struct Service {
    uint32_t sid = 0;                 ///< service identifier — half the recall key
    std::string label;                ///< 16-char programme service label
    std::vector<ServiceComponent> components;
};

/** Everything the FIC has told us so far. Persistent across FIBs on purpose — see the note on
 *  `merge`, and the "station list that does not flap" goal in BRIEF-dab.md. */
struct Ensemble {
    uint16_t eid = 0;
    std::string label;
    std::map<uint32_t, Service> services;      ///< by SId, so repeats update rather than duplicate
    std::map<int, SubChannel>   subChannels;   ///< by SubChId
    bool empty() const { return services.empty() && label.empty(); }
};

/** ★★ DAB labels are 16 bytes of EBU Latin, space padded, with a 16-bit character flag field
 *  saying which characters may be dropped for an 8-character abbreviation. We keep the full label
 *  and simply trim the padding.
 *  ★ Bytes above 0x7F are EBU Latin, not ISO-8859-1 or UTF-8 — mapping them properly is a table we
 *    do not have yet, so they are replaced rather than emitted as invalid UTF-8. A '?' in a station
 *    name is a visible, harmless defect; a malformed byte sequence breaks the JSON that carries it,
 *    which is the fault this project has already learned to fear (see vibe_utf8.h). */
inline std::string labelFromBytes(const uint8_t* p, size_t n = 16) {
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i) s += (p[i] >= 0x20 && p[i] < 0x7F) ? char(p[i]) : '?';
    while (!s.empty() && (s.back() == ' ' || s.back() == '?')) s.pop_back();
    return s;
}

/** Parse one 32-byte FIB into `e`. Returns false when the CRC fails (and changes nothing).
 *
 *  ★★★ A FAILED CRC MUST CHANGE NOTHING. Half-applying a corrupt FIB is how a station list starts
 *      flapping — a service appears with a mangled name, then corrects, then vanishes. The CRC is
 *      checked before a single byte is interpreted.
 */
inline bool parseFib(const uint8_t* fib32, Ensemble& e) {
    if (!fibCrcOk(fib32)) return false;

    size_t i = 0;
    while (i < 30) {
        const uint8_t hdr = fib32[i];
        if (hdr == 0xFF) break;                       // end-of-FIB padding
        const int type = hdr >> 5;
        const int len  = hdr & 0x1F;                  // bytes FOLLOWING the header
        if (len == 0 || i + 1 + size_t(len) > 30) break;
        const uint8_t* p = fib32 + i + 1;

        if (type == 0) {
            const int ext = p[0] & 0x1F;
            const uint8_t* q = p + 1;
            const size_t qn = size_t(len) - 1;
            if (ext == 1) {                            // sub-channel organisation
                size_t j = 0;
                while (j + 3 <= qn) {
                    SubChannel sc;
                    sc.id      = q[j] >> 2;
                    sc.startCu = ((q[j] & 0x03) << 8) | q[j + 1];
                    const bool shortForm = (q[j + 2] & 0x80) == 0;
                    if (shortForm) {
                        sc.eep       = false;                       // UEP table
                        sc.protLevel = q[j + 2] & 0x3F;
                        j += 3;
                    } else {
                        if (j + 4 > qn) break;
                        /* ★★★ THREE FIELDS, NOT TWO. EN 300 401 §6.2.1 packs this byte as
                         *  [1 bit short/long][3 bits Option][2 bits protection level][2 bits of
                         *  the 10-bit size]. We read `(>>2) & 0x07`, which takes the LOW BIT OF
                         *  THE OPTION together with both protection bits.
                         *  ★★★ WITH EEP-A (option 0) THAT IS ACCIDENTALLY CORRECT, which is why
                         *      12B decoded and 11D did not: an EEP-B sub-channel came out with a
                         *      protection level of 4-7, off the end of a table that holds 1-4, so
                         *      EVERY audio frame failed while the FIC stayed at a perfect 1.000.
                         *      Measured 2026-09-05: talkSPORT mp2In 1865 / mp2Bad 1865 / mp2Out 0.
                         *  ★ And the option was never stored at all, so the receiver was left
                         *    GUESSING it — trying EEP-A then EEP-B and taking whichever matched
                         *    the coded size, which two profiles can do. It is on air; read it. */
                        sc.eep       = true;
                        sc.option    = (q[j + 2] >> 4) & 0x07;
                        sc.protLevel = (q[j + 2] >> 2) & 0x03;
                        sc.sizeCu    = ((q[j + 2] & 0x03) << 8) | q[j + 3];
                        j += 4;
                    }
                    if (sc.id >= 0) e.subChannels[sc.id] = sc;
                }
            } else if (ext == 2) {                     // service organisation
                size_t j = 0;
                while (j + 3 <= qn) {
                    // ★ P/D flag (bit 7 of the first byte of the header we already consumed)
                    //   decides whether the SId is 16 or 32 bits. Programme services are 16.
                    const bool pd = (p[0] & 0x80) != 0;
                    uint32_t sid;
                    if (pd) { if (j + 4 > qn) break; sid = (uint32_t(q[j]) << 24) | (uint32_t(q[j+1]) << 16)
                                                        | (uint32_t(q[j+2]) << 8) | q[j+3]; j += 4; }
                    else    { sid = (uint32_t(q[j]) << 8) | q[j + 1]; j += 2; }
                    if (j >= qn) break;
                    const int ncomp = q[j] & 0x0F; ++j;
                    Service& s = e.services[sid];
                    s.sid = sid;
                    s.components.clear();
                    for (int c = 0; c < ncomp && j + 2 <= qn; ++c, j += 2) {
                        const int tmid = q[j] >> 6;
                        ServiceComponent sc;
                        if (tmid == 0) {                            // MSC stream audio
                            sc.scType  = q[j] & 0x3F;
                            sc.subChId = (q[j + 1] >> 2) & 0x3F;
                            sc.primary = (q[j + 1] & 0x02) != 0;
                        }
                        s.components.push_back(sc);
                    }
                }
            }
        } else if (type == 1) {
            const int ext = p[0] & 0x07;
            if (ext == 0 && len >= 1 + 2 + 16) {        // ensemble label
                e.eid   = uint16_t((p[1] << 8) | p[2]);
                e.label = labelFromBytes(p + 3);
            } else if (ext == 1 && len >= 1 + 2 + 16) { // programme service label
                const uint32_t sid = uint32_t((p[1] << 8) | p[2]);
                Service& s = e.services[sid];
                s.sid   = sid;
                s.label = labelFromBytes(p + 3);
            }
        }
        i += 1 + size_t(len);
    }
    return true;
}

}  // namespace vibedab
