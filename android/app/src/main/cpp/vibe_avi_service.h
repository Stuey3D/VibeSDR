// vibe_avi_service.h — V12 aviation datalink: the service around the decoders (vibe_acars.h, vibe_vdl2.h, vibe_hfdl.h).
// Its own thread fed by copy, the aircraft table every message lands in, and the JSON the clients draw.
// docs/v12/AVIATION.md §Service. Wired: the shim's "Aircraft datalink" mode (ACARS / VDL2, a whole-radio mode) and the
// listeners' HFDL decoder (vibe_hfdl_run.h) both land in sharedTracker(); /addon/hfdl/* is served from its HFDL rows.
//
// ★★★ OFF THE DSP THREAD, AND IT DROPS RATHER THAN WAITS (ADS-B's, ISM's and AIS's rule: Network > Audio > Spectrum >
//     Decoders). feed() copies and returns; a queue over half a second loses the OLDEST block and counts it.
// ★★★ ONE ROW PER AIRCRAFT, from every decoder: VDL2 names it by its ICAO 24-bit address (the AVLC source), ACARS by its
//     registration and flight, HFDL by its ICAO address (logons) or flight (performance / frequency data). A row keeps
//     whatever has been heard — registration, flight, ICAO, the last position, the labels — so the ADS-B view can join
//     it to the same aircraft by ICAO address (or by registration through the aircraft database).
// ★★★ MESSAGE BODIES ARE THE OWNER'S CHOICE (docs/v12/DECODER-RESEARCH.md §4.3). Flight IDs, registrations, positions
//     and labels are shown by default; ACARS free text only when the owner has switched `bodies` on (off by default,
//     with the jurisdiction notice on the setup page), and never in a public gallery.
#pragma once
#include "vibe_acars.h"
#include "vibe_acars_apps.h"   // ★ what the messages say: positions, OOOI, ADS-C / CPDLC named (§Applications)
#include "vibe_vdl2.h"
#include "vibe_hfdl.h"
#include "vibe_aircraft_share.h"   // ★ V12 2026-11-16 the shared table every radio on the machine writes (the AVIATION MAP)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vibeavi {

inline std::string jsonEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20 || c == 0x7F) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o;
}

/** A short, plain-words name for the common ACARS labels (ARINC 620 is paid; these are the widely published meanings). */
inline const char* labelName(const std::string& l) {
    static const struct { const char* l; const char* n; } t[] = {
        { "_\x7f", "link test / ack" }, { "Q0", "link test" }, { "SQ", "ground station squitter" }, { "H1", "message to/from crew or ops" },
        { "H2", "weather report" }, { "16", "position report" }, { "15", "position report" }, { "10", "departure" },
        { "11", "arrival" }, { "5U", "weather request" }, { "5Z", "airline ops" }, { "B6", "ADS-C" }, { "BA", "CPDLC / ATS" },
        { "SA", "media advisory" }, { "QA", "out of gate" }, { "QB", "off the ground" }, { "QC", "on the ground" }, { "QD", "in at the gate" },
        { "80", "airline ops" }, { "1L", "airline ops" }, { "Q1", "OOOI" }, { "RA", "uplink to the printer" },
        // ★ the labels vibe_acars_apps.h reads (libacars for the ATS ones, the MIT sample set for the OOOI ones)
        { "QP", "out of gate" }, { "QQ", "off the ground" }, { "QR", "on the ground" }, { "QS", "in at the gate" },
        { "AA", "ATS uplink (CPDLC / ADS-C)" }, { "A6", "ADS-C request" }, { "44", "position / OOOI" },
    };
    for (const auto& e : t) if (l == e.l) return e.n;
    return "";
}

