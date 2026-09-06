// vibe_dab_fic.h — FIB / FIG parsing: the ensemble, its services and their labels.
//
// This is the layer that produces THE STATION LIST, and it does so with no audio decoded at all —
// which is why it is the honest milestone to build towards: if services with correct names and
// labels appear, everything from the null symbol up through Viterbi is demonstrably working.
//
// Structure (EN 300 401 V2.2.1 clause 5.2.2): the FIC carries FIBs of 32 bytes — 30 of payload
// plus a CRC. Each FIB packs FIGs: one header byte (3-bit type, 5-bit length) then that many
// bytes. "FIGs shall not be split between FIBs", which is what makes a FIB independently
// parseable and therefore what makes a single CRC failure cost only that FIB.
//
// What we parse (2026-09-07 — audited against the standard and the open decoders):
//   FIG 0/0  ensemble information       — EId, CIF count (TII needs its phase), change flags
//   FIG 0/1  sub-channel organisation   — where a service sits in the mux, and its protection
//   FIG 0/2  service organisation       — the services and their components
//   FIG 0/7  configuration information  — service COUNT (is the MCI complete?) and version
//   FIG 0/8  component global definition — SCIdS, which binds labels and user apps to components
//   FIG 0/9  country / LTO / ECC        — what makes an EId and a SId unique in the world
//   FIG 0/13 user application info      — which X-PAD application carries the slideshow
//   FIG 0/17 programme type
//   FIG 1/0  ensemble label             — the multiplex name
//   FIG 1/1  programme service label    — the station name
//   FIG 1/4  service component label    — secondary components (TS 103 176 6.2.1 says shall)
//   FIG 1/5  data service label
//
// ★★★ THE TYPE 0 HEADER BYTE IS C/N (b7), OE (b6), P/D (b5), Extension (b4-b0) — clause 5.2.2.1.
//     The previous parser read P/D from BIT 7, which is C/N: a FIG 0/2 announcing the NEXT
//     configuration was parsed as 32-bit SIds and written into the live service map, and a
//     genuine data service (P/D = 1) was read as pairs of 16-bit ones. Found by comparing against
//     welle.io's reader, then confirmed in the clause. C/N = 1 and OE = 1 FIGs describe a
//     configuration that is not this one, and are now skipped.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "vibe_dab_fec.h"
#include "vibe_dab_charset.h"

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

/** One user application (FIG 0/13) — what rides in a component's PAD or data channel. */
struct UserApp {
    uint16_t type    = 0;    ///< TS 101 756 table 16: 0x002 MOT SlideShow, 0x007 SPI, 0x44A Journaline…
    int      xpadApp = -1;   ///< the X-PAD application type carrying it (audio components only)
    int      dscty   = -1;
    bool     ca      = false;
};

struct ServiceComponent {
    int  tmid     = 0;      ///< 0 stream audio, 1 stream data, 3 packet data
    int  subChId  = -1;
    int  scType   = 0;      ///< audio type: 0 = MPEG Layer II (DAB), 63 = DAB+ (AAC); DSCTy for data
    int  scid     = -1;     ///< packet-mode service component id (TMId 3)
    int  scids    = -1;     ///< SCIdS from FIG 0/8 — 0 is the primary component
    bool primary  = false;
    bool ca       = false;  ///< ★ "a non-CA receiver shall not decode a component with CA flag = 1"
    std::string label;      ///< FIG 1/4, secondary components only
    std::vector<UserApp> apps;
};

struct Service {
    uint32_t sid = 0;                 ///< service identifier — half the recall key
    bool     isData = false;          ///< 32-bit SId (P/D = 1)
    std::string label;                ///< 16-char label, UTF-8
    std::string shortLabel;           ///< the abbreviated label the character flag field selects
    int      pty = -1;                ///< FIG 0/17 international programme type code
    int      ecc = -1;                ///< FIG 0/9 override; -1 = the ensemble's
    std::vector<ServiceComponent> components;

