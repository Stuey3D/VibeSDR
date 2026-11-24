// vibe_skyplot.h — THE SKY PLOT (V12, 2026-11-24): where in the sky this station's aerial actually hears satellites.
//
// ★★★ WHAT IT IS (docs/v12/SKY-PLOT.md): a polar dome — outer ring the horizon, centre the zenith, north up, east right
//     (a map's view), rings at 30° and 60° — with one dot per decode at the satellite's azimuth / elevation at that moment,
//     coloured by SNR. It ACCUMULATES across passes, so after a few days it shows the aerial's real coverage: a YouLoop's
//     nulls, a vertical's overhead hole, a QFH's even dome, the house in the way to the north-east. A pass where the bird
//     was tracked but NOTHING decoded leaves grey dots — the blocked directions are the point.
//
// ★★ ONE FILE PER SERVER, FILTERED BY RADIO (the logs' rule, docs/v12/LOG-RADIO-FILTER.md): every radio process appends
//    to <gallery>/skyplot.dat under flock; each dot carries its radio's id (vibe_log_radio.h — never a serial). A sky plot
//    is about an AERIAL, so the views filter by radio (?radio=<id>); "all radios" is offered but is only meaningful
//    when they share an aerial.
//
// ★★ ANY SATELLITE DECODER CAN FEED IT (kind): ORBCOMM first (it is up almost all the time — a day fills the dome);
//    METEOR frames, APRS digipeats and FUNcube telemetry are kinds already, for when their decoders call add().
//
// ★ BOUNDED (256 MB boards): a dot is 24 bytes; at most kMaxDots (1.4 MB) — past that the oldest are dropped (a compaction
//   to kKeepDots under the lock). Callers aggregate: ORBCOMM writes one dot per satellite per kBucketSec, never a dot
//   per packet. The JSON answer is capped (kMaxOut, newest first) and the PNG is drawn from the same view.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vibe_canvas.h"
#include "vibe_log_radio.h"
#include "vibe_png.h"

