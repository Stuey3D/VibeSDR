// vibe_dab_packet.h — packet mode (EN 300 401 5.3.2): the transport under a DAB data service such
// as SPI ("BBC Guide" on 12B, measured 2026-09-07). Packets of 24/48/72/96 bytes ride in the
// sub-channel's logical frames; those with one address form a series (first … last) that carries
// one MSC data group (5.3.3), which MOT then reads.
#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>
#include <algorithm>
#include "vibe_dab_mot.h"   // motCrc16 — annex E's CRC-16, the same as the FIB's

namespace vibedab {

class PacketAssembler {
public:
    using GroupFn = std::function<void(const uint8_t*, size_t)>;
    void setAddress(int addr) { addr_ = addr; }
    void setSink(GroupFn fn) { sink_ = std::move(fn); }

    /** One logical frame of the packet-mode sub-channel: an integral number of packets. */
    void feedFrame(const uint8_t* fr, size_t fn) {
        /* ★ Parsed per frame. ✗ Dead end, measured on 12B: treating the frames as one stream and
         *  resyncing on a CRC failure halved the "failures" but doubled the lost groups. The
         *  failures were never packets: a logical frame is padded with zero bytes, and an
         *  all-zero block reads as a 24-byte packet at address 0 whose CRC cannot match. */
        const uint8_t* f = fr; const size_t n = fn;
        size_t p = 0;
        while (p + 5 <= n) {
            if (f[p] == 0 && f[p + 1] == 0 && f[p + 2] == 0) { p += 24; continue; }   // zero padding, not a packet
            /* Header (5.3.2.1, 24 bits): length(2) continuity(2) first(1) last(1) address(10)
             * command(1) useful length(7). Packet length code → 24/48/72/96 bytes in all. */
            const int lenCode = f[p] >> 6;
            const size_t plen = size_t(lenCode + 1) * 24;
            if (p + plen > n) { ++short_; break; }
            const int  cont  = (f[p] >> 4) & 0x03;
            const bool first = (f[p] & 0x08) != 0, last = (f[p] & 0x04) != 0;
            const int  addr  = ((f[p] & 0x03) << 8) | f[p + 1];
            const bool cmd   = (f[p + 2] & 0x80) != 0;
            const size_t useful = f[p + 2] & 0x7F;
            /* ★ FILL, NOT A PACKET. Measured on 12B (2026-09-08): every CRC "failure" was a 24-byte
             *  block with the same header bytes and a useful length of 94 — impossible in a
             *  24-byte packet (max 19) — at a steady 18 % of the stream while no data group was
             *  ever lost. The multiplexer pads the sub-channel with a fixed pattern; it is skipped
             *  and counted as fill, so the pane's "bad" count means bad. */
            if (useful > plen - 5) { ++fill_; p += plen; continue; }
            const uint16_t want = uint16_t((f[p + plen - 2] << 8) | f[p + plen - 1]);
            if (motCrc16(f + p, plen - 2) != want) { ++crcFail_; ++failByLen_[lenCode]; lastFail_[0] = f[p]; lastFail_[1] = f[p+1]; lastFail_[2] = f[p+2]; lastFailAddr_ = addr; p += plen; continue; }
            ++packets_;
            if (addr == 0 || cmd || (addr_ >= 0 && addr != addr_)) { p += plen; continue; }
            const uint8_t* d = f + p + 3;
            if (first) { buf_.clear(); open_ = true; }
            else if (open_ && ((lastCont_ + 1) & 3) != cont) { open_ = false; ++lost_; }   // a gap: this series is broken
            if (open_) {
                buf_.insert(buf_.end(), d, d + useful);
                lastCont_ = cont;
                if (last) { open_ = false; ++groups_; if (sink_) sink_(buf_.data(), buf_.size()); buf_.clear(); }
            }
            p += plen;
        }
    }
    void reset() { buf_.clear(); open_ = false; }
    uint32_t packets() const { return packets_; }
    uint32_t groups() const { return groups_; }
    uint32_t crcFails() const { return crcFail_; }
    uint32_t lost() const { return lost_; }
    uint32_t fill() const { return fill_; }
    uint32_t failByLen(int i) const { return failByLen_[i & 3]; }
    const uint8_t* lastFail() const { return lastFail_; }
    int lastFailAddr() const { return lastFailAddr_; }

private:
    int addr_ = -1;
    GroupFn sink_;
    std::vector<uint8_t> buf_;
    bool open_ = false;
    int  lastCont_ = 0;
    uint32_t packets_ = 0, groups_ = 0, crcFail_ = 0, lost_ = 0, short_ = 0, fill_ = 0;
    uint32_t failByLen_[4] = {0,0,0,0}; uint8_t lastFail_[3] = {0,0,0}; int lastFailAddr_ = -1;
};

}  // namespace vibedab
