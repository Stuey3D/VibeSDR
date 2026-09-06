// vibe_dab_charset.h — the DAB character sets, to UTF-8.
//
// ★★★ EN 300 401 5.2.2.2: a FIG type 1 label "shall be set to 0000 - Complete EBU Latin based
//     repertoire", interpreted per ETSI TS 101 756 table 1. Bytes 0x80..0xFF are NOT ISO-8859-1:
//     0xE1 is 'Å' in EBU Latin and 'á' in Latin-1. Until now anything above 0x7F was replaced
//     with '?', so a Welsh, Irish or Gaelic station name — and any DLS with a pound sign, an
//     accent or a typographic apostrophe — came out damaged. The table below is the repertoire
//     as TS 101 756 defines it; the same repertoire RDS uses for its RadioText.
// ★★ The same field in a DLS (EN 300 401 7.4.5.2, "Field 2: charset") selects among the same
//    sets, so one converter serves labels and now-playing text alike.
// ★ Every output is VALID UTF-8 by construction, which is the property the JSON layer depends on
//   (see vibe_utf8.h) — a control byte or an undefined code point becomes nothing, never a raw
//   byte the browser will hang up over.
#pragma once

#include <cstdint>
#include <string>

namespace vibedab {

/** Charset identifiers as carried in FIG type 1 and DLS prefixes (TS 101 756 table 1). */
enum : uint8_t {
    kCharsetEbuLatin = 0,   ///< complete EBU Latin based repertoire (the one on air in the UK)
    kCharsetUcs2     = 6,   ///< UCS-2, big-endian (legacy; still permitted in DLS)
    kCharsetUtf8     = 15,  ///< UTF-8 (DLS only)
};

/** ★ TS 101 756 table 1, code points 0x00..0xFF. 0 means "no character" (control positions and
 *  the padding byte). Rows 0x0_ and 0x1_ carry accented capitals/lower-case letters that the
 *  repertoire packs into what ASCII uses for control codes; 0x0A/0x0B are the DLS line-break
 *  controls and are left as NUL here — the label layer handles them, not the character set. */
inline uint32_t ebuLatinCodePoint(uint8_t b) {
    static const uint16_t kTable[256] = {
        // 0x00
        0x0000,0x0118,0x012E,0x0172,0x0102,0x0116,0x010E,0x0218,0x021A,0x010A,0x0000,0x0000,0x0120,0x0139,0x017B,0x0143,
        // 0x10
        0x0105,0x0119,0x012F,0x0173,0x0103,0x0117,0x010F,0x0219,0x021B,0x010B,0x0147,0x011A,0x0121,0x013A,0x017C,0x0000,
        // 0x20  (ASCII, except 0x24 which is the currency sign in EBU Latin, printed as '$' too)
        0x0020,0x0021,0x0022,0x0023,0x0142,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
        // 0x30
        0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
        // 0x40
        0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
        // 0x50
        0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,0x0058,0x0059,0x005A,0x005B,0x016E,0x005D,0x0141,0x005F,
        // 0x60
        0x0104,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
        // 0x70
        0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,0x0078,0x0079,0x007A,0x00AB,0x016F,0x00BB,0x013D,0x0126,
        // 0x80
        0x00E1,0x00E0,0x00E9,0x00E8,0x00ED,0x00EC,0x00F3,0x00F2,0x00FA,0x00F9,0x00D1,0x00C7,0x015E,0x00DF,0x00A1,0x0178,
        // 0x90
        0x00E2,0x00E4,0x00EA,0x00EB,0x00EE,0x00EF,0x00F4,0x00F6,0x00FB,0x00FC,0x00F1,0x00E7,0x015F,0x011F,0x0131,0x00FF,
        // 0xA0
        0x0136,0x0145,0x00A9,0x0122,0x011E,0x011B,0x0148,0x0151,0x0150,0x20AC,0x00A3,0x0024,0x0100,0x0112,0x012A,0x016A,
        // 0xB0
        0x0137,0x0146,0x013B,0x0123,0x013C,0x0130,0x0144,0x0171,0x0170,0x00BF,0x013E,0x00B0,0x0101,0x0113,0x012B,0x016B,
        // 0xC0
        0x00C1,0x00C0,0x00C9,0x00C8,0x00CD,0x00CC,0x00D3,0x00D2,0x00DA,0x00D9,0x0158,0x010C,0x0160,0x017D,0x0110,0x013F,
        // 0xD0
        0x00C2,0x00C4,0x00CA,0x00CB,0x00CE,0x00CF,0x00D4,0x00D6,0x00DB,0x00DC,0x0159,0x010D,0x0161,0x017E,0x0111,0x0140,
        // 0xE0
        0x00C3,0x00C5,0x00C6,0x0152,0x0177,0x00DD,0x00D5,0x00D8,0x00DE,0x014A,0x0154,0x0106,0x015A,0x0179,0x0164,0x00F0,
        // 0xF0
        0x00E3,0x00E5,0x00E6,0x0153,0x0175,0x00FD,0x00F5,0x00F8,0x00FE,0x014B,0x0155,0x0107,0x015B,0x017A,0x0165,0x0127,
    };
    return kTable[b];
}

/** Append one code point as UTF-8. Nothing is appended for 0 or for anything outside Unicode. */
inline void appendUtf8(std::string& s, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;
    if      (cp < 0x80)    s += char(cp);
    else if (cp < 0x800)  { s += char(0xC0 | (cp >> 6));  s += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000){ s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    else                  { s += char(0xF0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 0x3F));
                            s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
}

/** Decode `n` bytes in the named charset to UTF-8. Unknown charsets fall back to EBU Latin, which
 *  is what the standard says a label carries anyway. A byte sequence that is not valid UTF-8 under
 *  charset 15 is decoded byte-wise as EBU Latin rather than passed through — invalid UTF-8 on the
 *  wire is the fault this project has already learned to fear. */
inline std::string dabTextToUtf8(const uint8_t* p, size_t n, uint8_t charset = kCharsetEbuLatin) {
    std::string out;
    out.reserve(n + 8);
    if (charset == kCharsetUtf8) {
        // validate; on the first malformed sequence give up and decode as EBU Latin
        size_t i = 0; bool ok = true;
        while (i < n && ok) {
            const uint8_t c = p[i];
            size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
            if (len == 0 || i + len > n) { ok = false; break; }
            for (size_t k = 1; k < len; ++k) if ((p[i + k] & 0xC0) != 0x80) { ok = false; break; }
            i += len;
        }
        if (ok) {
            for (size_t k = 0; k < n; ++k) if (p[k] >= 0x20 || p[k] >= 0x80) out += char(p[k]);
            return out;
        }
        charset = kCharsetEbuLatin;
    }
    if (charset == kCharsetUcs2) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            const uint32_t cp = (uint32_t(p[i]) << 8) | p[i + 1];
            if (cp >= 0x20) appendUtf8(out, cp);
        }
        return out;
    }
    for (size_t i = 0; i < n; ++i) appendUtf8(out, ebuLatinCodePoint(p[i]));
    return out;
}

}  // namespace vibedab
