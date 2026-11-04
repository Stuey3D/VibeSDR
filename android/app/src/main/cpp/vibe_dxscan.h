#pragma once
// ★★★ THE IDLE DX SCANNER — the log, its rules and its schedule (V12, 2026-11-04). docs/v12/DX-SCANNER.md.
//
// Stuart's brief (BRIEF-dx-scanner.md): when a radio is idle, VibeServer slowly sweeps Broadcast FM (87.5–108 MHz) and DAB
// Band III and keeps what it hears in a DX LOG — apart from the bookmarks. Its job is to catch TRANSIENT stations (Sporadic-E,
// tropo, meteor scatter); strong locals are logged once, then stay out of the way.
//
// This header is PURE: no radio, no sockets, no threads, no clock of its own (every call is given `now`, UTC seconds). The
// shim drives it (dxTick, local_sdr_shim.cpp); vibeserver/test-dxscan.cpp drives every rule here with made-up detections.
//
//   Config      the owner's settings (RadioConfig::dxScan's JSON — one blob, as bgDecode)
//   planFm      the FM hops for this radio's capture: as wide as the rate allows, every raster channel in exactly one hop
//   Log         entries (FM keyed PI+frequency, DAB keyed EId+block[+TII]), their openings, presence, class, alerts
//
// ★★ THE RULES THAT MATTER (brief §6, §7 — each is a test):
//   • FM is logged only with the PI in ≥ 3 CRC-valid groups in ONE dwell, or the PS fully assembled — a single valid group
//     is never enough (fmConfirmed). Phantom PIs pass the CRC on weak signals; repetition is the defence.
//   • DAB is logged on the first CRC-valid FIB carrying an EId. With TII the key is EId + block + TII main/sub, so two SFN
//     sites are two entries; without it, EId + block.
//   • STABILITY IS JUDGED ON THE PI, NEVER THE PS. A scrolling PS is stored as variants for display only.
//   • A visit cut short (a listener arrived mid-hop) is never recorded — the caller simply does not commit it.
//   • Presence is counted per VISIT of the hop/block that covers an entry (not per whole sweep), in 4-hour buckets over 7
//     days, so a scanner interrupted all day still measures every station fairly.
//   • Local = present in ≥ 90 % of its visits across a 7-day window, every day of it (a 3-day duct is not a local). The
//     first 24 h of a new log are the BOOTSTRAP: everything provisional, no alerts; at its end the baseline is classified
//     on what the bootstrap saw (so the locals land once and never alert). A Local unheard for 7 days drops back to DX.
//   • A new FM detection on the IMAGE (mirror about the capture's DC) or a 3rd-order INTERMOD (2·f1 − f2) of known Locals
//     in the same capture is SUSPECT: kept, greyed, never alerted.
//   • An opening is a contiguous run of detections; a gap over 30 min (configurable) starts the next one.
//
// ★★ BOUNDED (the 256 MB floor): kMaxEntries entries, kMaxOpenings per entry, kMaxVariants PS variants, kMaxServices DAB
//    services, kMaxAlerts alerts. At the cap the oldest Suspect go first, then Provisional, then DX, then Local; a PROMOTED
//    entry is never pruned. Every page the API serves is paged (≤ 100 entries).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "vibe_antswitch.h"   // ★ its small strict JSON reader (Json / parseJson) — pure, no sockets

