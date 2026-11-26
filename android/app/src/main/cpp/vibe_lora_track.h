// vibe_lora_track.h — the LoRa rows of ISM mode's device list: one row per Meshtastic node, MeshCore node, LoRaWAN
// device; MeshCore's header-only traffic folded into one row; unrecognised LoRa folded by its radio settings; and the
// list of PUBLIC messages (Meshtastic's default channel, MeshCore's Public channel). docs/v12/LORA-MESH.md §Display.
//
// ★ A node's row builds up from several packets: its name and hardware from one (NODEINFO / ADVERT), its position from
//   another, its battery from a third. The row keeps the latest of each and when it was heard.
// ★★★ PRIVACY: only what the parsers already decided may be shown (vibe_lora_msg.h) reaches here. A packet from a node
//     on a private channel still says the node exists (its number is in the clear header) — no more.
#pragma once
#include "vibe_lora_service.h"
#include <map>

namespace vibelora {

inline std::string jesc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (uint8_t(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}
inline std::string f1(double v, int dp) { char b[32]; std::snprintf(b, sizeof b, "%.*f", dp, v); return b; }

/** What the gallery is handed for one new event (the ISM Heard's fields). */
struct Event { std::string kind, name, line; double freqHz = 0; };

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
// ★★★ PRIVACY — WHERE A NODE IS PLOTTED (Stuart, 2026-11-01, the ISM map; docs/v12/LORA-MESH.md §Privacy).
//   · REPEATERS, ROOM SERVERS and fixed SENSORS (MeshCore), ROUTERS / REPEATERS / SENSORS (Meshtastic) are infrastructure
//     their owners chose to advertise: their position is published as advertised.
//   · EVERY OTHER NODE IS A PERSON — a companion / chat node, a handheld, a tracker, a "client base" at someone's home, or
//     a node whose role we have not heard yet. Its position leaves this table ROUNDED TO 2 DECIMAL PLACES (~1 km) and
//     marked approximate ("approx":1) — in the JSON, in the list's line, to the gallery. The exact figure stays in RAM here
//     (a role heard later can still promote a router) and never goes further.
//   · No message text is ever part of a position (NodeFix has no field one could travel in).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/* ★ V12 2026-11-16 LoRa-APRS, FANET and MeshCom (vibe_lora_ham.h):
 *   · LoRa-APRS and MeshCom are AMATEUR radio: callsigns and positions are public by design — exact, as the APRS decoder
 *     shows them. Digipeaters / iGates (APRS symbols # and &) draw as repeaters, weather stations (_) as sensors.
 *   · FANET: an aircraft or a person on the ground is a PERSON. Exact only when the pilot's own "online tracking" bit is
 *     set (`consent` below — their choice to be tracked publicly); otherwise ~1 km and approximate, as a companion node.
 *     A FANET ground station (weather, internet gateway) is infrastructure. */
inline bool amateur(const std::string& proto) { return proto == "loraaprs" || proto == "meshcom"; }
/** The node's type as the map draws it: "repeater" | "room" | "sensor" | "companion" | "aircraft". */
inline const char* nodeType(const std::string& proto, const std::string& role, const std::string& sym = "") {
    if (amateur(proto)) {
        const char c = sym.size() == 2 ? sym[1] : 0;
        if (c == '#' || c == '&') return "repeater";
        if (c == '_') return "sensor";
        return "companion";
    }
    if (proto == "fanet") {
        if (role == "weather station") return "sensor";
        if (role == "internet gateway" || role == "ground station") return "repeater";
        static const char* air[] = {"aircraft", "paraglider", "hang glider", "balloon", "glider", "powered aircraft", "helicopter", "drone"};
        for (const char* a : air) if (role == a) return "aircraft";
        return "companion";
    }
    if (proto == "meshcore") {
        if (role == "repeater") return "repeater";
        if (role == "room server") return "room";
        if (role == "sensor") return "sensor";
        return "companion";
    }
    if (proto == "meshtastic") {
        if (role == "router" || role == "router-client" || role == "repeater" || role == "router (late)") return "repeater";
        if (role == "sensor") return "sensor";
        return "companion";
    }
    return "companion";
}
/** True when the node's position is published exactly (see the rule above). */
inline bool infrastructure(const std::string& proto, const std::string& role, const std::string& sym = "") {
    const std::string t = nodeType(proto, role, sym);
    return t == "repeater" || t == "room" || t == "sensor";
}
struct PubPos { double lat = NAN, lon = NAN; bool approx = false; };
/** The position as it may leave the node table. */
inline PubPos publishedPos(const std::string& proto, const std::string& role, double lat, double lon, bool consent = false,
                           const std::string& sym = "") {
    PubPos p;
    if (!(lat == lat) || !(lon == lon)) return p;
    if (amateur(proto) || (proto == "fanet" && consent) || infrastructure(proto, role, sym)) { p.lat = lat; p.lon = lon; return p; }
    p.lat = std::round(lat * 100.0) / 100.0; p.lon = std::round(lon * 100.0) / 100.0; p.approx = true;
    if (p.lat == 0) p.lat = 0; if (p.lon == 0) p.lon = 0;   // ★ never "-0.00"
    return p;
}
/** ★ One packet from a mesh node with a position — what the gallery's "stations received" card keeps
 *  (vibe_ism_sink.h NodeFix carries it). The position is already the PUBLISHED one. */
struct Fix {
    std::string net, type, name, node; double lat = NAN, lon = NAN; bool approx = false; int hops = -1; float snr = 0; double freqHz = 0;
    std::string relays;   ///< ★ beta96 MeshCore: this packet's relays, first relay first (Msg::relays); empty = none / first-hand
};

class MeshTable {
public:
    static constexpr size_t kMaxRows = 400, kMaxMsgs = 60;
    static constexpr double kMeshKeepSec = 24 * 3600.0;

    /** Take one decoded frame at time `t`. Returns true when the list changed; `ev` gets the gallery's line for news. */
    bool accept(const Msg& m, double t, std::vector<Event>* ev = nullptr, std::vector<Fix>* fixes = nullptr) {
        ++frames_[m.proto];
        std::string key;
        if (m.proto == "meshtastic") key = "mt:" + m.node;
        else if (m.proto == "meshcore") key = m.what == "advert" ? "mc:" + m.node : "mc:traffic";
        else if (m.proto == "lorawan") key = "lw:" + (m.node.empty() ? std::string("joinaccept") : m.node);
        else if (m.proto == "loraaprs") key = "la:" + m.node;
        else if (m.proto == "fanet") key = "fn:" + m.node;
        else if (m.proto == "meshcom") key = "mx:" + m.node;
        else { char b[64]; std::snprintf(b, sizeof b, "lora:%.0f:%d:%02X", m.freqHz / 1e4, m.sf, m.sync); key = b; }
        Row& r = rows_[key];
        const bool fresh = r.count == 0;
        if (fresh) { r.id = ++nextId_; r.first = t; r.proto = m.proto; r.node = m.node; }
        ++r.count;
        r.last = t; r.freq = m.freqHz; r.snr = m.snrDb; r.level = m.levelDb; r.sf = m.sf; r.bw = m.bw; r.cr = m.cr; r.sync = m.sync;
        if (!m.longName.empty()) r.longName = m.longName;
        if (!m.shortName.empty()) r.shortName = m.shortName;
        if (!m.hw.empty()) r.hw = m.hw;
        if (!m.role.empty() && !(m.proto == "fanet" && m.what == "name")) r.role = m.role;
        // ★ V12 LoRa-APRS / FANET / MeshCom: movement, the APRS symbol and line, the pilot's tracking choice, a station's wind
        if (m.speedKmh == m.speedKmh) r.speed = m.speedKmh;
        if (m.course == m.course) r.course = m.course;
        if (m.climb == m.climb) r.climb = m.climb;
        if (m.proto == "fanet" && (m.what == "tracking" || m.what == "ground")) r.online = m.online;
        if (!m.sym.empty() && m.lat == m.lat) r.sym = m.sym;
        if (!m.path.empty()) r.path = m.path;
        if (m.windKmh == m.windKmh) { r.wind = m.windKmh; r.windDir = m.windDir; r.gust = m.gustKmh; }
        if ((m.proto == "loraaprs" || m.proto == "meshcom") && m.text.empty()) r.lineHam = hamLine(m);
        if (m.lat == m.lat) { r.lat = m.lat; r.lon = m.lon; r.posT = t; if (m.altM == m.altM) r.alt = m.altM; r.prec = m.precisionBits; }
        if (m.batt >= 0) r.batt = m.batt;
        if (m.volt == m.volt) r.volt = m.volt;
        if (m.tempC == m.tempC) r.temp = m.tempC;
        if (m.hum == m.hum) r.hum = m.hum;
        if (m.hpa == m.hpa) r.hpa = m.hpa;
        if (m.chUtil == m.chUtil) r.chUtil = m.chUtil;
        if (m.hopStart >= 0 && m.hopLimit >= 0 && m.hopStart >= m.hopLimit) r.hops = m.hopStart - m.hopLimit;
        if (m.pathLen >= 0) r.hops = m.pathLen;
        // ★ beta96 the FEWEST hops this node has come in by (0 = heard first-hand: the aerial reached it) — a flood advert
        //   arrives direct AND again from each relay, the relayed copies last, so the newest hop count alone would call a
        //   node heard first-hand "via mesh". And the route of the newest flood packet (Msg::relays).
        if (r.hops >= 0 && (r.minHops < 0 || r.hops < r.minHops)) r.minHops = r.hops;
        if (m.proto == "meshcore" && m.route == "flood") r.relays = m.relays;
        if (!m.channel.empty()) r.channel = m.channel;
        r.found = m.found;   // ★ V12: on a channel found on air (the latest frame says)
        if (m.privateCh) ++r.priv; else ++r.open;
        r.lastWhat = m.what;
        if (m.proto == "lorawan") { r.mtype = m.mtype; r.fcnt = m.fcnt; r.fport = m.fport; r.encLen = m.encLen; r.net = m.net; r.lineLw = lorawan::line(m); }
        if (m.proto == "lora") r.lineLora = "LoRa" + mkDot() + "SF" + std::to_string(m.sf) + mkDot() + f1(m.bw / 1e3, m.bw < 1e5 ? 1 : 0) + " kHz"
                                           + mkDot() + "sync 0x" + hex2(m.sync) + mkDot() + std::to_string(m.len) + " bytes";
        bool news = fresh;
        /* ★ ONE MESSAGE, ONCE: a mesh floods every message, so the same text from the same sender arrives again from each
         *  relay in range (and a sender retries). The same sender + text inside 10 minutes is the same message. */
        bool dup = false;
        for (const Text& x : msgs_) if (x.net == m.proto && x.text == m.text && (x.node == m.node || (m.proto == "meshcore" && x.from == m.from)) && t - x.t < 600) dup = true;
        if (!m.text.empty() && !dup) {
            Text x; x.t = t; x.net = m.proto; x.node = m.node; x.text = m.text; x.channel = m.channel;
            x.from = (m.proto == "meshcore" || amateur(m.proto)) ? m.from : (r.longName.empty() ? m.node : r.longName);
            x.freq = m.freqHz;
            x.snr = m.snrDb; x.sf = m.sf;
            msgs_.push_back(x);
            while (msgs_.size() > kMaxMsgs) msgs_.pop_front();
            r.msgs.push_back(x);
            while (r.msgs.size() > 5) r.msgs.pop_front();
            ++texts_;
            news = true;
            // ★ the gallery keeps each public message as its own row ("meshmsg", vibe_ism_gallery.h)
            if (ev) ev->push_back({"meshmsg", x.from.empty() ? std::string(makeOf(m.proto)) : x.from,
                                   std::string(makeOf(m.proto)) + mkDot() + (x.channel.empty() ? "" : x.channel + mkDot())
                                   + (x.from.empty() ? "" : x.from + ": ") + "\xe2\x80\x9c" + x.text + "\xe2\x80\x9d", m.freqHz});
        }
        if (m.what == "position" || m.what == "advert" || m.what == "nodeinfo") news = news || fresh;
        r.line = lineOf(r);
        if (fresh && ev && m.text.empty()) ev->push_back({m.proto, nameOf(r), r.line, m.freqHz});
        // ★ every packet from a mesh node whose position we hold: the gallery's map card counts it (times heard, last heard)
        if (fixes && r.lat == r.lat && (r.proto == "meshtastic" || r.proto == "meshcore")) {
            const PubPos p = publishedPos(r.proto, r.role, r.lat, r.lon);
            Fix f; f.net = r.proto; f.type = nodeType(r.proto, r.role); f.name = r.longName.empty() ? r.node : r.longName; f.node = r.node;
            f.lat = p.lat; f.lon = p.lon; f.approx = p.approx; f.hops = r.hops; f.snr = r.snr; f.freqHz = r.freq; f.relays = r.relays;
            fixes->push_back(std::move(f));
        }
        prune(t);
        return true;
    }
    void prune(double t) {
        for (auto it = rows_.begin(); it != rows_.end();) {
            // ★ mesh nodes beacon every 15 min to 12 h (a MeshCore repeater's flood advert is 12 h by default): kept 24 h, so
            //   the map still has them between adverts. LoRaWAN devices and stray frames: 2 h. Newest wins: past kMaxRows the
            //   least recently heard row goes — the table never grows past it (256 MB boards).
            // ★ LoRa-APRS / MeshCom 6 h (the APRS decoder's keep), FANET 2 h (a flight)
            const std::string& pr = it->second.proto;
            const double keep = (pr == "meshtastic" || pr == "meshcore") ? kMeshKeepSec : amateur(pr) ? 6 * 3600.0 : 2 * 3600.0;
            if (t - it->second.last > keep) it = rows_.erase(it); else ++it;
        }
        while (rows_.size() > kMaxRows) {
            auto old = rows_.begin();
            for (auto it = rows_.begin(); it != rows_.end(); ++it) if (it->second.last < old->second.last) old = it;
            rows_.erase(old);
        }
    }
    void clear() { rows_.clear(); msgs_.clear(); frames_.clear(); texts_ = 0; }
    size_t size() const { return rows_.size(); }
    size_t nodesWithPosition() const { size_t n = 0; for (auto& kv : rows_) n += kv.second.lat == kv.second.lat; return n; }
    uint64_t texts() const { return texts_; }
    uint64_t frames(const std::string& p) const { auto it = frames_.find(p); return it == frames_.end() ? 0 : it->second; }

    /** The rows as ISM device-list objects, each with its last-heard time (for the merge with the sensors' rows). */
    std::vector<std::pair<double, std::string>> rowsJson(double t) const {
        std::vector<std::pair<double, std::string>> v;
        for (const auto& kv : rows_) v.push_back({kv.second.last, rowJson(kv.second, t)});
        return v;
    }
    /** "[{"ago":12,"net":"meshtastic","from":"Bob","node":"!a1b2c3d4","ch":"LongFast","text":"…"}, …]" newest first */
    std::string msgsJson(double t) const {
        std::string o = "[";
        for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it) {
            if (o.size() > 1) o += ',';
            o += "{\"ago\":" + f1(std::max(0.0, t - it->t), 0) + ",\"net\":\"" + jesc(it->net) + "\",\"from\":\"" + jesc(it->from)
               + "\",\"node\":\"" + jesc(it->node) + "\",\"ch\":\"" + jesc(it->channel) + "\",\"freq\":" + f1(it->freq, 0)
               + snrJson(*it) + ",\"text\":\"" + jesc(it->text) + "\"}";
        }
        return o + "]";
    }

private:
    struct Text { double t = 0; std::string net, node, from, text, channel; double freq = 0; float snr = NAN; int sf = 0; };   // ★ beta96 snr/sf: the frame's (Frame::snrDb, LoRa's in-band definition)
    /** ★ beta96 a message's SNR and spreading factor (the colour's scale is per SF): ",\"snr\":-7.5,\"sf\":9" or "". */
    static std::string snrJson(const Text& x) { return x.snr == x.snr && x.sf >= 5 ? ",\"snr\":" + f1(x.snr, 1) + ",\"sf\":" + std::to_string(x.sf) : std::string(); }
    struct Row {
        uint64_t id = 0; std::string proto, node;
        double first = 0, last = 0, freq = 0; float snr = 0, level = -99; int sf = 0, cr = 0; double bw = 0; uint8_t sync = 0;
        int count = 0, priv = 0, open = 0;
        std::string longName, shortName, hw, role, channel, lastWhat, mtype, net, lineLw, lineLora, line;
        double lat = NAN, lon = NAN, alt = NAN, posT = -1; int prec = 0;
        int batt = -1; double volt = NAN, temp = NAN, hum = NAN, hpa = NAN, chUtil = NAN;
        int hops = -1, fcnt = -1, fport = -1, encLen = -1;
        int minHops = -1;          ///< ★ beta96 the fewest hops it has been heard by (0 = first-hand)
        std::string relays;        ///< ★ beta96 MeshCore: the newest flood packet's relays ("a3 9f 1c")
        bool found = false;
        double speed = NAN, course = NAN, climb = NAN, wind = NAN, windDir = NAN, gust = NAN;
        bool online = false;
        std::string sym, path, lineHam;
        std::deque<Text> msgs;
    };
    static std::string hex2(uint8_t v) { char b[4]; std::snprintf(b, sizeof b, "%02X", v); return b; }
    static const char* makeOf(const std::string& p) {
        return p == "meshtastic" ? "Meshtastic" : p == "meshcore" ? "MeshCore" : p == "lorawan" ? "LoRaWAN"
             : p == "loraaprs" ? "LoRa APRS" : p == "fanet" ? "FANET" : p == "meshcom" ? "MeshCom" : "LoRa";
    }
    static std::string nameOf(const Row& r) {
        if (r.proto == "meshcore" && r.node.empty()) return "MeshCore traffic";
        if (r.proto == "lora") return "LoRa SF" + std::to_string(r.sf);
        if (r.proto == "lorawan") return r.node.empty() ? "LoRaWAN" : (r.net == "The Things Network" ? "TTN " : "LoRaWAN ") + r.node;
        std::string n = r.longName.empty() ? r.node : r.longName;
        if (!r.shortName.empty() && r.shortName != r.longName) n += " (" + r.shortName + ")";
        return n;
    }
    /** ★ V12 2026-11-12: a row heard on a channel found on air says so — on the list, the map and the gallery alike. */
    static std::string lineOf(const Row& r) { return lineOf0(r) + (r.found ? mkDot() + "found on air" : std::string()); }
    static std::string lineOf0(const Row& r) {
        const std::string d = mkDot();
        if (r.proto == "lorawan") return r.lineLw + d + "SF" + std::to_string(r.sf);
        if (r.proto == "lora") return r.lineLora + (r.count > 1 ? d + std::to_string(r.count) + " frames" : "");
        if (amateur(r.proto)) {
            std::string s = r.lineHam.empty() ? std::string(makeOf(r.proto)) + d + r.node : r.lineHam;
            if (!r.msgs.empty()) s += d + "\xe2\x80\x9c" + r.msgs.back().text + "\xe2\x80\x9d";
            return s;
        }
        if (r.proto == "fanet") {
            std::string s = "FANET" + d + nameOf(r);
            if (!r.role.empty()) s += d + r.role;
            if (r.alt == r.alt && nodeType(r.proto, r.role) == std::string("aircraft")) s += d + f1(r.alt, 0) + " m";
            if (r.speed == r.speed && r.speed >= 1) s += d + f1(r.speed, 0) + " km/h";
            if (r.climb == r.climb && std::fabs(r.climb) >= 0.1) s += d + (r.climb > 0 ? "+" : "") + f1(r.climb, 1) + " m/s";
            if (r.wind == r.wind) s += d + "wind " + f1(r.wind, 0) + " km/h from " + f1(r.windDir, 0) + "\xc2\xb0";
            if (r.temp == r.temp) s += d + f1(r.temp, 1) + " \xc2\xb0" "C";
            if (r.lat == r.lat) {
                const PubPos p = publishedPos(r.proto, r.role, r.lat, r.lon, r.online);
                s += d + (p.approx ? f1(p.lat, 2) + ", " + f1(p.lon, 2) + " approx." : f1(p.lat, 4) + ", " + f1(p.lon, 4));
            }
            if (!r.msgs.empty()) s += d + "\xe2\x80\x9c" + r.msgs.back().text + "\xe2\x80\x9d";
            return s;
        }
        if (r.proto == "meshcore" && r.node.empty())
            return "MeshCore" + d + "traffic" + d + std::to_string(r.count) + (r.count == 1 ? " packet" : " packets")
                 + (r.priv ? d + std::to_string(r.priv) + " encrypted" : "") + d + "last: " + r.lastWhat;
        std::string s = std::string(makeOf(r.proto)) + d + nameOf(r);
        if (!r.hw.empty()) s += d + r.hw;
        if (!r.role.empty() && r.role != "client") s += d + r.role;
        if (r.lat == r.lat) {   // ★ a person's node: ~1 km and said so (the PRIVACY rule above)
            const PubPos p = publishedPos(r.proto, r.role, r.lat, r.lon);
            s += d + (p.approx ? f1(p.lat, 2) + ", " + f1(p.lon, 2) + " approx." : f1(p.lat, 4) + ", " + f1(p.lon, 4));
        }
        if (r.batt > 100) s += d + "powered";
        else if (r.batt >= 0) s += d + "battery " + std::to_string(r.batt) + " %";
        if (r.temp == r.temp) s += d + f1(r.temp, 1) + " \xc2\xb0" "C";
        if (r.hops >= 0) s += d + (r.hops == 0 ? std::string("heard first-hand") : std::to_string(r.hops) + (r.hops == 1 ? " hop" : " hops"));
        if (!r.msgs.empty()) s += d + "\xe2\x80\x9c" + r.msgs.back().text + "\xe2\x80\x9d";
        else if (r.priv && !r.open) s += d + "private channel";
        return s;
    }
    static std::string rowJson(const Row& r, double t) {
        std::string o = "{\"key\":\"m" + std::to_string(r.id) + "\",\"kind\":\"" + r.proto + "\",\"make\":\"" + makeOf(r.proto)
                       + "\",\"model\":\"" + jesc(r.proto == "lorawan" ? r.mtype : r.hw.empty() ? r.role : r.hw)
                       + "\",\"name\":\"" + jesc(nameOf(r)) + "\",\"line\":\"" + jesc(r.line) + "\"";
        auto str = [&](const char* k, const std::string& v) { if (!v.empty()) { o += ",\""; o += k; o += "\":\"" + jesc(v) + "\""; } };
        auto num = [&](const char* k, double v, int dp) { if (v == v) { o += ",\""; o += k; o += "\":" + f1(v, dp); } };
        str("node", r.node); str("short", r.shortName); str("long", r.longName); str("hw", r.hw); str("role", r.role);
        if (r.found) o += ",\"onAir\":1";   // ★ found on air, not a standard channel
        str("chan", r.channel); str("net", r.net); str("mtype", r.mtype); str("what", r.lastWhat);
        if (r.lat == r.lat) {
            const PubPos p = publishedPos(r.proto, r.role, r.lat, r.lon, r.online, r.sym);
            num("lat", p.lat, p.approx ? 2 : 6); num("lon", p.lon, p.approx ? 2 : 6); if (p.approx) o += ",\"approx\":1";
            num("posAge", std::max(0.0, t - r.posT), 0); if (r.prec) num("prec", r.prec, 0);
        }
        if (r.proto == "meshtastic" || r.proto == "meshcore" || amateur(r.proto) || r.proto == "fanet") str("ntype", nodeType(r.proto, r.role, r.sym));
        // ★ V12 LoRa-APRS / FANET / MeshCom
        num("speed", r.speed, 0); num("course", r.course, 0); num("climb", r.climb, 1);
        num("wind", r.wind, 0); num("dir", r.windDir, 0); num("gust", r.gust, 0);
        str("sym", r.sym); str("path", r.path);
        if (r.proto == "fanet" && r.online) o += ",\"online\":1";
        num("alt", r.alt, 0);
        if (r.batt >= 0) num("battPct", r.batt, 0);
        num("volt", r.volt, 2); num("temp", r.temp, 1); num("hum", r.hum, 0); num("baro", r.hpa, 1); num("chUtil", r.chUtil, 1);
        if (r.hops >= 0) num("hops", r.hops, 0);
        if (r.minHops == 0) o += ",\"direct\":1";   // ★ beta96 heard first-hand at least once (the aerial's reach)
        if (!r.relays.empty()) str("relays", r.relays);
        if (r.fcnt >= 0) num("fcnt", r.fcnt, 0);
        if (r.fport >= 0) num("fport", r.fport, 0);
        if (r.encLen >= 0) num("encLen", r.encLen, 0);
        if (r.priv) num("priv", r.priv, 0);
        num("sf", r.sf, 0); num("bw", r.bw, 0); num("cr", r.cr, 0);
        { char b[8]; std::snprintf(b, sizeof b, "%02X", r.sync); str("sync", b); }
        if (!r.msgs.empty()) {
            o += ",\"msgs\":[";
            for (size_t i = r.msgs.size(); i-- > 0;) {
                const Text& x = r.msgs[i];
                if (i + 1 != r.msgs.size()) o += ',';
                o += "{\"ago\":" + f1(std::max(0.0, t - x.t), 0) + ",\"from\":\"" + jesc(x.from) + "\"" + snrJson(x) + ",\"text\":\"" + jesc(x.text) + "\"}";
            }
            o += "]";
        }
        char b[160];
        std::snprintf(b, sizeof b, ",\"freq\":%.0f,\"level\":%.1f,\"snr\":%.1f,\"seen\":%.0f,\"age\":%.0f,\"count\":%d}",
                      r.freq, r.level, r.snr, std::max(0.0, t - r.last), std::max(0.0, t - r.first), r.count);
        return o + b;
    }

    std::map<std::string, Row> rows_;
    std::deque<Text> msgs_;
    std::map<std::string, uint64_t> frames_;
    uint64_t nextId_ = 0, texts_ = 0;
};

} // namespace vibelora
