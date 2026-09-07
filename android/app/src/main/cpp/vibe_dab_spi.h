// vibe_dab_spi.h — Service and Programme Information off the air (TS 102 818 / TS 102 371):
// the binary SI document names every service's logo files by their MOT content names. Logos for
// the whole multiplex, from the multiplex — the DAB side of RadioDNS.
//
// Binary encoding (TS 102 371 5.x): tag-length-value throughout. Element tag + length (0xFE →
// 16-bit, 0xFF → 24-bit); attributes first (tag 0x01 = CDATA), then the string token table
// (attribute 0x04) and default language (0x06), then child elements. On the path to a logo:
// serviceInformation 0x03 → ensemble 0x26 → service 0x28 → bearer 0x29 (attribute 0x80 = the
// dab: id: flags/SCIdS, ECC, EId, SId) and mediaDescription 0x13 → multimedia 0x2B (0x82 url,
// 0x83 type: 0x02 unrestricted, 0x04 colour square 32x32, 0x06 colour rectangle 112x32,
// 0x84 width, 0x85 height, 0x80 mimeValue). The url is compared byte for byte with the MOT
// ContentName (TS 102 818 9.3; token compression is forbidden on it for that reason).
#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace vibedab {

struct SpiLogoRef {
    std::string url;      ///< = MOT ContentName of the image object
    int type = 0;         ///< 0x02 unrestricted, 0x04 32x32, 0x06 112x32
    int width = 0, height = 0;
    std::string mime;
};

struct SpiService {
    int ecc = -1; uint16_t eid = 0; uint32_t sid = 0; int scids = 0;
    std::vector<SpiLogoRef> logos;
};

class SpiDocument {
public:
    /** Parse a binary SI object. Returns the services with at least one bearer. */
    static std::vector<SpiService> parse(const uint8_t* d, size_t n) {
        std::vector<SpiService> out;
        walk(d, n, 0, out, nullptr);
        return out;
    }

private:
    static bool readTl(const uint8_t* d, size_t n, size_t& p, int& tag, size_t& len) {
        if (p + 2 > n) return false;
        tag = d[p]; size_t l = d[p + 1]; p += 2;
        if (l == 0xFE) { if (p + 2 > n) return false; l = (size_t(d[p]) << 8) | d[p + 1]; p += 2; }
        else if (l == 0xFF) { if (p + 3 > n) return false; l = (size_t(d[p]) << 16) | (size_t(d[p + 1]) << 8) | d[p + 2]; p += 3; }
        if (p + l > n) return false;
        len = l; return true;
    }
    /** Walk one element's content: attributes (tag < 0x80 for CDATA/token/lang, ≥ 0x80 for the
     *  element's own attributes — both are TLV), then child elements. Element tags and attribute
     *  tags share the byte space, so the ORDER rule decides: attributes come first. */
    static void walk(const uint8_t* d, size_t n, int elemTag, std::vector<SpiService>& out, SpiService* cur) {
        size_t p = 0; int tag; size_t len;
        SpiService svc; SpiService* here = cur;
        if (elemTag == 0x28) { here = &svc; }
        SpiLogoRef logo; bool inMultimedia = (elemTag == 0x2B);
        while (readTl(d, n, p, tag, len)) {
            const uint8_t* v = d + p;
            /* ★ Attributes are 0x80+, plus CDATA 0x01, the token table 0x04 and the default language
             *  0x06; 0x02 epg, 0x03 serviceInformation and 0x05 are ELEMENTS (measured: treating every
             *  tag ≤ 0x06 as an attribute swallowed the whole document at its root, 2026-09-08). */
            if (tag >= 0x80 || tag == 0x01 || tag == 0x04 || tag == 0x06) {
                // attribute of this element
                if (elemTag == 0x29 && tag == 0x80 && here && len >= 4) {
                    /* bearer id, dab: domain (5.4.5.1.2): [rfa|ens|xpad|sidFlag|SCIdS(4)] ECC EId(16) SId(16|32) */
                    const bool sid32 = (v[0] & 0x10) != 0;
                    here->scids = v[0] & 0x0F; here->ecc = v[1]; here->eid = uint16_t((v[2] << 8) | v[3]);
                    if (sid32 && len >= 8) here->sid = (uint32_t(v[4]) << 24) | (uint32_t(v[5]) << 16) | (uint32_t(v[6]) << 8) | v[7];
                    else if (len >= 6) here->sid = (uint32_t(v[4]) << 8) | v[5];
                } else if (inMultimedia) {
                    if (tag == 0x82) logo.url.assign(reinterpret_cast<const char*>(v), len);
                    else if (tag == 0x83 && len >= 1) logo.type = v[0];
                    else if (tag == 0x84 && len >= 2) logo.width = (v[0] << 8) | v[1];
                    else if (tag == 0x85 && len >= 2) logo.height = (v[0] << 8) | v[1];
                    else if (tag == 0x80) logo.mime.assign(reinterpret_cast<const char*>(v), len);
                }
            } else {
                // child element — descend, carrying the current service
                walk(v, len, tag, out, here);
            }
            p += len;
        }
        if (inMultimedia && cur && !logo.url.empty()) {
            if (logo.type == 0x04) { logo.width = 32; logo.height = 32; }
            if (logo.type == 0x06) { logo.width = 112; logo.height = 32; }
            cur->logos.push_back(logo);
        }
        if (elemTag == 0x28 && (svc.sid || !svc.logos.empty())) out.push_back(svc);
    }
};

}  // namespace vibedab