struct Aircraft {
    uint32_t icao = 0;               ///< 0 = not known (ACARS alone)
    std::string reg, flight;
    double lat = NAN, lon = NAN, posAt = -1;
    // ★ from the ACARS applications (vibe_acars_apps.h) — flight data, shown by default like the registration
    bool hasAlt = false; int altFt = 0;
    std::string posSrc, posTime;     ///< "ADS-C" | "FMS" | "ACARS" | "HFDL"; the fix time AS REPORTED ("21:57:54", ":06:36")
    std::string from, to;            ///< ICAO airports from an OOOI / FMS / weather report
    std::string oooi, oooiTime;      ///< the last OUT / OFF / ON / IN event and its time ("20:30")
    std::string app;                 ///< the last structured message's application: "ADS-C", "CPDLC", "OOOI", "FMS", "position"
    double first = 0, last = 0;
    int count = 0;
    std::string via;                 ///< "VDL2 136.975", "ACARS 131.725", "HFDL 8977 (Reykjavik)"
    float level = -120;
    float snr = NAN;                  ///< ★ beta96 the aircraft's LAST message's SNR, dB (its decoder's own measure)
    const char* snrMode = "";        ///< ★ which scale: "acars" "vdl2" "hfdl300" "hfdl600" "hfdl1200" "hfdl1800" (src/utils/snrColour.ts)
    std::string lastLabel, lastText; ///< lastText: only kept when bodies are on
    // ★ HFDL: the last burst's ground station, frequency and level, and when (the map's /addon/hfdl/aircraft fields)
    int gsId = -1; double hfdlKhz = 0, hfdlAt = -1; float hfdlLevel = 0;
    // ★ V12 2026-11-16 the shared table (vibe_aircraft_share.h): which decoders heard it and when (wall seconds), and how many
    //   of its messages have already been handed over
    uint8_t srcMask = 0; double srcAt[vibeavshare::kNSrc] = {}; int countShared = 0;
};

/** ★ The HFDL ground stations (the public HFDL station list: id, name, position to a tenth of a degree). */
struct GroundStation { int id; const char* name; double lat, lon; };
inline const std::vector<GroundStation>& groundStations() {
    static const std::vector<GroundStation> v = {
        { 1, "San Francisco", 37.4, -122.4 }, { 2, "Molokai", 21.2, -157.2 }, { 3, "Reykjavik", 64.1, -21.9 },
        { 4, "Riverhead", 40.9, -72.6 }, { 5, "Auckland", -37.0, 174.8 }, { 6, "Hat Yai", 6.9, 100.4 },
        { 7, "Shannon", 52.7, -8.9 }, { 8, "Johannesburg", -26.1, 28.1 }, { 9, "Barrow", 71.3, -156.8 },
        { 10, "Muan", 35.0, 126.4 }, { 11, "Albrook", 9.0, -79.6 }, { 13, "Santa Cruz", -17.7, -63.1 },
        { 14, "Krasnoyarsk", 56.2, 92.5 }, { 15, "Al Muharraq", 26.3, 50.6 }, { 16, "Agana", 13.5, 144.8 },
        { 17, "Canarias", 27.9, -15.4 },
    };
    return v;
}

/** The table: one row per aircraft, merged from every decoder. Thread-safe. */
class Tracker {
public:
    explicit Tracker(bool bodies = false) : bodies_(bodies) {}
    void setBodies(bool on) { std::lock_guard<std::mutex> lk(m_); bodies_ = on; if (!on) for (auto& a : rows_) a.lastText.clear(); }