namespace vibe {
namespace dx {

using Json = vibe::antswitch::Json;

// ── limits ──────────────────────────────────────────────────────────────────────────────────────────────────────────
static constexpr int    kMaxEntries   = 1000;
static constexpr int    kMaxOpenings  = 32;      // per entry, newest kept
static constexpr int    kMaxVariants  = 8;       // PS variants per FM entry
static constexpr int    kMaxServices  = 32;      // DAB services per entry / opening
static constexpr int    kMaxAlerts    = 100;
static constexpr int    kBuckets      = 42;      // 7 days of 4-hour presence buckets
static constexpr int64_t kBucketSec   = 4 * 3600;
static constexpr int64_t kWindowSec   = 7 * 86400;
static constexpr int64_t kBootstrapSec = 24 * 3600;
static constexpr double kLocalRatio   = 0.90;
static constexpr int    kLocalMinVisitsBootstrap = 3;   // a baseline local must have been visited at least this often
static constexpr int    kFmMinGroups  = 3;      // ★ brief §6: PI in at least 3 CRC-valid groups within one dwell
static constexpr double kFmBandLoHz   = 87.5e6, kFmBandHiHz = 108.0e6;

inline std::string esc(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}
inline std::string trimRight(std::string s) { while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back(); return s; }

// ── the owner's settings ────────────────────────────────────────────────────────────────────────────────────────────

/** RadioConfig::dxScan, one JSON blob: {"on":false,"bands":"both","waitMin":5,"raster":100,"viewAdmin":false,
 *  "proof":false,"gapMin":30}. Absent keys take these defaults. ★ Default OFF per radio (brief §10). */
struct Config {
    bool on = false;
    bool fm = true, dab = true;        // "bands": "fm" | "dab" | "both"
    int  waitMin = 5;                  // idle delay before scanning starts
    int  rasterKhz = 100;              // FM raster: 100 or 50
    bool viewAdmin = false;            // admin lock on viewing the log (default open)
    bool proof = false;                // ★ §12 q2 decided: proof clips opt-in, default OFF (not drawn yet — see the doc)
    int  gapMin = 30;                  // a new opening after this many minutes unheard
    bool any() const { return on && (fm || dab); }
};
inline std::string bandsWord(const Config& c) { return c.fm && c.dab ? "both" : c.fm ? "fm" : c.dab ? "dab" : "none"; }
inline std::string toJson(const Config& c) {
    char b[200];
    std::snprintf(b, sizeof b, "{\"on\":%s,\"bands\":\"%s\",\"waitMin\":%d,\"raster\":%d,\"viewAdmin\":%s,\"proof\":%s,\"gapMin\":%d}",
                  c.on ? "true" : "false", bandsWord(c).c_str(), c.waitMin, c.rasterKhz, c.viewAdmin ? "true" : "false",
                  c.proof ? "true" : "false", c.gapMin);
    return b;
}
/** @return false (and `err`) on a blob that is not a JSON object. Out-of-range numbers are clamped, never refused —
 *  a hand-edited config should still scan. "" = the defaults (off). */
inline bool parseConfig(const std::string& text, Config& out, std::string& err) {
    out = Config{};
    if (text.empty()) return true;
    Json j;
    if (!vibe::antswitch::parseJson(text, j) || j.kind != Json::Obj) { err = "the DX scanner settings are not a JSON object"; return false; }
    auto num = [&](const char* k, double d) { const Json* v = j.get(k); return v && v->kind == Json::Num ? v->n : d; };
    auto bo  = [&](const char* k, bool d) { const Json* v = j.get(k); return v && v->kind == Json::Bool ? v->b : d; };
    out.on = bo("on", false);
    const std::string bands = j.str("bands", "both");
    out.fm = bands == "fm" || bands == "both";
    out.dab = bands == "dab" || bands == "both";
    if (!out.fm && !out.dab) { out.fm = out.dab = true; }
    out.waitMin = std::max(1, std::min(240, (int)std::lround(num("waitMin", 5))));
    out.rasterKhz = (int)std::lround(num("raster", 100)) == 50 ? 50 : 100;
    out.viewAdmin = bo("viewAdmin", false);
    out.proof = bo("proof", false);
    out.gapMin = std::max(5, std::min(24 * 60, (int)std::lround(num("gapMin", 30))));
    return true;
}

// ── confirmation (brief §6) ─────────────────────────────────────────────────────────────────────────────────────────

/** ★★ FM: the PI in at least kFmMinGroups CRC-valid groups within one dwell, OR the PS fully assembled (which needs all
 *  four segments, so it cannot come from one group either). `groups` = CRC-valid groups whose block A carried this PI. */
inline bool fmConfirmed(int groups, bool psComplete) { return groups >= kFmMinGroups || (psComplete && groups >= 1); }
/** ★ DAB: the first CRC-valid FIB carrying an EId. */
inline bool dabConfirmed(uint32_t eid, int fibOk) { return eid != 0 && fibOk > 0; }

// ── the FM hops ─────────────────────────────────────────────────────────────────────────────────────────────────────

/** Half the capture a hop may use: the outer tenth of each side is the radio's own filter roll-off, and a channel needs
 *  ±100 kHz of it — so a channel's centre must sit inside ±(0.4·rate − 100 kHz) of the capture centre. */
inline double fmHalfUse(double rateHz) { return std::max(0.0, 0.4 * rateHz - 100e3); }

struct FmHopPlan {
    double centreHz = 0;           // the capture centre (tuned)
    double loHz = 0, hiHz = 0;     // the raster channels this hop owns, inclusive (each channel in exactly one hop)
};
/** The hops that cover every raster channel of [loHz, hiHz] once each. A capture centre sits half a raster step off the
 *  raster, so no channel lands on the dongle's DC. Empty when the capture is too narrow to hold one channel. */
inline std::vector<FmHopPlan> planFm(double rateHz, int rasterKhz, double loHz = kFmBandLoHz, double hiHz = kFmBandHiHz) {
    std::vector<FmHopPlan> out;
    const double half = fmHalfUse(rateHz), r = rasterKhz * 1000.0;
    if (half < r || hiHz < loHz) return out;
    // the raster: 87.5 + n·r (100 kHz: 87.5, 87.6 …; 50 kHz: 87.50, 87.55 …)
    const long nCh = (long)std::floor((hiHz - loHz) / r + 1e-6) + 1;
    long i = 0;
    while (i < nCh) {
        const double f0 = loHz + i * r;
        // the highest centre ≡ r/2 (mod r) whose window still reaches down to f0
        double c = f0 + half;
        c = std::floor((c - loHz - r / 2) / r) * r + loHz + r / 2;
        long last = i;
        while (last + 1 < nCh && loHz + (last + 1) * r <= c + half + 1e-3) ++last;
        out.push_back({ c, f0, loHz + last * r });
        i = last + 1;
    }
    return out;
}
/** Which hop of `plan` owns `khz` (the resume cursor is a FREQUENCY, so a changed rate or raster resumes sensibly). */
inline int fmHopFor(const std::vector<FmHopPlan>& plan, int khz) {
    for (size_t k = 0; k < plan.size(); ++k) if (khz <= (int)std::lround(plan[k].hiHz / 1e3)) return (int)k;
    return 0;
}

// ── detections ──────────────────────────────────────────────────────────────────────────────────────────────────────

struct FmHit {
    int      khz = 0;
    uint16_t pi = 0;
    std::string ps;            // as last assembled (display only)
    bool     psComplete = false;
    int      groups = 0;       // CRC-valid groups carrying `pi` in this dwell
    float    levelDb = -200;   // channel power, dB full scale (uncalibrated — not dBm)
    float    snrDb = 0;        // over the hop's noise floor
};
struct DabHit {
    int      block = -1;       // index into vibedab::kBandIII
    std::string blockName;     // "12B"
    uint32_t eid = 0;
    int      ecc = -1;
    std::string label;         // ensemble label
    std::vector<std::pair<uint32_t, std::string>> services;   // SId, label
    float    snrDb = 0;        // MER
    float    fibRate = 0;
    int      fibOk = 0;
    int      tiiMain = -1, tiiSub = -1;
    double   hz = 0;
};

// ── the log ─────────────────────────────────────────────────────────────────────────────────────────────────────────

enum class Cls { Provisional, Local, Dx, Suspect };
inline const char* clsWord(Cls c) {
    switch (c) { case Cls::Provisional: return "provisional"; case Cls::Local: return "local"; case Cls::Suspect: return "suspect"; default: return "dx"; }
}
inline Cls clsFrom(const std::string& s) {
    return s == "provisional" ? Cls::Provisional : s == "local" ? Cls::Local : s == "suspect" ? Cls::Suspect : Cls::Dx;
}

struct Opening {
    int64_t start = 0, end = 0;
    int     detections = 0;
    float   peakLevelDb = -200, peakSnrDb = 0;
    std::vector<std::string> services;   // ★ §12 q3 decided: a DAB opening records the services that decoded
    std::string proof;                   // optional clip/slide ref (proof clips are opt-in; nothing writes it yet)
};

struct Entry {
    int      id = 0;
    bool     dab = false;
    std::string key;
    Cls      cls = Cls::Dx;
    std::string suspectWhy;
    // FM
    int      khz = 0;
    uint16_t pi = 0;
    int      dcKhz = 0, halfKhz = 0;     // ★ the capture it was last heard in (its DC and half-width) — the image check
    std::string psLast;
    std::vector<std::string> psVariants;
    // DAB
    int      block = -1;
    std::string blockName;
    uint32_t eid = 0;
    int      ecc = -1;
    std::string ensLabel;
    std::vector<std::pair<uint32_t, std::string>> services;
    int      tiiMain = -1, tiiSub = -1;
    // both
    int64_t  firstHeard = 0, lastHeard = 0;
    int      timesHeard = 0;
    float    peakLevelDb = -200, peakSnrDb = 0;
    float    presence = 0;               // presence_ratio_7d, refreshed on every visit
    std::string promoted;                // promoted_bookmark_id ("" = not promoted)
    std::vector<Opening> openings;
    uint32_t lastSweep = 0;              // the band's sweep number when last heard ("Live now")
    // presence buckets (4 h each): visits of the hop/block that covers it, and how many of those heard it
    int32_t  bStamp[kBuckets] = {};
    uint16_t bVisits[kBuckets] = {}, bHits[kBuckets] = {};
};

inline std::string fmKey(uint16_t pi, int khz) { char b[32]; std::snprintf(b, sizeof b, "FM:%04X:%d", pi, khz); return b; }
inline std::string dabKey(uint32_t eid, const std::string& block, int tiiMain, int tiiSub) {
    char b[64];
    if (tiiMain >= 0) std::snprintf(b, sizeof b, "DAB:%04X:%s:%d/%d", (unsigned)eid, block.c_str(), tiiMain, tiiSub);
    else std::snprintf(b, sizeof b, "DAB:%04X:%s", (unsigned)eid, block.c_str());
    return b;
}

struct Alert { uint32_t seq = 0; int64_t utc = 0; int entryId = 0; std::string kind, text; };

/** What the log hands back from a committed visit, for the admin line and the server's own log. */
struct VisitResult { int newEntries = 0, newOpenings = 0, alerts = 0, suspects = 0; };

class Log {
public:
    explicit Log(int64_t now = 0) { created_ = now; }