    const ServiceComponent* primaryComponent() const {
        for (const auto& c : components) if (c.primary) return &c;
        return components.empty() ? nullptr : &components.front();
    }
    /** ★ TS 103 176 6.3.3: "a service element for which incomplete MCI is provided shall be
     *  ignored". Complete means: a label, and a primary component whose sub-channel we know. */
    bool complete(const std::map<int, SubChannel>& subs) const {
        if (label.empty()) return false;
        const ServiceComponent* p = primaryComponent();
        return p && p->subChId >= 0 && subs.count(p->subChId) > 0;
    }
    bool hasSlideshow() const {
        for (const auto& c : components) for (const auto& a : c.apps) if (a.type == 0x002) return true;
        return false;
    }
};

/** Everything the FIC has told us so far. Persistent across FIBs on purpose — see the note on
 *  parseFib, and the "station list that does not flap" goal in BRIEF-dab.md. */
struct Ensemble {
    uint16_t eid = 0;
    std::string label;
    std::string shortLabel;
    int  ecc      = -1;      ///< FIG 0/9 ensemble ECC (UK: 0xE1)
    int  ltoHalfHours = 0;   ///< FIG 0/9 local time offset, signed half hours
    /** ★ FIG 0/0: CIF count mod 5000, updated every frame. The TII pattern is present only in the
     *  null symbol of frames whose CIF count mod 8 is 0..3 (14.8), so a TII decoder needs this. */
    int  cifCount = -1;
    uint8_t changeFlags = 0;      ///< FIG 0/0: 0 none, 3 complete next MCI signalled
    int  occurrenceChange = -1;   ///< the lower CIF count at which the new configuration applies
    bool alarm = false;           ///< FIG 0/0 Al flag
    int  serviceCount = -1;       ///< FIG 0/7: how many services the MCI describes
    int  configCount  = -1;       ///< FIG 0/7: reconfiguration counter, mod 1024
    std::map<uint32_t, Service> services;      ///< by SId, so repeats update rather than duplicate
    std::map<int, SubChannel>   subChannels;   ///< by SubChId
    bool empty() const { return services.empty() && label.empty(); }
    /** ★ The MCI is complete when FIG 0/7 has said how many services there are and we have that
     *  many with sub-channels. Under a weak signal this is the difference between "reading the
     *  multiplex" and a list that is genuinely finished (EN 300 401 6.4.2). */
    bool mciComplete() const {
        if (serviceCount < 0) return false;
        int n = 0;
        for (const auto& kv : services) if (kv.second.complete(subChannels)) ++n;
        return n >= serviceCount;
    }
};

/** ★★ A label is 16 bytes in the charset the FIG names (EBU Latin, 0000, is what the standard
 *  requires for type 1), with unused bytes 0x00 — "the characters are coded from byte 15 to byte
 *  0" is the standard's way of numbering bits, not a reversal: byte 15 is the FIRST byte on the
 *  wire. Trailing spaces are padding, not a name. */
inline std::string labelFromBytes(const uint8_t* p, size_t n = 16, uint8_t charset = kCharsetEbuLatin) {
    size_t end = n;
    while (end > 0 && (p[end - 1] == 0x00 || p[end - 1] == ' ')) --end;
    return dabTextToUtf8(p, end, charset);
}

/** The abbreviated label: the characters whose flag bit is set (b15 = first character). Falls
 *  back to the full label when the flag field is empty, which some ensembles transmit. */
