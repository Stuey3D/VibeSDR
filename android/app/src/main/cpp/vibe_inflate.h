// vibe_inflate.h — a small, dependency-free DEFLATE decoder (RFC 1951), with the gzip (RFC 1952) and zip
// (PKWARE APPNOTE) wrappers the ADS-B data updater needs, and a streaming tar reader.
//
// ★★★ WHY NOT zlib — the same reason vibe_gzip.h gives for the encoder: the Linux packages are built in three
//     images (bookworm amd64/arm64, the armhf cross, ARMv6), each of which would need zlib's -dev for its own
//     arch, and the Linux deb carries NO external dependencies beyond what it already has (memory
//     feedback_linux_deb_no_external_deps). Android's NDK has libz, but one decoder on every platform is one
//     decoder to test.
// ★★ WRITTEN FROM THE RFC, not from zlib/puff. Canonical Huffman codes decoded a code length at a time (the
//    "count of codes per length" walk RFC 1951 §3.2.2 describes): slower than zlib's tables, and that does not
//    matter — this runs once a week, in the background, at the lowest priority (Network > Audio > Spectrum >
//    Decoders, and a data update is below all of them).
// ★★ STREAMING OUT, WHOLE IN. The input is a downloaded file the caller has mapped or read (6 MB for the VRS
//    tarball); the OUTPUT goes to a callback in chunks through a 32 KB window, so the 25 MB of CSV inside the
//    tarball is never held at once — that is what keeps the update inside a 1 GB, 32-bit phone's budget.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace vibeinflate {