    int64_t created() const { return created_; }
    bool bootstrapping(int64_t now) const { return now - created_ < kBootstrapSec; }
    void setGapMin(int m) { gapSec_ = std::max(5, m) * 60; }
    int  gapMin() const { return (int)(gapSec_ / 60); }
    size_t size() const { return entries_.size(); }
    uint32_t sweeps(bool dab) const { return dab ? sweepsDab_ : sweepsFm_; }
    uint32_t alertSeq() const { return alertSeq_; }
    bool dirty() const { return dirty_; }
    void clean() { dirty_ = false; }
    const std::map<int, Entry>& entries() const { return entries_; }
    const std::vector<Alert>& alerts() const { return alerts_; }
    const Entry* find(int id) const { auto it = entries_.find(id); return it == entries_.end() ? nullptr : &it->second; }
    const Entry* findKey(const std::string& k) const { auto it = byKey_.find(k); return it == byKey_.end() ? nullptr : find(it->second); }

    // ── the resume cursor (brief §3) — persisted with the log ──
    int  fmCursorKhz = 87500;   // the lowest channel of the next FM hop to visit
    int  dabCursor = 0;         // the next Band III block to visit
    bool nextDab = false;       // which band the scheduler does next

    /** A whole sweep of a band has been completed (the cursor wrapped). */
    void completeSweep(bool dab) { (dab ? sweepsDab_ : sweepsFm_)++; dirty_ = true; }

    /** ★★ COMMIT ONE COMPLETED FM VISIT: the hop covering [loKhz, hiKhz] with its capture DC at `dcKhz` heard `hits`
     *  (already through fmConfirmed or not — unconfirmed hits are dropped here too). Every entry inside the hop counts one
     *  visit, and one hit if heard. A visit cut short is never committed. */
    VisitResult commitFm(int loKhz, int hiKhz, int dcKhz, int rasterKhz, const std::vector<FmHit>& hits, int64_t now) {
        VisitResult vr;
        classifyBootstrapEnd(now);
        std::vector<int> heard;
        for (const auto& h : hits) {
            if (!fmConfirmed(h.groups, h.psComplete) || h.pi == 0) continue;
            const std::string k = fmKey(h.pi, h.khz);
            int id;
            auto it = byKey_.find(k);
            const bool isNew = it == byKey_.end();
            if (isNew) {
                if (!makeRoom()) continue;
                Entry e; e.id = nextId_++; e.dab = false; e.key = k; e.khz = h.khz; e.pi = h.pi; e.firstHeard = now;
                e.cls = bootstrapping(now) ? Cls::Provisional : Cls::Dx;
                e.dcKhz = dcKhz; e.halfKhz = std::max(dcKhz - loKhz, hiKhz - dcKhz);
                const std::string mirror = mirrorOf(h, hits, dcKhz, rasterKhz);
                const std::string why = !mirror.empty() ? mirror : suspectWhy(h.khz, h.pi, dcKhz, e.halfKhz, rasterKhz);
                // ★ A mirror heard IN THIS VERY VISIT (the same PI, far weaker, on the other side of the DC) is an image
                //   whatever the class — the bootstrap included, so a new server's first day does not log its locals twice.
                if (!mirror.empty() || (!why.empty() && e.cls != Cls::Provisional)) { e.cls = Cls::Suspect; e.suspectWhy = why; ++vr.suspects; }
                else if (!why.empty()) e.suspectWhy = why;   // ★ remembered: the bootstrap's end decides with it
                entries_[e.id] = e; byKey_[k] = e.id; id = e.id;
                ++vr.newEntries;
            } else id = it->second;
            Entry& e = entries_[id];
            e.dcKhz = dcKhz; e.halfKhz = std::max(dcKhz - loKhz, hiKhz - dcKhz); rasterKhz_ = rasterKhz;
            const std::string ps = trimRight(h.ps);
            if (!ps.empty()) {
                e.psLast = ps;
                if (std::find(e.psVariants.begin(), e.psVariants.end(), ps) == e.psVariants.end()) {
                    e.psVariants.push_back(ps);
                    if ((int)e.psVariants.size() > kMaxVariants) e.psVariants.erase(e.psVariants.begin());
                }
            }
            heardOne(e, isNew, h.levelDb, h.snrDb, now, {}, vr, sweepsFm_);
            heard.push_back(id);
        }
        // every entry this hop covers was visited
        for (auto& kv : entries_) {
            Entry& e = kv.second;
            if (e.dab || e.khz < loKhz || e.khz > hiKhz) continue;
            visit(e, std::find(heard.begin(), heard.end(), e.id) != heard.end(), now);
        }
        dirty_ = true;
        return vr;
    }

