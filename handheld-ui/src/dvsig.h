// dvsig.h — V12 DV ID on the R36S: DMR, D-STAR, System Fusion, NXDN and dPMR signalling, NEVER THE VOICE — the line rules of
// src/services/dvsig.ts (the app and the web page read that file), line for line. docs/v12/DV-SIGNALLING.md.
//
// ★★★ WHAT IS SHOWN IS THE SERVER'S DECISION (UK WTA 2006 s.48): an identities copy ("ids": true) only on an amateur band
//     or to the signed-in owner who opted in; a public copy carries no ID and none is read from it here either.
// ★★ OFFERED ONLY WHERE THE SERVER LISTS IT: vibeserver.json `modes` has "dvsig" (app.h offeredModes).
// ★ The bar has one text: the VTS line and its info together (dvsigLine) — "DMR · G4ABC Stuart · CC 1 · TS2",
//   "DMR · heard · CC 1 · TS2 · IDs not shown off the amateur bands". Entering the mode asks for the report.
// ★ Not built here: the pins (AUTO is what the handheld runs) and the log list — the line shows the newest transmission.
#pragma once
#include "json.h"
#include "m17.h"   // m17Clean, m17d::num / fixed4 — the same cleaning and number rules the TS shares

#include <cmath>
#include <string>
#include <vector>

