// vibe_dab_pad.h — PAD: the Dynamic Label (DAB's "now playing") and the MOT carrier under it.
//
// ★★★ WHAT THIS IS FOR. EN 300 401 clause 7.4. Every DAB audio frame carries a small Programme
//     Associated Data field at its END, and that is where the text a listener actually reads
//     lives: the Dynamic Label Segment — artist, track, programme — DAB's answer to RDS
//     RadioText. Station logos and now-playing artwork ride the same channel as MOT objects.
//
// ★★★ AND IT MUST BE READ BEFORE THE AUDIO DECODER SEES THE BYTES. For DAB+ the PAD lives in the
//     AAC data stream element inside each access unit, and AMediaCodec — the platform decoder we
//     now hand those units to — discards it. So the PAD is parsed from the access unit here, on
//     the way past, and only then is the AU handed on. For MP2 it is simply the tail of the frame.
//     One implementation serves both because the PAD structure is identical once located; only
//     finding it differs.
//
// ★★ THE FIELD GROWS BACKWARDS, which is the whole difficulty. F-PAD is the LAST two bytes of the
//    frame. X-PAD sits immediately before it, and its Contents Indicators are at the END of the
//    X-PAD field — adjacent to F-PAD — while the data sub-fields they describe run the other way.
//    Indexing this the intuitive way round produces a decoder that finds a plausible application
//    type and then reads text out of the audio.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vibedab {

/** X-PAD application types we act on (EN 300 401 table 26). */
enum : uint8_t {
    kXpadEnd        = 0,    ///< end marker in the contents indicator list
    kXpadDataGroup  = 1,    ///< data group length indicator
    kXpadDlsStart   = 2,    ///< dynamic label, start of an X-PAD data group
    kXpadDlsCont    = 3,    ///< dynamic label, continuation
    kXpadMotStart   = 12,   ///< MOT (slideshow / artwork), start of an X-PAD data group
    kXpadMotCont    = 13,   ///< MOT, continuation
};

/** ★ Sub-field length for a contents indicator's 3-bit length index (EN 300 401 table 25). */
inline int xpadSubFieldLen(int lenIndex) {
    static const int kLen[8] = { 4, 6, 8, 12, 16, 24, 32, 48 };
    return kLen[lenIndex & 7];
}