inline std::string shortLabelFromBytes(const uint8_t* p, uint16_t flags, uint8_t charset = kCharsetEbuLatin) {
    if (flags == 0) return labelFromBytes(p, 16, charset);
    std::string s;
    for (int i = 0; i < 16; ++i)
        if (flags & (0x8000u >> i)) appendUtf8(s, charset == kCharsetEbuLatin ? ebuLatinCodePoint(p[i]) : p[i]);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

/** Parse one 32-byte FIB into `e`. Returns false when the CRC fails (and changes nothing).
 *
 *  ★★★ A FAILED CRC MUST CHANGE NOTHING. Half-applying a corrupt FIB is how a station list starts
 *      flapping — a service appears with a mangled name, then corrects, then vanishes. The CRC is
 *      checked before a single byte is interpreted.
 *  ★★ Clause 5.2.2.0: an unrecognised FIG is skipped by its length; a FIG that cannot be parsed
 *     is abandoned and the NEXT one is read. Neither aborts the FIB.
 */
inline bool parseFib(const uint8_t* fib32, Ensemble& e) {
    if (!fibCrcOk(fib32)) return false;

    size_t i = 0;
    while (i < 30) {
        const uint8_t hdr = fib32[i];
        if (hdr == 0xFF) break;                       // end marker (type 7, length 31)
        if (hdr == 0x00) break;                       // padding
        const int type = hdr >> 5;
        const int len  = hdr & 0x1F;                  // bytes FOLLOWING the header
        if (len == 0 || i + 1 + size_t(len) > 30) break;
        const uint8_t* p = fib32 + i + 1;
        i += 1 + size_t(len);                          // ★ advance FIRST: nothing below can skip it

        if (type == 0) {
            const bool cn  = (p[0] & 0x80) != 0;      // 1 = the NEXT configuration
            const bool oe  = (p[0] & 0x40) != 0;      // 1 = another ensemble
            const bool pd  = (p[0] & 0x20) != 0;      // 1 = 32-bit (data) SIds
            const int  ext = p[0] & 0x1F;
            const uint8_t* q = p + 1;
            const size_t qn = size_t(len) - 1;
            /* ★ MCI for a configuration that is not the current one, or for a different ensemble,
             *  must not be written into this one. Announcing it is what the change flags in 0/0
             *  are for; applying it early is what plays the wrong sub-channel at the switch. */
            const bool foreign = cn || oe;

            if (ext == 0 && qn >= 4) {                 // ensemble information
                e.eid          = uint16_t((q[0] << 8) | q[1]);
                e.changeFlags  = uint8_t(q[2] >> 6);
                e.alarm        = (q[2] & 0x20) != 0;
                const int hi   = q[2] & 0x1F;          // 0..19
                const int lo   = q[3];                 // 0..249
                if (hi < 20 && lo < 250) e.cifCount = hi * 250 + lo;
                e.occurrenceChange = (e.changeFlags != 0 && qn >= 5) ? int(q[4]) : -1;
            } else if (ext == 7 && qn >= 2) {          // configuration information
                e.serviceCount = q[0] >> 2;
                e.configCount  = ((q[0] & 0x03) << 8) | q[1];
            } else if (ext == 1 && !foreign) {         // sub-channel organisation
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
                         *  the 10-bit size]. Reading `(>>2) & 0x07` took the LOW BIT OF THE
                         *  OPTION together with both protection bits — accidentally right for
                         *  EEP-A (option 0), which is why 12B decoded and 11D did not (talkSPORT,
                         *  EEP-B: mp2In 1865 / mp2Bad 1865 / mp2Out 0, 2026-09-05). */
                        sc.eep       = true;
                        sc.option    = (q[j + 2] >> 4) & 0x07;
                        sc.protLevel = (q[j + 2] >> 2) & 0x03;
                        sc.sizeCu    = ((q[j + 2] & 0x03) << 8) | q[j + 3];
                        j += 4;
                    }
                    if (sc.id >= 0) e.subChannels[sc.id] = sc;
                }
            } else if (ext == 2 && !foreign) {         // service organisation
                size_t j = 0;
                while (j + (pd ? 5u : 3u) <= qn) {
                    uint32_t sid;
                    if (pd) { sid = (uint32_t(q[j]) << 24) | (uint32_t(q[j+1]) << 16)
                                  | (uint32_t(q[j+2]) << 8) | q[j+3]; j += 4; }
                    else    { sid = (uint32_t(q[j]) << 8) | q[j + 1]; j += 2; }
                    const int ncomp = q[j] & 0x0F; ++j;
                    Service& s = e.services[sid];
                    s.sid    = sid;
                    s.isData = pd;
                    std::vector<ServiceComponent> comps;
                    for (int c = 0; c < ncomp && j + 2 <= qn; ++c, j += 2) {
                        ServiceComponent sc;
                        sc.tmid = q[j] >> 6;
                        if (sc.tmid == 3) {                          // packet data: SCId 12 bits
                            sc.scid    = ((q[j] & 0x3F) << 6) | (q[j + 1] >> 2);
                        } else {                                     // stream audio / stream data
                            sc.scType  = q[j] & 0x3F;
                            sc.subChId = (q[j + 1] >> 2) & 0x3F;
                        }
                        sc.primary = (q[j + 1] & 0x02) != 0;
                        sc.ca      = (q[j + 1] & 0x01) != 0;
                        /* ★ Keep what other FIGs have attached to this component (SCIdS, a label,
                         *  its user apps) across the repeat — 0/2 is sent every frame and must
                         *  not wipe them, or the slideshow flag flaps at the FIG rate. */
                        for (const auto& old : s.components)
                            if (old.tmid == sc.tmid && old.subChId == sc.subChId && old.scid == sc.scid) {
                                sc.scids = old.scids; sc.label = old.label; sc.apps = old.apps;
                            }
                        comps.push_back(sc);
                    }
                    s.components.swap(comps);
                }
            } else if (ext == 8 && !foreign) {         // service component global definition
                size_t j = 0;
                while (j + (pd ? 6u : 4u) <= qn) {
                    uint32_t sid;
                    if (pd) { sid = (uint32_t(q[j]) << 24) | (uint32_t(q[j+1]) << 16)
                                  | (uint32_t(q[j+2]) << 8) | q[j+3]; j += 4; }
                    else    { sid = (uint32_t(q[j]) << 8) | q[j + 1]; j += 2; }
                    const bool extFlag = (q[j] & 0x80) != 0;
                    const int  scids   = q[j] & 0x0F; ++j;
                    const bool longForm = (q[j] & 0x80) != 0;
                    int subCh = -1, scid = -1;
                    if (longForm) { if (j + 2 > qn) break; scid = ((q[j] & 0x0F) << 8) | q[j + 1]; j += 2; }
                    else          { subCh = q[j] & 0x3F; j += 1; }
                    if (extFlag) j += 1;                             // Rfa byte
                    auto it = e.services.find(sid);
                    if (it == e.services.end()) continue;            // 0/2 has not arrived yet
                    for (auto& c : it->second.components)
                        if ((subCh >= 0 && c.subChId == subCh) || (scid >= 0 && c.scid == scid)) c.scids = scids;
                }
            } else if (ext == 9 && qn >= 3) {          // country, LTO and international table
                const bool extFlag = (q[0] & 0x80) != 0;
                const int  lto     = q[0] & 0x3F;
                e.ltoHalfHours = (lto & 0x20) ? -int(lto & 0x1F) : int(lto & 0x1F);
                e.ecc = q[1];
                // q[2] = international table id
                size_t j = 3;
                while (extFlag && j + 2 <= qn) {
                    const int n   = q[j] >> 6;
                    const int ecc = q[j + 1];
                    j += 2;
                    for (int k = 0; k < n && j + 2 <= qn; ++k, j += 2) {
                        const uint32_t sid = (uint32_t(q[j]) << 8) | q[j + 1];
                        auto it = e.services.find(sid);
                        if (it != e.services.end()) it->second.ecc = ecc;
                    }
                }
            } else if (ext == 13 && !foreign) {        // user application information
                size_t j = 0;
                while (j + (pd ? 5u : 3u) <= qn) {
                    uint32_t sid;
                    if (pd) { sid = (uint32_t(q[j]) << 24) | (uint32_t(q[j+1]) << 16)
                                  | (uint32_t(q[j+2]) << 8) | q[j+3]; j += 4; }
                    else    { sid = (uint32_t(q[j]) << 8) | q[j + 1]; j += 2; }
                    const int scids = q[j] >> 4;
                    const int napps = q[j] & 0x0F; ++j;
                    std::vector<UserApp> apps;
                    bool ok = true;
                    for (int a = 0; a < napps && ok; ++a) {
                        if (j + 2 > qn) { ok = false; break; }
                        UserApp ua;
                        ua.type = uint16_t((q[j] << 3) | (q[j + 1] >> 5));
                        const size_t dlen = q[j + 1] & 0x1F;
                        j += 2;
                        if (j + dlen > qn) { ok = false; break; }
                        if (dlen >= 2) {                          // X-PAD data field present
                            ua.ca      = (q[j] & 0x80) != 0;
                            ua.xpadApp = q[j] & 0x1F;
                            ua.dscty   = q[j + 1] & 0x3F;
                        }
                        j += dlen;
                        apps.push_back(ua);
                    }
                    if (!ok) break;
                    auto it = e.services.find(sid);
                    if (it == e.services.end()) continue;
                    for (auto& c : it->second.components) {
                        /* ★ Bound by SCIdS when 0/8 has given it; otherwise SCIdS 0 is the primary
                         *  component by definition (6.3.5). */
                        const bool match = (c.scids >= 0) ? (c.scids == scids) : (scids == 0 && c.primary);
                        if (match) c.apps = apps;
                    }
                }
            } else if (ext == 17 && !oe) {             // programme type
                size_t j = 0;
                while (j + 4 <= qn) {
                    const uint32_t sid = (uint32_t(q[j]) << 8) | q[j + 1];
                    const int code = q[j + 3] & 0x1F;
                    j += 4;
                    auto it = e.services.find(sid);
                    if (it != e.services.end()) it->second.pty = code;
                }
            }
            // ★ everything else (0/3, 0/5, 0/10, 0/14, 0/18-0/26) is skipped by length, as 5.2.2.0 says
        } else if (type == 1) {
            const uint8_t charset = uint8_t(p[0] >> 4);
            const int ext = p[0] & 0x07;
            /* Identifier field length by extension: 1/0 EId(2), 1/1 SId(2), 1/4 P/D+SCIdS(1) +
             * SId(2 or 4), 1/5 SId(4). Then 16 bytes of label and 2 of character flags. */
            if (ext == 0 && len >= 1 + 2 + 16) {
                e.eid   = uint16_t((p[1] << 8) | p[2]);
                e.label = labelFromBytes(p + 3, 16, charset);
                if (len >= 1 + 2 + 18) e.shortLabel = shortLabelFromBytes(p + 3, uint16_t((p[19] << 8) | p[20]), charset);
            } else if (ext == 1 && len >= 1 + 2 + 16) {
                const uint32_t sid = uint32_t((p[1] << 8) | p[2]);
                Service& s = e.services[sid];
                s.sid   = sid;
                s.label = labelFromBytes(p + 3, 16, charset);
                if (len >= 1 + 2 + 18) s.shortLabel = shortLabelFromBytes(p + 3, uint16_t((p[19] << 8) | p[20]), charset);
            } else if (ext == 5 && len >= 1 + 4 + 16) {
                const uint32_t sid = (uint32_t(p[1]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 8) | p[4];
                Service& s = e.services[sid];
                s.sid = sid; s.isData = true;
                s.label = labelFromBytes(p + 5, 16, charset);
                if (len >= 1 + 4 + 18) s.shortLabel = shortLabelFromBytes(p + 5, uint16_t((p[21] << 8) | p[22]), charset);
            } else if (ext == 4 && len >= 1 + 1 + 2 + 16) {
                const bool pd    = (p[1] & 0x80) != 0;
                const int  scids = p[1] & 0x0F;
                const size_t sidLen = pd ? 4 : 2;
                if (len < int(1 + 1 + sidLen + 16)) { continue; }
                uint32_t sid = 0;
                for (size_t k = 0; k < sidLen; ++k) sid = (sid << 8) | p[2 + k];
                auto it = e.services.find(sid);
                if (it == e.services.end()) continue;
                const std::string lbl = labelFromBytes(p + 2 + sidLen, 16, charset);
                for (auto& c : it->second.components) if (c.scids == scids) c.label = lbl;
            }
        }
        // types 2 (extended labels), 5 (FIDC), 6 (CA) and unknown: skipped by length
    }
    return true;
}

}  // namespace vibedab
