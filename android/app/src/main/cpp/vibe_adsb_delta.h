// vibe_adsb_delta.h — ★★ V12 beta96 THE ADS-B DELTA PROTOCOL: a listener's aircraft as changes, not the whole list.
//
// Stuart approved it on 2026-11-26. The `adsb` report re-sends every aircraft every second: measured on the Sony
// (beta95, 45–66 aircraft) at 11.75 KB/s for one listener, about 0.2 KB/s per aircraft per listener. A Beast-fed
// Lenovo with 150 aircraft and 100 listeners would need about 25 Mbit/s of upload. A client that says it understands
// deltas (the spectrum socket's `adsbd=1`) gets these two messages instead of `adsb`; every other client gets `adsb`
// exactly as before (docs/v12/ADSB-DELTA.md).
//
//   KEYFRAME  {"type":"adsb_k","s":S,"t":T,"h":{…the report's header…},"a":[ROW,…]}
//             on joining, on {"type":"adsb_resync"}, after ADS-B (re)starts, and every 60 s.
//   DELTA     {"type":"adsb_d","s":S,"t":T,"h":{changed header keys},"hx":[removed header keys],
//              "g":[ids gone],"n":[ROW,…],"u":[UPD,…]}  — each part only when it has something in it.
//   ROW       id,"hex",mask,v…     (a new aircraft, or every aircraft in a keyframe)
//   UPD       id,mask,v…           (an aircraft that changed)
//   ★ FLAT: "a", "n" and "u" are one array each, the rows one after another with no brackets of their own (2 bytes a
//     row — 10 % of a tick). The mask says how many values follow, so the client knows where the next row starts.
//
// S counts ticks (one a second). A client applies a delta only when S is one more than the state it holds; a keyframe
// always replaces the table. A gap means it missed something: it asks for {"type":"adsb_resync"} and gets a keyframe.
// The client applies g, then n, then u (an id gone this tick may be given to a new aircraft in the same tick).
//
// `mask` says which columns follow, in column order (Col below). A column's value is `null` when the field has gone.
// The first five columns are DIFFERENCES from the value the client holds (absolute when it holds none): positions in
// 1e-5°, the two "heard at" times in whole seconds of the server's ADS-B clock, and the message count. The rest are
// absolute. Quantised: lat/lon 1e-5° (the report's own 5 decimals), altitudes 25 ft, speeds 1 kt, track and heading 1°,
// signal 1 dB (rounded down), SNR in 3 dB bands on its colour edges (snrBand), tracked distance 1 NM (as the report).
//
// TIMES ON THE TICK GRID. The two "heard at" times are whole ticks: stamp(x) = floor(x − φ), where φ is fixed when
// ADS-B starts so that each tick's own T = floor(now − φ) sits 0.85 s into its second. So everything heard since the last
// tick (except in its first 0.15 s) is stamped T; a tick may run up to 0.15 s late (the Beast radio polls every 100 ms). The ticks are kept on a 1 s grid (the shim adds 1.0 s, it never
// re-bases on a late tick), so a stamp, once given, never changes until the aircraft is heard again.
//
// TWO TIMES LEFT OUT WHEN THEY ARE WHAT THE CLIENT WOULD GUESS (most of the saving): when a position moved and its time
// is this tick's T, posAt is not sent; when the message count moved and the last-heard time is T, seenAt is not sent.
// The client sets each to T in that case. The rule is checked on both ends from the same values, so it is exact.
//
// Ages: the client works out seen = (T − seenAt) + 0.35 and seen_pos = (T − posAt) + 0.35 (the middle of the
// stamp's second), to 0.1 s — within half a second of the full report's own figure. Every client threshold on them (just now
// < 2 s, fresh < 10 s, aging < 30 s) is whole seconds, and the 60 s position expiry is the server's (the field goes).
//
// What each aircraft IS (registration, type, operator, route, country) is not here: `adsb_info` already sends it once.
//
// Bounded: one encoder per radio, its table no bigger than the tracker's (capped at kMaxRows). Built ONCE per tick and
// the same bytes go to every delta listener.
#pragma once
#include "vibe_adsb.h"
#include "vibe_adsb_commb.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace vibeadsb {
namespace delta {

enum Col {
    cLat = 0, cLon, cPosAt, cSeenAt, cMsgs,          // ★ differences (kDiffCols)
    cAlt, cRssi, cGs, cTrack, cVr, cGeom, cHdg, cTas, cIas, cSquawk, cEmerg, cTracked,   // absolute numbers
    cSnr,                                            // ★ beta96 SNR over the floor, in 3 dB BANDS (snrBand below)
    cFlight, cCat, cCommb,                           // text / an object
    kCols
};
constexpr int kNumCols = cFlight;                    // columns 0 … kNumCols-1 are numbers
constexpr uint32_t kDiffCols = (1u << cLat) | (1u << cLon) | (1u << cPosAt) | (1u << cSeenAt) | (1u << cMsgs);
constexpr int64_t kGround = INT64_MIN / 2;           // cAlt's value for "ground"
constexpr size_t kMaxRows = 2000;                    // the web client's cap (spectrum.ts slice(0, 2000))
constexpr int kKeyframeTicks = 60;                   // a keyframe for everyone every 60 s

/** A value printed as the report prints it ("%.*f"), read back as an integer in units of 10^-dp — so a quantity is
 *  quantised from exactly the text an older client reads, never from the double behind it (no tie can differ). */
inline int64_t fixedOf(double v, int dp) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", dp, v);
    int64_t n = 0; bool neg = false;
    for (const char* p = b; *p; ++p) {
        if (*p == '-') neg = true;
        else if (*p >= '0' && *p <= '9') n = n * 10 + (*p - '0');
    }
    return neg ? -n : n;
}
inline int64_t floorDiv(int64_t a, int64_t b) { int64_t q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }
/** ★★ beta96 (Stuart 11-26: "use coarser to save resources") THE SNR COLUMN IN 3 dB BANDS: k = floor((snr − 10) / 3), the
 *  client shows 11.5 + 3k. The steps sit on ADS-B's colour edges (src/utils/snrColour.ts 'adsb': threshold 10, red under
 *  13, green from 16 — 10 + 3 and 10 + 6), so rounding never changes a colour; and the column goes out only when an
 *  aircraft's band changes. VIBE_ADSB_SNR_STEP (tenths of a dB, a measurement seam) = 10 → whole dB, 0 → no column. */
