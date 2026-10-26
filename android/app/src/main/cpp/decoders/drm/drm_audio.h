// decoders/drm/drm_audio.h — DRM audio super frames (ETSI ES 201 980 V4.3.1 §5.2–5.4) into ACCESS UNITS, and the
// configuration a platform decoder needs to decode them. No codec is here and none is shipped.
//
// ★★★ THE POSITION (Stuart; docs/v12/DECODER-RESEARCH.md §4.4): as for DAB+ (vibe_dab_aacdec.h) the SERVER'S OPERATING
//     SYSTEM decodes — FFmpeg run as a process on Linux/macOS, AudioToolbox on Apple, MediaCodec on Android. This file
//     turns DRM's own framing into what those decoders take: an AudioSpecificConfig and raw access units, carried to FFmpeg
//     as LOAS/LATM (ISO/IEC 14496-3 §1.7), which is the one MPEG-4 transport that carries ANY AudioSpecificConfig —
//     ADTS cannot describe USAC.
//
// ★★★ WHAT CAN BE DECODED, HONESTLY:
//   • xHE-AAC (USAC, the newer stations): the DRM "static config" (§5.3.2, table 4) is the standard UsacConfig with the
//     constant parts left out. usacFromDrm() puts them back bit for bit, so any USAC decoder can be configured. FFmpeg has
//     decoded USAC natively since 7.1 (2024); Apple since iOS 13 / macOS 10.15 (kAudioFormatMPEGD_USAC); Android from
//     API 29 on devices whose codec lists it.
//   • DRM's AAC (the older stations) is NOT ordinary AAC: it is ER AAC Scalable (object type 20) with the HCR and VCB11
//     error-resilience tools mandatory, a 960-sample transform, and SBR data stored BACKWARDS at the end of each frame
//     (§5.4.1–5.4.2). FFmpeg's native decoder refuses ER resilience and object type 20; AudioToolbox and MediaCodec do not
//     offer it. So an AAC station's frames are found, counted and reported — and the listener is told, in words, that this
//     server's decoder cannot play that codec. (The repack to plain AAC — HCR decode, VCB11 → book 11, SBR moved into a
//     fill element — is lossless and possible; it is listed as remaining work in docs/v12/DRM.md, not pretended.)
#pragma once
#include "drm_coding.h"
#include "drm_info.h"
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace vibedrm {

// ── Bit writer ──────────────────────────────────────────────────────────────────────────────────────────────────────

struct BitWriter {
    std::vector<uint8_t> b; int nbits = 0;
    void put(uint32_t v, int k) {
        for (int i = k - 1; i >= 0; --i) {
            if ((nbits & 7) == 0) b.push_back(0);
            if ((v >> i) & 1u) b.back() |= uint8_t(0x80u >> (nbits & 7));
            ++nbits;
        }
    }
    void escaped(uint32_t v, int n1, int n2, int n3) {
        const uint32_t m1 = (1u << n1) - 1u;
        if (v < m1) { put(v, n1); return; }
        put(m1, n1); v -= m1;
        const uint32_t m2 = (1u << n2) - 1u;
        if (v < m2 || n3 == 0) { put(v, n2); return; }
        put(m2, n2); put(v - m2, n3);
    }
    void align() { while (nbits & 7) put(0, 1); }
};
struct ByteBitReader {
    const uint8_t* p; size_t nbits; size_t pos = 0;
    ByteBitReader(const uint8_t* d, size_t nBytes) : p(d), nbits(nBytes * 8) {}
    uint32_t get(int k) { uint32_t v = 0; for (int i = 0; i < k; ++i) { v <<= 1; if (pos < nbits) v |= (p[pos >> 3] >> (7 - (pos & 7))) & 1u; ++pos; } return v; }
    uint32_t escaped(int n1, int n2, int n3) {
        uint32_t v = get(n1);
        if (v == (1u << n1) - 1u) { const uint32_t a = get(n2); v += a; if (n3 && a == (1u << n2) - 1u) v += get(n3); }
        return v;
    }
    bool overrun() const { return pos > nbits; }
};

// ── xHE-AAC: the DRM static config → the standard UsacConfig → an AudioSpecificConfig ─────────────────────────────

/** usacSamplingFrequencyIndex (ISO/IEC 23003-3 table 69 / 14496-3 table 1.16), or 0x1F = explicit. */
inline int usacSfIndex(int hz) {
    static const int t[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    for (int i = 0; i < 13; ++i) if (t[i] == hz) return i;
    if (hz == 57600) return 0x11; if (hz == 51200) return 0x12; if (hz == 40000) return 0x13; if (hz == 38400) return 0x14;
    if (hz == 34150) return 0x15; if (hz == 28800) return 0x16; if (hz == 25600) return 0x17; if (hz == 20000) return 0x18;
    if (hz == 19200) return 0x19; if (hz == 17075) return 0x1A; if (hz == 14400) return 0x1B; if (hz == 12800) return 0x1C;
    if (hz == 9600) return 0x1D;
    return 0x1F;
}

/** Copy SbrConfig() (23003-3 §5.2, unchanged in DRM). */
inline void copySbrConfig(ByteBitReader& r, BitWriter& w) {
    w.put(r.get(1), 1); w.put(r.get(1), 1); w.put(r.get(1), 1);           // harmonicSBR, bs_interTes, bs_pvc
    w.put(r.get(4), 4); w.put(r.get(4), 4);                                 // dflt_start_freq, dflt_stop_freq
    const uint32_t e1 = r.get(1), e2 = r.get(1); w.put(e1, 1); w.put(e2, 1);
    if (e1) { w.put(r.get(2), 2); w.put(r.get(1), 1); w.put(r.get(2), 2); } // freq_scale, alter_scale, noise_bands
    if (e2) { w.put(r.get(2), 2); w.put(r.get(2), 2); w.put(r.get(1), 1); w.put(r.get(1), 1); }
}

struct UsacInfo {
    bool ok = false;
    int coreSbrFrameLengthIndex = 0, sbrRatioIndex = 0, outputFrameLength = 0;
    int stereoConfigIndex = 0;
    std::vector<uint8_t> asc;       // the AudioSpecificConfig (object type 42) for the decoder, byte-aligned
    size_t ascBits = 0;             // ★ its EXACT length: LATM (audioMuxVersion 0) carries the config with no length field
    std::string why;
};

/** ★ Rebuild the standard configuration from the SDC's xHE-AAC static config (§5.3.2 tables 4–8, 8A–8C). `rateHz` is
 *  usacSamplingFrequency: the SDC's audio sampling rate. */
inline UsacInfo usacFromDrm(const AudioInfo& a, int rateHz) {
    UsacInfo u;
    if (a.coding != 3) { u.why = "not xHE-AAC"; return u; }
    if (a.config.empty()) { u.why = "no xHE-AAC static config in the SDC yet"; return u; }
    ByteBitReader r(a.config.data(), a.config.size());
    BitWriter w;
    // AudioSpecificConfig: object type 42 (escape 31 + 10), explicit sampling frequency, channel configuration
    const int ch = a.audioMode == 2 ? 2 : 1;
    w.put(31, 5); w.put(42 - 32, 6);
    const int sfi = usacSfIndex(rateHz);
    if (sfi < 13) w.put(uint32_t(sfi), 4); else { w.put(0xF, 4); w.put(uint32_t(rateHz), 24); }
    w.put(uint32_t(ch), 4);
    // UsacConfig()
    const int idx = int(r.get(2)) + 1;          // coreSbrFrameLengthIndexDrm + 1
    u.coreSbrFrameLengthIndex = idx;
    static const int sbrRatio[5] = { 0, 0, 2, 3, 1 }, outLen[5] = { 768, 1024, 2048, 2048, 4096 };
    u.sbrRatioIndex = sbrRatio[idx]; u.outputFrameLength = outLen[idx];
    if (sfi <= 0x1D && sfi != 0x1F) w.put(uint32_t(sfi), 5); else { w.put(0x1F, 5); w.put(uint32_t(rateHz), 24); }
    w.put(uint32_t(idx), 3);
    w.put(uint32_t(ch), 5);                      // channelConfigurationIndex: 1 mono, 2 stereo
    // UsacDecoderConfig(): the core element, then the extension elements
    BitWriter el;   // element configs, written after we know numElements
    int nEl = 1;
    if (ch == 1) {
        el.put(0, 2);                            // ID_USAC_SCE
        el.put(0, 1);                            // tw_mdct (not used in DRM)
        el.put(r.get(1), 1);                     // noiseFilling
        if (u.sbrRatioIndex > 0) copySbrConfig(r, el);
    } else {
        el.put(1, 2);                            // ID_USAC_CPE
        el.put(0, 1); el.put(r.get(1), 1);
        int sci = 0;
        if (u.sbrRatioIndex > 0) { copySbrConfig(r, el); sci = int(r.get(2)); }
        el.put(uint32_t(sci), 2);
        u.stereoConfigIndex = sci;
        if (sci > 0) {
            // Mps212Config: DRM drops bsDecorrConfig (always 0) and codes bsTempShapeConfig in one bit (1 ≙ 3)
            el.put(r.get(3), 3); el.put(r.get(3), 3);                   // bsFreqRes, bsFixedGainDMX
            const uint32_t ts = r.get(1); el.put(ts ? 3u : 0u, 2);      // bsTempShapeConfig
            el.put(0, 2);                                               // bsDecorrConfig
            el.put(r.get(1), 1); el.put(r.get(1), 1);                   // bsHighRateMode, bsPhaseCoding
            const uint32_t pp = r.get(1); el.put(pp, 1);
            if (pp) el.put(r.get(5), 5);
            if (sci >= 2) { el.put(r.get(5), 5); el.put(r.get(1), 1); } // bsResidualBands, bsPseudoLr
        }
    }
    const uint32_t numExt = r.escaped(2, 4, 8);
    for (uint32_t e = 0; e < numExt && !r.overrun(); ++e) {
        el.put(3, 2);                            // ID_USAC_EXT
        const uint32_t type = r.escaped(4, 8, 16), len = r.escaped(4, 8, 16);
        el.escaped(type, 4, 8, 16); el.escaped(len, 4, 8, 16);
        const uint32_t dflt = r.get(1); el.put(dflt, 1);
        if (dflt) el.escaped(r.escaped(8, 16, 0), 8, 16, 0);
        el.put(r.get(1), 1);                     // usacExtElementPayloadFrag
        for (uint32_t i = 0; i < len; ++i) el.put(r.get(8), 8);
        ++nEl;
    }
    // ★ UsacConfigExtensionDrm (loudness) is not carried over: it is optional, and its DRM form differs from the
    //   standard's. usacConfigExtensionPresent = 0.
    w.escaped(uint32_t(nEl - 1), 4, 8, 16);
    for (int i = 0; i < el.nbits; ++i) w.put((el.b[size_t(i >> 3)] >> (7 - (i & 7))) & 1u, 1);
    w.put(0, 1);
    if (r.overrun()) { u.why = "the xHE-AAC static config is shorter than its own fields"; return u; }
    u.ascBits = size_t(w.nbits);
    w.align();
    u.asc = w.b; u.ok = true;
    return u;
}

// ── LOAS / LATM (ISO/IEC 14496-3 §1.7.3) — how an access unit and its config reach FFmpeg ────────────────────────────

inline std::vector<uint8_t> loasFrame(const std::vector<uint8_t>& asc, size_t ascBits, const uint8_t* au, size_t n) {
    BitWriter m;
    m.put(0, 1);                 // useSameStreamMux = 0: the config travels with EVERY unit (a decoder may join anywhere)
    m.put(0, 1);                 // audioMuxVersion
    m.put(1, 1);                 // allStreamsSameTimeFraming
    m.put(0, 6); m.put(0, 4); m.put(0, 3);   // numSubFrames, numProgram, numLayer
    for (size_t i = 0; i < ascBits; ++i) m.put((asc[i >> 3] >> (7 - (i & 7))) & 1u, 1);
    m.put(0, 3);                 // frameLengthType 0
    m.put(0xFF, 8);              // latmBufferFullness
    m.put(0, 1); m.put(0, 1);    // otherDataPresent, crcCheckPresent
    size_t left = n;             // PayloadLengthInfo
    while (left >= 255) { m.put(255, 8); left -= 255; }
    m.put(uint32_t(left), 8);
    for (size_t i = 0; i < n; ++i) m.put(au[i], 8);
    m.align();
    std::vector<uint8_t> o;
    const size_t len = m.b.size();
    o.push_back(0x56); o.push_back(uint8_t(0xE0 | ((len >> 8) & 0x1F))); o.push_back(uint8_t(len & 0xFF));
    o.insert(o.end(), m.b.begin(), m.b.end());
    return o;
}

// ── The audio super frame → access units ────────────────────────────────────────────────────────────────────────────

/** ★ `crc` is DRM AAC's per-frame CRC-8 from the super frame (§5.4.1): it covers the frame's side information, so only a
 *  decoder that parses the frame can check it — FAAD2's DRM mode takes it as the byte before the frame. */
struct AccessUnit { std::vector<uint8_t> data; bool crcOk = true; uint8_t crc = 0; bool hasCrc = false; };

class AudioFramer {
public:
    /** (Re)configure for a service. Returns true when this is a change (the decoder must be rebuilt). */
    bool configure(const AudioInfo& a) {
        const bool same = cfgSet_ && a.coding == cfg_.coding && a.sbr == cfg_.sbr && a.audioMode == cfg_.audioMode &&
                          a.srCode == cfg_.srCode && a.config == cfg_.config;
        if (same) return false;
        cfg_ = a; cfgSet_ = true; pend_.clear(); havePend_ = false; tail_.clear();
        if (a.coding == 3) usac_ = usacFromDrm(a, a.sampleRateHz());
        return true;
    }
    const AudioInfo& info() const { return cfg_; }
    const UsacInfo& usac() const { return usac_; }

    /** One audio super frame (the logical frame without the text bytes). */
    void push(const uint8_t* sf, size_t n, std::vector<AccessUnit>& out) {
        ++superFrames_;
        if (!cfgSet_) return;
        if (cfg_.coding == 3) pushXhe(sf, n, out);
        else if (cfg_.coding == 0) pushAac(sf, n, out);
    }
    int64_t superFrames() const { return superFrames_; }
    int64_t units() const { return units_; }
    int64_t crcFailures() const { return crcFail_; }
    /** Audio frames per super frame seen lately (xHE-AAC varies; AAC is 5 or 10). */
    double framesPerSuper() const { return superFrames_ > 0 ? fpsAvg_ : 0; }

private:
    void count(size_t nUnits) { fpsAvg_ = superFrames_ <= 1 ? double(nUnits) : 0.9 * fpsAvg_ + 0.1 * double(nUnits); }
    // §5.3.1: header (frame border count, bit reservoir level, CRC-8), payload, directory of 16-bit borders (reverse order)
    void pushXhe(const uint8_t* sf, size_t n, std::vector<AccessUnit>& out) {
        if (n < 2) return;
        int b = sf[0] >> 4;
        std::vector<uint8_t> hb; putBits(hb, sf[0], 8);
        const bool hdrOk = crcBits(hb.data(), 8, kCrc8Poly, 8) == sf[1];
        if (!hdrOk && n >= 4) b = sf[n - 1] & 15;   // ★ every directory entry repeats the count (§5.3.1.1 NOTE)
        const size_t dirBytes = size_t(2 * b);
        if (2 + dirBytes > n) { havePend_ = false; pend_.clear(); return; }
        const uint8_t* pay = sf + 2;
        const size_t payLen = n - 2 - dirBytes;
        // borders in payload order: the LAST directory element is the FIRST border
        std::vector<int> borders;
        for (int i = b - 1; i >= 0; --i) {
            const uint8_t* e = sf + n - 2 * size_t(b) + 2 * size_t(i);
            borders.push_back((e[0] << 4) | (e[1] >> 4));
        }
        size_t nUnits = 0;
        size_t pos = 0;
        for (int bd : borders) {
            if (bd >= 0xFFE) {
                // a frame that started in the LAST 1–2 bytes of the previous payload (0xFFF = last byte)
                const size_t back = bd == 0xFFF ? 1 : 2;
                if (havePend_ && pend_.size() >= back && tail_.size() >= back) {
                    std::vector<uint8_t> done(pend_.begin(), pend_.end() - long(back));
                    emit(done, out); ++nUnits;
                    pend_.assign(pend_.end() - long(back), pend_.end());
                } else { pend_.clear(); }
                havePend_ = true;
                continue;
            }
            if (size_t(bd) > payLen || size_t(bd) < pos) { havePend_ = false; pend_.clear(); pos = payLen; break; }
            if (havePend_) { pend_.insert(pend_.end(), pay + pos, pay + bd); emit(pend_, out); ++nUnits; }
            pend_.clear(); havePend_ = true; pos = size_t(bd);
        }
        if (havePend_ && pos < payLen) pend_.insert(pend_.end(), pay + pos, pay + payLen);
        tail_.assign(pay + (payLen >= 2 ? payLen - 2 : 0), pay + payLen);
        count(nUnits);
    }
    void emit(const std::vector<uint8_t>& frame, std::vector<AccessUnit>& out) {
        if (frame.size() < 3) return;
        AccessUnit au;
        au.data.assign(frame.begin(), frame.end() - 2);
        const uint32_t crc = (uint32_t(frame[frame.size() - 2]) << 8) | frame[frame.size() - 1];
        au.crcOk = crcBytes(au.data.data(), au.data.size(), kCrc16Poly, 16) == crc;
        if (!au.crcOk) ++crcFail_;
        ++units_;
        out.push_back(std::move(au));
    }
    // §5.4.1 table 10/11: (n−1) 12-bit borders (+4 bits when n = 10), n CRC bytes, then the frames
    void pushAac(const uint8_t* sf, size_t n, std::vector<AccessUnit>& out) {
        const int nf = cfg_.sampleRateHz() == 12000 ? 5 : 10;
        const size_t hdrBytes = nf == 10 ? 14 : 6;
        if (n < hdrBytes + size_t(nf)) return;
        ByteBitReader r(sf, n);
        std::vector<int> borders;
        int prev = 0, wrap = 0;
        for (int i = 0; i < nf - 1; ++i) {
            int bd = int(r.get(12)) + wrap;
            if (bd < prev) { wrap += 4096; bd += 4096; }   // ★ NOTE 2: only 12 LSBs are sent
            borders.push_back(bd); prev = bd;
        }
        const size_t payStart = hdrBytes + size_t(nf);
        const size_t payLen = n - payStart;
        size_t start = 0;
        size_t nUnits = 0;
        for (int i = 0; i < nf; ++i) {
            const size_t end = i < nf - 1 ? size_t(borders[size_t(i)]) : payLen;
            if (end < start || end > payLen) break;
            AccessUnit au; au.data.assign(sf + payStart + start, sf + payStart + end);
            au.crcOk = true; au.crc = sf[hdrBytes + size_t(i)]; au.hasCrc = true;   // ★ the per-frame CRC covers only the side information; checking it needs the AAC parse
            ++units_; ++nUnits;
            out.push_back(std::move(au));
            start = end;
        }
        count(nUnits);
    }
    AudioInfo cfg_; bool cfgSet_ = false;
    UsacInfo usac_;
    std::vector<uint8_t> pend_, tail_; bool havePend_ = false;
    int64_t superFrames_ = 0, units_ = 0, crcFail_ = 0;
    double fpsAvg_ = 0;
};

}  // namespace vibedrm
