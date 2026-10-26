// decoders/drm/drm_info.h — the FAC and SDC contents (ETSI ES 201 980 V4.3.1 §6.3, §6.4) and the text message application
// (§6.5): parsed by the receiver, built by the test transmitter. Readable names for the codes live here too (tables 18
// and 19), so a client gets words, not numbers.
#pragma once
#include "drm_coding.h"
#include <array>
#include <cstdio>
#include <string>

namespace vibedrm {

// ── FAC (§6.3) ────────────────────────────────────────────────────────────────────────────────────────────────────

struct Fac {
    // channel parameters
    int baseEnh = 0, identity = 0, rm = 0, so = 3, longInterleave = 1, mscMode = 0, sdcMode = 1, services = 4, reconf = 0, toggle = 0;
    // service parameters (one set in modes A–D)
    uint32_t serviceId = 0; int shortId = 0, audioCa = 0, language = 0, isData = 0, descriptor = 0, dataCa = 0;

    int mscLevels() const { return mscMode == 0 ? 3 : 2; }          // 00 = 64-QAM, 11 = 16-QAM
    int sdcLevels() const { return sdcMode == 0 ? 2 : 1; }          // 0 = 16-QAM, 1 = 4-QAM
    int frameInSuper() const { return identity == 0 || identity == 3 ? 0 : identity == 1 ? 1 : 2; }
    int interleaveDepth() const { return longInterleave ? 5 : 1; }  // ★ the FLAG is 0 for long (2 s)
    int audioServices() const { static const int a[16] = {4,0,0,0,1,1,1,1,2,2,2,0,3,3,0,0}; return a[services & 15]; }
    int dataServices() const  { static const int d[16] = {0,1,2,3,0,1,2,3,0,1,2,0,0,1,0,4}; return d[services & 15]; }

