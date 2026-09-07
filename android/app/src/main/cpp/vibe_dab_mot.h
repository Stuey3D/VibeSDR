// vibe_dab_mot.h — MOT objects out of X-PAD: the slideshow (TS 101 499) that carries station
// logos and now-playing artwork over the air.
//
// ★★★ THE CHAIN, as the standards draw it:
//   X-PAD app 1  = data group length indicator (EN 300 401 7.4.5.1.1): 14-bit length + CRC.
//   X-PAD app 12 = start of an MSC data group, app 13 = its continuation (7.4.5.1.2).
//   MSC data group (5.3.3): header [ext, crc, segment, user-access flags | type | continuity |
//   repetition] [extension 16] [segment: last(1) number(15)] [user access: rfa(3) tid-flag(1)
//   length(4) [transport id 16] [end-user address]] data field [CRC-16].
//   Data field (EN 301 234): segmentation header [repetition(3) size(13)] then the segment.
//   Data group type 3 = a segment of the MOT HEADER, 4 = a segment of the MOT BODY, both keyed
//   by transport id. Header core (7 bytes): body size(28) header size(13) content type(6)
//   sub-type(9), then parameters [PLI(2) id(6) …]; content name is parameter 0x0C.
//   Slideshow: content type 2 (image), sub-type 1 JFIF (JPEG) or 3 PNG.
//
// ★ Measured on NNDAB 7D (2026-09-07): every service signals the slideshow application (FIG 0/13
//   type 0x002) and the MOT application types run continuously; 12B/10C/11A/11D signal none.
#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

namespace vibedab {

inline uint16_t motCrc16(const uint8_t* d, size_t n) {   // the same CRC-16 as the FIB and the DLS
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= uint16_t(d[i]) << 8;
        for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return uint16_t(~crc);
}

struct MotObject {
    uint16_t    transportId = 0;
    int         contentType = -1, subType = -1;
    std::string name;
    std::vector<uint8_t> body;
    std::string mime() const {
        if (contentType == 2 && subType == 1) return "image/jpeg";
        if (contentType == 2 && subType == 3) return "image/png";
        return "application/octet-stream";
    }
    bool isImage() const { return contentType == 2 && (subType == 1 || subType == 3); }
};

class MotAssembler {
public:
    /** One complete MSC data group, exactly as long as the length indicator said. */
    void feedDataGroup(const uint8_t* g, size_t n) {
        ++groups_;
        if (n < 2) { ++bad_; return; }
        const bool ext = (g[0] & 0x80) != 0, crc = (g[0] & 0x40) != 0, seg = (g[0] & 0x20) != 0, ua = (g[0] & 0x10) != 0;
        const int  type = g[0] & 0x0F;
        size_t p = 2;
        if (crc) {
            if (n < 4) { ++bad_; return; }
            const uint16_t want = uint16_t((g[n - 2] << 8) | g[n - 1]);
            if (motCrc16(g, n - 2) != want) { ++crcFail_; return; }
            n -= 2;
        }
        if (ext) p += 2;
        bool last = true; int segNum = 0;
        if (seg) { if (p + 2 > n) { ++bad_; return; } last = (g[p] & 0x80) != 0; segNum = ((g[p] & 0x7F) << 8) | g[p + 1]; p += 2; }
        uint16_t tid = 0; bool haveTid = false;
        if (ua) {
            if (p + 1 > n) { ++bad_; return; }
            const bool tidFlag = (g[p] & 0x10) != 0; const size_t len = g[p] & 0x0F; p += 1;
            if (p + len > n) { ++bad_; return; }
            if (tidFlag && len >= 2) { tid = uint16_t((g[p] << 8) | g[p + 1]); haveTid = true; }
            p += len;
        }
        if (!haveTid) { ++bad_; return; }                 // MOT needs the transport id
        if (type != 3 && type != 4) { ++other_; return; } // directory mode and the rest: not here
        if (p + 2 > n) { ++bad_; return; }
        const size_t segSize = ((g[p] & 0x1F) << 8) | g[p + 1];   // repetition(3) size(13)
        p += 2;
        if (p + segSize > n) { ++bad_; return; }
        Part& obj = parts_[tid];
        std::map<int, std::vector<uint8_t>>& segs = (type == 3) ? obj.hdr : obj.body;
        int& lastN = (type == 3) ? obj.hdrLast : obj.bodyLast;
        segs[segNum].assign(g + p, g + p + segSize);
        if (last) lastN = segNum;
        tryComplete(tid, obj);
        if (parts_.size() > 8) parts_.erase(parts_.begin());   // a bounded scratch: objects are small and few
    }