    void addVdl2(const vibevdl2::Frame& f, double now) {
        std::lock_guard<std::mutex> lk(m_);
        const bool fromAc = f.src.type == 1;
        const uint32_t ic = fromAc ? f.src.addr : (f.dst.type == 1 ? f.dst.addr : 0);
        if (!ic) { ++other_; return; }
        Aircraft& a = row_(ic, f.hasAcars ? f.acars.reg : "", now);
        char via[48]; std::snprintf(via, sizeof via, "VDL2 %.3f", f.hz / 1e6);
        a.via = via;
        heard_(a, vibeavshare::kVdl2, now);
        if (fromAc) a.level = f.levelDb;
        a.snr = fromAc ? f.snrDb : NAN;   // ★ beta96 an uplink's SNR is the ground station's, not this aircraft's
        a.snrMode = "vdl2";
        if (f.hasAcars && f.acarsCrcOk) acars_(a, f.acars, now);
        ++msgs_; heardAt_(now);
    }
    void addAcars(const vibeacars::Block& b, double now) {
        std::lock_guard<std::mutex> lk(m_);
        if (b.reg.empty()) { ++other_; return; }
        Aircraft& a = row_(0, b.reg, now);
        char via[48]; std::snprintf(via, sizeof via, "ACARS %.3f", b.hz / 1e6);
        a.via = via; a.level = b.levelDb; a.snr = b.downlink ? b.snrDb : NAN; a.snrMode = "acars";
        heard_(a, vibeavshare::kAcars, now);
        acars_(a, b, now);
        ++msgs_; heardAt_(now);
    }
    void addHfdl(const vibehfdl::Message& m, double now) {
        std::lock_guard<std::mutex> lk(m_);
        heardGs_(m.downlink ? m.dstId : m.srcId, m.hz / 1e3, m.snrDb, now);
        if (!m.downlink || (!m.icao && m.flight.empty() && !m.hasAcars)) { ++other_; return; }
        Aircraft& a = row_(m.icao, m.hasAcars ? m.acars.reg : "", now, m.icao ? "" : m.flight);
        char via[64]; std::snprintf(via, sizeof via, "HFDL %.0f%s%s", m.hz / 1e3, *vibehfdl::gsName(m.dstId) ? " " : "", vibehfdl::gsName(m.dstId));
        a.via = via;
        heard_(a, vibeavshare::kHfdl, now);
        a.gsId = m.dstId; a.hfdlKhz = m.hz / 1e3; a.hfdlAt = now; a.hfdlLevel = m.snrDb; a.snr = m.snrDb;
        { const int bps = vibehfdl::bitRate(m.mode); a.snrMode = bps >= 1800 ? "hfdl1800" : bps >= 1200 ? "hfdl1200" : bps >= 600 ? "hfdl600" : "hfdl300"; }
        if (!m.flight.empty()) a.flight = m.flight;
        if (!std::isnan(m.lat)) { a.lat = m.lat; a.lon = m.lon; a.posAt = now; a.posSrc = "HFDL"; a.posTime.clear(); a.hasAlt = false; }
        if (m.hasAcars && m.acarsCrcOk) acars_(a, m.acars, now);
        ++msgs_; heardAt_(now);
    }
    /** Rows heard in the last `keepS` seconds, newest first: {"aircraft":[{icao, reg, flight, lat, lon, via, n, age, label, text}]}. */
    /** `withText`: the asker may see message bodies (the owner's socket) — still only when the owner switched them on. */
    std::string json(double now, double keepS = 3600, bool withText = true) {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<const Aircraft*> v;
        for (const auto& a : rows_) if (now - a.last <= keepS) v.push_back(&a);
        std::sort(v.begin(), v.end(), [](const Aircraft* x, const Aircraft* y) { return x->last > y->last; });
        std::string o = "{\"msgs\":" + std::to_string(msgs_);
        char cb[160];
        // ★★ beta96 (Stuart 11-26: "a messages per minute like ais … it will allow me to check antenna performance quickly"):
        //    the last 60 s, AIS's rule (vibe_ais_service.h) — over the time listened when that is under a minute
        while (!rateWin_.empty() && now - rateWin_.front() > 60) rateWin_.pop_front();
        if (start_ < 0) start_ = now;
        std::snprintf(cb, sizeof cb, ",\"msgRate\":%.2f", rateWin_.size() / std::min(60.0, std::max(1.0, now - start_)) * 60.0);
        o += cb;
        std::snprintf(cb, sizeof cb, ",\"apps\":{\"pos\":%llu,\"oooi\":%llu,\"adsc\":%llu,\"cpdlc\":%llu,\"unparsed\":%llu}",
                      (unsigned long long)nPos_, (unsigned long long)nOooi_, (unsigned long long)nAdsc_, (unsigned long long)nCpdlc_, (unsigned long long)nUnparsed_);
        o += cb;
        o += ",\"aircraft\":[";
        char b[160];
        for (size_t i = 0; i < v.size(); ++i) {
            const Aircraft& a = *v[i];
            if (i) o += ',';
            o += '{';
            if (a.icao) { std::snprintf(b, sizeof b, "\"icao\":\"%06X\",", a.icao); o += b; }
            if (!a.reg.empty()) o += "\"reg\":\"" + jsonEsc(a.reg) + "\",";
            if (!a.flight.empty()) o += "\"flight\":\"" + jsonEsc(a.flight) + "\",";
            if (!std::isnan(a.lat)) {
                std::snprintf(b, sizeof b, "\"lat\":%.4f,\"lon\":%.4f,\"posAge\":%.0f,", a.lat, a.lon, now - a.posAt); o += b;
                if (a.hasAlt) { std::snprintf(b, sizeof b, "\"alt\":%d,", a.altFt); o += b; }
                if (!a.posSrc.empty()) o += "\"posSrc\":\"" + jsonEsc(a.posSrc) + "\",";
                if (!a.posTime.empty()) o += "\"posTime\":\"" + jsonEsc(a.posTime) + "\",";
            }
            if (!a.from.empty()) o += "\"from\":\"" + jsonEsc(a.from) + "\",\"to\":\"" + jsonEsc(a.to) + "\",";
            if (!a.oooi.empty()) o += "\"oooi\":\"" + jsonEsc(a.oooi) + "\",\"oooiTime\":\"" + jsonEsc(a.oooiTime) + "\",";
            if (!a.app.empty()) o += "\"app\":\"" + jsonEsc(a.app) + "\",";
            if (!a.lastLabel.empty()) o += "\"label\":\"" + jsonEsc(a.lastLabel) + "\",\"labelName\":\"" + jsonEsc(labelName(a.lastLabel)) + "\",";
            if (bodies_ && withText && !a.lastText.empty()) o += "\"text\":\"" + jsonEsc(a.lastText) + "\",";
            std::snprintf(b, sizeof b, "\"level\":%.1f,\"n\":%d,\"age\":%.0f,", a.level, a.count, now - a.last); o += b;
            // ★ beta96 the last message's SNR (MESSAGE-SNR.md: ACARS carrier / VDL2 burst over the channel floor, HFDL the
            //   equaliser's) — only when it came from the aircraft, as the level (a ground uplink says nothing of our aerial)
            if (a.snr == a.snr) { std::snprintf(b, sizeof b, "\"snr\":%.1f,\"sm\":\"%s\",", a.snr, a.snrMode); o += b; }
            o += "\"via\":\"" + jsonEsc(a.via) + "\"}";
        }
        o += "]}";
        return o;
    }
    size_t size() { std::lock_guard<std::mutex> lk(m_); return rows_.size(); }
    uint64_t messages() { std::lock_guard<std::mutex> lk(m_); return msgs_; }
    /** Forget every row (a fresh entry into the mode keeps nothing from a radio that has moved on). */
    void clear() { std::lock_guard<std::mutex> lk(m_); rows_.clear(); gs_.clear(); msgs_ = other_ = 0; rateWin_.clear(); start_ = -1; nPos_ = nOooi_ = nAdsc_ = nCpdlc_ = nUnparsed_ = 0; reasm_ = vibeacarsapps::Reassembler(); }