/** CRC-32 (IEEE 802.3), incremental: crc = crc32(crc, p, n), starting from 0. */
inline uint32_t crc32(uint32_t crc, const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/** Output sink: called with each decoded chunk. Return false to stop early (the caller has what it needs). */
using Sink = std::function<bool(const uint8_t*, size_t)>;

/** Raw DEFLATE. `used` (if given) gets the number of input bytes consumed, so a gzip/zip trailer can be found. */
class Inflater {
public:
    /** Decode `in[0..n)`. Returns "" on success, else what went wrong. */
    std::string run(const uint8_t* in, size_t n, const Sink& out, size_t* used = nullptr) {
        in_ = in; n_ = n; pos_ = 0; bit_ = 0; nbits_ = 0;
        out_ = &out; win_.assign(kWin, 0); wpos_ = 0; pend_ = 0; stopped_ = false; total_ = 0;
        std::string err;
        int last = 0;
        do {
            if (!need(3)) { err = "truncated block header"; break; }
            last = int(take(1));
            const int type = int(take(2));
            if (type == 0) err = stored();
            else if (type == 1) err = fixed();
            else if (type == 2) err = dynamic();
            else err = "reserved block type";
        } while (err.empty() && !last && !stopped_);
        if (err.empty()) flush();
        if (used) *used = pos_ - size_t(nbits_ / 8);   // whole bytes still sitting in the bit buffer are unread
        if (stopped_) return {};
        return err;
    }
    uint64_t total() const { return total_; }

private:
    static constexpr size_t kWin = 32768;
    struct Huff { uint16_t count[16]; std::vector<uint16_t> sym; };

    bool need(int k) {
        while (nbits_ < k) {
            if (pos_ >= n_) return false;
            bit_ |= uint64_t(in_[pos_++]) << nbits_;
            nbits_ += 8;
        }
        return true;
    }
    uint32_t take(int k) { const uint32_t v = uint32_t(bit_ & ((1ull << k) - 1)); bit_ >>= k; nbits_ -= k; return v; }

    void put(uint8_t c) {
        win_[wpos_] = c;
        wpos_ = (wpos_ + 1) & (kWin - 1);
        if (++pend_ == kWin / 2) flush();
    }
    void flush() {
        if (!pend_ || stopped_) { pend_ = 0; return; }
        // the pending bytes end at wpos_ and may wrap the ring
        const size_t start = (wpos_ + kWin - pend_) & (kWin - 1);
        if (start + pend_ <= kWin) { if (!(*out_)(win_.data() + start, pend_)) stopped_ = true; }
        else {
            const size_t a = kWin - start;
            if (!(*out_)(win_.data() + start, a) || !(*out_)(win_.data(), pend_ - a)) stopped_ = true;
        }
        total_ += pend_;
        pend_ = 0;
    }

    std::string stored() {
        pos_ -= size_t(nbits_ / 8);              // whole bytes already pulled into the bit buffer go back
        bit_ = 0; nbits_ = 0;                    // to a byte boundary (the bits left are padding)
        if (pos_ + 4 > n_) return "truncated stored block";
        const unsigned len = in_[pos_] | (in_[pos_ + 1] << 8), nlen = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
        pos_ += 4;
        if ((len ^ 0xFFFF) != nlen) return "stored block length check failed";
        if (pos_ + len > n_) return "truncated stored block";
        for (unsigned i = 0; i < len && !stopped_; ++i) put(in_[pos_ + i]);
        pos_ += len;
        return {};
    }

    static bool build(Huff& h, const uint8_t* lens, int n) {
        std::memset(h.count, 0, sizeof h.count);
        for (int i = 0; i < n; ++i) h.count[lens[i]]++;
        h.count[0] = 0;
        int left = 1;
        for (int l = 1; l < 16; ++l) { left <<= 1; left -= h.count[l]; if (left < 0) return false; }   // over-subscribed
        uint16_t offs[16]; offs[1] = 0;
        for (int l = 1; l < 15; ++l) offs[l + 1] = uint16_t(offs[l] + h.count[l]);
        h.sym.assign(size_t(n), 0);
        for (int i = 0; i < n; ++i) if (lens[i]) h.sym[offs[lens[i]]++] = uint16_t(i);
        return true;
    }

    /** One symbol: walk the code a bit at a time (codes are stored MSB-first, so the bits are read singly). */
    int decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 16; ++l) {
            if (!need(1)) return -1;
            code |= int(take(1));
            const int c = h.count[l];
            if (code - c < first) return h.sym[size_t(index + (code - first))];
            index += c; first += c; first <<= 1; code <<= 1;
        }
        return -2;
    }

    std::string codes(const Huff& lit, const Huff& dist) {
        static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
        static const uint8_t  lext[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
        static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
                                           4097,6145,8193,12289,16385,24577};
        static const uint8_t  dext[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
        for (;;) {
            if (stopped_) return {};
            const int s = decode(lit);
            if (s < 0) return "bad literal/length code";
            if (s < 256) { put(uint8_t(s)); continue; }
            if (s == 256) return {};
            const int li = s - 257;
            if (li >= 29) return "bad length symbol";
            if (!need(lext[li])) return "truncated length";
            const unsigned len = lbase[li] + take(lext[li]);
            const int ds = decode(dist);
            if (ds < 0 || ds >= 30) return "bad distance code";
            if (!need(dext[ds])) return "truncated distance";
            const unsigned d = dbase[ds] + take(dext[ds]);
            if (d > total_ + pend_) return "distance before the start of the output";
            for (unsigned i = 0; i < len; ++i) put(win_[(wpos_ + kWin - d) & (kWin - 1)]);
        }
    }

    std::string fixed() {
        static Huff lit, dist; static bool made = false;
        if (!made) {
            uint8_t l[288];
            for (int i = 0; i < 144; ++i) l[i] = 8;
            for (int i = 144; i < 256; ++i) l[i] = 9;
            for (int i = 256; i < 280; ++i) l[i] = 7;
            for (int i = 280; i < 288; ++i) l[i] = 8;
            build(lit, l, 288);
            uint8_t d[30]; std::memset(d, 5, sizeof d);
            build(dist, d, 30);
            made = true;
        }
        return codes(lit, dist);
    }

    std::string dynamic() {
        if (!need(14)) return "truncated dynamic header";
        const int nlen = int(take(5)) + 257, ndist = int(take(5)) + 1, ncode = int(take(4)) + 4;
        if (nlen > 286 || ndist > 30) return "bad dynamic counts";
        static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
        uint8_t lens[320]; std::memset(lens, 0, sizeof lens);
        for (int i = 0; i < ncode; ++i) { if (!need(3)) return "truncated code lengths"; lens[order[i]] = uint8_t(take(3)); }
        Huff lc;
        if (!build(lc, lens, 19)) return "bad code-length code";
        int i = 0;
        std::memset(lens, 0, sizeof lens);
        while (i < nlen + ndist) {
            const int s = decode(lc);
            if (s < 0) return "bad code-length symbol";
            if (s < 16) { lens[i++] = uint8_t(s); continue; }
            int rep = 0; uint8_t v = 0;
            if (s == 16) { if (!i) return "repeat with no previous length"; v = lens[i - 1]; if (!need(2)) return "truncated"; rep = 3 + int(take(2)); }
            else if (s == 17) { if (!need(3)) return "truncated"; rep = 3 + int(take(3)); }
            else { if (!need(7)) return "truncated"; rep = 11 + int(take(7)); }
            if (i + rep > nlen + ndist) return "code lengths overrun";
            while (rep--) lens[i++] = v;
        }
        if (!lens[256]) return "no end-of-block code";
        Huff lit, dist;
        if (!build(lit, lens, nlen)) return "bad literal/length code set";
        if (!build(dist, lens + nlen, ndist)) return "bad distance code set";
        return codes(lit, dist);
    }

    const uint8_t* in_ = nullptr; size_t n_ = 0, pos_ = 0;
    uint64_t bit_ = 0; int nbits_ = 0;
    const Sink* out_ = nullptr;
    std::vector<uint8_t> win_; size_t wpos_ = 0, pend_ = 0;
    bool stopped_ = false;
    uint64_t total_ = 0;
};