    /** ★★ COMMIT ONE COMPLETED DAB BLOCK VISIT: `hit` is null when nothing decoded there. */
    VisitResult commitDab(int block, const DabHit* hit, int64_t now) {
        VisitResult vr;
        classifyBootstrapEnd(now);
        int heardId = -1;
        if (hit && dabConfirmed(hit->eid, hit->fibOk)) {
            const std::string k = dabKey(hit->eid, hit->blockName, hit->tiiMain, hit->tiiSub);
            auto it = byKey_.find(k);
            const bool isNew = it == byKey_.end();
            int id = -1;
            if (isNew && makeRoom()) {
                Entry e; e.id = nextId_++; e.dab = true; e.key = k; e.block = block; e.blockName = hit->blockName;
                e.eid = hit->eid; e.tiiMain = hit->tiiMain; e.tiiSub = hit->tiiSub; e.firstHeard = now;
                e.cls = bootstrapping(now) ? Cls::Provisional : Cls::Dx;
                entries_[e.id] = e; byKey_[k] = e.id; id = e.id; ++vr.newEntries;
            } else if (!isNew) id = it->second;
            if (id >= 0) {
                Entry& e = entries_[id];
                if (hit->ecc >= 0) e.ecc = hit->ecc;
                if (!trimRight(hit->label).empty()) e.ensLabel = trimRight(hit->label);
                std::vector<std::string> names;
                for (const auto& s : hit->services) {
                    const std::string l = trimRight(s.second);
                    if (l.empty()) continue;
                    names.push_back(l);
                    bool have = false;
                    for (auto& es : e.services) if (es.first == s.first) { es.second = l; have = true; break; }
                    if (!have && (int)e.services.size() < kMaxServices) e.services.push_back({ s.first, l });
                }
                if ((int)names.size() > kMaxServices) names.resize(kMaxServices);
                heardOne(e, isNew, -200.0f, hit->snrDb, now, names, vr, sweepsDab_);
                heardId = id;
            }
        }
        const std::string bn = hit ? hit->blockName : std::string();
        for (auto& kv : entries_) {
            Entry& e = kv.second;
            if (!e.dab || e.block != block) continue;
            visit(e, e.id == heardId, now);
        }
        dirty_ = true;
        return vr;
    }

    /** Promote: the caller wrote the bookmark; this remembers it (and makes the entry unprunable). */
    bool setPromoted(int id, const std::string& bm) {
        auto it = entries_.find(id); if (it == entries_.end()) return false;
        it->second.promoted = bm; dirty_ = true; return true;
    }
    /** Clear the log. ★ Promoted entries are kept (never auto-deleted — and "clear" is the closest thing to it). The
     *  bootstrap does NOT restart: the baseline is a property of the aerial, not of the list. */
    int clear(bool keepPromoted = true) {
        int n = 0;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (keepPromoted && !it->second.promoted.empty()) { ++it; continue; }
            byKey_.erase(it->second.key); it = entries_.erase(it); ++n;
        }
        alerts_.clear();
        dirty_ = true;
        return n;
    }

    /** Live now: heard in the current or the last completed sweep of its band. */
    bool liveNow(const Entry& e) const {
        const uint32_t cur = e.dab ? sweepsDab_ : sweepsFm_;
        return e.timesHeard > 0 && e.lastSweep + 1 >= cur;
    }

    /** Re-judge every entry's class (called on every commit; also from the tick once a minute so a quiet band still ages). */
    void evaluate(int64_t now) {
        classifyBootstrapEnd(now);
        for (auto& kv : entries_) judge(kv.second, now);
    }

    // ── JSON ──

    /** One entry for the API. */
    std::string entryJson(const Entry& e, bool withOpenings) const {
        std::string j = "{\"id\":" + std::to_string(e.id) + ",\"band\":\"" + (e.dab ? "DAB" : "FM") + "\",\"key\":\"" + esc(e.key)
                      + "\",\"class\":\"" + clsWord(e.cls) + "\"";
        if (!e.suspectWhy.empty() && e.cls == Cls::Suspect) j += ",\"suspectWhy\":\"" + esc(e.suspectWhy) + "\"";
        char b[160];
        if (!e.dab) {
            std::snprintf(b, sizeof b, ",\"freqKhz\":%d,\"pi\":\"%04X\"", e.khz, e.pi); j += b;
            j += ",\"ps\":\"" + esc(e.psLast) + "\",\"psVariants\":[";
            for (size_t i = 0; i < e.psVariants.size(); ++i) j += (i ? ",\"" : "\"") + esc(e.psVariants[i]) + "\"";
            j += "]";
        } else {
            std::snprintf(b, sizeof b, ",\"block\":\"%s\",\"eid\":\"%04X\",\"ecc\":%d", esc(e.blockName).c_str(), (unsigned)e.eid, e.ecc); j += b;
            j += ",\"ensemble\":\"" + esc(e.ensLabel) + "\",\"services\":[";
            for (size_t i = 0; i < e.services.size(); ++i)
                j += std::string(i ? "," : "") + "{\"sid\":" + std::to_string(e.services[i].first) + ",\"label\":\"" + esc(e.services[i].second) + "\"}";
            j += "]";
            if (e.tiiMain >= 0) { std::snprintf(b, sizeof b, ",\"tii\":{\"main\":%d,\"sub\":%d}", e.tiiMain, e.tiiSub); j += b; }
        }
        std::snprintf(b, sizeof b, ",\"firstHeard\":%lld,\"lastHeard\":%lld,\"timesHeard\":%d,\"presence\":%.3f,\"openings\":%zu,\"live\":%s",
                      (long long)e.firstHeard, (long long)e.lastHeard, e.timesHeard, e.presence, e.openings.size(), liveNow(e) ? "true" : "false");
        j += b;
        if (e.peakLevelDb > -199) { std::snprintf(b, sizeof b, ",\"peakLevelDb\":%.1f", e.peakLevelDb); j += b; }
        std::snprintf(b, sizeof b, ",\"peakSnrDb\":%.1f", e.peakSnrDb); j += b;
        if (!e.promoted.empty()) j += ",\"promoted\":\"" + esc(e.promoted) + "\"";
        if (withOpenings) {
            j += ",\"openingList\":[";
            for (size_t i = e.openings.size(); i-- > 0;) {   // newest first
                const Opening& o = e.openings[i];
                std::snprintf(b, sizeof b, "%s{\"start\":%lld,\"end\":%lld,\"detections\":%d,\"peakSnrDb\":%.1f",
                              i + 1 == e.openings.size() ? "" : ",", (long long)o.start, (long long)o.end, o.detections, o.peakSnrDb);
                j += b;
                if (o.peakLevelDb > -199) { std::snprintf(b, sizeof b, ",\"peakLevelDb\":%.1f", o.peakLevelDb); j += b; }
                if (!o.services.empty()) {
                    j += ",\"services\":[";
                    for (size_t s = 0; s < o.services.size(); ++s) j += (s ? ",\"" : "\"") + esc(o.services[s]) + "\"";
                    j += "]";
                }
                if (!o.proof.empty()) j += ",\"proof\":\"" + esc(o.proof) + "\"";
                j += "}";
            }
            j += "]";
        }
        return j + "}";
    }