/** CRC-16-CCITT as the DLS data group specifies: G(x) = x^16+x^12+x^5+1, preset ones, inverted. */
inline uint16_t dlsCrc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= uint16_t(d[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return uint16_t(~crc);
}

/** The assembled dynamic label, plus enough state to know when it CHANGED rather than repeated. */
struct DynamicLabel {
    std::string text;          ///< the complete label, once every segment has arrived
    uint32_t    changes = 0;   ///< how many complete, DIFFERENT labels have been seen
    bool        valid   = false;
};

/** ★★★ ASSEMBLES ONE DYNAMIC LABEL FROM ITS SEGMENTS.
 *
 *  A DLS data group is: a 2-byte prefix, the text segment, then a 2-byte CRC over both.
 *      prefix[0]: bit 7 Toggle, bit 6 First, bit 5 Last, bit 4 Command, bits 3..0 field 2
 *      prefix[1]: bits 7..4 character set, bits 3..0 reserved
 *  With C = 0, field 2 is (segment length - 1). Segments are concatenated First..Last to make the
 *  label; the Toggle bit flips when a NEW label begins, which is what distinguishes "the same text
 *  sent again" from "the track changed".
 *
 *  ★★ THE CRC IS CHECKED AND A FAILED SEGMENT IS DROPPED, not patched in. A dynamic label is read
 *     by a human; half a corrupted track name looks like a bug in the radio, and unlike audio there
 *     is no benefit to showing a damaged version of it. Silence is the honest failure here too.
 */
class DlsAssembler {
public:
    /** Feed one complete DLS data group (prefix + segment + CRC). */
    void pushGroup(const uint8_t* g, size_t n) {
        if (n < 4) return;
        const size_t bodyLen = n - 2;
        const uint16_t want = uint16_t((g[n - 2] << 8) | g[n - 1]);
        if (dlsCrc16(g, bodyLen) != want) { ++crcFail_; return; }
        ++crcOk_;

        const bool tog   = (g[0] & 0x80) != 0;
        const bool first = (g[0] & 0x40) != 0;
        const bool last  = (g[0] & 0x20) != 0;
        const bool cmd   = (g[0] & 0x10) != 0;
        if (cmd) {
            /* ★ A command group, not text. Field 2 = 1 is "clear display" — the station telling us
             *  it has nothing to say, which must blank the label rather than leave the last track
             *  showing under the next programme. */
            if ((g[0] & 0x0F) == 1) { label_.text.clear(); label_.valid = true; ++label_.changes; }
            return;
        }
        const size_t segLen = size_t(g[0] & 0x0F) + 1;
        if (2 + segLen > bodyLen) return;

        // ★ A toggle flip abandons whatever was half-assembled: it belongs to the previous label.
        if (first || tog != toggle_) { build_.clear(); toggle_ = tog; started_ = first; }
        if (!started_ && !first) return;          // joined mid-label — wait for the next First
        started_ = true;
        build_.append(reinterpret_cast<const char*>(g + 2), segLen);
        if (last) {
            std::string t = build_;
            while (!t.empty() && (t.back() == ' ' || t.back() == '\0')) t.pop_back();
            if (t != label_.text) { label_.text = t; ++label_.changes; }
            label_.valid = true;
            build_.clear(); started_ = false;
        }
    }
    const DynamicLabel& label() const { return label_; }
    uint32_t crcOk()   const { return crcOk_; }
    uint32_t crcFail() const { return crcFail_; }
    void reset() { build_.clear(); started_ = false; label_ = DynamicLabel{}; }

private:
    std::string  build_;
    DynamicLabel label_;
    bool     toggle_  = false;
    bool     started_ = false;
    uint32_t crcOk_ = 0, crcFail_ = 0;
};

/** ★★★ WALKS THE PAD AT THE END OF ONE AUDIO FRAME AND FEEDS OUT THE DATA GROUPS IT CARRIES.
 *
 *  `pad` points at the PAD field, `n` is its length, F-PAD being its last two bytes. The X-PAD
 *  contents indicators sit immediately before F-PAD and run backwards; each names an application
 *  type and a sub-field length, and the sub-fields themselves are laid out FORWARDS from the start
 *  of the X-PAD field. Reassembling a data group therefore means reading the indicator list in one
 *  direction and the payload in the other, which is exactly the trap noted at the top of this file.
 */
class PadReader {
public:
    /** Feed the PAD of one audio frame. Safe to call with a short or absent PAD. */
    void feed(const uint8_t* pad, size_t n) {
        if (!pad || n < 2) return;
        /* ★★★ THE X-PAD INDICATOR IS BITS 5-4 OF F-PAD BYTE L-1, NOT ITS TWO LSBs.
         *  EN 300 401 clause 7.4.1: byte L-1 is b7-b6 F-PAD type, b5-b4 X-PAD indicator, and the
         *  rest is type-dependent. Read from the bottom two bits instead, this returned 0 — "no
         *  X-PAD" — on 1898 consecutive frames of Times Radio, a station that transmits a dynamic
         *  label continuously. A field read at the wrong offset does not announce itself; it
         *  reports, calmly, that the feature is not being transmitted. */
        const uint8_t fpad0 = pad[n - 2];          // F-PAD byte L-1
        const int xInd = (fpad0 >> 4) & 0x03;      // X-PAD indicator
        ++frames_;
        ++xInd_[xInd & 3];
        if (xInd == 0) return;                     // no X-PAD in this frame

        if (xInd == 1) {
            /* ★ SHORT X-PAD: a fixed 4-byte field, one contents indicator plus 3 data bytes. */
            if (n < 2 + 4) return;
            const uint8_t* x = pad + n - 2 - 4;
            const uint8_t ci = x[3];               // the CI sits adjacent to F-PAD
            append(uint8_t(ci & 0x1F), x, 3);
            return;
        }

        /* ★★★ VARIABLE X-PAD, AND THE LAYOUT IS THE WHOLE PROBLEM. EN 300 401 clause 7.4.2:
         *      [ data sub-fields, forwards ][ contents indicators ][ F-PAD (2 bytes) ]
         *  The indicator list sits at the END of the X-PAD field, immediately before F-PAD, and is
         *  read BACKWARDS from there; the sub-fields it describes lie before it and run FORWARDS.
         *  So the field start cannot be known until the whole indicator list has been read and its
         *  lengths summed — which is why this is done in two passes rather than one walk.
         *
         *  ★★★ AND THE CRC IS THE ORACLE THAT MAKES THIS SAFE TO GET WRONG. A dynamic label group
         *      carries its own CRC-16, so a mis-parsed layout produces failing CRCs and shows the
         *      listener NOTHING. It cannot put garbage on screen. That is why this is measured
         *      against live air by watching crcOk against crcFail rather than argued from the
         *      clause — the same discipline the rest of this decoder was built with. */
        const size_t xEnd = n - 2;                  // one past the last X-PAD byte
        struct Ci { uint8_t app; int len; };
        Ci cis[8]; int nci = 0;
        size_t ciPos = xEnd;
        while (nci < 8 && ciPos > 0) {
            const uint8_t ci = pad[ciPos - 1];
            const uint8_t app = uint8_t(ci & 0x1F);
            --ciPos;
            if (app == kXpadEnd) break;             // end of the list
            cis[nci++] = { app, xpadSubFieldLen((ci >> 5) & 0x07) };
        }
        if (nci == 0) return;
        size_t total = 0;
        for (int i = 0; i < nci; ++i) total += size_t(cis[i].len);
        if (total > ciPos) return;                  // the field cannot reach back that far
        /* ★ The sub-fields end where the indicator list begins, so the field STARTS `total` bytes
         *  before it. Indicators were read back-to-front, so they are walked in reverse to match
         *  the forward order of the data they describe. */
        size_t off = ciPos - total;
        for (int i = nci - 1; i >= 0; --i) {
            append(cis[i].app, pad + off, size_t(cis[i].len));
            off += size_t(cis[i].len);
        }
    }

    const DlsAssembler& dls() const { return dls_; }
    DlsAssembler&       dls()       { return dls_; }
    uint32_t framesSeen() const { return frames_; }
    uint32_t dlsBytes()   const { return dlsBytes_; }
    /** ★ How the X-PAD indicator read on each frame: [none, short, variable, reserved]. If this is
     *  all "none" the PAD field is not where we are looking, which is a different fault from a
     *  label that fails its CRC — and the two look identical from the outside. */
    uint32_t xIndCount(int i) const { return xInd_[i & 3]; }
    uint32_t appSeen(int i)   const { return appSeen_[i & 31]; }
    void reset() { dls_.reset(); pend_.clear(); pendLen_ = 0; }

private:
    /** ★ Accumulate one application's bytes, completing a data group when its declared length is
     *  reached. DLS groups are short and self-describing, so the length comes from the prefix. */
    void append(uint8_t app, const uint8_t* d, size_t n) {
        ++appSeen_[app & 31];
        if (app == kXpadDlsStart) {
            pend_.assign(d, d + n);
            pendLen_ = pend_.size() >= 1 ? size_t(pend_[0] & 0x0F) + 1 + 2 + 2 : 0;
            dlsBytes_ += uint32_t(n);
            flush();
        } else if (app == kXpadDlsCont) {
            if (pend_.empty()) return;              // continuation with no start — joined mid-group
            pend_.insert(pend_.end(), d, d + n);
            dlsBytes_ += uint32_t(n);
            flush();
        }
        /* ★ MOT (12/13) is recognised above and deliberately not assembled yet — slideshow needs
         *  MSC data groups and a MOT directory on top of this, and half an implementation that
         *  silently drops objects is worse than none. The application types are named so the next
         *  step has somewhere obvious to begin. */
    }
    void flush() {
        if (pendLen_ && pend_.size() >= pendLen_) {
            dls_.pushGroup(pend_.data(), pendLen_);
            pend_.clear(); pendLen_ = 0;
        }
    }

    DlsAssembler         dls_;
    std::vector<uint8_t> pend_;
    size_t               pendLen_ = 0;
    uint32_t             frames_ = 0, dlsBytes_ = 0;
    uint32_t             xInd_[4] = {0,0,0,0};
    uint32_t             appSeen_[32] = {0};
};

}  // namespace vibedab