    /** ★ /addon/hfdl/aircraft — the HFDL map's shape (src/components/MapOverlay.tsx fetchAC): an array of
     *  {key, icao, flight, reg, lat, lon, last_seen (unix s), gs_id, freq_khz, msg_count}. ★ No sig_level: the map reads
     *  it as dBFS and the demodulator measures an SNR — a wrong unit on a meter is worse than no meter. Rows HFDL has heard
     *  in the last `keepS` seconds; positions only where one was reported. `nowUnix` is the wall clock (the map ages by it). */
    std::string hfdlAircraftJson(double now, double nowUnix, double keepS = 3600) {
        std::lock_guard<std::mutex> lk(m_);
        std::string o = "[";
        char b[200];
        bool first = true;
        for (const auto& a : rows_) {
            if (a.hfdlAt < 0 || now - a.hfdlAt > keepS) continue;
            o += first ? "{" : ",{"; first = false;
            if (a.icao) std::snprintf(b, sizeof b, "\"key\":\"%06X\",\"icao\":\"%06X\",", a.icao, a.icao);
            else std::snprintf(b, sizeof b, "\"key\":\"%s\",", jsonEsc(!a.flight.empty() ? a.flight : a.reg).c_str());
            o += b;
            if (!a.flight.empty()) o += "\"flight\":\"" + jsonEsc(a.flight) + "\",";
            if (!a.reg.empty()) o += "\"reg\":\"" + jsonEsc(a.reg) + "\",";
            if (!std::isnan(a.lat)) { std::snprintf(b, sizeof b, "\"lat\":%.4f,\"lon\":%.4f,", a.lat, a.lon); o += b; }
            std::snprintf(b, sizeof b, "\"last_seen\":%.0f,\"gs_id\":%d,\"freq_khz\":%.0f,\"msg_count\":%d}",
                          nowUnix - (now - a.hfdlAt), a.gsId, a.hfdlKhz, a.count);
            o += b;
        }
        return o + "]";
    }
    /** ★ /addon/hfdl/groundstations — every station on the public list with its position; `frequencies` are the ones
     *  THIS receiver has heard it on (never a list of our own), `last_heard` (unix s) when heard (no dBFS level: see above). */
    std::string hfdlGroundStationsJson(double now, double nowUnix) {
        std::lock_guard<std::mutex> lk(m_);
        std::string o = "[";
        char b[200];
        bool first = true;
        for (const auto& g : groundStations()) {
            std::snprintf(b, sizeof b, "%s{\"gs_id\":%d,\"location\":\"%s\",\"lat\":%.1f,\"lon\":%.1f,\"frequencies\":[", first ? "" : ",",
                          g.id, g.name, g.lat, g.lon);
            o += b; first = false;
            auto it = gs_.find(g.id);
            if (it != gs_.end()) {
                bool f1 = true;
                for (double k : it->second.khz) { std::snprintf(b, sizeof b, "%s{\"freq_khz\":%.0f,\"enabled\":true}", f1 ? "" : ",", k); o += b; f1 = false; }
                std::snprintf(b, sizeof b, "],\"last_heard\":%.0f}", nowUnix - (now - it->second.at));
                o += b;
            } else o += "]}";
        }
        return o + "]";
    }

