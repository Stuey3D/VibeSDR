// vibe_dab_pad.h — PAD: the Dynamic Label (DAB's "now playing") and the MOT carrier under it.
//
// ★★★ WHAT THIS IS FOR. EN 300 401 clause 7.4. Every DAB audio frame carries a small Programme
//     Associated Data field at its END, and that is where the text a listener actually reads
//     lives: the Dynamic Label Segment — artist, track, programme — DAB's answer to RDS
//     RadioText. Station logos and now-playing artwork ride the same channel as MOT objects.
//
// ★★★ AND IT MUST BE READ BEFORE THE AUDIO DECODER SEES THE BYTES. For DAB+ the PAD lives in the
//     AAC data stream element at the START of each access unit (TS 102 563), and the platform
//     decoder we hand those units to discards it. So the PAD is lifted out of the AU here, on the
//     way past — feedAccessUnit() — and only then is the AU handed on. For MP2 it is the tail of
//     the frame. One implementation serves both, because clause 7.4 says the structure is the
//     same "for both audio coding methods"; only the placement differs.
//
// ★★★ THE FIELD IS TRANSMITTED BACKWARDS. Clause 7.4.2.0: "Before transmission, the order of the
//     bytes within each X-PAD field shall be reversed" — the whole field, contents indicators AND
//     data, so the indicators come last on the wire, adjacent to F-PAD, and every data sub-field
//     arrives with its bytes in reverse order. The previous reader read the indicator list
//     backwards (right) and then copied each sub-field FORWARDS in wire order (wrong): the DLS
//     prefix, text and CRC all arrived reversed, the CRC never matched, and the label never
//     assembled. Found 2026-09-07 by comparing against dablin, then confirmed in the clause.
//     Undoing it is one reversal of the field into logical order, after which everything reads
//     forwards exactly as the standard draws it.
//
// ★★ THE CI FLAG (F-PAD byte L, bit 1) SAYS WHETHER THERE IS AN INDICATOR LIST AT ALL. When a
//    data group continues from the previous frame the list is omitted and the field is one
//    sub-field for the same application, the same length as last time (7.4.2.1, 7.4.2.2). Reading
//    indicators out of a field that has none produces plausible-looking application types and
//    lengths from what is actually text — and no label.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "vibe_dab_charset.h"
#include "vibe_dab_mot.h"

namespace vibedab {

/** X-PAD application types we act on (EN 300 401 table 11, clause 7.4.3). */
enum : uint8_t {
    kXpadEnd        = 0,    ///< end marker in the contents indicator list
    kXpadDataGroup  = 1,    ///< data group length indicator
    kXpadDlsStart   = 2,    ///< dynamic label, start of an X-PAD data group
    kXpadDlsCont    = 3,    ///< dynamic label, continuation
    kXpadMotStart   = 12,   ///< MOT (slideshow / artwork), start of an X-PAD data group
    kXpadMotCont    = 13,   ///< MOT, continuation
};

/** ★ Sub-field length for a contents indicator's 3-bit length index (EN 300 401 table 10). */
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

/** ★★★ DL PLUS CONTENT TYPES (ETSI TS 102 980 table 3) — the same table RDS RT+ uses, which is
 *  the point: `RdsDecoder::applyRtPlus` already lifts title (1) and artist (4) out of FM
 *  RadioText, and a listener who sees "artist / title" on 96.1 should see it on 12B too.
 *  ★ Only the types worth SHOWING are named. An unnamed type is still carried — the tag list is
 *    the decoder's output and the UI decides what to draw — but a radio that renders
 *    "STATIONNOW.5" at a listener has not helped them. */
inline const char* dlPlusTypeName(int type) {
    switch (type) {
        case 1:  return "title";        case 2:  return "album";
        case 3:  return "track";        case 4:  return "artist";
        case 5:  return "composer";     case 6:  return "band";
        case 7:  return "comment";      case 8:  return "genre";
        case 9:  return "news";         case 11: return "sport";
        case 12: return "news";         case 15: return "weather";
        case 16: return "traffic";      case 17: return "alarm";
        case 18: return "advertisement";
        case 25: return "presenter";    case 26: return "editor";
        case 27: return "programme";    case 28: return "programme";
        case 31: return "homepage";
        case 33: return "phone";        case 36: return "email";
        case 39: return "sms";
        case 41: return "station";      case 42: return "slogan";
        case 43: return "logo";         case 44: return "country";
        default: return nullptr;
    }
}

/** One DL Plus object: a span of the dynamic label, said to BE something. */
struct DlPlusTag {
    int         type = 0;      ///< TS 102 980 content type; 0 is DUMMY and is never stored
    std::string text;          ///< the span of the label this tag points at, already UTF-8
};

/** The assembled dynamic label, plus enough state to know when it CHANGED rather than repeated. */
struct DynamicLabel {
    std::string text;          ///< the complete label, UTF-8, once every segment has arrived
    uint32_t    changes = 0;   ///< how many complete, DIFFERENT labels have been seen
    bool        valid   = false;
    /* ★★★ DL PLUS (TS 102 980): the station saying which PART of the label is the artist and
     *  which is the title, instead of leaving a receiver to guess at the dash. */
    std::vector<DlPlusTag> tags;             ///< the tagged objects, in transmission order
    bool        itemToggle  = false;         ///< flips when the ITEM (the track) changes
    bool        itemRunning = false;         ///< false = the item has ended (an ad break, news)
    bool        hasDlPlus   = false;         ///< a tags command has been received for this label
    uint32_t    itemChanges = 0;             ///< how many times itemToggle has flipped