    struct Query {
        bool showLocal = false, showSuspect = false;   // ★ default: DX only (provisional shows too — it is not yet judged)
        std::string band;                              // "" | "FM" | "DAB"
        std::string sort = "last";                     // last | first | peak | times
        int offset = 0, limit = 50;
    };
    /** The DX Log page: filtered, sorted, paged. `total` = how many match the filter. */
    std::string listJson(const Query& q, int64_t now) const {
        std::vector<const Entry*> v;
        int nLocal = 0, nSuspect = 0, nDx = 0, nProv = 0;
        for (const auto& kv : entries_) {
            const Entry& e = kv.second;
            switch (e.cls) { case Cls::Local: ++nLocal; break; case Cls::Suspect: ++nSuspect; break; case Cls::Provisional: ++nProv; break; default: ++nDx; }
            if (!q.band.empty() && q.band != (e.dab ? "DAB" : "FM")) continue;
            if (e.cls == Cls::Local && !q.showLocal) continue;
            if (e.cls == Cls::Suspect && !q.showSuspect) continue;
            v.push_back(&e);
        }
        auto peak = [](const Entry* e) { return e->peakLevelDb > -199 ? e->peakLevelDb : e->peakSnrDb - 100.0f; };
        std::stable_sort(v.begin(), v.end(), [&](const Entry* a, const Entry* b) {
            if (q.sort == "first") return a->firstHeard > b->firstHeard;
            if (q.sort == "peak")  return a->peakSnrDb != b->peakSnrDb ? a->peakSnrDb > b->peakSnrDb : peak(a) > peak(b);
            if (q.sort == "times") return a->timesHeard > b->timesHeard;
            return a->lastHeard > b->lastHeard;
        });
        const int lim = std::max(1, std::min(100, q.limit));
        const int off = std::max(0, std::min((int)v.size(), q.offset));
        char b[256];
        std::snprintf(b, sizeof b, "{\"ok\":true,\"total\":%zu,\"offset\":%d,\"limit\":%d,\"counts\":{\"dx\":%d,\"local\":%d,\"suspect\":%d,\"provisional\":%d},"
                      "\"bootstrap\":%s,\"bootstrapEnds\":%lld,\"sweeps\":{\"fm\":%u,\"dab\":%u},\"alertSeq\":%u,\"now\":%lld,\"entries\":[",
                      v.size(), off, lim, nDx, nLocal, nSuspect, nProv, bootstrapping(now) ? "true" : "false",
                      (long long)(created_ + kBootstrapSec), sweepsFm_, sweepsDab_, alertSeq_, (long long)now);
        std::string j = b;
        for (int i = off; i < std::min((int)v.size(), off + lim); ++i) j += (i > off ? "," : "") + entryJson(*v[(size_t)i], false);
        return j + "]}";
    }
    /** Alerts newer than `since` (the client keeps the last seq it showed). */
    std::string alertsJson(uint32_t since) const {
        std::string j = "{\"ok\":true,\"alertSeq\":" + std::to_string(alertSeq_) + ",\"alerts\":[";
        bool first = true;
        for (const auto& a : alerts_) {
            if (a.seq <= since) continue;
            char b[96]; std::snprintf(b, sizeof b, "{\"seq\":%u,\"utc\":%lld,\"id\":%d,\"kind\":\"", a.seq, (long long)a.utc, a.entryId);
            j += std::string(first ? "" : ",") + b + esc(a.kind) + "\",\"text\":\"" + esc(a.text) + "\"}";
            first = false;
        }
        return j + "]}";
    }