    /** ★★ V12 2026-11-16 THE SHARED TABLE's provider (vibe_aircraft_share.h Sharer): the rows heard since `sinceMs` (wall
     *  clock), as updates — never a message body — and the HFDL ground stations heard since. Under the table's lock, briefly. */
    void shareSince(int64_t sinceMs, std::vector<vibeavshare::Update>& out, std::vector<vibeavshare::GsHeard>& gs) {
        std::lock_guard<std::mutex> lk(m_);
        const double since = sinceMs / 1000.0;
        for (auto& a : rows_) {
            if (a.last < since || !a.srcMask) continue;
            vibeavshare::Update u;
            u.icao = a.icao; u.reg = a.reg; u.flight = a.flight; u.from = a.from; u.to = a.to; u.label = a.lastLabel; u.app = a.app; u.via = a.via;
            u.srcMask = a.srcMask;
            for (int s = 0; s < vibeavshare::kNSrc; ++s) u.srcMs[s] = (int64_t)std::llround(a.srcAt[s] * 1000);
            u.heardMs = (int64_t)std::llround(a.last * 1000);
            if (!std::isnan(a.lat)) {
                u.hasPos = true; u.lat = a.lat; u.lon = a.lon; u.posMs = (int64_t)std::llround(a.posAt * 1000);
                u.posSrc = a.posSrc == "ADS-C" ? vibeavshare::kAdsc : a.posSrc == "HFDL" ? vibeavshare::kHfdl
                         : a.via.compare(0, 4, "VDL2") == 0 ? vibeavshare::kVdl2 : a.via.compare(0, 4, "HFDL") == 0 ? vibeavshare::kHfdl : vibeavshare::kAcars;
                u.hasAlt = a.hasAlt; u.altFt = a.altFt; u.posTime = a.posTime;
            }
            u.count = (uint32_t)std::max(0, a.count - a.countShared); a.countShared = a.count;
            out.push_back(std::move(u));
            if (out.size() >= vibeavshare::kCap) break;
        }
        for (const auto& kv : gs_) if (kv.second.at >= since) {
            vibeavshare::GsHeard g; g.id = kv.first; g.ms = (int64_t)std::llround(kv.second.at * 1000); g.khz = kv.second.khz.empty() ? 0 : kv.second.khz.back();
            gs.push_back(g);
        }
    }

private:
    static void heard_(Aircraft& a, int src, double now) { a.srcMask |= uint8_t(1u << src); a.srcAt[src] = now; }
    void acars_(Aircraft& a, const vibeacars::Block& b, double now) {
        if (!b.reg.empty()) a.reg = b.reg;
        if (!b.flight.empty()) a.flight = b.flight;
        a.lastLabel = b.label;
        if (bodies_) a.lastText = b.text; else a.lastText.clear();
        // ★ the applications: only once the whole message is in (ETB blocks joined), only what vibe_acars_apps.h is sure of
        std::string whole;
        if (!reasm_.add(b, now, whole)) return;
        const vibeacarsapps::Result r = vibeacarsapps::parse(b.label, whole, b.downlink);
        if (r.kind.empty()) { if (!whole.empty()) ++nUnparsed_; return; }
        if (r.app == "cpdlc") { ++nCpdlc_; a.app = "CPDLC"; }        // ★ named only: CPDLC content is never decoded
        else if (r.app == "adsc") { ++nAdsc_; a.app = "ADS-C"; }
        else if (r.app == "oooi") { ++nOooi_; a.app = "OOOI"; }
        else if (r.app == "pos") { ++nPos_; a.app = r.kind.compare(0, 3, "FMS") == 0 ? "FMS" : "position"; }
        else a.app = "FMS";
        if (r.hasPos) {
            a.lat = r.lat; a.lon = r.lon; a.posAt = now;
            a.hasAlt = r.hasAlt; a.altFt = r.altFt;
            a.posSrc = r.app == "adsc" ? "ADS-C" : r.kind.compare(0, 3, "FMS") == 0 ? "FMS" : "ACARS";
            if (r.app == "adsc") heard_(a, vibeavshare::kAdsc, now);   // ★ the map names ADS-C beside its bearer
            a.posTime = vibeacarsapps::fixTimeText(r);
        }
        if (!r.from.empty()) { a.from = r.from; a.to = r.to; }
        if (!r.oooi.empty()) {
            a.oooi = r.oooi;
            char t[8];
            if (r.oooiSec >= 0) { std::snprintf(t, sizeof t, "%02d:%02d", r.oooiSec / 3600, r.oooiSec / 60 % 60); a.oooiTime = t; } else a.oooiTime.clear();
        }
        if (a.flight.empty() && !r.flight.empty()) a.flight = r.flight;
    }
    /** The row for this aircraft: by ICAO address when known, else by registration, else by flight. Adopts a row found by
     *  registration when the ICAO address arrives later (VDL2 then ACARS for the same aircraft, or the other way). */
    Aircraft& row_(uint32_t icao, const std::string& reg, double now, const std::string& flight = "") {
        Aircraft* hit = nullptr;
        for (auto& a : rows_) {
            if ((icao && a.icao == icao) || (!reg.empty() && a.reg == reg) || (!icao && reg.empty() && !flight.empty() && a.flight == flight)) { hit = &a; break; }
        }
        if (!hit) {
            if (rows_.size() >= 2000) rows_.pop_front();
            rows_.emplace_back(); hit = &rows_.back(); hit->first = now;
        }
        if (icao) hit->icao = icao;
        if (!reg.empty()) hit->reg = reg;
        hit->last = now; hit->count++;
        return *hit;
    }
    struct GsHeard { double at = 0; float level = 0; std::vector<double> khz; };
    void heardGs_(int id, double khz, float level, double now) {
        if (id < 0 || id > 127) return;
        GsHeard& g = gs_[id];
        g.at = now; g.level = level;
        const double k = std::round(khz);
        if (std::find(g.khz.begin(), g.khz.end(), k) == g.khz.end()) {
            if (g.khz.size() >= 16) g.khz.erase(g.khz.begin());   // ★ bounded: a station has at most a dozen
            g.khz.push_back(k);
        }
    }
    std::mutex m_;
    std::deque<Aircraft> rows_;
    std::map<int, GsHeard> gs_;   // ★ ≤ 128 ids
    bool bodies_;
    uint64_t msgs_ = 0, other_ = 0;
    /** ★★ beta96 when each message of the last minute arrived (msgRate), and when listening began (−1 = not yet) */
    std::deque<double> rateWin_;
    double start_ = -1;
    void heardAt_(double now) {
        if (start_ < 0) start_ = now;
        rateWin_.push_back(now);
        if (rateWin_.size() > 20000) rateWin_.pop_front();
    }
    uint64_t nPos_ = 0, nOooi_ = 0, nAdsc_ = 0, nCpdlc_ = 0, nUnparsed_ = 0;   ///< ★ unparsed = had text, no layout we are sure of
    vibeacarsapps::Reassembler reasm_;   // ★ bounded (32 messages × 8 blocks)
};