#ifndef VIBE_ADSB_SNR_STEP
#define VIBE_ADSB_SNR_STEP 30
#endif
inline int64_t snrBand(float snr) { return floorDiv(fixedOf(snr, 1) - (VIBE_ADSB_SNR_STEP == 30 ? 100 : 0), VIBE_ADSB_SNR_STEP > 0 ? VIBE_ADSB_SNR_STEP : 1); }
/** JavaScript's Math.round(t / d) for an integer t — half rounds up. */
inline int64_t jsRound(int64_t t, int64_t d) { return floorDiv(2 * t + d, 2 * d); }

struct Row {
    uint32_t key = 0;                 // (nonIcao << 24) | icao
    std::string hex;
    uint32_t has = 0;                 // bit per Col present
    int64_t n[kNumCols] = {};
    std::string flight, cat, commb;   // commb: the object's JSON text
    bool operator==(const Row& o) const {
        if (has != o.has) return false;
        for (int c = 0; c < kNumCols; ++c) if ((has >> c & 1) && n[c] != o.n[c]) return false;
        return flight == o.flight && cat == o.cat && commb == o.commb;
    }
};

/** One aircraft as the report would show it at `now` — Tracker::json's rules, field for field (the same presence
 *  tests), quantised as the header says. */
inline int64_t stampOf(double x, double phi) { return (int64_t)std::floor(x - phi); }
inline Row rowOf(const Aircraft& a, double now, double phi) {
    Row r;
    r.key = (a.nonIcao ? 1u << 24 : 0u) | (a.icao & 0xFFFFFFu);
    char h[12];
    std::snprintf(h, sizeof h, "%s%06x", a.nonIcao ? "~" : "", a.icao);
    r.hex = h;
    auto put = [&](Col c, int64_t v) { r.has |= 1u << c; r.n[c] = v; };
    if (!a.callsign.empty()) { r.has |= 1u << cFlight; r.flight = a.callsign; }
    if (a.category) {
        char c[8]; std::snprintf(c, sizeof c, "%c%d", "?DCBA"[std::min(a.category >> 4, 4)], a.category & 7);
        r.has |= 1u << cCat; r.cat = c;
    }
    if (a.hasPos && now - a.seenPos <= 60) {
        put(cLat, fixedOf(a.lat, 5)); put(cLon, fixedOf(a.lon, 5));
        put(cPosAt, stampOf(a.seenPos, phi));
    }
    if (a.onGround) put(cAlt, kGround);
    else if (a.hasAlt) put(cAlt, jsRound(a.altBaro, 25));
    if (a.hasGeo) put(cGeom, jsRound(a.altGeom, 25));
    if (a.hasGs) put(cGs, jsRound(fixedOf(a.gs, 1), 10));
    if (a.hasTrack) put(cTrack, jsRound(fixedOf(a.track, 1), 10));
    if (a.hasHdg) put(cHdg, jsRound(fixedOf(a.heading, 1), 10));
    if (a.hasAs) put(a.tas ? cTas : cIas, a.airspeed);
    if (a.hasVr) put(cVr, a.vr);
    if (a.squawk >= 0) put(cSquawk, a.squawk);
    if (a.emergency) put(cEmerg, a.emergency);
    if (a.trackedNm >= 0.5) put(cTracked, fixedOf(a.trackedNm, 0));
    if (a.cb) {
        const std::string cb = commb::json(*a.cb, now);          // ",\"commb\":{…}" or ""
        const size_t k = cb.find('{');
        if (k != std::string::npos) { r.has |= 1u << cCommb; r.commb = cb.substr(k); }
    }
    put(cSeenAt, stampOf(a.seen, phi));
    put(cMsgs, a.messages);
    put(cRssi, floorDiv(fixedOf(a.level, 1), 10));
    if (VIBE_ADSB_SNR_STEP > 0) put(cSnr, snrBand(a.snr));
    return r;
}