    /** The text of the first tag of a given content type, or empty. */
    const std::string* tag(int type) const {
        for (const auto& t : tags) if (t.type == type) return &t.text;
        return nullptr;
    }
};

/** ★★★ ASSEMBLES ONE DYNAMIC LABEL FROM ITS SEGMENTS (EN 300 401 7.4.5.2).
 *
 *  A DLS data group is: a 2-byte prefix, the segment, then a 2-byte CRC over both.
 *      prefix[0]: bit 7 Toggle, bit 6 First, bit 5 Last, bit 4 Command, bits 3..0 Length-1
 *      prefix[1]: bits 7..4 = Charset when First, else Rfa + SegNum; bits 3..0 text control / Rfa
 *  Segments are concatenated First..Last to make the label; the Toggle bit flips when a NEW label
 *  begins, which is what distinguishes "the same text sent again" from "the track changed". Up to
 *  8 segments of 16 bytes: 128 bytes, in the charset the FIRST segment named.
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
            /* ★ A command group, not text. Command 1 is "clear display" — the station telling us
             *  it has nothing to say, which must blank the label rather than leave the last track
             *  showing under the next programme. */
            if ((g[0] & 0x0F) == 1) {
                label_.text.clear(); label_.valid = true; ++label_.changes;
                clearDlPlus();                    // the tags pointed into text that is now gone
                return;
            }
            /* ★★★ COMMAND 2 IS DL PLUS, AND ITS LENGTH IS IN THE OTHER BYTE. A text segment's
             *  length is prefix[0] bits 3..0; a command group's is prefix[1] bits 3..0
             *  (TS 102 980 clause 5.1). Reading it from prefix[0] — where the COMMAND NUMBER
             *  lives — yields length 3 for every tags command there will ever be, which is
             *  exactly long enough to look plausible and always one tag short.
             *  ★ And its reassembly is gated on the LINK BIT, not the toggle: prefix[1] bit 7
             *    associates a tags command with the label it describes, and the toggle in a
             *    command group's prefix[0] is not the label's toggle. */
            if ((g[0] & 0x0F) != 2) return;                    // no other command is defined
            if (bodyLen < 2) return;
            const size_t cmdLen = size_t(g[1] & 0x0F) + 1;
            if (2 + cmdLen > bodyLen) return;
            const bool link  = (g[1] & 0x80) != 0;
            const bool cFirst = (g[0] & 0x40) != 0;
            const bool cLast  = (g[0] & 0x20) != 0;
            if (cFirst || link != dpLink_) { dpBuild_.clear(); dpLink_ = link; dpStarted_ = cFirst; }
            if (!dpStarted_ && !cFirst) return;                // joined mid-command
            dpStarted_ = true;
            dpBuild_.insert(dpBuild_.end(), g + 2, g + 2 + cmdLen);
            if (dpBuild_.size() > 64) { dpBuild_.clear(); dpStarted_ = false; return; }
            if (cLast) { applyDlPlus(dpBuild_); dpBuild_.clear(); dpStarted_ = false; }
            return;
        }
        const size_t segLen = size_t(g[0] & 0x0F) + 1;
        if (2 + segLen > bodyLen) return;

        // ★ A toggle flip abandons whatever was half-assembled: it belongs to the previous label.
        if (first || tog != toggle_) { build_.clear(); toggle_ = tog; started_ = first; }
        if (!started_ && !first) return;          // joined mid-label — wait for the next First
        if (first) charset_ = uint8_t(g[1] >> 4);
        started_ = true;
        build_.insert(build_.end(), g + 2, g + 2 + segLen);
        if (build_.size() > 128) { build_.clear(); started_ = false; return; }
        if (last) {
            std::string t = dabTextToUtf8(build_.data(), build_.size(), charset_);
            while (!t.empty() && (t.back() == ' ' || t.back() == '\0')) t.pop_back();
            /* ★★★ THE TAGS BELONG TO THE TEXT THEY POINT INTO. A DL Plus tag is a (start, length)
             *  pair indexing the label — so the instant the label changes, every tag held from
             *  the previous one describes a span of text that no longer exists. Keeping them is
             *  how a receiver shows the last track's artist under the new track's title. */
            if (t != label_.text) { label_.text = t; ++label_.changes; clearDlPlus(); }
            label_.valid = true;
            build_.clear(); started_ = false;
            /* ★ A tags command that arrived BEFORE its label completed is held, not dropped:
             *  the two are separate data groups and nothing guarantees their order. */
            if (!dpPending_.empty()) resolveDlPlus();
        }
    }
    const DynamicLabel& label() const { return label_; }