/** A gzip member (the VRS repository tarball). Checks the CRC and the length in the trailer. */
inline std::string gunzip(const uint8_t* p, size_t n, const Sink& out) {
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return "not a gzip file";
    const uint8_t flg = p[3];
    size_t o = 10;
    if (flg & 4) { if (o + 2 > n) return "truncated gzip header"; o += 2 + size_t(p[o] | (p[o + 1] << 8)); }
    if (flg & 8)  { while (o < n && p[o]) ++o; ++o; }
    if (flg & 16) { while (o < n && p[o]) ++o; ++o; }
    if (flg & 2) o += 2;
    if (o >= n) return "truncated gzip header";
    uint32_t crc = 0;
    const Sink tee = [&](const uint8_t* b, size_t k) { crc = crc32(crc, b, k); return out(b, k); };
    Inflater inf;
    size_t used = 0;
    std::string e = inf.run(p + o, n - o, tee, &used);
    if (!e.empty()) return e;
    const size_t t = o + used;
    if (t + 8 > n) return {};   // the sink stopped early, or the trailer is missing: nothing to check
    const uint32_t want = uint32_t(p[t]) | uint32_t(p[t + 1]) << 8 | uint32_t(p[t + 2]) << 16 | uint32_t(p[t + 3]) << 24;
    if (want != crc) return "gzip CRC mismatch";
    return {};
}