/** Every aircraft, sorted by key (a stable order for the client's table). Call under the tracker's lock. */
inline void rowsOf(const Tracker& t, double now, double phi, std::vector<Row>& out) {
    out.clear();
    out.reserve(t.aircraft().size());
    for (const auto& kv : t.aircraft()) { out.push_back(rowOf(kv.second, now, phi)); if (out.size() >= kMaxRows) break; }
    std::sort(out.begin(), out.end(), [](const Row& x, const Row& y) { return x.key < y.key; });
}

/** The top-level keys of a JSON object and each value's raw text ("type" and "aircraft" left out). Small and strict
 *  enough for the report's own header; anything it cannot read ends the walk. */
inline std::vector<std::pair<std::string, std::string>> topPairs(const std::string& s) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = s.find('{');
    if (i == std::string::npos) return out;
    ++i;
    auto skipWs = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) ++i; };
    auto strEnd = [&](size_t p) {   // p at the opening quote → index after the closing quote
        for (++p; p < s.size(); ++p) { if (s[p] == '\\') ++p; else if (s[p] == '"') return p + 1; }
        return std::string::npos;
    };
    for (;;) {
        skipWs();
        if (i >= s.size() || s[i] != '"') break;
        const size_t ke = strEnd(i);
        if (ke == std::string::npos) break;
        std::string key = s.substr(i + 1, ke - i - 2);
        i = ke; skipWs();
        if (i >= s.size() || s[i] != ':') break;
        ++i; skipWs();
        const size_t vs = i;
        int depth = 0;
        for (; i < s.size(); ++i) {
            const char c = s[i];
            if (c == '"') { const size_t e = strEnd(i); if (e == std::string::npos) return out; i = e - 1; continue; }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (depth == 0) break; --depth; }
            else if (c == ',' && depth == 0) break;
        }
        if (i >= s.size()) break;
        if (key != "type" && key != "aircraft") out.emplace_back(std::move(key), s.substr(vs, i - vs));
        if (s[i] == '}') break;
        ++i;
    }
    return out;
}