    /** 72 bits: the 64 information bits and the CRC. */
    std::vector<uint8_t> toBits() const {
        std::vector<uint8_t> b;
        putBits(b, uint32_t(baseEnh), 1); putBits(b, uint32_t(identity), 2); putBits(b, uint32_t(rm), 1); putBits(b, uint32_t(so), 3);
        putBits(b, uint32_t(longInterleave ? 0 : 1), 1); putBits(b, uint32_t(mscMode), 2); putBits(b, uint32_t(sdcMode), 1);
        putBits(b, uint32_t(services), 4); putBits(b, uint32_t(reconf), 3); putBits(b, uint32_t(toggle), 1); putBits(b, 0, 1);
        putBits(b, serviceId, 24); putBits(b, uint32_t(shortId), 2); putBits(b, uint32_t(audioCa), 1); putBits(b, uint32_t(language), 4);
        putBits(b, uint32_t(isData), 1); putBits(b, uint32_t(descriptor), 5); putBits(b, uint32_t(dataCa), 1); putBits(b, 0, 6);
        putBits(b, crcBits(b.data(), 64, kCrc8Poly, 8), 8);
        return b;
    }
    /** ★ Parse and VALIDATE: the CRC, then every field against what modes A–D allow. A reserved value is a failure even
     *  with a good CRC — a 1-in-256 CRC collision on noise must not become a "station". */
    bool fromBits(const uint8_t* b) {
        if (crcBits(b, 64, kCrc8Poly, 8) != BitReader(b + 64, 8).get(8)) return false;
        BitReader r(b, 64);
        baseEnh = int(r.get(1)); identity = int(r.get(2)); rm = int(r.get(1)); so = int(r.get(3));
        longInterleave = r.get(1) == 0 ? 1 : 0; mscMode = int(r.get(2)); sdcMode = int(r.get(1));
        services = int(r.get(4)); reconf = int(r.get(3)); toggle = int(r.get(1)); r.get(1);
        serviceId = r.get(24); shortId = int(r.get(2)); audioCa = int(r.get(1)); language = int(r.get(4));
        isData = int(r.get(1)); descriptor = int(r.get(5)); dataCa = int(r.get(1));
        if (rm != 0) return false;                       // robustness mode E — not this receiver
        if (so > 5) return false;
        if (mscMode == 1 || mscMode == 2) return false;  // reserved
        if (services == 11 || services == 14) return false;
        return true;
    }
};

inline const char* languageName(int code) {
    static const char* n[16] = { "", "Arabic", "Bengali", "Chinese (Mandarin)", "Dutch", "English", "French", "German", "Hindi",
                                 "Japanese", "Javanese", "Korean", "Portuguese", "Russian", "Spanish", "Other language" };
    return n[code & 15];
}
inline const char* programmeTypeName(int code) {
    static const char* n[32] = { "", "News", "Current Affairs", "Information", "Sport", "Education", "Drama", "Culture", "Science",
        "Varied", "Pop Music", "Rock Music", "Easy Listening Music", "Light Classical", "Serious Classical", "Other Music",
        "Weather/meteorology", "Finance/Business", "Children's programmes", "Social Affairs", "Religion", "Phone In", "Travel",
        "Leisure", "Jazz Music", "Country Music", "National Music", "Oldies Music", "Folk Music", "Documentary", "", "" };
    return n[code & 31];
}

// ── SDC (§6.4) ────────────────────────────────────────────────────────────────────────────────────────────────────

struct StreamDesc { int lenA = 0, lenB = 0; };
struct AudioInfo {
    bool present = false;
    int streamId = 0, coding = 0 /*0 AAC, 3 xHE-AAC*/, sbr = 0, audioMode = 0, srCode = 0, text = 0, enh = 0, mps = 0;
    std::vector<uint8_t> config;      // xHE-AAC static config (codec specific config field)
    /** The core sampling rate the SDC states, Hz (0 = reserved). */
    int sampleRateHz() const {
        if (coding == 0) { static const int t[8] = {0, 12000, 0, 24000, 0, 48000, 0, 0}; return t[srCode & 7]; }
        if (coding == 3) { static const int t[8] = {0, 0, 16000, 19200, 24000, 32000, 38400, 48000}; return t[srCode & 7]; }
        return 0;
    }
    const char* codecName() const {
        if (coding == 3) return "xHE-AAC";
        if (coding == 0) return sbr ? (audioMode == 1 ? "HE-AAC v2" : "HE-AAC") : "AAC";
        return "reserved";
    }
    const char* modeName() const {
        if (coding == 0) return audioMode == 0 ? "mono" : audioMode == 1 ? "parametric stereo" : audioMode == 2 ? "stereo" : "?";
        return audioMode == 0 ? "mono" : audioMode == 2 ? "stereo" : "?";
    }
};
struct ServiceSdc { std::string label; std::string lang3, country2; AudioInfo audio; };

struct Sdc {
    int afs = 0;
    bool haveMux = false; int protA = 0, protB = 0; std::vector<StreamDesc> streams;
    std::array<ServiceSdc, 4> svc;
    bool haveTime = false; int mjd = 0, hour = 0, minute = 0; int ltoHalfHours = 0; bool haveLto = false;

    /** ★ Parse one SDC block: `bits` = the decoded, de-dispersed block (L_SDC bits); `dataBytes` = table 21's length.
     *  The CRC covers the AFS index (as an 8-bit field) and the data field. Entities are applied to *this (it keeps
     *  what earlier blocks delivered — a label sent every other block must not vanish in between). */
    bool parse(const uint8_t* bits, size_t nBits, int dataBytes) {
        if (nBits < size_t(4 + dataBytes * 8 + 16)) return false;
        std::vector<uint8_t> crcIn; putBits(crcIn, 0, 4);
        crcIn.insert(crcIn.end(), bits, bits + 4 + dataBytes * 8);
        const uint32_t got = BitReader(bits + 4 + dataBytes * 8, 16).get(16);
        if (crcBits(crcIn.data(), crcIn.size(), kCrc16Poly, 16) != got) return false;
        BitReader r(bits, size_t(4 + dataBytes * 8));
        afs = int(r.get(4));
        while (r.left() >= 16) {
            const int len = int(r.get(7)); const int ver = int(r.get(1)); const int type = int(r.get(4));
            if (len == 0 && type == 0 && ver == 0) {
                // ★ a zero header is the start of the padding (0x00 bytes) — a real type-0 entity always has a body
                break;
            }
            const size_t bodyBits = size_t(4 + 8 * len);
            if (r.left() < bodyBits) break;
            std::vector<uint8_t> body(bits + r.pos, bits + r.pos + bodyBits);
            r.pos += bodyBits;
            entity(type, ver, len, body);
        }
        return true;
    }