    /** The whole log, for the file (flushed every ~5 min and at shutdown — never per detection: SD-card wear). */
    std::string toFileJson() const {
        std::string j;
        j.reserve(256 + entries_.size() * 600);
        char b[256];
        std::snprintf(b, sizeof b, "{\"v\":1,\"created\":%lld,\"nextId\":%d,\"sweepsFm\":%u,\"sweepsDab\":%u,\"alertSeq\":%u,"
                      "\"fmCursorKhz\":%d,\"dabCursor\":%d,\"nextDab\":%s,\"bootDone\":%s,\"entries\":[",
                      (long long)created_, nextId_, sweepsFm_, sweepsDab_, alertSeq_, fmCursorKhz, dabCursor,
                      nextDab ? "true" : "false", bootDone_ ? "true" : "false");
        j += b;
        bool first = true;
        for (const auto& kv : entries_) {
            const Entry& e = kv.second;
            j += first ? "" : ",";
            first = false;
            j += entryJson(e, true);
            j.pop_back();   // reopen the object for the private fields
            std::snprintf(b, sizeof b, ",\"khz\":%d,\"piN\":%u,\"dc\":%d,\"hw\":%d,\"blk\":%d,\"eidN\":%u,\"tm\":%d,\"ts\":%d,\"lastSweep\":%u,\"buckets\":[",
                          e.khz, e.pi, e.dcKhz, e.halfKhz, e.block, e.eid, e.tiiMain, e.tiiSub, e.lastSweep);
            j += b;
            bool fb = true;
            for (int i = 0; i < kBuckets; ++i) {
                if (!e.bVisits[i]) continue;
                std::snprintf(b, sizeof b, "%s[%d,%u,%u]", fb ? "" : ",", e.bStamp[i], e.bVisits[i], e.bHits[i]);
                j += b; fb = false;
            }
            j += "]}";
        }
        j += "],\"alerts\":[";
        for (size_t i = 0; i < alerts_.size(); ++i) {
            const Alert& a = alerts_[i];
            std::snprintf(b, sizeof b, "%s{\"seq\":%u,\"utc\":%lld,\"id\":%d,\"kind\":\"", i ? "," : "", a.seq, (long long)a.utc, a.entryId);
            j += b + esc(a.kind) + "\",\"text\":\"" + esc(a.text) + "\"}";
        }
        return j + "]}";
    }
    /** Load a file written by toFileJson. @return false on anything unreadable (the caller starts a fresh log). */
    bool fromFileJson(const std::string& text) {
        Json j;
        if (!vibe::antswitch::parseJson(text, j) || j.kind != Json::Obj) return false;
        auto num = [](const Json& o, const char* k, double d) { const Json* v = o.get(k); return v && v->kind == Json::Num ? v->n : d; };
        auto bo  = [](const Json& o, const char* k, bool d) { const Json* v = o.get(k); return v && v->kind == Json::Bool ? v->b : d; };
        entries_.clear(); byKey_.clear(); alerts_.clear();
        created_ = (int64_t)num(j, "created", 0);
        nextId_ = (int)num(j, "nextId", 1);
        sweepsFm_ = (uint32_t)num(j, "sweepsFm", 0); sweepsDab_ = (uint32_t)num(j, "sweepsDab", 0);
        alertSeq_ = (uint32_t)num(j, "alertSeq", 0);
        fmCursorKhz = (int)num(j, "fmCursorKhz", 87500); dabCursor = (int)num(j, "dabCursor", 0);
        nextDab = bo(j, "nextDab", false); bootDone_ = bo(j, "bootDone", false);
        if (const Json* es = j.get("entries"); es && es->kind == Json::Arr) {
            for (const auto& o : es->a) {
                if (o.kind != Json::Obj || (int)entries_.size() >= kMaxEntries) continue;
                Entry e;
                e.id = (int)num(o, "id", 0);
                if (e.id <= 0) continue;
                e.dab = o.str("band") == "DAB";
                e.key = o.str("key");
                if (e.key.empty()) continue;
                e.cls = clsFrom(o.str("class"));
                e.suspectWhy = o.str("suspectWhy");
                e.khz = (int)num(o, "khz", 0); e.pi = (uint16_t)num(o, "piN", 0);
                e.dcKhz = (int)num(o, "dc", 0); e.halfKhz = (int)num(o, "hw", 0);
                e.psLast = o.str("ps");
                if (const Json* pv = o.get("psVariants"); pv && pv->kind == Json::Arr)
                    for (const auto& s : pv->a) if (s.kind == Json::Str && (int)e.psVariants.size() < kMaxVariants) e.psVariants.push_back(s.s);
                e.block = (int)num(o, "blk", -1); e.blockName = o.str("block"); e.eid = (uint32_t)num(o, "eidN", 0);
                e.ecc = (int)num(o, "ecc", -1); e.ensLabel = o.str("ensemble");
                e.tiiMain = (int)num(o, "tm", -1); e.tiiSub = (int)num(o, "ts", -1);
                if (const Json* sv = o.get("services"); sv && sv->kind == Json::Arr)
                    for (const auto& s : sv->a)
                        if (s.kind == Json::Obj && (int)e.services.size() < kMaxServices)
                            e.services.push_back({ (uint32_t)num(s, "sid", 0), s.str("label") });
                e.firstHeard = (int64_t)num(o, "firstHeard", 0); e.lastHeard = (int64_t)num(o, "lastHeard", 0);
                e.timesHeard = (int)num(o, "timesHeard", 0);
                e.peakLevelDb = (float)num(o, "peakLevelDb", -200); e.peakSnrDb = (float)num(o, "peakSnrDb", 0);
                e.presence = (float)num(o, "presence", 0);
                e.promoted = o.str("promoted");
                e.lastSweep = (uint32_t)num(o, "lastSweep", 0);
                if (const Json* ol = o.get("openingList"); ol && ol->kind == Json::Arr) {
                    for (size_t i = ol->a.size(); i-- > 0;) {   // stored newest first
                        const Json& p = ol->a[i];
                        if (p.kind != Json::Obj) continue;
                        Opening op;
                        op.start = (int64_t)num(p, "start", 0); op.end = (int64_t)num(p, "end", 0);
                        op.detections = (int)num(p, "detections", 0);
                        op.peakLevelDb = (float)num(p, "peakLevelDb", -200); op.peakSnrDb = (float)num(p, "peakSnrDb", 0);
                        if (const Json* sv = p.get("services"); sv && sv->kind == Json::Arr)
                            for (const auto& s : sv->a) if (s.kind == Json::Str && (int)op.services.size() < kMaxServices) op.services.push_back(s.s);
                        op.proof = p.str("proof");
                        e.openings.push_back(op);
                    }
                    if ((int)e.openings.size() > kMaxOpenings) e.openings.erase(e.openings.begin(), e.openings.end() - kMaxOpenings);
                }
                if (const Json* bk = o.get("buckets"); bk && bk->kind == Json::Arr)
                    for (const auto& t : bk->a) {
                        if (t.kind != Json::Arr || t.a.size() != 3) continue;
                        const int32_t st = (int32_t)t.a[0].n;
                        const int slot = ((st % kBuckets) + kBuckets) % kBuckets;
                        e.bStamp[slot] = st; e.bVisits[slot] = (uint16_t)t.a[1].n; e.bHits[slot] = (uint16_t)t.a[2].n;
                    }
                if (byKey_.count(e.key) || entries_.count(e.id)) continue;
                nextId_ = std::max(nextId_, e.id + 1);
                byKey_[e.key] = e.id;
                entries_[e.id] = e;
            }
        }
        if (const Json* al = j.get("alerts"); al && al->kind == Json::Arr)
            for (const auto& a : al->a) {
                if (a.kind != Json::Obj || (int)alerts_.size() >= kMaxAlerts) continue;
                Alert x; x.seq = (uint32_t)num(a, "seq", 0); x.utc = (int64_t)num(a, "utc", 0); x.entryId = (int)num(a, "id", 0);
                x.kind = a.str("kind"); x.text = a.str("text");
                alerts_.push_back(x);
            }
        dirty_ = false;
        return true;
    }