/** The whole-radio decoder (ACARS or VDL2) on its own thread. `kind` "acars" | "vdl2" | "acars+vdl2".
 *  ★★ V12 2026-11-24 "acars+vdl2" (the combined band, vibe_avi_mode.h): BOTH decoders on one wide capture, each on its
 *  OWN thread (a LANE: its own queue, its own channeliser), the channels split by isAcarsChannel. VDL2's channels then
 *  spread over its own workers (vibe_avi_workers.h) as before — D8PSK per channel is the heavy half; ACARS' four MSK
 *  channels sit on their lane's thread. A block is copied ONCE and shared by the lanes (at 8 MS/s half a second is 32 MB
 *  of samples; two copies would be 64). Each lane drops its own oldest past half a second, and counts it. */
class Service {
public:
    explicit Service(Tracker& t) : tr_(t) {}
    ~Service() { stop(); }
    /** ★ Which decoder a channel belongs to in the combined band: ACARS lives at 129–132 MHz, VDL2 at 136–137 MHz. */
    static bool isAcarsChannel(double hz) { return hz < 134.0e6; }
    void start(const std::string& kind, double hwCentreHz, double rateHz, const std::vector<double>& chans) {
        stop();
        kind_ = kind; rate_ = rateHz;
        std::vector<double> ac, vd;
        if (kind == "acars") ac = chans;
        else if (kind == "vdl2") vd = chans;
        else for (double h : chans) (isAcarsChannel(h) ? ac : vd).push_back(h);
        if (!vd.empty()) { lanes_.emplace_back(new Lane); lanes_.back()->vdl2.reset(new vibevdl2::Receiver(rateHz, hwCentreHz, vd)); }
        if (!ac.empty()) { lanes_.emplace_back(new Lane); lanes_.back()->acars.reset(new vibeacars::Receiver(rateHz, hwCentreHz, ac)); }
        { std::lock_guard<std::mutex> lk(mtx_); dropped_ = 0; run_ = true; health_ = Health(); clipN_ = clipSeen_ = 0; winStart_ = 0; }
        for (size_t i = 0; i < lanes_.size(); ++i) {
            Lane* L = lanes_[i].get();
            const bool first = i == 0;   // ★ the clip share is measured once, on the first lane
            L->th = std::thread([this, L, first] { vibeDecoderThread(L->acars ? "vibe-acars" : "vibe-vdl2"); loop_(*L, first); });   // ★ decoder priority (vibe_thread.h)
        }
    }
    void stop() {
        { std::lock_guard<std::mutex> lk(mtx_); run_ = false; }
        cv_.notify_all();
        for (auto& L : lanes_) if (L->th.joinable()) L->th.join();
        std::lock_guard<std::mutex> lk(mtx_);
        lanes_.clear();
    }
    bool running() { std::lock_guard<std::mutex> lk(mtx_); return run_; }
    /** From the DSP thread: copy once and return. Over half a second queued on a lane, that lane's OLDEST block goes
     *  (and is counted). */
    void feed(const cf32* iq, size_t n) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!run_ || lanes_.empty()) return;
            auto blk = std::make_shared<const std::vector<cf32>>(iq, iq + n);
            for (auto& L : lanes_) {
                L->q.push_back(blk);
                L->queued += n;
                while (L->queued > size_t(rate_ * 0.5) && L->q.size() > 1) { L->queued -= L->q.front()->size(); L->q.pop_front(); ++dropped_; }
            }
        }
        cv_.notify_all();
    }
    uint64_t dropped() { std::lock_guard<std::mutex> lk(mtx_); return dropped_; }
    uint64_t messages() const { return msgs_.load(); }
    const std::string& kind() const { return kind_; }
    /** How many decoders run (1, or 2 for "acars+vdl2") — each on its own thread. */
    size_t lanes() { std::lock_guard<std::mutex> lk(mtx_); return lanes_.size(); }
    /** ★ The gain rule's input (ISM's / AIS's vibeism::GainRule): the share of samples near full scale over the last
     *  second, measured on the service thread (every 4th sample), never the DSP's. `valid` once a second has passed. */
    struct Health { double clipFrac = 0; uint64_t msgs = 0; bool valid = false; };
    Health health() { std::lock_guard<std::mutex> lk(mtx_); return health_; }

