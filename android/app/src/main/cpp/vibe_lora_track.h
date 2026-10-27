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

class MeshTable {
public:
    static constexpr size_t kMaxRows = 400, kMaxMsgs = 60;

    /** Take one decoded frame at time `t`. Returns true when the list changed; `ev` gets the gallery's line for news. */
    bool accept(const Msg& m, double t, std::vector<Event>* ev = nullptr) {
        ++frames_[m.proto];
        std::string key;
        if (m.proto == "meshtastic") key = "mt:" + m.node;
        else if (m.proto == "meshcore") key = m.what == "advert" ? "mc:" + m.node : "mc:traffic";
        else if (m.proto == "lorawan") key = "lw:" + (m.node.empty() ? std::string("joinaccept") : m.node);
        else { char b[64]; std::snprintf(b, sizeof b, "lora:%.0f:%d:%02X", m.freqHz / 1e4, m.sf, m.sync); key = b; }
        Row& r = rows_[key];
        const bool fresh = r.count == 0;
        if (fresh) { r.id = ++nextId_; r.first = t; r.proto = m.proto; r.node = m.node; }
        ++r.count;
        r.last = t; r.freq = m.freqHz; r.snr = m.snrDb; r.level = m.levelDb; r.sf = m.sf; r.bw = m.bw; r.cr = m.cr; r.sync = m.sync;
        if (!m.longName.empty()) r.longName = m.longName;
        if (!m.shortName.empty()) r.shortName = m.shortName;
        if (!m.hw.empty()) r.hw = m.hw;
        if (!m.role.empty()) r.role = m.role;
        if (m.lat == m.lat) { r.lat = m.lat; r.lon = m.lon; r.posT = t; if (m.altM == m.altM) r.alt = m.altM; r.prec = m.precisionBits; }
        if (m.batt >= 0) r.batt = m.batt;
        if (m.volt == m.volt) r.volt = m.volt;
        if (m.tempC == m.tempC) r.temp = m.tempC;
        if (m.hum == m.hum) r.hum = m.hum;
        if (m.hpa == m.hpa) r.hpa = m.hpa;
        if (m.chUtil == m.chUtil) r.chUtil = m.chUtil;
        if (m.hopStart >= 0 && m.hopLimit >= 0 && m.hopStart >= m.hopLimit) r.hops = m.hopStart - m.hopLimit;
        if (m.pathLen >= 0) r.hops = m.pathLen;
        if (!m.channel.empty()) r.channel = m.channel;
        if (m.privateCh) ++r.priv; else ++r.open;
        r.lastWhat = m.what;
        if (m.proto == "lorawan") { r.mtype = m.mtype; r.fcnt = m.fcnt; r.fport = m.fport; r.encLen = m.encLen; r.net = m.net; r.lineLw = lorawan::line(m); }
        if (m.proto == "lora") r.lineLora = "LoRa" + mkDot() + "SF" + std::to_string(m.sf) + mkDot() + f1(m.bw / 1e3, m.bw < 1e5 ? 1 : 0) + " kHz"
                                           + mkDot() + "sync 0x" + hex2(m.sync) + mkDot() + std::to_string(m.len) + " bytes";
        bool news = fresh;
        if (!m.text.empty()) {
            Text x; x.t = t; x.net = m.proto; x.node = m.node; x.text = m.text; x.channel = m.channel;
            x.from = m.proto == "meshcore" ? m.from : (r.longName.empty() ? m.node : r.longName);
            x.freq = m.freqHz;
            msgs_.push_back(x);
            while (msgs_.size() > kMaxMsgs) msgs_.pop_front();
            r.msgs.push_back(x);
            while (r.msgs.size() > 5) r.msgs.pop_front();
            ++texts_;
            news = true;
            if (ev) ev->push_back({m.proto, x.from.empty() ? std::string(m.proto == "meshcore" ? "MeshCore" : "Meshtastic") : x.from,
                                   (m.proto == "meshcore" ? std::string("MeshCore") : std::string("Meshtastic")) + mkDot() + (x.channel.empty() ? "" : x.channel + mkDot())
                                   + (x.from.empty() ? "" : x.from + ": ") + x.text, m.freqHz});
        }
        if (m.what == "position" || m.what == "advert" || m.what == "nodeinfo") news = news || fresh;
        r.line = lineOf(r);
        if (fresh && ev && m.text.empty()) ev->push_back({m.proto, nameOf(r), r.line, m.freqHz});
        prune(t);
        return true;
    }
    void prune(double t) {
        for (auto it = rows_.begin(); it != rows_.end();) {
            // ★ mesh nodes beacon every 15 min to 3 h: kept 6 h. LoRaWAN devices and stray frames: 2 h.
            const double keep = (it->second.proto == "meshtastic" || it->second.proto == "meshcore") ? 6 * 3600.0 : 2 * 3600.0;
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
               + ",\"text\":\"" + jesc(it->text) + "\"}";
        }
        return o + "]";
    }

private:
    struct Text { double t = 0; std::string net, node, from, text, channel; double freq = 0; };
    struct Row {
        uint64_t id = 0; std::string proto, node;
        double first = 0, last = 0, freq = 0; float snr = 0, level = -99; int sf = 0, cr = 0; double bw = 0; uint8_t sync = 0;
        int count = 0, priv = 0, open = 0;
        std::string longName, shortName, hw, role, channel, lastWhat, mtype, net, lineLw, lineLora, line;
        double lat = NAN, lon = NAN, alt = NAN, posT = -1; int prec = 0;
        int batt = -1; double volt = NAN, temp = NAN, hum = NAN, hpa = NAN, chUtil = NAN;
        int hops = -1, fcnt = -1, fport = -1, encLen = -1;
        std::deque<Text> msgs;
    };
    static std::string hex2(uint8_t v) { char b[4]; std::snprintf(b, sizeof b, "%02X", v); return b; }
    static const char* makeOf(const std::string& p) {
        return p == "meshtastic" ? "Meshtastic" : p == "meshcore" ? "MeshCore" : p == "lorawan" ? "LoRaWAN" : "LoRa";
    }
    static std::string nameOf(const Row& r) {
        if (r.proto == "meshcore" && r.node.empty()) return "MeshCore traffic";
        if (r.proto == "lora") return "LoRa SF" + std::to_string(r.sf);
        if (r.proto == "lorawan") return r.node.empty() ? "LoRaWAN" : (r.net == "The Things Network" ? "TTN " : "LoRaWAN ") + r.node;
        std::string n = r.longName.empty() ? r.node : r.longName;
        if (!r.shortName.empty() && r.shortName != r.longName) n += " (" + r.shortName + ")";
        return n;
    }
    static std::string lineOf(const Row& r) {
        const std::string d = mkDot();
        if (r.proto == "lorawan") return r.lineLw + d + "SF" + std::to_string(r.sf);
        if (r.proto == "lora") return r.lineLora + (r.count > 1 ? d + std::to_string(r.count) + " frames" : "");
        if (r.proto == "meshcore" && r.node.empty())
            return "MeshCore" + d + "traffic" + d + std::to_string(r.count) + (r.count == 1 ? " packet" : " packets")
                 + (r.priv ? d + std::to_string(r.priv) + " encrypted" : "") + d + "last: " + r.lastWhat;
        std::string s = std::string(makeOf(r.proto)) + d + nameOf(r);
        if (!r.hw.empty()) s += d + r.hw;
        if (!r.role.empty() && r.role != "client") s += d + r.role;
        if (r.lat == r.lat) s += d + f1(r.lat, 4) + ", " + f1(r.lon, 4);
        if (r.batt > 100) s += d + "powered";
        else if (r.batt >= 0) s += d + "battery " + std::to_string(r.batt) + " %";
        if (r.temp == r.temp) s += d + f1(r.temp, 1) + " \xc2\xb0" "C";
        if (r.hops >= 0) s += d + (r.hops == 0 ? std::string("direct") : std::to_string(r.hops) + (r.hops == 1 ? " hop" : " hops"));
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
        str("chan", r.channel); str("net", r.net); str("mtype", r.mtype); str("what", r.lastWhat);
        if (r.lat == r.lat) { num("lat", r.lat, 6); num("lon", r.lon, 6); num("posAge", std::max(0.0, t - r.posT), 0); if (r.prec) num("prec", r.prec, 0); }
        num("alt", r.alt, 0);
        if (r.batt >= 0) num("battPct", r.batt, 0);
        num("volt", r.volt, 2); num("temp", r.temp, 1); num("hum", r.hum, 0); num("baro", r.hpa, 1); num("chUtil", r.chUtil, 1);
        if (r.hops >= 0) num("hops", r.hops, 0);
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
                o += "{\"ago\":" + f1(std::max(0.0, t - x.t), 0) + ",\"from\":\"" + jesc(x.from) + "\",\"text\":\"" + jesc(x.text) + "\"}";
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