namespace vibe {
namespace skyplot {

enum Kind : uint8_t { kOrbcomm = 1, kMeteor = 2, kAprs = 3, kFuncube = 4 };
inline const char* kindName(int k) {
    switch (k) { case kOrbcomm: return "orbcomm"; case kMeteor: return "meteor"; case kAprs: return "aprs"; case kFuncube: return "funcube"; default: return "other"; }
}
inline int kindOf(const std::string& s) {
    if (s == "orbcomm") return kOrbcomm; if (s == "meteor") return kMeteor; if (s == "aprs") return kAprs; if (s == "funcube") return kFuncube;
    return 0;
}
constexpr int8_t kNothing = -128;          ///< "tracked, nothing decoded" (a grey dot)
constexpr size_t kMaxDots = 60000, kKeepDots = 50000, kMaxOut = 20000;
constexpr double kBucketSec = 5;           ///< the aggregation a decoder should use (one dot per bird per 5 s)

struct Dot {
    uint32_t t = 0;          ///< UTC seconds
    float azDeg = 0, elDeg = 0;
    int8_t snr = kNothing;   ///< dB (−127 … 127), kNothing = none decoded
    uint8_t kind = 0;
    uint16_t count = 0;      ///< decodes in the bucket
    uint32_t norad = 0;
    std::string radio;       ///< vibe_log_radio id ("" = unknown)
};

constexpr size_t kRec = 24;
inline void pack(const Dot& d, uint8_t* o) {
    std::memset(o, 0, kRec);
    auto p32 = [&](size_t at, uint32_t v) { o[at] = uint8_t(v); o[at + 1] = uint8_t(v >> 8); o[at + 2] = uint8_t(v >> 16); o[at + 3] = uint8_t(v >> 24); };
    auto p16 = [&](size_t at, uint16_t v) { o[at] = uint8_t(v); o[at + 1] = uint8_t(v >> 8); };
    p32(0, d.t);
    p16(4, uint16_t(std::lround(std::fmod(std::fmod(d.azDeg, 360.0) + 360.0, 360.0) * 10) % 3600));
    p16(6, uint16_t(int16_t(std::lround(std::max(-90.f, std::min(90.f, d.elDeg)) * 10))));
    o[8] = uint8_t(d.snr); o[9] = d.kind; p16(10, d.count); p32(12, d.norad);
    for (size_t i = 0; i < 8 && i < d.radio.size(); i++) o[16 + i] = uint8_t(d.radio[i]);
}
inline Dot unpack(const uint8_t* o) {
    auto g32 = [&](size_t at) { return uint32_t(o[at]) | uint32_t(o[at + 1]) << 8 | uint32_t(o[at + 2]) << 16 | uint32_t(o[at + 3]) << 24; };
    auto g16 = [&](size_t at) { return uint16_t(o[at] | o[at + 1] << 8); };
    Dot d; d.t = g32(0); d.azDeg = g16(4) / 10.f; d.elDeg = int16_t(g16(6)) / 10.f; d.snr = int8_t(o[8]); d.kind = o[9]; d.count = g16(10); d.norad = g32(12);
    for (size_t i = 0; i < 8 && o[16 + i]; i++) d.radio += char(o[16 + i]);
    if (!vibe::logradio::validId(d.radio)) d.radio.clear();
    return d;
}

/** The machine's one file. All calls take the flock (several radio processes) and a process mutex. */
class Store {
public:
    static constexpr const char* kMagic = "VSKY0001";
    void setDir(const std::string& dir) { std::lock_guard<std::mutex> lk(m_); dir_ = dir; }
    std::string dir() const { std::lock_guard<std::mutex> lk(m_); return dir_; }
    /** Append dots (this process's radio id is filled in when a dot has none). False = no directory / a write failed. */
    bool add(std::vector<Dot> dots) {
        if (dots.empty()) return true;
        std::lock_guard<std::mutex> lk(m_);
        if (dir_.empty()) return false;
        const std::string me = vibe::logradio::mine();
        for (auto& d : dots) if (d.radio.empty()) d.radio = me;
        FLock fl(dir_);
        const std::string path = dir_ + "/skyplot.dat";
        struct stat st{};
        const bool exists = ::stat(path.c_str(), &st) == 0;
        std::FILE* f = std::fopen(path.c_str(), "ab");
        if (!f) return false;
        if (!exists || st.st_size < 8) std::fwrite(kMagic, 1, 8, f);
        uint8_t rec[kRec];
        for (auto& d : dots) { pack(d, rec); std::fwrite(rec, 1, kRec, f); }
        const bool ok = std::fflush(f) == 0;
        std::fclose(f);
        const size_t n = exists ? size_t(st.st_size > 8 ? (st.st_size - 8) / kRec : 0) + dots.size() : dots.size();
        if (n > kMaxDots) compact_(path);
        return ok;
    }
    struct Filter { int kind = 0; std::string radio; uint32_t since = 0; long long norad = 0; };
    /** Newest first, at most `max`. `radios` (optional): how many dots each radio holds under the other filters. */
    std::vector<Dot> read(const Filter& fl, size_t max = kMaxOut, std::map<std::string, uint64_t>* radios = nullptr) const {
        std::vector<Dot> all;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (dir_.empty()) return all;
            FLock lock(dir_);
            all = load_(dir_ + "/skyplot.dat");
        }
        std::vector<Dot> out;
        for (size_t i = all.size(); i-- > 0;) {
            const Dot& d = all[i];
            if (fl.kind && d.kind != fl.kind) continue;
            if (fl.since && d.t < fl.since) continue;
            if (fl.norad && d.norad != uint32_t(fl.norad)) continue;
            if (radios) (*radios)[d.radio]++;
            if (!fl.radio.empty() && !d.radio.empty() && d.radio != fl.radio) continue;   // ★ untagged dots pass (the logs' rule)
            if (out.size() < max) out.push_back(d);
        }
        return out;
    }
private:
    struct FLock {
        int fd = -1;
        explicit FLock(const std::string& dir) { fd = ::open((dir + "/.skyplot.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644); if (fd >= 0) ::flock(fd, LOCK_EX); }
        ~FLock() { if (fd >= 0) { ::flock(fd, LOCK_UN); ::close(fd); } }
    };
    static std::vector<Dot> load_(const std::string& path) {
        std::vector<Dot> v;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return v;
        char mg[8];
        if (std::fread(mg, 1, 8, f) == 8 && std::memcmp(mg, kMagic, 8) == 0) {
            uint8_t rec[kRec];
            while (v.size() < kMaxDots * 2 && std::fread(rec, 1, kRec, f) == kRec) v.push_back(unpack(rec));
        }
        std::fclose(f);
        return v;
    }
    static void compact_(const std::string& path) {
        std::vector<Dot> v = load_(path);
        if (v.size() <= kKeepDots) return;
        v.erase(v.begin(), v.end() - long(kKeepDots));
        const std::string tmp = path + ".tmp";
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return;
        std::fwrite(kMagic, 1, 8, f);
        uint8_t rec[kRec];
        for (auto& d : v) { pack(d, rec); std::fwrite(rec, 1, kRec, f); }
        const bool ok = std::fflush(f) == 0;
        std::fclose(f);
        if (ok) std::rename(tmp.c_str(), path.c_str()); else std::remove(tmp.c_str());
    }
    mutable std::mutex m_;
    std::string dir_;
};
inline Store& shared() { static Store s; return s; }

/** The view's JSON: {"v":1,"kind":…,"dots":[[az10,el10,snr|null,t,kind,norad,count,radioIndex]…],"radioIds":[…],
 *  "radios":[{"id","name","n"}…],"count":N,"truncated":bool}. snr null = tracked, nothing decoded. */
inline std::string json(const std::vector<Dot>& dots, const std::map<std::string, uint64_t>& radios, const std::string& kind, bool truncated) {
    std::vector<std::string> ids;
    std::map<std::string, int> idx;
    std::string o = "{\"v\":1,\"kind\":\"" + vibe::logradio::jsonEsc(kind) + "\",\"dots\":[";
    for (size_t i = 0; i < dots.size(); i++) {
        const Dot& d = dots[i];
        int ri = -1;
        if (!d.radio.empty()) {
            auto it = idx.find(d.radio);
            if (it == idx.end()) { ri = int(ids.size()); idx[d.radio] = ri; ids.push_back(d.radio); } else ri = it->second;
        }
        char b[128];
        if (d.snr == kNothing) std::snprintf(b, sizeof b, "%s[%d,%d,null,%u,%d,%u,%u,%d]", i ? "," : "", int(std::lround(d.azDeg * 10)), int(std::lround(d.elDeg * 10)), d.t, d.kind, d.norad, d.count, ri);
        else std::snprintf(b, sizeof b, "%s[%d,%d,%d,%u,%d,%u,%u,%d]", i ? "," : "", int(std::lround(d.azDeg * 10)), int(std::lround(d.elDeg * 10)), int(d.snr), d.t, d.kind, d.norad, d.count, ri);
        o += b;
    }
    o += "],\"radioIds\":[";
    for (size_t i = 0; i < ids.size(); i++) o += (i ? ",\"" : "\"") + ids[i] + "\"";
    std::vector<std::pair<std::string, uint64_t>> counts; uint64_t unknown = 0;
    for (auto& r : radios) { if (r.first.empty()) unknown += r.second; else counts.push_back(r); }
    o += "],\"radios\":" + vibe::logradio::radiosJson(counts, unknown);
    o += ",\"count\":" + std::to_string(dots.size()) + ",\"truncated\":" + (truncated ? "true" : "false") + "}";
    return o;
}

/** The SNR colour ramp (dB): 0 → deep blue, 6 → teal, 12 → green, 18+ → yellow. Grey = nothing decoded. The clients
 *  use the same four stops (src/utils/skyPlot.ts snrColour). */
inline void snrColour(int snr, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (snr == kNothing) { r = g = b = 110; return; }
    static const float stops[4][4] = { { 0, 50, 60, 200 }, { 6, 30, 170, 190 }, { 12, 60, 200, 80 }, { 18, 250, 225, 40 } };
    const float s = std::max(0.f, std::min(18.f, float(snr)));
    int k = 0; while (k < 2 && s > stops[k + 1][0]) k++;
    const float f = (s - stops[k][0]) / (stops[k + 1][0] - stops[k][0]);
    r = uint8_t(stops[k][1] + f * (stops[k + 1][1] - stops[k][1]));
    g = uint8_t(stops[k][2] + f * (stops[k + 1][2] - stops[k][2]));
    b = uint8_t(stops[k][3] + f * (stops[k + 1][3] - stops[k][3]));
}

/** The picture (the gallery's copy): a W×W dome + a legend strip, PNG. Oldest drawn first so the newest sit on top;
 *  grey (nothing decoded) under everything. `title` in the 5×7 font's characters (A–Z 0–9 . - space). */
inline std::string png(const std::vector<Dot>& dotsNewestFirst, const std::string& title, int W = 520) {
    const int H = W + 64;
    vibe::canvas::Canvas c(W, H, 14, 18, 26);
    const int cx = W / 2, cy = 36 + (W - 72) / 2, R = (W - 72) / 2;
    auto ring = [&](double elDeg, uint8_t v) {
        const double rr = R * (90 - elDeg) / 90.0;
        const int n = int(2 * M_PI * rr);
        for (int i = 0; i < n; i++) { const double a = 2 * M_PI * i / n; c.dot(int(cx + rr * std::cos(a)), int(cy + rr * std::sin(a)), v, v, uint8_t(v + 12)); }
    };
    c.disc(cx, cy, R, 22, 28, 40);
    ring(0, 150); ring(30, 80); ring(60, 80);
    c.line(cx - R, cy, cx + R, cy, 50, 56, 70); c.line(cx, cy - R, cx, cy + R, 50, 56, 70);
    c.text(cx - 2, cy - R - 12, "N", 220, 220, 230); c.text(cx - 2, cy + R + 5, "S", 220, 220, 230);
    c.text(cx + R + 6, cy - 3, "E", 220, 220, 230); c.text(cx - R - 12, cy - 3, "W", 220, 220, 230);
    c.text(cx + 3, cy - int(R * 60 / 90.0) - 9, "30", 120, 128, 150); c.text(cx + 3, cy - int(R * 30 / 90.0) - 9, "60", 120, 128, 150);
    auto xy = [&](const Dot& d, int& x, int& y) {
        const double rr = R * (90 - std::max(0.f, d.elDeg)) / 90.0, a = d.azDeg * M_PI / 180.0;
        x = int(std::lround(cx + rr * std::sin(a))); y = int(std::lround(cy - rr * std::cos(a)));
    };
    for (int pass = 0; pass < 2; pass++)   // grey first, then the decodes, oldest first
        for (size_t i = dotsNewestFirst.size(); i-- > 0;) {
            const Dot& d = dotsNewestFirst[i];
            if ((d.snr == kNothing) != (pass == 0) || d.elDeg < -1) continue;
            int x, y; xy(d, x, y);
            uint8_t r, g, b; snrColour(d.snr, r, g, b);
            c.disc(x, y, pass == 0 ? 1 : 2, r, g, b);
        }
    std::string t = title.substr(0, size_t((W - 16) / 6));
    c.text(8, 8, t, 230, 230, 240);
    // the legend: the SNR ramp 0–18 dB and the grey "nothing decoded"
    const int ly = H - 24, lx = 40, lw = W - 220;
    for (int i = 0; i < lw; i++) { uint8_t r, g, b; snrColour(int(std::lround(18.0 * i / lw)), r, g, b); for (int k = 0; k < 8; k++) c.dot(lx + i, ly + k, r, g, b); }
    c.text(lx - 8 - 6, ly, "0", 200, 200, 210); c.text(lx + lw + 4, ly, "18 DB SNR", 200, 200, 210);
    c.disc(W - 92, ly + 4, 3, 110, 110, 110); c.text(W - 84, ly, "NONE", 200, 200, 210);
    return vibe::png::encode(W, H, 3, c.px.data());
}

}  // namespace skyplot
}  // namespace vibe