private:
    /** Forget every tag. The label itself is untouched — text without tags is still a label. */
    void clearDlPlus() {
        label_.tags.clear(); label_.hasDlPlus = false;
    }

    /** Hold a complete tags command, and resolve it as soon as there is a label to resolve against. */
    void applyDlPlus(const std::vector<uint8_t>& raw) {
        if (raw.empty()) return;
        /* ★ High nibble 0 marks the "DL Plus tags" command. TS 102 980 reserves the rest, and a
         *  reserved command whose body we parsed as tags would publish garbage as a track name. */
        if ((raw[0] >> 4) != 0) return;
        dpPending_ = raw;
        if (label_.valid) resolveDlPlus();
    }

    /** ★★★ THE MARKERS COUNT CHARACTERS, NOT BYTES. start and length index the dynamic label as
     *  the listener reads it, and our label is already UTF-8 — where an EBU Latin byte ≥ 0x80
     *  (every accent, and the whole of Greek and Cyrillic) has become two or three bytes. Slicing
     *  by byte offset therefore cuts a multi-byte character in half on exactly the stations whose
     *  names most need the accents, and hands the UI invalid UTF-8. So the offsets are walked as
     *  characters. */
    void resolveDlPlus() {
        std::vector<uint8_t> raw;
        raw.swap(dpPending_);
        if (raw.empty() || !label_.valid) return;

        const bool it = (raw[0] & 0x08) != 0;
        if (label_.hasDlPlus && it != label_.itemToggle) ++label_.itemChanges;
        label_.itemToggle  = it;
        label_.itemRunning = (raw[0] & 0x04) != 0;
        const size_t nTags = size_t(raw[0] & 0x03) + 1;      // NT is the count MINUS one
        if (1 + nTags * 3 > raw.size()) return;              // truncated — publish nothing

        // Character boundaries of the label, so a marker pair becomes a byte range.
        std::vector<size_t> at;                              // byte offset of each character
        for (size_t i = 0; i < label_.text.size(); ++i)
            if ((uint8_t(label_.text[i]) & 0xC0) != 0x80) at.push_back(i);
        at.push_back(label_.text.size());
        const size_t nChars = at.size() - 1;

        label_.tags.clear();
        for (size_t i = 0; i < nTags; ++i) {
            const uint8_t* t = &raw[1 + i * 3];
            const int type = t[0] & 0x7F;
            if (type == 0) continue;                         // DUMMY — a padded-out tag slot
            const size_t start = size_t(t[1] & 0x7F);
            const size_t len   = size_t(t[2] & 0x7F) + 1;    // stored as length-1
            if (start >= nChars) continue;                   // points past the label we hold
            const size_t end = start + len > nChars ? nChars : start + len;
            label_.tags.push_back(DlPlusTag{ type, label_.text.substr(at[start], at[end] - at[start]) });
        }
        label_.hasDlPlus = true;
    }

