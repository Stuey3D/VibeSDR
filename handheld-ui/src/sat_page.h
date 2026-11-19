// sat_page.h — V12 SATELLITES on the R36S (docs/v12/SATELLITE-ENGINE.md): the web's satellites panel
// (web/client/src/satellites.ts) and the app's SatellitePanel, natively — the picker (where each bird is, its next pass,
// its downlinks), the lock's readout, the DOPPLER key — plus the one-line readout over the waterfall while a lock runs.
//
// ★★★ THE SAME MESSAGES AS THE WEB, ON THE SPECTRUM SOCKET: {"type":"sat_list"} (the picker's question — free),
//     {"type":"sat","op":"lock"|"unlock",norad?} (the lock). The server answers `sat_list` and `sat` (twice a second
//     while locked; OFF with `why` when it ends); engine.h keeps the last of each (RadioState::msg / seq). The server owns
//     the lock (the shared-dial contract): the dial it snaps to arrives in the config like any other.
// ★★ OFFERED ONLY WHERE THE SERVER SAYS: vibeserver.json `doppler` (read with the map modes' caps, map_modes.h) — absent
//    = no SAT key at all (the radio reaches no listed downlink: no dead control); `dopplerWhy` = shown, DOPPLER greyed.
// ★ The words are src/services/satellites.ts's, ported (satChanText / satStateWord / clockIn): "ISS · ▼ 3.21 kHz",
//   TRACK / COAST / ARMED / NO TIME / STALE. No flashing: COAST dims.
// ★ BOUNDED: 20 rows; the list re-asked every 30 s while the page is open.
#pragma once
#include "map_modes.h"

#include <cmath>
#include <ctime>
#include <string>
#include <vector>