namespace hh {

inline const char* const kDvsigLabel = "DV ID";

/** The newest entry of a report and the report's flags (dvsig.ts DvState / DvEntry; only what the line uses). */
struct DvsigState {
    bool have = false;                // a report has come
    std::string pin = "auto";
    bool amateur = false, ids = false;
    bool any = false;                 // the log has an entry
    std::string sys, kind, mode, from, to, via1, alias, text;
    bool open = false, enc = false;
    bool hasSlot = false, hasCc = false, hasRan = false, hasDg = false, hasSrc = false, hasDst = false, hasGrp = false, grp = false, hasGps = false;
    int slot = 0, cc = 0, ran = 0, dg = 0;
    double src = 0, dst = 0, lat = 0, lon = 0;
};

inline bool parseDvsig(const Json& m, DvsigState& st) {
    if (m.kind != Json::Obj || m["type"].str() != "dvsig") return false;
    st = DvsigState{};
    st.have = true;
    static const char* pins[] = {"auto", "dmr", "dstar", "ysf", "nxdn", "dpmr"};
    for (const char* p : pins) if (m["pin"].str() == p) st.pin = p;
    st.amateur = m["amateur"].kind == Json::Bool && m["amateur"].b;
    st.ids = m["ids"].kind == Json::Bool && m["ids"].b;
    auto intIn = [](const Json& j, double lo, double hi, bool& has, int& v) { if (j.kind == Json::Num && std::isfinite(j.n) && j.n >= lo && j.n <= hi) { has = true; v = (int)std::floor(j.n + 0.5); } };
    for (const auto& e : m["log"].a) {
        const std::string sys = e["sys"].str();
        if (e.kind != Json::Obj || e["id"].kind != Json::Num || e["t"].kind != Json::Num) continue;
        if (sys != "DMR" && sys != "D-STAR" && sys != "YSF" && sys != "NXDN" && sys != "dPMR") continue;
        st.any = true; st.sys = sys;
        st.kind = m17Clean(e["kind"], 12); if (st.kind.empty()) st.kind = "voice";
        st.open = e["open"].kind == Json::Bool && e["open"].b;
        st.enc = e["enc"].kind == Json::Bool && e["enc"].b;
        intIn(e["slot"], 0, 2, st.hasSlot, st.slot); intIn(e["cc"], 0, 4095, st.hasCc, st.cc); intIn(e["ran"], 0, 63, st.hasRan, st.ran);
        st.mode = m17Clean(e["mode"], 24);
        if (st.ids) {
            int v = 0;
            if (e["src"].kind == Json::Num && e["src"].n >= 0 && e["src"].n <= 16777215) { st.hasSrc = true; st.src = std::floor(e["src"].n + 0.5); }
            if (e["dst"].kind == Json::Num && e["dst"].n >= 0 && e["dst"].n <= 16777215) { st.hasDst = true; st.dst = std::floor(e["dst"].n + 0.5); }
            if (e["grp"].kind == Json::Bool) { st.hasGrp = true; st.grp = e["grp"].b; }
            st.from = m17Clean(e["from"], 16); st.to = m17Clean(e["to"], 16); st.via1 = m17Clean(e["via1"], 16);
            st.alias = m17Clean(e["alias"], 32); st.text = m17Clean(e["text"], 120);
            intIn(e["dg"], 0, 127, st.hasDg, st.dg); (void)v;
            const Json& g = e["gps"];
            if (g.kind == Json::Obj && g["lat"].kind == Json::Num && g["lon"].kind == Json::Num && std::fabs(g["lat"].n) <= 90 && std::fabs(g["lon"].n) <= 180
                && !(g["lat"].n == 0 && g["lon"].n == 0)) { st.hasGps = true; st.lat = g["lat"].n; st.lon = g["lon"].n; }
        }
        break;   // ★ newest first: the line needs only the first
    }
    return true;
}
inline bool dvsigOffered(const Json& j) { for (auto& m : j["modes"].a) if (m.str() == "dvsig") return true; return false; }

namespace dvd {
inline const std::string& dot() { static const std::string d = " \xc2\xb7 "; return d; }
inline std::string join(const std::vector<std::string>& v) { std::string s; for (const auto& x : v) if (!x.empty()) s += (s.empty() ? "" : dot()) + x; return s; }
}  // namespace dvd
/** dvWhere */
inline std::string dvsigWhere(const DvsigState& e) {
    std::vector<std::string> p;
    if (e.sys == "DMR") { if (e.hasCc) p.push_back("CC " + std::to_string(e.cc)); if (e.hasSlot && e.slot) p.push_back("TS" + std::to_string(e.slot)); }
    else if (e.sys == "NXDN") { if (e.hasRan) p.push_back("RAN " + std::to_string(e.ran)); }
    else if (e.sys == "dPMR") { if (e.hasCc) { char b[16]; std::snprintf(b, sizeof b, "CC 0x%X", e.cc); p.push_back(b); } }
    else if (e.sys == "YSF" && e.hasDg) p.push_back("DG-ID " + std::to_string(e.dg));
    if ((e.sys == "YSF" || e.sys == "NXDN") && !e.mode.empty()) p.push_back(e.mode);
    return dvd::join(p);
}
/** dvWho */
inline std::string dvsigWho(const DvsigState& e) {
    static const std::string arrow = " \xe2\x86\x92 ";
    if (e.sys == "DMR" || e.sys == "NXDN" || e.sys == "dPMR") {
        if (!e.hasSrc && !e.hasDst) return "";
        std::string to;
        if (e.hasDst) to = (e.hasGrp && !e.grp) || e.sys == "dPMR" ? m17d::num(e.dst) : "TG " + m17d::num(e.dst);
        std::string s = e.hasSrc ? m17d::num(e.src) : std::string("\xe2\x80\xa6");
        if (!to.empty()) s += arrow + to;
        return s;
    }
    if (e.from.empty() && e.to.empty()) return "";
    std::string via;
    if (!e.via1.empty()) { std::string v; bool sp = false; for (char c : e.via1) { if (c == ' ') { sp = true; continue; } if (sp && !v.empty()) v += ' '; sp = false; v += c; } via = " via " + v; }
    return (e.from.empty() ? std::string("\xe2\x80\xa6") : e.from) + (e.to.empty() ? "" : arrow + e.to) + via;
}
inline std::string dvsigPosition(double lat, double lon) {
    return m17d::fixed4(std::fabs(lat)) + "\xc2\xb0 " + (lat >= 0 ? "N" : "S") + " " + m17d::fixed4(std::fabs(lon)) + "\xc2\xb0 " + (lon >= 0 ? "E" : "W");
}
/** dvsigVts. `st` null = no report yet. */
inline std::string dvsigVts(const DvsigState* st) {
    if (!st || !st->any) {
        if (st && st->pin != "auto") {
            const std::string l = st->pin == "dmr" ? "DMR" : st->pin == "dstar" ? "D-STAR" : st->pin == "ysf" ? "YSF" : st->pin == "nxdn" ? "NXDN" : "dPMR";
            return l + dvd::dot() + "searching";
        }
        return "DV" + dvd::dot() + "searching";
    }
    const std::string who = dvsigWho(*st), name = !st->alias.empty() ? st->alias : who;
    if (st->open) return dvd::join({st->sys, name.empty() ? std::string("heard") : name, name.empty() ? dvsigWhere(*st) : std::string()}) + (st->enc ? dvd::dot() + "encrypted" : "");
    return "DV" + dvd::dot() + "last " + st->sys + (name.empty() ? "" : " " + name);
}
/** dvsigInfo */
inline std::string dvsigInfo(const DvsigState* st) {
    if (!st || !st->any) return st && !st->amateur && !st->ids ? "Not an amateur frequency: who is talking is not shown" : "";
    std::vector<std::string> p;
    p.push_back(dvsigWhere(*st));
    if (!st->alias.empty() && !dvsigWho(*st).empty()) p.push_back(dvsigWho(*st));
    p.push_back(st->text);
    if (st->hasGps) p.push_back(dvsigPosition(st->lat, st->lon));
    if (!st->ids) p.push_back("IDs not shown off the amateur bands");
    return dvd::join(p);
}
inline std::string dvsigLine(const DvsigState* st) {
    const std::string a = dvsigVts(st), b = dvsigInfo(st);
    return b.empty() ? a : a + dvd::dot() + b;
}
/** The passband to draw (Hz from the dial): the 12.5 kHz channel the server keeps (dvsig.ts DVSIG_PASSBAND). */
inline void dvsigPassband(int& lo, int& hi) { lo = -6250; hi = 6250; }
inline std::string dvsigStatusMessage() { return "{\"type\":\"dvsig_status\"}"; }

}  // namespace hh