    /** Presence over the window (exposed for tests). */
    static float presenceOf(const Entry& e, int64_t now, int* visitsOut = nullptr) {
        const int32_t nb = (int32_t)(now / kBucketSec);
        unsigned v = 0, h = 0;
        for (int i = 0; i < kBuckets; ++i)
            if (e.bVisits[i] && e.bStamp[i] > nb - kBuckets && e.bStamp[i] <= nb) { v += e.bVisits[i]; h += e.bHits[i]; }
        if (visitsOut) *visitsOut = (int)v;
        return v ? (float)h / (float)v : 0.0f;
    }

private:
    std::map<int, Entry> entries_;
    std::map<std::string, int> byKey_;
    std::vector<Alert> alerts_;
    int64_t  created_ = 0;
    int64_t  gapSec_ = 30 * 60;
    int      rasterKhz_ = 100;
    int      nextId_ = 1;
    uint32_t sweepsFm_ = 0, sweepsDab_ = 0, alertSeq_ = 0;
    bool     bootDone_ = false;
    bool     dirty_ = false;

    void addAlert(const Entry& e, const char* kind, int64_t now, VisitResult& vr) {
        Alert a; a.seq = ++alertSeq_; a.utc = now; a.entryId = e.id; a.kind = kind;
        char b[64];
        if (!e.dab) {
            std::snprintf(b, sizeof b, "%.1f MHz", e.khz / 1000.0);
            a.text = std::string(kind[0] == 'n' ? "New DX: " : "DX back: ") + b + " PI " + e.key.substr(3, 4) + (e.psLast.empty() ? "" : " " + e.psLast);
        } else {
            a.text = std::string(kind[0] == 'n' ? "New DX: DAB " : "DX back: DAB ") + e.blockName + (e.ensLabel.empty() ? "" : " " + e.ensLabel);
        }
        alerts_.push_back(a);
        if ((int)alerts_.size() > kMaxAlerts) alerts_.erase(alerts_.begin());
        ++vr.alerts;
    }

    /** One detection of `e`: counters, peaks, the opening (new after the gap), and the alert if it is DX. */
    void heardOne(Entry& e, bool isNew, float level, float snr, int64_t now, const std::vector<std::string>& svcs,
                  VisitResult& vr, uint32_t sweepNow) {
        const bool newOpening = e.openings.empty() || now - e.openings.back().end > gapSec_;
        if (newOpening) {
            Opening o; o.start = now; o.end = now;
            e.openings.push_back(o);
            if ((int)e.openings.size() > kMaxOpenings) e.openings.erase(e.openings.begin());
            if (!isNew) ++vr.newOpenings;
        }
        Opening& o = e.openings.back();
        o.end = now; ++o.detections;
        if (level > o.peakLevelDb) o.peakLevelDb = level;
        if (snr > o.peakSnrDb) o.peakSnrDb = snr;
        for (const auto& s : svcs)
            if ((int)o.services.size() < kMaxServices && std::find(o.services.begin(), o.services.end(), s) == o.services.end()) o.services.push_back(s);
        // ★ a Local unheard for over the window has dropped back (judge does it on visits; this catches a band unvisited)
        if (e.cls == Cls::Local && e.lastHeard > 0 && now - e.lastHeard > kWindowSec) e.cls = Cls::Dx;
        e.lastHeard = now; ++e.timesHeard; e.lastSweep = sweepNow;
        if (level > e.peakLevelDb) e.peakLevelDb = level;
        if (snr > e.peakSnrDb) e.peakSnrDb = snr;
        // ★ Alerts: DX only — never Local, Suspect or Provisional (the bootstrap floods nobody)
        if (e.cls == Cls::Dx) {
            if (isNew) addAlert(e, "new", now, vr);
            else if (newOpening) addAlert(e, "opening", now, vr);
        }
    }

    void visit(Entry& e, bool hit, int64_t now) {
        const int32_t nb = (int32_t)(now / kBucketSec);
        const int slot = ((nb % kBuckets) + kBuckets) % kBuckets;
        if (e.bStamp[slot] != nb) { e.bStamp[slot] = nb; e.bVisits[slot] = 0; e.bHits[slot] = 0; }
        if (e.bVisits[slot] < 65535) ++e.bVisits[slot];
        if (hit && e.bHits[slot] < 65535) ++e.bHits[slot];
        judge(e, now);
    }

    /** ★★ THE CLASS (brief §7). Never PS — only presence of the KEY (PI+frequency / EId+block). */
    void judge(Entry& e, int64_t now) {
        int visits = 0;
        e.presence = presenceOf(e, now, &visits);
        if (e.cls == Cls::Provisional) return;                  // the bootstrap's end decides (classifyBootstrapEnd)
        if (e.cls == Cls::Local) {
            if (now - e.lastHeard > kWindowSec) e.cls = Cls::Dx;   // ★ gone a week: drops back, unclassified
            return;
        }
        if (e.cls == Cls::Suspect) return;
        // DX → Local: heard for the whole window, ≥ 90 % of its visits, and on every day of it that had visits
        if (now - e.firstHeard < kWindowSec || e.presence < kLocalRatio) return;
        const int32_t nb = (int32_t)(now / kBucketSec);
        int daysVisited = 0;
        for (int d = 0; d < 7; ++d) {
            unsigned v = 0, h = 0;
            for (int k = 0; k < 6; ++k) {
                const int32_t st = nb - (d * 6 + k);
                const int slot = ((st % kBuckets) + kBuckets) % kBuckets;
                if (e.bStamp[slot] == st) { v += e.bVisits[slot]; h += e.bHits[slot]; }
            }
            if (!v) continue;
            ++daysVisited;
            if ((double)h / v < 0.75) return;   // a day it mostly was not there: not a local (a duct that came and went)
        }
        if (daysVisited >= 6) e.cls = Cls::Local;
    }