    /** The newest complete image, once. */
    bool take(MotObject& out) {
        if (!ready_) return false;
        out = done_; ready_ = false;
        return true;
    }
    void reset() { parts_.clear(); ready_ = false; }
    uint32_t groups() const { return groups_; }
    uint32_t crcFails() const { return crcFail_; }
    uint32_t objects() const { return objects_; }

private:
    struct Part {
        std::map<int, std::vector<uint8_t>> hdr, body;
        int hdrLast = -1, bodyLast = -1;
        bool emitted = false;
    };
    static bool complete(const std::map<int, std::vector<uint8_t>>& segs, int lastN) {
        if (lastN < 0) return false;
        for (int i = 0; i <= lastN; ++i) if (!segs.count(i)) return false;
        return true;
    }
    static std::vector<uint8_t> join(const std::map<int, std::vector<uint8_t>>& segs, int lastN) {
        std::vector<uint8_t> out;
        for (int i = 0; i <= lastN; ++i) { const auto& s = segs.at(i); out.insert(out.end(), s.begin(), s.end()); }
        return out;
    }
    void tryComplete(uint16_t tid, Part& obj) {
        if (obj.emitted || !complete(obj.hdr, obj.hdrLast) || !complete(obj.body, obj.bodyLast)) return;
        const std::vector<uint8_t> h = join(obj.hdr, obj.hdrLast);
        if (h.size() < 7) { ++bad_; obj.emitted = true; return; }
        uint64_t v = 0; for (int i = 0; i < 7; ++i) v = (v << 8) | h[size_t(i)];
        const uint32_t bodySize   = uint32_t(v >> 28) & 0x0FFFFFFF;
        const uint32_t headerSize = uint32_t(v >> 15) & 0x1FFF;
        MotObject o;
        o.transportId = tid;
        o.contentType = int((v >> 9) & 0x3F);
        o.subType     = int(v & 0x1FF);
        // parameters
        size_t p = 7; const size_t end = std::min(h.size(), size_t(headerSize));
        while (p < end) {
            const int pli = h[p] >> 6, id = h[p] & 0x3F; p += 1;
            size_t len = 0;
            if (pli == 1) len = 1; else if (pli == 2) len = 4;
            else if (pli == 3) { if (p >= end) break; if (h[p] & 0x80) { if (p + 1 >= end) break; len = ((h[p] & 0x7F) << 8) | h[p + 1]; p += 2; } else { len = h[p] & 0x7F; p += 1; } }
            if (p + len > end) break;
            if (id == 0x0C && len >= 1) o.name.assign(reinterpret_cast<const char*>(&h[p + 1]), len - 1);   // charset byte first
            p += len;
        }
        o.body = join(obj.body, obj.bodyLast);
        obj.emitted = true;
        if (bodySize && o.body.size() != bodySize) { ++bad_; return; }
        if (!o.isImage()) { ++other_; return; }
        done_ = std::move(o); ready_ = true; ++objects_;
    }

    std::map<uint16_t, Part> parts_;
    MotObject done_;
    bool ready_ = false;
    uint32_t groups_ = 0, crcFail_ = 0, bad_ = 0, other_ = 0, objects_ = 0;
};

/** ★★★ DIRECTORY MODE (EN 301 234 clause 7): the carousel a data service such as SPI uses. The
 *  directory (data group type 6; 7 = compressed, counted and skipped) lists every object's
 *  transport id with its full header — name, type, body size — and the bodies arrive as type-4
 *  segments keyed by transport id. An object is complete when its body has reached the size the
 *  directory gave. Objects are kept by content name, which is what the SI document refers to. */
class MotCarousel {
public:
    struct Object { std::string name; int contentType = -1, subType = -1; uint32_t bodySize = 0; std::vector<uint8_t> body; bool complete = false; };