inline void jsonStrTo(std::string& o, const std::string& v) {
    o += '"';
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    o += '"';
}

/** The columns that changed from `old` (nullptr = an aircraft the client does not hold) to `cur`, written as
 *  `mask,v…` — with the two implied times left out when they are T (see the header). */
inline void encodeChange(std::string& o, const Row* old, const Row& cur, int64_t T) {
    uint32_t mask = 0;
    const uint32_t oh = old ? old->has : 0;
    for (int c = 0; c < kCols; ++c) {
        const bool a = oh >> c & 1, b = cur.has >> c & 1;
        if (!a && !b) continue;
        if (a != b) { mask |= 1u << c; continue; }
        bool same;
        if (c < kNumCols) same = old->n[c] == cur.n[c];
        else if (c == cFlight) same = old->flight == cur.flight;
        else if (c == cCat) same = old->cat == cur.cat;
        else same = old->commb == cur.commb;
        if (!same) mask |= 1u << c;
    }
    // ★ the implied times: what the client will assume when the column is not sent
    auto implied = [&](Col timeCol, uint32_t movedBits, bool& has, int64_t& v) {
        if ((mask & movedBits) && (cur.has & movedBits) == movedBits) { has = true; v = T; }
        else { has = oh >> timeCol & 1; v = has ? old->n[timeCol] : 0; }
    };
    for (Col tc : {cPosAt, cSeenAt}) {
        const uint32_t moved = tc == cPosAt ? ((1u << cLat) | (1u << cLon)) : (1u << cMsgs);
        // the movement bits must be settled before the time's (lat/lon/msgs come first in Col order)
        bool ih; int64_t iv;
        implied(tc, moved, ih, iv);
        const bool ch = cur.has >> tc & 1;
        const bool differs = ih != ch || (ch && iv != cur.n[tc]);
        if (differs) mask |= 1u << tc; else mask &= ~(1u << tc);
    }
    o += std::to_string(mask);
    for (int c = 0; c < kCols; ++c) {
        if (!(mask >> c & 1)) continue;
        o += ',';
        if (!(cur.has >> c & 1)) { o += "null"; continue; }
        if (c < kNumCols) {
            int64_t v = cur.n[c];
            if ((kDiffCols >> c & 1) && (oh >> c & 1)) v -= old->n[c];
            if (c == cAlt && v == kGround) o += "\"g\"";
            else o += std::to_string(v);
        } else if (c == cFlight) jsonStrTo(o, cur.flight);
        else if (c == cCat) jsonStrTo(o, cur.cat);
        else o += cur.commb;
    }
}

/** ★ One per radio: the table every delta listener holds, and the next message that brings them up to date. Not
 *  thread-safe on its own — the Service calls it under its encoder lock. */
class Encoder {
public:
    /** Forget everything (ADS-B started again): the next tick is a keyframe for everyone. `s` keeps counting. */
    void reset() {
        prev_.clear(); ids_.clear(); free_.clear(); nextId_ = 0; header_.clear(); keyCache_.clear(); keyCacheSeq_ = ~0ull;
        forceKey_ = true;
    }
    uint64_t seq() const { return seq_; }

