// vibe_dab_padtap.h — read a service's PAD (its dynamic label) WITHOUT decoding its audio.
//
// ★★★ WHAT THIS IS FOR. Stuart, 2026-09-07: "Is it possible to read the radio text for every
//     station in an ensemble simultaneously? … periodically poll all stations on the multiplex
//     maybe every 30 seconds." It is: the label rides in the PAD, and the PAD can be lifted from
//     a logical frame with no audio decode at all — for Layer II it is the frame's tail, for
//     DAB+ it is the data stream element at the start of each access unit, which needs only the
//     super frame's Reed-Solomon and header, never AAC. What it does cost is the sub-channel's
//     Viterbi, so the receiver runs a few extra sub-channels at a time and rotates through the
//     ensemble (see DabService::pumpScan) rather than all thirty at once.
//
// One PadTap per scanned sub-channel: it holds the LSF pairing and the five-frame super frame
// window exactly as the playing path does, and feeds one PadReader.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <vector>

#include "vibe_dab_aac.h"
#include "vibe_dab_mp2.h"
#include "vibe_dab_pad.h"

namespace vibedab {

class PadTap {
public:
    /** Feed one logical frame of the sub-channel; scType 0 = Layer II, 63 = DAB+. */
    void feed(const std::vector<uint8_t>& frame, int scType) {
        if (scType == 63) feedDabPlus(frame); else feedMp2(frame);
    }
    const DynamicLabel& label() const { return pad_.dls().label(); }
    uint32_t groupsOk() const { return pad_.dls().crcOk(); }
    void reset() { pad_.reset(); sf_.clear(); lsfPend_.clear(); }

private:
    void feedMp2(const std::vector<uint8_t>& f0) {
        // ★ A 24 kHz (LSF) frame spans two logical frames — join the halves, as drainAudio does.
        std::vector<uint8_t> f = f0;
        if (!lsfPend_.empty()) { lsfPend_.insert(lsfPend_.end(), f.begin(), f.end()); f.swap(lsfPend_); lsfPend_.clear(); }
        else {
            const Mp2Info hi = mp2Header(f.data(), f.size());
            if (hi.valid && size_t(hi.frameBytes) > f.size()) { lsfPend_ = f; return; }
        }
        const Mp2Info mi = mp2Header(f.data(), f.size());
        if (!mi.valid) return;
        // ★ The ScF-CRC sits between X-PAD and F-PAD — the same rule as the playing path.
        size_t scfCrcLen = 4;
        if (!mi.lsf && mi.bitrateKbps < (mi.channels == 1 ? 56 : 112)) scfCrcLen = 2;
        if (f.size() <= scfCrcLen + 2) return;
        const size_t fpadAt = f.size() - 2, xEnd = fpadAt - scfCrcLen;
        const size_t take = xEnd < 200 ? xEnd : 200;
        std::vector<uint8_t> win(f.begin() + long(xEnd - take), f.begin() + long(xEnd));
        win.push_back(f[fpadAt]); win.push_back(f[fpadAt + 1]);
        pad_.feed(win.data(), win.size());
    }
    void feedDabPlus(const std::vector<uint8_t>& frame) {
        if (frame.empty()) return;
        sf_.push_back(frame);
        if (sf_.size() > 5) sf_.pop_front();
        if (sf_.size() < 5) return;
        const size_t per = sf_.front().size();
        for (const auto& x : sf_) if (x.size() != per) { sf_.pop_front(); return; }
        const int index = int(per / 24);
        if (index < 1 || index > 24) { sf_.pop_front(); return; }
        std::vector<uint8_t> wire; wire.reserve(per * 5);
        for (const auto& x : sf_) wire.insert(wire.end(), x.begin(), x.end());
        const SuperFrame s = decodeSuperFrame(wire.data(), wire.size(), index);
        if (!s.valid || !s.firecodeOk) { sf_.pop_front(); return; }   // slide by one, as the player does
        sf_.clear();
        for (const auto& au : s.aus) pad_.feedAccessUnit(au.data(), au.size());
    }

    PadReader pad_;
    std::deque<std::vector<uint8_t>> sf_;
    std::vector<uint8_t> lsfPend_;
};

}  // namespace vibedab