    void feedDataGroup(const uint8_t* g, size_t n) {
        ++groups_;
        if (n < 2) return;
        const bool ext = (g[0] & 0x80) != 0, crc = (g[0] & 0x40) != 0, seg = (g[0] & 0x20) != 0, ua = (g[0] & 0x10) != 0;
        const int type = g[0] & 0x0F;
        size_t p = 2;
        if (crc) { if (n < 4) return; const uint16_t want = uint16_t((g[n - 2] << 8) | g[n - 1]); if (motCrc16(g, n - 2) != want) { ++crcFail_; return; } n -= 2; }
        if (ext) p += 2;
        bool last = true; int segNum = 0;
        if (seg) { if (p + 2 > n) return; last = (g[p] & 0x80) != 0; segNum = ((g[p] & 0x7F) << 8) | g[p + 1]; p += 2; }
        uint16_t tid = 0; bool haveTid = false;
        if (ua) { if (p + 1 > n) return; const bool tf = (g[p] & 0x10) != 0; const size_t len = g[p] & 0x0F; p += 1; if (p + len > n) return; if (tf && len >= 2) { tid = uint16_t((g[p] << 8) | g[p + 1]); haveTid = true; } p += len; }
        if (p + 2 > n) return;
        const size_t segSize = ((g[p] & 0x1F) << 8) | g[p + 1]; p += 2;
        if (p + segSize > n) return;
        if (type == 6) {
            if (haveTid && tid != dirTid_) { dirSegs_.clear(); dirLast_ = -1; dirTid_ = tid; }   // a new directory
            dirSegs_[segNum].assign(g + p, g + p + segSize);
            if (last) dirLast_ = segNum;
            if (dirLast_ >= 0) { bool ok = true; for (int i = 0; i <= dirLast_; ++i) if (!dirSegs_.count(i)) { ok = false; break; } if (ok) parseDirectory(); }
        } else if (type == 7) {
            ++compressed_;
        } else if (type == 4 && haveTid) {
            auto it = byTid_.find(tid);
            if (it == byTid_.end()) return;                         // a body for an object the directory has not named yet
            Object& o = objects_[it->second];
            if (o.complete) return;
            std::map<int, std::vector<uint8_t>>& segs = bodySegs_[tid];
            segs[segNum].assign(g + p, g + p + segSize);
            if (last) bodyLast_[tid] = segNum;
            auto bl = bodyLast_.find(tid);
            if (bl != bodyLast_.end()) {
                bool ok = true; size_t total = 0;
                for (int i = 0; i <= bl->second; ++i) { auto s2 = segs.find(i); if (s2 == segs.end()) { ok = false; break; } total += s2->second.size(); }
                if (ok && (o.bodySize == 0 || total == o.bodySize)) {
                    o.body.clear(); o.body.reserve(total);
                    for (int i = 0; i <= bl->second; ++i) { const auto& s2 = segs[i]; o.body.insert(o.body.end(), s2.begin(), s2.end()); }
                    o.complete = true; ++completed_; ++version_;
                    bodySegs_.erase(tid); bodyLast_.erase(tid);
                }
            }
        }
    }
    const std::map<std::string, Object>& objects() const { return objects_; }
    const Object* find(const std::string& name) const { auto it = objects_.find(name); return it == objects_.end() || !it->second.complete ? nullptr : &it->second; }
    uint32_t version() const { return version_; }        ///< bumps whenever an object completes
    uint32_t groups() const { return groups_; }
    uint32_t crcFails() const { return crcFail_; }
    uint32_t completed() const { return completed_; }
    size_t   named() const { return objects_.size(); }
    bool     haveDirectory() const { return haveDir_; }
    void reset() { dirSegs_.clear(); dirLast_ = -1; dirTid_ = 0; objects_.clear(); byTid_.clear(); bodySegs_.clear(); bodyLast_.clear(); haveDir_ = false; }

private:
    void parseDirectory() {
        std::vector<uint8_t> d;
        for (int i = 0; i <= dirLast_; ++i) { const auto& s = dirSegs_[i]; d.insert(d.end(), s.begin(), s.end()); }
        if (d.size() < 13) return;
        /* CF(1) rfu(1) DirectorySize(30) | NumberOfObjects(16) | DataCarouselPeriod(24) |
         * rfu(1) rfa(2) SegmentSize(13) | DirectoryExtensionLength(16) | extension | entries */
        const uint32_t nObj = (uint32_t(d[4]) << 8) | d[5];
        const size_t extLen = (size_t(d[11]) << 8) | d[12];
        size_t p = 13 + extLen;
        std::map<uint16_t, std::string> newByTid;
        for (uint32_t k = 0; k < nObj && p + 9 <= d.size(); ++k) {
            const uint16_t tid = uint16_t((d[p] << 8) | d[p + 1]); p += 2;
            uint64_t v = 0; for (int i = 0; i < 7; ++i) v = (v << 8) | d[p + size_t(i)];
            const uint32_t bodySize = uint32_t(v >> 28) & 0x0FFFFFFF;
            const uint32_t hdrSize  = uint32_t(v >> 15) & 0x1FFF;
            const int ct = int((v >> 9) & 0x3F), st = int(v & 0x1FF);
            if (hdrSize < 7 || p + hdrSize > d.size()) break;
            std::string name;
            size_t q = p + 7; const size_t end = p + hdrSize;
            while (q < end) {
                const int pli = d[q] >> 6, id = d[q] & 0x3F; q += 1; size_t len = 0;
                if (pli == 1) len = 1; else if (pli == 2) len = 4;
                else if (pli == 3) { if (q >= end) break; if (d[q] & 0x80) { if (q + 1 >= end) break; len = ((d[q] & 0x7F) << 8) | d[q + 1]; q += 2; } else { len = d[q] & 0x7F; q += 1; } }
                if (q + len > end) break;
                if (id == 0x0C && len >= 1) name.assign(reinterpret_cast<const char*>(&d[q + 1]), len - 1);
                q += len;
            }
            p += hdrSize;
            if (name.empty()) continue;
            Object& o = objects_[name];
            if (o.bodySize != bodySize || o.contentType != ct) { o = Object{}; }   // a changed object starts again
            o.name = name; o.contentType = ct; o.subType = st; o.bodySize = bodySize;
            newByTid[tid] = name;
        }
        byTid_ = newByTid;
        haveDir_ = true;
    }

    std::map<int, std::vector<uint8_t>> dirSegs_; int dirLast_ = -1; uint16_t dirTid_ = 0;
    std::map<std::string, Object> objects_;
    std::map<uint16_t, std::string> byTid_;
    std::map<uint16_t, std::map<int, std::vector<uint8_t>>> bodySegs_;
    std::map<uint16_t, int> bodyLast_;
    bool haveDir_ = false;
    uint32_t groups_ = 0, crcFail_ = 0, completed_ = 0, compressed_ = 0, version_ = 0;
};

}  // namespace vibedab