    /** One tick: `rows` (rowsOf), the report's header object text, the tick's T. Returns the delta message, and
     *  sets `keyAll` when this tick should go to every delta listener as a keyframe instead (the 60 s keyframe, or
     *  the first tick after a reset) — the delta then still moved the state; keyframe() is that state. */
    std::string tick(std::vector<Row>&& rows, const std::string& headerObj, int64_t T, bool& keyAll) {
        ++seq_;
        t_ = T;
        std::string o = "{\"type\":\"adsb_d\",\"s\":" + std::to_string(seq_) + ",\"t\":" + std::to_string(T);
        // ── the header: changed keys, removed keys ──
        {
            std::vector<std::pair<std::string, std::string>> h = topPairs(headerObj);
            std::map<std::string, std::string> now(h.begin(), h.end());
            std::string ch, rm;
            for (const auto& kv : now) {
                auto it = header_.find(kv.first);
                if (it != header_.end() && it->second == kv.second) continue;
                ch += ch.empty() ? "" : ","; jsonStrTo(ch, kv.first); ch += ':'; ch += kv.second;
            }
            for (const auto& kv : header_) if (!now.count(kv.first)) { rm += rm.empty() ? "" : ","; jsonStrTo(rm, kv.first); }
            if (!ch.empty()) o += ",\"h\":{" + ch + "}";
            if (!rm.empty()) o += ",\"hx\":[" + rm + "]";
            header_.swap(now);
        }
        // ── gone first (their ids are free for this tick's new aircraft) ──
        std::set<uint32_t> keys;
        for (const Row& r : rows) keys.insert(r.key);
        std::string g;
        for (auto it = prev_.begin(); it != prev_.end();) {
            if (keys.count(it->first)) { ++it; continue; }
            const int id = ids_[it->first];
            g += g.empty() ? "" : ","; g += std::to_string(id);
            free_.insert(id); ids_.erase(it->first); it = prev_.erase(it);
        }
        std::string n, u;
        for (Row& r : rows) {
            auto it = prev_.find(r.key);
            if (it == prev_.end()) {
                int id;
                if (!free_.empty()) { id = *free_.begin(); free_.erase(free_.begin()); } else id = nextId_++;
                ids_[r.key] = id;
                if (!n.empty()) n += ',';
                n += std::to_string(id); n += ','; jsonStrTo(n, r.hex); n += ',';
                encodeChange(n, nullptr, r, T);
                prev_.emplace(r.key, std::move(r));
            } else {
                if (it->second == r) continue;
                std::string one = std::to_string(ids_[r.key]) + ",";
                encodeChange(one, &it->second, r, T);
                u += u.empty() ? "" : ","; u += one;
                it->second = std::move(r);
            }
        }
        if (!g.empty()) o += ",\"g\":[" + g + "]";
        if (!n.empty()) o += ",\"n\":[" + n + "]";
        if (!u.empty()) o += ",\"u\":[" + u + "]";
        o += '}';
        keyAll = forceKey_ || ++sinceKey_ >= kKeyframeTicks;
        if (keyAll) { forceKey_ = false; sinceKey_ = 0; }
        return o;
    }

    /** The whole table as it stands after the last tick — for a listener joining, asking to resync, or everyone. */
    const std::string& keyframe() {
        if (keyCacheSeq_ == seq_ && !keyCache_.empty()) return keyCache_;
        std::string o = "{\"type\":\"adsb_k\",\"s\":" + std::to_string(seq_) + ",\"t\":" + std::to_string(t_) + ",\"h\":{";
        bool first = true;
        for (const auto& kv : header_) { if (!first) o += ','; first = false; jsonStrTo(o, kv.first); o += ':'; o += kv.second; }
        o += "},\"a\":[";
        first = true;
        for (const auto& kv : prev_) {
            if (!first) o += ',';
            first = false;
            o += std::to_string(ids_[kv.first]); o += ','; jsonStrTo(o, kv.second.hex); o += ',';
            encodeChange(o, nullptr, kv.second, t_);
        }
        o += "]}";
        keyCache_.swap(o); keyCacheSeq_ = seq_;
        return keyCache_;
    }
    size_t size() const { return prev_.size(); }

private:
    std::map<uint32_t, Row> prev_;                  // key → what the clients hold
    std::map<uint32_t, int> ids_;                   // key → its small id on the wire
    std::set<int> free_;                            // ids given back, lowest reused first
    int nextId_ = 0;
    std::map<std::string, std::string> header_;
    uint64_t seq_ = 0;
    int64_t t_ = 0;
    int sinceKey_ = 0;
    bool forceKey_ = true;
    std::string keyCache_;
    uint64_t keyCacheSeq_ = ~0ull;
};

} // namespace delta
} // namespace vibeadsb