public:
    uint32_t crcOk()   const { return crcOk_; }
    uint32_t crcFail() const { return crcFail_; }
    void reset() {
        build_.clear(); started_ = false; label_ = DynamicLabel{};
        dpBuild_.clear(); dpPending_.clear(); dpStarted_ = false;
    }

private:
    std::vector<uint8_t> build_;
    DynamicLabel label_;
    bool     toggle_  = false;
    bool     started_ = false;
    uint8_t  charset_ = kCharsetEbuLatin;
    uint32_t crcOk_ = 0, crcFail_ = 0;
    // ── DL Plus: a second reassembly, gated on the link bit rather than the toggle ──────────
    std::vector<uint8_t> dpBuild_;      ///< the tags command being assembled from its segments
    std::vector<uint8_t> dpPending_;    ///< a complete command waiting for a label to point into
    bool     dpLink_    = false;
    bool     dpStarted_ = false;
};

/** ★★★ WALKS THE PAD OF ONE AUDIO FRAME OR ACCESS UNIT AND FEEDS OUT THE DATA GROUPS IT CARRIES.
 *
 *  feed(): `pad` is the PAD as transmitted — the X-PAD field (byte-reversed on the wire) followed
 *  by the two F-PAD bytes, F-PAD last. For MP2 that is the tail of the frame with the scale
 *  factor CRC removed; the X-PAD length is not known in advance and is derived from the contents
 *  indicators, so `pad` may start earlier than the field does. For DAB+ the caller knows the exact
 *  length (the data stream element's) and says so with `exact`.
 */