    /** ★ The bootstrap's end: the baseline is judged on what it saw. Present in ≥ 90 % of ≥ 3 visits → Local (logged once,
     *  silent); a provisional on an image of a new Local → Suspect; the rest → DX, with no alert for having been found
     *  during the bootstrap (their NEXT opening alerts). */
    void classifyBootstrapEnd(int64_t now) {
        if (bootDone_ || bootstrapping(now)) return;
        bootDone_ = true;
        for (auto& kv : entries_) {
            Entry& e = kv.second;
            if (e.cls != Cls::Provisional) continue;
            int visits = 0;
            const float p = presenceOf(e, now, &visits);
            e.cls = (visits >= kLocalMinVisitsBootstrap && p >= kLocalRatio) ? Cls::Local : Cls::Dx;
        }
        // ★ …and now the Locals are known, a baseline detection on one's image or intermod is Suspect
        for (auto& kv : entries_) {
            Entry& e = kv.second;
            if (e.dab || e.cls != Cls::Dx || !e.dcKhz) continue;
            const std::string why = suspectWhy(e.khz, e.pi, e.dcKhz, e.halfKhz, rasterKhz_);
            if (!why.empty()) { e.cls = Cls::Suspect; e.suspectWhy = why; }
        }
        dirty_ = true;
    }

    /** ★★ THE RTL IMAGE / INTERMOD CHECK: a new FM detection on the mirror of a known Local about this capture's DC, or on
     *  2·f1 − f2 of two Locals both inside the capture. "" = not suspect. */
    std::string suspectWhy(int khz, uint16_t pi, int dcKhz, int halfKhz, int rasterKhz) const {
        const int tol = std::max(1, rasterKhz / 2);
        const int spanLo = dcKhz - halfKhz - 200, spanHi = dcKhz + halfKhz + 200;   // the capture this one was heard in
        std::vector<const Entry*> locals;
        for (const auto& kv : entries_) {
            const Entry& e = kv.second;
            if (e.dab || e.cls != Cls::Local) continue;
            if (e.khz < spanLo || e.khz > spanHi) continue;
            locals.push_back(&e);
        }
        char b[96];
        for (const Entry* l : locals) {
            const int img = 2 * dcKhz - l->khz;
            if (std::abs(img - khz) < tol && l->khz != khz) {
                std::snprintf(b, sizeof b, "image of %.1f MHz (PI %04X)%s", l->khz / 1000.0, l->pi, l->pi == pi ? ", same PI" : "");
                return b;
            }
        }
        for (const Entry* a : locals)
            for (const Entry* c : locals) {
                if (a == c) continue;
                const int imd = 2 * a->khz - c->khz;
                if (std::abs(imd - khz) < tol && a->khz != khz && c->khz != khz) {
                    std::snprintf(b, sizeof b, "intermod of %.1f and %.1f MHz", a->khz / 1000.0, c->khz / 1000.0);
                    return b;
                }
            }
        return "";
    }

    /** ★★ THE IMAGE IN THE SAME CAPTURE: another hit of this visit carries the SAME PI on the mirror frequency about the DC
     *  and is at least 10 dB stronger — the dongle's own IQ image of it (RTL images sit 25–45 dB down: strong enough to
     *  decode on a local). "" = not one. */
    static std::string mirrorOf(const FmHit& h, const std::vector<FmHit>& all, int dcKhz, int rasterKhz) {
        const int tol = std::max(1, rasterKhz / 2);
        for (const auto& g : all) {
            if (&g == &h || g.pi != h.pi || g.khz == h.khz) continue;
            if (std::abs((2 * dcKhz - g.khz) - h.khz) < tol && g.levelDb >= h.levelDb + 10.0f) {
                char b[96]; std::snprintf(b, sizeof b, "image of %.1f MHz (same PI %04X, %.0f dB weaker)", g.khz / 1000.0, g.pi, g.levelDb - h.levelDb);
                return b;
            }
        }
        return "";
    }
    /** Make room for one more entry: the oldest Suspect, then Provisional, then DX, then Local — never Promoted. */
    bool makeRoom() {
        if ((int)entries_.size() < kMaxEntries) return true;
        for (Cls c : { Cls::Suspect, Cls::Provisional, Cls::Dx, Cls::Local }) {
            int victim = -1; int64_t oldest = INT64_MAX;
            for (const auto& kv : entries_)
                if (kv.second.cls == c && kv.second.promoted.empty() && kv.second.lastHeard < oldest) { oldest = kv.second.lastHeard; victim = kv.first; }
            if (victim >= 0) { byKey_.erase(entries_[victim].key); entries_.erase(victim); dirty_ = true; return true; }
        }
        return false;
    }
};

// ── the schedule (brief §3): which visit is next, and moving the cursor on ──────────────────────────────────────────

/** One visit: an FM hop (index into planFm's list) or a DAB block (index into vibedab::kBandIII). */
struct Job { bool dab = false; int index = 0; };

/** ★ THE NEXT VISIT. Each band has its own cursor, so an idle period carries on from the next unscanned hop or block —
 *  never from the start of the band. With both bands on, whole sweeps alternate (FM, then DAB, then FM …): a sweep is
 *  short (FM ≈ a minute, DAB a few) and Es openings are short too, so neither band waits long. */
inline Job nextJob(const Log& log, const Config& c, const std::vector<FmHopPlan>& fm, int dabBlocks) {
    Job j;
    const bool canFm = c.fm && !fm.empty(), canDab = c.dab && dabBlocks > 0;
    j.dab = canDab && (!canFm || log.nextDab);
    j.index = j.dab ? std::max(0, std::min(dabBlocks - 1, log.dabCursor)) : fmHopFor(fm, log.fmCursorKhz);
    return j;
}
/** ★ A visit COMPLETED (never called for one cut short): the cursor moves on; a wrap completes that band's sweep, and
 *  with both bands on the other band goes next. */
inline void advance(Log& log, const Config& c, const Job& j, const std::vector<FmHopPlan>& fm, int dabBlocks) {
    const bool both = c.fm && c.dab && !fm.empty() && dabBlocks > 0;
    if (j.dab) {
        log.dabCursor = j.index + 1;
        if (log.dabCursor >= dabBlocks) { log.dabCursor = 0; log.completeSweep(true); if (both) log.nextDab = false; }
    } else {
        const int nx = j.index + 1;
        if (nx >= (int)fm.size()) {
            log.fmCursorKhz = fm.empty() ? 87500 : (int)std::lround(fm.front().loHz / 1e3);
            log.completeSweep(false);
            if (both) log.nextDab = true;
        } else log.fmCursorKhz = (int)std::lround(fm[(size_t)nx].loHz / 1e3);
    }
}

}  // namespace dx
}  // namespace vibe