namespace hh {

struct SatUi {
    Scroller scroll;
    double askedAt = -1e9;
    double lockAt = 0; long long lockNorad = 0;   // ★ TRACK: the lock goes 400 ms after the tune, at the new dial
};
inline SatUi& satUi() { static SatUi x; return x; }

/** vibeserver.json `doppler` → -1 absent · 0 greyed · 1 live (map_modes.h mapFetchCaps fills MapModes::satCap). */
inline int satCap(std::string* why = nullptr) {
    MapModes& x = mapModes();
    std::lock_guard<std::mutex> lk(x.capMtx);
    if (why) *why = x.satWhy;
    return x.satCap;
}

/** satellites.ts satChanText: "ISS · ▼ 3.21 kHz" ("" when not locked); ARMED: "SO-50 · AOS 3:45". */
inline std::string satClockIn(double t, double now) {
    if (t <= 0) return "";
    long d = std::lround(t - now);
    if (d <= 0) return "now";
    char b[24];
    if (d >= 3600) std::snprintf(b, sizeof b, "%ld:%02ld:%02ld", d / 3600, (d % 3600) / 60, d % 60);
    else std::snprintf(b, sizeof b, "%ld:%02ld", d / 60, d % 60);
    return b;
}
inline std::string satChanText(const Json& r) {
    const std::string st = r["state"].str(), name = r["name"].str().substr(0, 40);
    if (st.empty() || st == "OFF" || name.empty()) return "";
    if (st == "ARMED") return name + " · AOS " + satClockIn(r["aos"].num(0), (double)std::time(nullptr));
    const double off = r["offset"].num(0);
    char b[48]; std::snprintf(b, sizeof b, " · %s %.2f kHz", off >= 0 ? "▲" : "▼", std::fabs(off) / 1000.0);
    return name + b;
}
/** satellites.ts satCreditText: "Orbits: CelesTrak · 5 h old" — CelesTrak's data is theirs, say so with its age. */
inline std::string satCreditText(const Json& l, double now) {
    const Json& so = l["source"];
    const std::string orbits = so["orbits"].str();
    if (orbits.empty()) return "";
    std::string out = orbits.find("CelesTrak") != std::string::npos ? std::string("Orbits: CelesTrak") : orbits.substr(0, 60);
    const double at = so.has("fetched") ? so["fetched"].num(0) : so["newestEpoch"].num(0);
    if (at > 0) {
        const double h = std::max(0.0, (now - at) / 3600.0);
        char b[40];
        if (h < 1) std::snprintf(b, sizeof b, " · under an hour old");
        else if (h < 48) std::snprintf(b, sizeof b, " · %d h old", (int)std::lround(h));
        else std::snprintf(b, sizeof b, " · %d days old", (int)std::lround(h / 24));
        out += b;
    }
    if (so["transmitters"].str().find("SatNOGS") != std::string::npos) out += " · Transmitters: SatNOGS DB (CC BY-SA 4.0)";
    return out;
}
inline std::string satStateWord(const Json& r) {
    const std::string st = r["state"].str();
    if (st.empty() || st == "OFF") return "";
    if (r["time"].str() == "none") return "NO TIME";
    const std::string age = r["ageState"].str();
    if (age == "stale" && st != "ARMED") return "STALE";
    if (age == "old") { char b[32]; std::snprintf(b, sizeof b, "ELEMENTS %d d", (int)std::lround(r["age"].num(0))); return b; }
    if (r["time"].str() == "system" && st != "ARMED") return st + " · TIME: SYSTEM";
    return st == "ENDED" ? "DOPPLER ENDED" : st;
}
inline bool satLocked(const RadioState& s) {
    const std::string st = s.msg("sat")["state"].str();
    return !st.empty() && st != "OFF";
}

/** The mode sheet's SAT key (after the map modes). */
inline void satModeEntries(const RadioState& s, std::vector<ModeDef>& ms) {
    (void)s;
    if (satCap() >= 0) ms.push_back({"sat", "SAT"});
}
inline void satOpen(App& a) {
    SatUi& u = satUi();
    u.scroll = {};
    a.nav_.forget("sat");
    a.open(App::S_SAT);
    a.eng_->send("{\"type\":\"sat_list\"}");
    u.askedAt = nowSec();
    a.nav_.setFocus("sat", "sat.dop");
}

/** Over the waterfall while a lock runs: "ISS · ▼ 3.21 kHz · TRACK", dimmed in COAST (the web's chan slot — the deck has
 *  no room beside the digits at 640 px). */
inline void satDrawLine(Ctx& c, const RadioState& s, float x, float y) {
    const Json& r = s.msg("sat");
    const std::string t = satChanText(r);
    if (t.empty()) return;
    const std::string line = t + " · " + satStateWord(r);
    const bool coast = r["state"].str() == "COAST";
    const float w = (float)c.tc.width(c.f.small, line) + 20, h = (float)c.f.small.lineHeight() + 8;
    c.g.rrect(x, y, w, h, 6, rgb(0x000000, 0.62f));
    c.tc.draw(c.g, c.f.small, line, x + 10, y + 4, coast ? th::dim : th::white);
}

inline void App::drawSat(Ctx& c, const RadioState& s) {
    SatUi& u = satUi();
    bool closed = false;
    const Rect panel{12, 10, 616, 460};
    const Rect area = sheetPanel(c, panel, "SATELLITES", "sat.close", closed);
    if (closed) { closeTop(); return; }
    const double mono = nowSec(), now = (double)std::time(nullptr);
    if (mono - u.askedAt > 30) { eng_->send("{\"type\":\"sat_list\"}"); u.askedAt = mono; }
    if (u.lockAt > 0 && mono >= u.lockAt) {
        eng_->send("{\"type\":\"sat\",\"op\":\"lock\",\"norad\":" + std::to_string(u.lockNorad) + "}");
        u.lockAt = 0;
    }
    std::string why;
    const int cap = satCap(&why);
    const Json& ro = s.msg("sat");
    const bool locked = satLocked(s);
    float y = area.y;
    // DOPPLER, and the reason when it cannot run
    {
        const Rect k{area.x, y, 160, 44};
        if (cap == 1) {
            if (toggle(c, "sat.dop", k, locked, "DOPPLER", "DOPPLER"))
                eng_->send(locked ? "{\"type\":\"sat\",\"op\":\"unlock\"}" : "{\"type\":\"sat\",\"op\":\"lock\"}");
        } else {
            c.g.rrect(k.x, k.y, k.w, k.h, 7, th::btnBg);
            textIn(c, c.f.btn, "DOPPLER", k, th::dimmer);
        }
        const std::string note = cap != 1 ? why : (ro["state"].str() == "OFF" ? ro["why"].str().substr(0, 160) : std::string());
        if (!note.empty()) c.tc.draw(c.g, c.f.small, bmFit(c.f.small, note, (int)(area.w - 176)), area.x + 172, y + 12, th::amber);
        y += 52;
    }
    // the readout
    if (locked) {
        const bool coast = ro["state"].str() == "COAST";
        c.tc.draw(c.g, c.f.bodyB, satChanText(ro), area.x + 2, y, coast ? th::dim : th::white);
        y += c.f.bodyB.lineHeight() + 2;
        std::string l2 = satStateWord(ro);
        if (!ro["tx"].str().empty()) l2 += " · " + ro["tx"].str().substr(0, 30);
        if (ro["state"].str() != "ARMED") {
            char b[64]; std::snprintf(b, sizeof b, " · AZ %d° EL %d° · LOS %s", (int)std::lround(ro["az"].num(0)),
                                      (int)std::lround(ro["el"].num(0)), satClockIn(ro["los"].num(0), now).c_str());
            l2 += b;
        }
        c.tc.draw(c.g, c.f.small, bmFit(c.f.small, l2, (int)area.w - 4), area.x + 2, y, th::dim);
        y += c.f.small.lineHeight() + 8;
    }
    // the list: up first (highest), then by next AOS; only birds with a downlink this radio hears
    struct Row { const Json* sat; bool up; double el, aos, maxEl; const Json* tx; };
    std::vector<Row> rows;
    const Json& l = s.msg("sat_list");
    for (const auto& sat : l["sats"].a) {
        const Json* tx = nullptr;
        for (const auto& t : sat["tx"].a) if (t["reach"].boolean()) { tx = &t; break; }
        if (!tx) continue;
        double aos = 1e300, maxEl = 0;
        for (const auto& p : sat["passes"].a) if (p["los"].num(0) > now) { aos = p["aos"].num(0); maxEl = p["maxEl"].num(0); break; }
        const double el = sat.has("el") ? sat["el"].num(-90) : -90;
        rows.push_back({&sat, el >= 0, el, aos, maxEl, tx});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.up != b.up) return a.up;
        return a.up ? a.el > b.el : a.aos < b.aos;
    });
    if (rows.size() > 20) rows.resize(20);
    // ★ the credit line at the foot of the panel (CelesTrak's orbits, SatNOGS where used, the data's age)
    const std::string credit = satCreditText(l, now);
    const float creditH = credit.empty() ? 0.f : (float)c.f.small.lineHeight() + 6;
    if (!credit.empty()) c.tc.draw(c.g, c.f.small, bmFit(c.f.small, credit, (int)area.w - 4), area.x + 2, panel.b() - 6 - creditH + 4, th::dim);
    const Rect view{area.x - 8, y, area.w + 16, panel.b() - 8 - y - creditH};
    if (l.kind == Json::Null) c.tc.draw(c.g, c.f.small, "Asking the server…", area.x + 2, y, th::dim);
    else if (rows.empty()) c.tc.draw(c.g, c.f.small, "No listed satellite can be heard on this receiver", area.x + 2, y, th::dim);
    c.g.clip(view.x, view.y, view.w, view.h);
    const float rowH = 50, gap = 4;
    float ly = view.y - u.scroll.y;
    for (size_t i = 0; i < rows.size(); i++, ly += rowH + gap) {
        const Rect row{area.x, ly, area.w, rowH};
        const Nav::Hit h = c.nav.item("sat.r." + std::to_string(i), row);
        if (ly + rowH > view.y && ly < view.b()) {
            c.g.rrect(row.x, row.y, row.w, row.h, 7, h.focused ? th::activeBg : th::btnBg);
            c.g.rframe(row.x, row.y, row.w, row.h, 7, 1, h.focused ? th::activeBdr : th::btnBorder);
            const Json& sat = *rows[i].sat;
            c.tc.draw(c.g, c.f.body, bmFit(c.f.body, sat["name"].str(), 260), row.x + 10, row.y + 4, th::white);
            char when[80];
            if (rows[i].up) std::snprintf(when, sizeof when, "UP · EL %d°", (int)std::lround(rows[i].el));
            else if (rows[i].aos < 1e299) std::snprintf(when, sizeof when, "AOS in %s · max %d°", satClockIn(rows[i].aos, now).c_str(), (int)std::lround(rows[i].maxEl));
            else std::snprintf(when, sizeof when, "no pass in the next day");
            c.tc.draw(c.g, c.f.small, when, row.r() - 10, row.y + 6, rows[i].up ? th::amber : th::dim, 2);
            const Json& t = *rows[i].tx;
            char tx[96];
            std::snprintf(tx, sizeof tx, "%s · %.3f MHz %s%s", t["label"].str().substr(0, 24).c_str(), t["hz"].num(0) / 1e6,
                          t["mode"].str() == "nfm" ? "FM" : t["mode"].str() == "usb" ? "USB" : t["mode"].str() == "cw" ? "CW" : "",
                          t["status"].str() == "inactive" ? " · INACTIVE" : "");
            c.tc.draw(c.g, c.f.small, tx, row.x + 10, row.y + 28, th::dim);
            if (h.focused) focusRing(c, row, 7);
        }
        if (h.pressed && cap == 1) {
            // ★ TRACK: the first downlink this radio hears — tuned as a bookmark is (mode, band step), then the lock
            const Json& t = *rows[i].tx;
            const double hz = t["hiHz"].num(0) > t["hz"].num(0) ? std::round((t["hz"].num(0) + t["hiHz"].num(0)) / 2) : t["hz"].num(0);
            const std::string m = t["mode"].str();
            const std::string mode = (m == "usb" || m == "lsb" || m == "cw") ? (m == "cw" ? std::string("cwu") : m) : std::string("nfm");
            c.g.unclip();
            bmTuneTo(*this, hz, mode, false, 0, 0);
            u.lockAt = mono + 0.4; u.lockNorad = (long long)(*rows[i].sat)["norad"].num(0);
            return;
        }
    }
    c.g.unclip();
    u.scroll.contentH = (float)rows.size() * (rowH + gap);
    u.scroll.follow(c.nav.focusedRect(), view);
    if (nav_.focus("sat").empty()) nav_.setFocus("sat", "sat.dop");
}

}  // namespace hh