class PadReader {
public:
    /** Feed the PAD of one audio frame. Safe to call with a short or absent PAD. */
    void feed(const uint8_t* pad, size_t n, bool exact = false) {
        if (!pad || n < 2) return;
        /* ★★★ THE X-PAD INDICATOR IS BITS 5-4 OF F-PAD BYTE L-1, and the CI FLAG is bit 1 of
         *  byte L. EN 300 401 clause 7.4.1. Read from the bottom two bits instead, the indicator
         *  returned 0 — "no X-PAD" — on 1898 consecutive frames of Times Radio, a station that
         *  transmits a dynamic label continuously. A field read at the wrong offset does not
         *  announce itself; it reports, calmly, that the feature is not being transmitted. */
        const uint8_t fpad0 = pad[n - 2];          // byte L-1
        const uint8_t fpad1 = pad[n - 1];          // byte L
        ++frames_;
        if ((fpad0 >> 6) != 0) { ++xInd_[3]; return; }      // F-PAD type != 00: byte L-1 is not ours
        const int  xInd   = (fpad0 >> 4) & 0x03;
        const bool ciFlag = (fpad1 & 0x02) != 0;
        ++xInd_[xInd & 3];
        if (xInd == 0 || xInd == 3) return;        // no X-PAD in this frame

        const size_t avail = n - 2;                // bytes before F-PAD, wire order
        size_t fieldLen;
        if (xInd == 1) fieldLen = 4;               // short X-PAD is exactly four bytes (7.4.2.1)
        else if (exact) fieldLen = avail;
        else if (ciFlag) fieldLen = avail;         // sized from the indicator list below
        else fieldLen = lastFieldLen_;             // "the same as in the previous frame" (7.4.2.2)
        if (fieldLen == 0 || fieldLen > avail) return;

        /* ★★★ UNDO THE TRANSMISSION REVERSAL. Logical order = the last `fieldLen` wire bytes
         *  before F-PAD, read back to front. From here on, everything is as the standard draws it:
         *  the indicator list first, then the sub-fields in order, each byte in its right place. */
        std::vector<uint8_t>& L = logical_;
        L.resize(fieldLen);
        for (size_t i = 0; i < fieldLen; ++i) L[i] = pad[avail - 1 - i];

        if (xInd == 1) {
            if (ciFlag) { const uint8_t app = uint8_t(L[0] & 0x1F); lastApp_ = app; append(app, &L[1], 3); }
            else if (lastApp_ != kXpadEnd) append(contApp(lastApp_), &L[0], 4);   // continuation, no CI
            lastFieldLen_ = 4;
            return;
        }

        // ── variable-size X-PAD ──────────────────────────────────────────────────────────
        if (!ciFlag) {
            /* One sub-field, same application and length as before (7.4.2.2). If we have not seen
             * a list yet there is nothing to attach it to — wait for the next one. */
            if (lastApp_ != kXpadEnd && lastFieldLen_ > 0) append(contApp(lastApp_), L.data(), lastFieldLen_);
            return;
        }
        struct Ci { uint8_t app; int len; };
        Ci cis[4]; int nci = 0; size_t pos = 0;
        while (nci < 4 && pos < L.size()) {
            const uint8_t ci  = L[pos++];
            const uint8_t app = uint8_t(ci & 0x1F);
            if (app == kXpadEnd) break;             // end marker terminates a list shorter than 4
            cis[nci++] = { app, xpadSubFieldLen((ci >> 5) & 0x07) };
        }
        if (nci == 0) { lastFieldLen_ = 0; return; }
        size_t total = 0;
        for (int i = 0; i < nci; ++i) total += size_t(cis[i].len);
        if (pos + total > L.size()) {
            /* The list claims more than we were handed. With an inexact window that means the
             * field started before it (our window is too short); with an exact one the list is
             * corrupt. Either way nothing here can be trusted. */
            ++overrun_;
            lastFieldLen_ = 0;
            return;
        }
        if (!exact) {
            /* ★ The field is exactly the list plus its sub-fields; the bytes before it are audio.
             *  Re-read from the true start so the sub-fields are in the right place. */
            const size_t realLen = pos + total;
            if (realLen != fieldLen) {
                L.resize(realLen);
                for (size_t i = 0; i < realLen; ++i) L[i] = pad[avail - 1 - i];
                // the indicator list is unchanged (it is at the front of the logical field)
            }
            fieldLen = realLen;
        }
        for (int i = 0; i < nci; ++i) {
            append(cis[i].app, &L[pos], size_t(cis[i].len));
            pos += size_t(cis[i].len);
        }
        lastApp_      = cis[nci - 1].app;
        lastFieldLen_ = fieldLen;
    }

    /** ★★★ DAB+: the PAD is the FIRST syntactic element of the access unit when present — an AAC
     *  data_stream_element (id_syn_ele 4) whose payload is [X-PAD as transmitted][F-PAD 2 bytes]
     *  (TS 102 563 clause 5.4.3). Header: 3 bits id, 4 bits instance tag, 1 bit byte-align, 8 bits
     *  count, plus 8 bits esc_count when count is 255 — 16 or 24 bits, both byte aligned, so the
     *  payload starts at byte 2 or 3. An AU with no DSE carries no PAD; its F-PAD is taken as
     *  {0,0} by convention, i.e. nothing. The AU itself is untouched: the decoder still gets it. */
    void feedAccessUnit(const uint8_t* au, size_t n) {
        if (!au || n < 3) return;
        if ((au[0] >> 5) != 4) { ++frames_; ++xInd_[0]; return; }
        size_t start = 2, len = au[1];
        if (len == 255) { len += au[2]; start = 3; }
        if (len < 2 || start + len > n) { ++frames_; ++dseBad_; return; }
        feed(au + start, len, true);
    }