private:
    struct Lane {
        std::unique_ptr<vibeacars::Receiver> acars;
        std::unique_ptr<vibevdl2::Receiver> vdl2;
        std::deque<std::shared_ptr<const std::vector<cf32>>> q;
        size_t queued = 0;
        std::thread th;
    };
    static double now_() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    void loop_(Lane& L, bool measure) {
        std::unique_lock<std::mutex> lk(mtx_);
        while (run_) {
            cv_.wait_for(lk, std::chrono::milliseconds(250), [&] { return !run_ || !L.q.empty(); });
            while (run_ && !L.q.empty()) {
                std::shared_ptr<const std::vector<cf32>> b = std::move(L.q.front());
                L.q.pop_front();
                L.queued -= b->size();
                lk.unlock();
                size_t clip = 0, seen = 0;
                if (measure)
                    for (size_t i = 0; i < b->size(); i += 4, ++seen) if (std::fabs((*b)[i].real()) > 0.93f || std::fabs((*b)[i].imag()) > 0.93f) ++clip;
                const double t = now_();
                if (L.acars) L.acars->feed(b->data(), b->size(), [&](const vibeacars::Block& x) { tr_.addAcars(x, t); ++msgs_; });
                if (L.vdl2) L.vdl2->feed(b->data(), b->size(), [&](const vibevdl2::Frame& x) { tr_.addVdl2(x, t); ++msgs_; });
                b.reset();
                lk.lock();
                if (!measure) continue;
                clipN_ += clip; clipSeen_ += seen;
                if (t - winStart_ >= 1.0) {
                    if (winStart_ > 0) { health_.clipFrac = clipSeen_ ? double(clipN_) / double(clipSeen_) : 0; health_.msgs = msgs_.load(); health_.valid = true; }
                    winStart_ = t; clipN_ = clipSeen_ = 0;
                }
            }
        }
    }
    Tracker& tr_;
    std::string kind_;
    double rate_ = 0;
    std::vector<std::unique_ptr<Lane>> lanes_;
    std::mutex mtx_;
    std::condition_variable cv_;
    uint64_t dropped_ = 0, clipN_ = 0, clipSeen_ = 0;
    double winStart_ = 0;
    Health health_;
    bool run_ = false;
    std::atomic<uint64_t> msgs_{0};
};

/** ★ THE ONE TABLE of this radio process: the datalink mode (ACARS / VDL2) and every listener's HFDL land here, so an
 *  aircraft heard on VHF and on HF is one row, and /addon/hfdl/aircraft + groundstations and the mode's report read the same rows. Bounded
 *  (2000 rows, 128 stations). Bodies start OFF: the shim sets the owner's choice before anything is decoded. */
inline Tracker& sharedTracker() { static Tracker t(false); return t; }

/** ★★ V12 2026-11-16: hand this process's datalink table to the shared table (vibe_aircraft_share.h) — called when the
 *  datalink mode starts or a listener's HFDL decoder attaches. Idempotent (the provider is replaced by name). */
inline void shareTrackerProvide() {
    vibeavshare::sharer().provide("avi", [](int64_t since, std::vector<vibeavshare::Update>& out, std::vector<vibeavshare::GsHeard>& gs) {
        sharedTracker().shareSince(since, out, gs);
    });
}

}  // namespace vibeavi