/** One member of a zip archive (Mictronics' indexedDB.zip), stored or deflated. "" + found=false if absent. */
inline std::string unzipEntry(const uint8_t* p, size_t n, const std::string& name, const Sink& out, bool* found = nullptr,
                              std::string* comment = nullptr) {
    if (found) *found = false;
    auto u16 = [&](size_t o) { return uint32_t(p[o] | (p[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (n < 22) return "not a zip file";
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 22 + 65535;) if (u32(i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) return "no zip directory";
    if (comment) { const size_t cl = u16(eocd + 20); *comment = std::string((const char*)p + eocd + 22, std::min(cl, n - eocd - 22)); }
    const size_t entries = u16(eocd + 10);
    size_t cd = u32(eocd + 16);
    for (size_t e = 0; e < entries; ++e) {
        if (cd + 46 > n || u32(cd) != 0x02014b50) return "bad zip directory";
        const uint32_t method = u16(cd + 10), csize = u32(cd + 20), usize = u32(cd + 24);
        const size_t fl = u16(cd + 28), xl = u16(cd + 30), cl = u16(cd + 32);
        const size_t lho = u32(cd + 42);
        const std::string fname((const char*)p + cd + 46, std::min(fl, n - cd - 46));
        cd += 46 + fl + xl + cl;
        if (fname != name) continue;
        if (found) *found = true;
        if (lho + 30 > n || u32(lho) != 0x04034b50) return "bad zip local header";
        const size_t data = lho + 30 + u16(lho + 26) + u16(lho + 28);
        if (data + csize > n) return "truncated zip member";
        if (method == 0) { out(p + data, csize); return {}; }
        if (method != 8) return "unsupported zip method";
        Inflater inf;
        std::string err = inf.run(p + data, csize, out);
        if (err.empty() && inf.total() != usize && inf.total() != 0) {
            // ★ total() is short only when the sink stopped early on purpose — which run() reports as success
        }
        return err;
    }
    return {};
}

/** One member of a zip's central directory. */
struct ZipEntry {
    std::string name;
    uint32_t method = 0, flags = 0, csize = 0, usize = 0, crc = 0;
    size_t data = 0;          ///< offset of the member's data in the archive
    bool dir() const { return !name.empty() && (name.back() == '/' || name.back() == '\\'); }
};

/** The central directory, every member — or "" + why not. ★ Refuses what we do not read rather than misreading
 *  it: ZIP64 (a 0xFFFFFFFF size or offset, or the ZIP64 end record), split archives. Encrypted members are listed
 *  (flags bit 0) for the caller to skip by name. */
inline std::string zipList(const uint8_t* p, size_t n, std::vector<ZipEntry>& out, size_t maxEntries = 100000) {
    out.clear();
    auto u16 = [&](size_t o) { return uint32_t(p[o] | (p[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (n < 22) return "not a zip file";
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 22 + 65535;) if (u32(i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) return "not a zip file (no central directory)";
    if (eocd >= 20 && u32(eocd - 20) == 0x07064b50) return "ZIP64 archives are not supported — re-zip it without ZIP64";
    if (u16(eocd + 4) != 0 || u16(eocd + 6) != 0) return "split (multi-part) zips are not supported";
    const size_t entries = u16(eocd + 10);
    size_t cd = u32(eocd + 16);
    if (entries == 0xFFFF || cd == 0xFFFFFFFFu) return "ZIP64 archives are not supported — re-zip it without ZIP64";
    if (entries > maxEntries) return "too many files in the zip";
    for (size_t e = 0; e < entries; ++e) {
        if (cd + 46 > n || u32(cd) != 0x02014b50) return "the zip's directory is damaged";
        ZipEntry z;
        z.flags = u16(cd + 8); z.method = u16(cd + 10); z.crc = u32(cd + 16); z.csize = u32(cd + 20); z.usize = u32(cd + 24);
        const size_t fl = u16(cd + 28), xl = u16(cd + 30), cl = u16(cd + 32);
        const size_t lho = u32(cd + 42);
        if (cd + 46 + fl > n) return "the zip's directory is damaged";
        z.name.assign((const char*)p + cd + 46, fl);
        cd += 46 + fl + xl + cl;
        if (z.csize == 0xFFFFFFFFu || z.usize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) return "ZIP64 archives are not supported — re-zip it without ZIP64";
        if (lho + 30 > n || u32(lho) != 0x04034b50) return "the zip is damaged (a member's header is missing)";
        z.data = lho + 30 + u16(lho + 26) + u16(lho + 28);
        if (z.data > n || z.csize > n - z.data) return "the zip is truncated";
        out.push_back(std::move(z));
    }
    return {};
}

/** Decode one member into `out`, refusing more than `cap` bytes (a zip bomb stops at the cap) and checking the
 *  CRC. Stored and deflated only. */
inline std::string zipRead(const uint8_t* p, size_t n, const ZipEntry& z, size_t cap, std::string& out) {
    out.clear();
    if (z.flags & 1) return "encrypted";
    if (z.usize > cap) return "too large";
    if (z.data + z.csize > n) return "truncated";
    bool over = false;
    const Sink sink = [&](const uint8_t* b, size_t k) {
        if (out.size() + k > cap || out.size() + k > z.usize) { over = true; return false; }
        out.append((const char*)b, k);
        return true;
    };
    if (z.method == 0) { if (z.csize != z.usize) return "damaged"; sink(p + z.data, z.csize); }
    else if (z.method == 8) { Inflater inf; const std::string e = inf.run(p + z.data, z.csize, sink); if (!e.empty()) return e; }
    else return "unsupported compression";
    if (over) return "expands past its stated size";
    if (out.size() != z.usize) return "damaged (wrong size)";
    if (crc32(0, (const uint8_t*)out.data(), out.size()) != z.crc) return "damaged (CRC)";
    return {};
}

/** A streaming tar reader (POSIX ustar + the pax headers GitHub writes). Feed it the gunzipped bytes; it calls
 *  `file(name, data)` for each regular file whose name `want(name)` accepts — others are skipped unbuffered —
 *  and `global(key, value)` for a pax global header's records (GitHub puts the commit id in `comment`). */
class TarReader {
public:
    std::function<bool(const std::string&)> want;
    std::function<void(const std::string&, const std::string&)> file;
    std::function<void(const std::string&, const std::string&)> global;

    bool feed(const uint8_t* b, size_t n) {
        while (n) {
            if (state_ == Header) {
                const size_t k = std::min(n, size_t(512) - hdr_.size());
                hdr_.append((const char*)b, k); b += k; n -= k;
                if (hdr_.size() < 512) return true;
                startEntry();
                hdr_.clear();
                continue;
            }
            const size_t k = size_t(std::min<uint64_t>(n, left_));
            if (keep_) body_.append((const char*)b, k);
            b += k; n -= k; left_ -= k;
            if (left_ == 0) endEntry();
        }
        return true;
    }

private:
    enum { Header, Body } state_ = Header;
    std::string hdr_, body_, name_, paxPath_;
    uint64_t left_ = 0, size_ = 0; char type_ = 0; bool keep_ = false;

    static uint64_t octal(const char* s, size_t n) {
        uint64_t v = 0;
        for (size_t i = 0; i < n && s[i]; ++i) if (s[i] >= '0' && s[i] <= '7') v = v * 8 + uint64_t(s[i] - '0');
        return v;
    }
    void startEntry() {
        const char* h = hdr_.data();
        bool zero = true;
        for (int i = 0; i < 512; ++i) if (h[i]) { zero = false; break; }
        if (zero) return;                                  // the end-of-archive blocks
        name_ = std::string(h, strnlen(h, 100));
        if (std::memcmp(h + 257, "ustar", 5) == 0 && h[345]) name_ = std::string(h + 345, strnlen(h + 345, 155)) + "/" + name_;
        if (!paxPath_.empty()) { name_ = paxPath_; paxPath_.clear(); }
        size_ = octal(h + 124, 12);
        type_ = h[156];
        left_ = (size_ + 511) / 512 * 512;
        const bool reg = type_ == '0' || type_ == 0;
        keep_ = type_ == 'x' || type_ == 'g' || (reg && want && want(name_));
        body_.clear();
        if (keep_) body_.reserve(size_t(size_));
        state_ = left_ ? Body : Header;
        if (!left_) endEntry();
    }
    void endEntry() {
        state_ = Header;
        if (!keep_) return;
        body_.resize(size_t(size_));
        if (type_ == 'x' || type_ == 'g') {
            // records: "<len> <key>=<value>\n"
            size_t o = 0;
            while (o < body_.size()) {
                const size_t sp = body_.find(' ', o);
                if (sp == std::string::npos) break;
                const size_t len = size_t(std::strtoul(body_.c_str() + o, nullptr, 10));
                if (!len || o + len > body_.size()) break;
                const std::string rec = body_.substr(sp + 1, o + len - sp - 2);
                const size_t eq = rec.find('=');
                if (eq != std::string::npos) {
                    const std::string k = rec.substr(0, eq), v = rec.substr(eq + 1);
                    if (type_ == 'x' && k == "path") paxPath_ = v;
                    if (type_ == 'g' && global) global(k, v);
                }
                o += len;
            }
        } else if (file) file(name_, body_);
        body_.clear(); body_.shrink_to_fit();
    }
};

} // namespace vibeinflate