    const DlsAssembler& dls() const { return dls_; }
    DlsAssembler&       dls()       { return dls_; }
    MotAssembler&       mot()       { return mot_; }
    const MotAssembler& mot() const { return mot_; }
    uint32_t framesSeen() const { return frames_; }
    uint32_t dlsBytes()   const { return dlsBytes_; }
    /** ★ How the X-PAD indicator read on each frame: [none, short, variable, other]. If this is
     *  all "none" the PAD field is not where we are looking, which is a different fault from a
     *  label that fails its CRC — and the two look identical from the outside. */
    uint32_t xIndCount(int i) const { return xInd_[i & 3]; }
    uint32_t appSeen(int i)   const { return appSeen_[i & 31]; }
    uint32_t overruns()       const { return overrun_; }
    uint32_t dseBad()         const { return dseBad_; }
    void reset() { dls_.reset(); pend_.clear(); pendLen_ = 0; lastApp_ = kXpadEnd; lastFieldLen_ = 0; mot_.reset(); motPend_.clear(); motLen_ = 0; motLenBuf_.clear(); }

private:
    /** ★ A field with no indicator list continues the previous sub-field (7.4.2.1/7.4.2.2). For
     *  the dynamic label that means "continuation" even when the last CI said "start" — passing 2
     *  again would restart the group from its own second half. */
    static uint8_t contApp(uint8_t app) {
        /* ★ The same for MOT: measured on 7D, 402 "starts" against 23 length indicators in a minute
         *  — every uncounted continuation restarted the group and no image ever completed. */
        return app == kXpadDlsStart ? kXpadDlsCont : app == kXpadMotStart ? kXpadMotCont : app;
    }
    /** ★ Accumulate one application's bytes, completing a data group when its declared length is
     *  reached. DLS groups are short and self-describing, so the length comes from the prefix. */
    void append(uint8_t app, const uint8_t* d, size_t n) {
        ++appSeen_[app & 31];
        if (app == kXpadDlsStart) {
            pend_.assign(d, d + n);
            pendLen_ = pend_.size() >= 1 ? size_t(pend_[0] & 0x0F) + 1 + 2 + 2 : 0;
            if (pend_.size() >= 1 && (pend_[0] & 0x10)) pendLen_ = 2 + 2;   // a command group has no text
            dlsBytes_ += uint32_t(n);
            flush();
        } else if (app == kXpadDlsCont) {
            if (pend_.empty()) return;              // continuation with no start — joined mid-group
            pend_.insert(pend_.end(), d, d + n);
            dlsBytes_ += uint32_t(n);
            flush();
        }
        else if (app == 1) {
            /* ★ Data group length indicator (7.4.5.1.1): 14-bit length + CRC, four bytes that may
             *  arrive split (short X-PAD sends 3 then 1). It names the length of the MOT data group
             *  that follows in app 12/13. */
            motLenBuf_.insert(motLenBuf_.end(), d, d + n);
            if (motLenBuf_.size() >= 4) {
                const uint16_t want = uint16_t((motLenBuf_[2] << 8) | motLenBuf_[3]);
                if (motCrc16(motLenBuf_.data(), 2) == want) motLen_ = size_t(((motLenBuf_[0] & 0x3F) << 8) | motLenBuf_[1]);
                else motLen_ = 0;
                motLenBuf_.clear();
            }
        } else if (app == kXpadMotStart) {
            motPend_.assign(d, d + n);
            motFlush();
        } else if (app == kXpadMotCont) {
            if (motPend_.empty()) return;
            motPend_.insert(motPend_.end(), d, d + n);
            motFlush();
        }
    }
    /** ★ A data group is complete when the length the indicator gave has arrived (the X-PAD sub-field
     *  may pad the last few bytes). */
    void motFlush() {
        if (motLen_ && motPend_.size() >= motLen_) {
            mot_.feedDataGroup(motPend_.data(), motLen_);
            motPend_.clear(); motLen_ = 0;
        }
    }
    void flush() {
        if (pendLen_ && pend_.size() >= pendLen_) {
            dls_.pushGroup(pend_.data(), pendLen_);
            pend_.clear(); pendLen_ = 0;
        }
    }

    DlsAssembler         dls_;
    MotAssembler         mot_;
    std::vector<uint8_t> motPend_, motLenBuf_;
    size_t               motLen_ = 0;
    std::vector<uint8_t> pend_;
    std::vector<uint8_t> logical_;
    size_t               pendLen_ = 0;
    uint8_t              lastApp_ = kXpadEnd;
    size_t               lastFieldLen_ = 0;
    uint32_t             frames_ = 0, dlsBytes_ = 0, overrun_ = 0, dseBad_ = 0;
    uint32_t             xInd_[4] = {0,0,0,0};
    uint32_t             appSeen_[32] = {0};
};

}  // namespace vibedab
