// vibe_ncdxf_run.h — V12 2026-11-16 one NCDXF beacon tracker inside a decoder host (vibe_decoder_host.h, ext "ncdxf"): a
// listener's decoder on their audio. The engine is decoders/ncdxf_tracker.h; this is what turns its slots into what the
// clients are sent. docs/v12/NCDXF-BEACONS.md.
//
// Wire (on the decoder socket):
//   0x01 | u64 BE ts | u32 BE len | UTF-8     "08:05:10z  14.100  OH2B  Lohja, Finland · 4 steps (down to 0.1 W) ·
//                                             19 dB · ID ✓" — one line per beacon HEARD (any client that shows decoder
//                                             text shows these)
//   0x03 | u8 state                           0 listening (not on a beacon frequency) · 3 tracking a band
//   {"type":"ncdxf","data":{"now":UTC s,"band":0–4|-1,"tone":Hz,"slot":{"beacon":i,"at":UTC s},
//                           "last":{beacon,band,at,steps,snr:[4],id,other,lag,off}|null,
//                           "beacons":[[call,where,lat,lon]…],"bands":[Hz…],
//                           "cells":[[beacon,band,heardAgo,steps,snr,id,triedAgo,best,bestAgo]…]}}
//                                             every slot (10 s) and when the band changes; a cell exists once tried
// ★ The band comes from the listener's dial and mode (the host's audio→RF map, inverted): the beacon's carrier must fall
//   at 100–3000 Hz of their audio. Anywhere else the tracker says where to tune and judges nothing.
#pragma once
#include "decoders/ncdxf_tracker.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <string>

namespace vibe {

class NcdxfRun {
public:
    struct Out {
        std::function<void(const std::string&)> text;   ///< 0x01: one line per beacon heard
        std::function<void(uint8_t)> state;             ///< 0x03
        std::function<void(const std::string&)> json;   ///< a JSON text frame
    };
    NcdxfRun(std::function<double(double)> rfOfAudio, Out out) : rfOf_(std::move(rfOfAudio)), out_(std::move(out)) {
        tr_.reset(new ncdxf::Tracker(48000));
        tr_->onSlot = [this](const ncdxf::SlotResult& r) { onSlot(r); };
    }
    /** 48 kHz mono float; `utc` = the capture UTC of the block's last sample (NaN = now). */
    void feedAudio(const float* x, int n, double utc) {
        if (n <= 0) return;
        if (!std::isfinite(utc)) utc = nowS();
        locate();
        tr_->feed(x, n, utc);
        // ★ off a beacon frequency: say so every 10 s (the client's hint), and nothing is judged
        if (tr_->band() < 0 && utc - lastIdleAt_ >= 10) { lastIdleAt_ = utc; sendJson(utc, nullptr); }
    }
    static double nowS() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

private:
    /** Which beacon frequency the listener's audio holds, and at what pitch. */
    void locate() {
        if (!rfOf_) return;
        const double r0 = rfOf_(0), r1 = rfOf_(1000);
        const double slope = (r1 - r0) / 1000.0;   // +1 USB / CW-U, −1 LSB
        int band = -1; double tone = 0;
        if (std::fabs(std::fabs(slope) - 1) < 0.01) for (int b = 0; b < 5; ++b) {
            const double a = (ncdxf::kBandHz[b] - r0) / slope;
            if (a >= 100 && a <= 3000) { band = b; tone = a; break; }
        }
        if (band != tr_->band() || std::fabs(tone - tone_) > 2) {
            tone_ = tone;
            tr_->setTone(tone, band);
            const uint8_t st = band >= 0 ? 3 : 0;
            if (st != state_) { state_ = st; if (out_.state) out_.state(st); }
            lastIdleAt_ = 0;
            sendJson(nowS(), nullptr);
        }
    }
    void onSlot(const ncdxf::SlotResult& r) {
        if (r.steps > 0 && r.id >= 0 && out_.text) {
            const time_t tt = (time_t)r.utc;
            struct tm u{}; gmtime_r(&tt, &u);
            char b[256];
            std::snprintf(b, sizeof b, "%02d:%02d:%02dz  %.3f  %-6s  %s \xc2\xb7 %d step%s (down to %s) \xc2\xb7 %.0f dB%s\n",
                          u.tm_hour, u.tm_min, u.tm_sec, ncdxf::kBandHz[r.band] / 1e6, ncdxf::kBeacons[r.beacon].call,
                          ncdxf::kBeacons[r.beacon].where, r.steps, r.steps == 1 ? "" : "s", ncdxf::kStepName[r.steps - 1],
                          r.snr[0], r.id == 1 ? " \xc2\xb7 ID \xe2\x9c\x93" : "");
            out_.text(b);
        }
        sendJson(nowS(), &r);
    }
    void sendJson(double now, const ncdxf::SlotResult* last) {
        if (!out_.json) return;
        const int band = tr_->band();
        const long long slot = (long long)std::floor(now / 10.0) * 10;
        char b[400];
        std::snprintf(b, sizeof b, "{\"type\":\"ncdxf\",\"data\":{\"now\":%.0f,\"band\":%d,\"tone\":%.0f,\"slot\":{\"beacon\":%d,\"at\":%lld},\"last\":",
                      now, band, tone_, band >= 0 ? ncdxf::beaconAt(band, slot) : -1, slot);
        std::string j = b;
        if (last) {
            std::snprintf(b, sizeof b, "{\"beacon\":%d,\"band\":%d,\"at\":%lld,\"steps\":%d,\"snr\":[%.1f,%.1f,%.1f,%.1f],\"id\":%d,\"other\":%d,\"lag\":%.2f,\"off\":%.0f}",
                          last->beacon, last->band, last->utc, last->steps, last->snr[0], last->snr[1], last->snr[2], last->snr[3],
                          last->id, last->id == -1 ? last->other : -1, last->lagS, last->offHz);
            j += b;
        } else j += "null";
        j += "," + tr_->gridJson(now) + "}}";
        out_.json(j);
    }

    std::function<double(double)> rfOf_;
    Out out_;
    std::unique_ptr<ncdxf::Tracker> tr_;
    double tone_ = 0, lastIdleAt_ = 0;
    uint8_t state_ = 255;
};

} // namespace vibe