    static std::string utf8Clean(const std::vector<uint8_t>& s) {
        std::string o;
        for (uint8_t c : s) { if (c == 0) break; if (c >= 0x20 || c >= 0x80) o.push_back(char(c)); }
        // ★ trailing spaces are padding on air
        while (!o.empty() && o.back() == ' ') o.pop_back();
        return o;
    }

private:
    void entity(int type, int ver, int len, const std::vector<uint8_t>& body) {
        BitReader r(body.data(), body.size());
        switch (type) {
            case 0: {   // multiplex description — the CURRENT configuration only (version flag 0)
                if (ver != 0) return;
                protA = int(r.get(2)); protB = int(r.get(2));
                streams.clear();
                for (int i = 0; i < len / 3 && i < 4; ++i) { StreamDesc s; s.lenA = int(r.get(12)); s.lenB = int(r.get(12)); streams.push_back(s); }
                haveMux = true;
                break;
            }
            case 1: {   // label
                const int sid = int(r.get(2)); r.get(2);
                std::vector<uint8_t> s;
                for (int i = 0; i < len; ++i) s.push_back(uint8_t(r.get(8)));
                // ★ a text-control byte (0x01–0x0F) leads the label when any of its bits is set (§6.4.3.2) — not a character
                if (!s.empty() && s[0] >= 0x01 && s[0] <= 0x0F) s.erase(s.begin());
                svc[size_t(sid)].label = utf8Clean(s);
                break;
            }
            case 8: {   // time and date
                if (len < 3) return;
                // ★ 17 + 11 bits = 28; the first 4 bits are the "first nibble" of the body
                mjd = int(r.get(17)); hour = int(r.get(5)); minute = int(r.get(6));
                haveTime = true;
                if (len >= 4) { r.get(2); const int sense = int(r.get(1)); const int v = int(r.get(5)); ltoHalfHours = sense ? -v : v; haveLto = true; }
                break;
            }
            case 9: {   // audio information — current configuration
                if (ver != 0) return;
                const int sid = int(r.get(2));
                AudioInfo a; a.present = true;
                a.streamId = int(r.get(2)); a.coding = int(r.get(2)); a.sbr = int(r.get(1)); a.audioMode = int(r.get(2));
                a.srCode = int(r.get(3)); a.text = int(r.get(1)); a.enh = int(r.get(1)); a.mps = int(r.get(3)); r.get(2); r.get(1);
                for (int i = 2; i < len; ++i) a.config.push_back(uint8_t(r.get(8)));
                svc[size_t(sid)].audio = a;
                break;
            }
            case 12: {  // language and country
                const int sid = int(r.get(2)); r.get(2);
                std::string l, c;
                for (int i = 0; i < 3; ++i) l.push_back(char(r.get(8)));
                for (int i = 0; i < 2; ++i) c.push_back(char(r.get(8)));
                if (l == "---") l.clear();
                if (c == "--") c.clear();
                svc[size_t(sid)].lang3 = l; svc[size_t(sid)].country2 = c;
                break;
            }
            default: break;   // ★ AFS, schedules, announcements… parsed by nobody yet; skipped by length
        }
    }
};

/** Builder for the test transmitter: entities appended, then the block finished to the exact data length. */
struct SdcBuilder {
    std::vector<uint8_t> data;   // bits of the data field
    void entity(int type, int ver, const std::vector<uint8_t>& bodyBits) {
        const int len = int((bodyBits.size() - 4) / 8);
        putBits(data, uint32_t(len), 7); putBits(data, uint32_t(ver), 1); putBits(data, uint32_t(type), 4);
        data.insert(data.end(), bodyBits.begin(), bodyBits.end());
    }
    void mux(int protA, int protB, const std::vector<StreamDesc>& st) {
        std::vector<uint8_t> b; putBits(b, uint32_t(protA), 2); putBits(b, uint32_t(protB), 2);
        for (const auto& s : st) { putBits(b, uint32_t(s.lenA), 12); putBits(b, uint32_t(s.lenB), 12); }
        entity(0, 0, b);
    }
    void label(int sid, const std::string& s) {
        std::vector<uint8_t> b; putBits(b, uint32_t(sid), 2); putBits(b, 0, 2);
        for (char c : s) putBits(b, uint8_t(c), 8);
        entity(1, 0, b);
    }
    void audio(int sid, const AudioInfo& a) {
        std::vector<uint8_t> b; putBits(b, uint32_t(sid), 2); putBits(b, uint32_t(a.streamId), 2); putBits(b, uint32_t(a.coding), 2);
        putBits(b, uint32_t(a.sbr), 1); putBits(b, uint32_t(a.audioMode), 2); putBits(b, uint32_t(a.srCode), 3); putBits(b, uint32_t(a.text), 1);
        putBits(b, uint32_t(a.enh), 1); putBits(b, uint32_t(a.mps), 3); putBits(b, 0, 2); putBits(b, 0, 1);
        for (uint8_t c : a.config) putBits(b, c, 8);
        entity(9, 0, b);
    }
    void langCountry(int sid, const std::string& l3, const std::string& c2) {
        std::vector<uint8_t> b; putBits(b, uint32_t(sid), 2); putBits(b, 0, 2);
        for (int i = 0; i < 3; ++i) putBits(b, uint8_t(i < int(l3.size()) ? l3[size_t(i)] : '-'), 8);
        for (int i = 0; i < 2; ++i) putBits(b, uint8_t(i < int(c2.size()) ? c2[size_t(i)] : '-'), 8);
        entity(12, 0, b);
    }
    void time(int mjd, int h, int m) {
        std::vector<uint8_t> b; putBits(b, uint32_t(mjd), 17); putBits(b, uint32_t(h), 5); putBits(b, uint32_t(m), 6);
        entity(8, 0, b);
    }
    /** The whole SDC block: AFS, data (padded with zeros), CRC, padding to `lSdc` bits. */
    std::vector<uint8_t> block(int afs, int dataBytes, int lSdc) const {
        std::vector<uint8_t> b; putBits(b, uint32_t(afs), 4);
        std::vector<uint8_t> d = data; d.resize(size_t(dataBytes * 8), 0);
        b.insert(b.end(), d.begin(), d.end());
        std::vector<uint8_t> crcIn; putBits(crcIn, 0, 4); crcIn.insert(crcIn.end(), b.begin(), b.end());
        putBits(b, crcBits(crcIn.data(), crcIn.size(), kCrc16Poly, 16), 16);
        b.resize(size_t(lSdc), 0);
        return b;
    }
};

// ── Text messages (§6.5): four bytes at the end of each audio logical frame ─────────────────────────────────────────

class TextMessageDecoder {
public:
    /** Feed one frame's four bytes. Returns true when a complete message has just been assembled (text()). */
    bool push(const uint8_t b[4]) {
        if (b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF && b[3] == 0xFF) { seg_.clear(); inSeg_ = true; return false; }
        if (!inSeg_) return false;
        seg_.insert(seg_.end(), b, b + 4);
        if (seg_.size() < 2) return false;
        const int hdr = (seg_[0] << 8) | seg_[1];
        const bool cmd = (hdr >> 12) & 1;
        int bodyLen = cmd ? 0 : ((hdr >> 8) & 15) + 1;
        if (cmd && ((hdr >> 8) & 15) == 2) bodyLen = 0;     // DL Plus: not interpreted
        const size_t need = size_t(2 + bodyLen + 2);
        if (seg_.size() < need) return false;
        inSeg_ = false;
        const uint32_t crc = (uint32_t(seg_[need - 2]) << 8) | seg_[need - 1];
        if (crcBytes(seg_.data(), need - 2, kCrc16Poly, 16) != crc) { ++crcFail_; return false; }
        const bool toggle = (hdr >> 15) & 1, first = (hdr >> 14) & 1, last = (hdr >> 13) & 1;
        if (cmd) {
            if (((hdr >> 8) & 15) == 1) { cur_.clear(); text_.clear(); return true; }   // clear display
            return false;
        }
        const int segNum = first ? 0 : ((hdr >> 4) & 7);
        if (first) { parts_.assign(8, std::string()); have_ = 0; toggle_ = toggle; lastSeg_ = -1; }
        else if (toggle != toggle_ || parts_.empty()) return false;
        parts_[size_t(segNum)].assign(reinterpret_cast<const char*>(seg_.data() + 2), size_t(bodyLen));
        have_ |= 1u << segNum;
        if (last) lastSeg_ = segNum;
        if (lastSeg_ >= 0 && have_ == ((1u << (lastSeg_ + 1)) - 1u)) {
            std::string t;
            for (int i = 0; i <= lastSeg_; ++i) t += parts_[size_t(i)];
            // ★ 0x0A = preferred line break, 0x0B = end of headline, 0x1F = preferred word break (§6.7) — shown as spaces
            for (char& c : t) if (c == 0x0A || c == 0x0B || c == 0x1F || c == 0x0D) c = ' ';
            text_ = t; lastSeg_ = -1; have_ = 0;
            return true;
        }
        return false;
    }
    const std::string& text() const { return text_; }
    int crcFailures() const { return crcFail_; }
private:
    std::vector<uint8_t> seg_; bool inSeg_ = false;
    std::vector<std::string> parts_; uint32_t have_ = 0; bool toggle_ = false; int lastSeg_ = -1;
    std::string text_, cur_; int crcFail_ = 0;
};

/** The test transmitter's side: a message cut into segments, then into four-byte pieces (with the 0xFF markers). */
inline std::vector<std::array<uint8_t, 4>> textMessagePieces(const std::string& msg, bool toggle) {
    std::vector<std::array<uint8_t, 4>> out;
    const size_t nSeg = std::max<size_t>(1, (msg.size() + 15) / 16);
    for (size_t s = 0; s < nSeg && s < 8; ++s) {
        const std::string body = msg.substr(s * 16, 16);
        std::vector<uint8_t> seg;
        const int first = s == 0, last = s + 1 == nSeg;
        const int f2 = first ? 15 : int(s), f3 = 0;
        const uint32_t hdr = (uint32_t(toggle) << 15) | (uint32_t(first) << 14) | (uint32_t(last) << 13) |
                             (uint32_t(body.size() - 1) << 8) | (uint32_t(f2) << 4) | uint32_t(f3);
        seg.push_back(uint8_t(hdr >> 8)); seg.push_back(uint8_t(hdr));
        seg.insert(seg.end(), body.begin(), body.end());
        const uint32_t crc = crcBytes(seg.data(), seg.size(), kCrc16Poly, 16);
        seg.push_back(uint8_t(crc >> 8)); seg.push_back(uint8_t(crc));
        out.push_back({0xFF, 0xFF, 0xFF, 0xFF});
        for (size_t i = 0; i < seg.size(); i += 4) {
            std::array<uint8_t, 4> p{0, 0, 0, 0};
            for (size_t j = 0; j < 4 && i + j < seg.size(); ++j) p[j] = seg[i + j];
            out.push_back(p);
        }
    }
    return out;
}

}  // namespace vibedrm
