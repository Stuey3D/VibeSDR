#pragma once
// ★★★ THE EXTERNAL ANTENNA SWITCH — what it is, how it is found, and which antenna is connected (2026-10-09).
//
// Stuart's design (docs/v12/ANTENNA-SWITCH-ROTATOR.md §3a): "Set up an antenna switch? Yes → Search for switch… → Switch found
// on 192.168.86.77:12345 — Generic Antenna Switch Example → How many antennas connected? → Antenna 1 (Name)(Details) …", then
// an Antenna selector in the clients, a per-band antenna preset, and an admin lock. First backend: MQTT (vibe_mqtt.h) —
// Chicopee runs Lite on Android, so it has to be network-only.
//
// This header is PURE (no sockets): discovery parsing, the command list for a selection, and reading the state back. The
// shim owns the broker connection and the config; test-antswitch.cpp drives everything here with canned messages.
//
// ★★ TWO DISCOVERY FORMATS, because between them they cover most hobby relay boards:
//   • Tasmota publishes `tasmota/discovery/<MAC>/config` (retained): {"dn":device name,"t":topic,"rl":[relay types…],…}.
//     A relay n (1-based) is commanded on `cmnd/<t>/POWER<n>` and reports on `stat/<t>/POWER<n>` ("ON"/"OFF").
//   • Home-Assistant MQTT discovery publishes `homeassistant/switch/[<node>/]<object>/config` (retained), one per relay:
//     {"name","command_topic"|"cmd_t","state_topic"|"stat_t","payload_on"|"pl_on","payload_off"|"pl_off","device":{…}}.
//     Many ESP32 builds (ESPHome among them) speak it.
// ★★ BREAK BEFORE MAKE. Selecting an antenna turns every OTHER relay of the switch off first, then the chosen one on — two
//    antennas joined for a moment is exactly what a coax switch must not do. A switch with its own interlock (Tasmota
//    `Interlock`) does this as well; doing it here too costs nothing.
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vibe {
namespace antswitch {

// ── a small, strict JSON reader (discovery payloads only) ───────────────────────────────────

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false; double n = 0; std::string s;
    std::vector<Json> a; std::map<std::string, Json> o;
    const Json* get(const std::string& k) const { auto it = o.find(k); return kind == Obj && it != o.end() ? &it->second : nullptr; }
    std::string str(const std::string& k, const std::string& def = "") const {
        const Json* v = get(k); return v && v->kind == Str ? v->s : def;
    }
};
class Parser {
public:
    explicit Parser(const std::string& t) : t_(t) {}
    bool parse(Json& out) { depth_ = 0; i_ = 0; if (!value(out)) return false; ws(); return i_ == t_.size(); }
private:
    const std::string& t_; size_t i_ = 0; int depth_ = 0;
    void ws() { while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\n' || t_[i_] == '\r' || t_[i_] == '\t')) ++i_; }
    bool lit(const char* w) { size_t n = std::strlen(w); if (t_.compare(i_, n, w) != 0) return false; i_ += n; return true; }
    bool string(std::string& out) {
        if (i_ >= t_.size() || t_[i_] != '"') return false;
        ++i_;
        while (i_ < t_.size() && t_[i_] != '"') {
            char c = t_[i_++];
            if (c == '\\') {
                if (i_ >= t_.size()) return false;
                char e = t_[i_++];
                switch (e) {
                    case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                    case 'u': {                                   // BMP only, to UTF-8; enough for device names
                        if (i_ + 4 > t_.size()) return false;
                        unsigned cp = unsigned(std::stoul(t_.substr(i_, 4), nullptr, 16)); i_ += 4;
                        if (cp < 0x80) out += char(cp);
                        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                        else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: out += e;
                }
            } else out += c;
            if (out.size() > 4096) return false;
        }
        if (i_ >= t_.size()) return false;
        ++i_; return true;
    }
    bool value(Json& v) {
        if (++depth_ > 32) return false;
        ws();
        if (i_ >= t_.size()) return false;
        const char c = t_[i_];
        bool ok = false;
        if (c == '{') {
            v.kind = Json::Obj; ++i_; ws();
            if (i_ < t_.size() && t_[i_] == '}') { ++i_; ok = true; }
            else for (;;) {
                ws(); std::string k; if (!string(k)) break; ws();
                if (i_ >= t_.size() || t_[i_] != ':') break; ++i_;
                Json child; if (!value(child)) break; v.o[k] = std::move(child); ws();
                if (i_ < t_.size() && t_[i_] == ',') { ++i_; continue; }
                if (i_ < t_.size() && t_[i_] == '}') { ++i_; ok = true; }
                break;
            }
        } else if (c == '[') {
            v.kind = Json::Arr; ++i_; ws();
            if (i_ < t_.size() && t_[i_] == ']') { ++i_; ok = true; }
            else for (;;) {
                Json child; if (!value(child)) break; v.a.push_back(std::move(child)); ws();
                if (i_ < t_.size() && t_[i_] == ',') { ++i_; continue; }
                if (i_ < t_.size() && t_[i_] == ']') { ++i_; ok = true; }
                break;
            }
        } else if (c == '"') { v.kind = Json::Str; ok = string(v.s); }
        else if (lit("true")) { v.kind = Json::Bool; v.b = true; ok = true; }
        else if (lit("false")) { v.kind = Json::Bool; ok = true; }
        else if (lit("null")) { v.kind = Json::Null; ok = true; }
        else {
            size_t start = i_;
            while (i_ < t_.size() && (std::isdigit((unsigned char)t_[i_]) || t_[i_] == '-' || t_[i_] == '+' || t_[i_] == '.' || t_[i_] == 'e' || t_[i_] == 'E')) ++i_;
            if (i_ > start) { v.kind = Json::Num; try { v.n = std::stod(t_.substr(start, i_ - start)); ok = true; } catch (...) {} }
        }
        --depth_;
        return ok;
    }
};
inline bool parseJson(const std::string& text, Json& out) {
    if (text.size() > 65536) return false;
    return Parser(text).parse(out);
}

// ── what a discovered switch looks like ─────────────────────────────────────────────────────

struct Relay {
    std::string label;          // what the device calls it ("Relay 2", "Sky Loop")
    std::string cmdTopic, stateTopic;
    std::string on = "ON", off = "OFF";
};
struct Device {
    std::string id;             // stable: "tasmota:<MAC>" or "ha:<device id or node>"
    std::string name;           // what the owner sees ("Generic Antenna Switch Example")
    std::string kind;           // "tasmota" | "homeassistant"
    std::vector<Relay> relays;
};

/** Tasmota discovery: topic `tasmota/discovery/<MAC>/config`. Relays are the non-zero entries of "rl". */
inline bool parseTasmota(const std::string& topic, const std::string& payload, Device& d) {
    static const std::string pre = "tasmota/discovery/", post = "/config";
    if (topic.compare(0, pre.size(), pre) != 0 || topic.size() <= pre.size() + post.size()
        || topic.compare(topic.size() - post.size(), post.size(), post) != 0) return false;
    Json j; if (!parseJson(payload, j) || j.kind != Json::Obj) return false;
    const std::string t = j.str("t");
    if (t.empty()) return false;
    d = Device{};
    d.kind = "tasmota";
    d.id = "tasmota:" + topic.substr(pre.size(), topic.size() - pre.size() - post.size());
    d.name = j.str("dn", j.str("hn", t));
    // ★ Full-topic layout is "%prefix%/%topic%/" by default; the prefixes come in "tp" (cmnd, stat, tele).
    std::string cmnd = "cmnd", stat = "stat";
    if (const Json* tp = j.get("tp"); tp && tp->kind == Json::Arr && tp->a.size() >= 2) {
        if (tp->a[0].kind == Json::Str) cmnd = tp->a[0].s;
        if (tp->a[1].kind == Json::Str) stat = tp->a[1].s;
    }
    const Json* rl = j.get("rl");
    const Json* fn = j.get("fn");                            // friendly names per relay, when set
    if (rl && rl->kind == Json::Arr) {
        for (size_t i = 0; i < rl->a.size() && i < 32; ++i) {
            if (rl->a[i].kind != Json::Num || rl->a[i].n == 0) continue;
            Relay r;
            const std::string n = std::to_string(i + 1);
            r.label = (fn && fn->kind == Json::Arr && i < fn->a.size() && fn->a[i].kind == Json::Str && !fn->a[i].s.empty())
                          ? fn->a[i].s : "Relay " + n;
            r.cmdTopic = cmnd + "/" + t + "/POWER" + n;
            r.stateTopic = stat + "/" + t + "/POWER" + n;
            d.relays.push_back(r);
        }
    }
    return !d.relays.empty();
}

/** Home-Assistant discovery: `homeassistant/switch/[<node>/]<object>/config`, one relay per message. Returns the relay and
 *  the device it belongs to (merge by device id in the caller). */
inline bool parseHomeAssistant(const std::string& topic, const std::string& payload, Device& d) {
    static const std::string pre = "homeassistant/switch/", post = "/config";
    if (topic.compare(0, pre.size(), pre) != 0 || topic.size() <= pre.size() + post.size()
        || topic.compare(topic.size() - post.size(), post.size(), post) != 0) return false;
    Json j; if (!parseJson(payload, j) || j.kind != Json::Obj) return false;
    auto pick = [&](const char* a, const char* b, const std::string& def = "") {
        std::string v = j.str(a); return v.empty() ? j.str(b, def) : v;
    };
    std::string base = j.str("~");                           // HA's topic abbreviation: "~" expands at the start
    auto expand = [&](std::string t) { if (!base.empty() && !t.empty() && t[0] == '~') t = base + t.substr(1); return t; };
    Relay r;
    r.cmdTopic = expand(pick("command_topic", "cmd_t"));
    r.stateTopic = expand(pick("state_topic", "stat_t"));
    r.on = pick("payload_on", "pl_on", "ON");
    r.off = pick("payload_off", "pl_off", "OFF");
    r.label = j.str("name", "Relay");
    if (r.cmdTopic.empty()) return false;
    const std::string node = topic.substr(pre.size(), topic.size() - pre.size() - post.size());
    std::string devId, devName;
    const Json* dev = j.get("device"); if (!dev) dev = j.get("dev");
    if (dev && dev->kind == Json::Obj) {
        devName = dev->str("name");
        if (const Json* ids = dev->get("identifiers"); ids) {
            if (ids->kind == Json::Arr && !ids->a.empty() && ids->a[0].kind == Json::Str) devId = ids->a[0].s;
            else if (ids->kind == Json::Str) devId = ids->s;
        }
        if (devId.empty()) if (const Json* ids = dev->get("ids"); ids) {
            if (ids->kind == Json::Arr && !ids->a.empty() && ids->a[0].kind == Json::Str) devId = ids->a[0].s;
            else if (ids->kind == Json::Str) devId = ids->s;
        }
    }
    if (devId.empty()) devId = node.substr(0, node.find('/'));   // no device block: the node groups the relays
    d = Device{};
    d.kind = "homeassistant";
    d.id = "ha:" + devId;
    d.name = devName.empty() ? devId : devName;
    d.relays.push_back(r);
    return true;
}

/** The discovery filters to subscribe to while searching. */
inline std::vector<std::string> discoveryFilters() {
    return { "tasmota/discovery/+/config", "homeassistant/switch/+/config", "homeassistant/switch/+/+/config" };
}

/** Add what one discovery message says to the list (HA relays of one device merge; a re-announcement replaces). */
inline void noteDiscovery(std::vector<Device>& found, const std::string& topic, const std::string& payload) {
    Device d;
    if (parseTasmota(topic, payload, d)) {
        for (auto& f : found) if (f.id == d.id) { f = d; return; }
        found.push_back(d); return;
    }
    if (parseHomeAssistant(topic, payload, d)) {
        for (auto& f : found) if (f.id == d.id) {
            for (auto& r : f.relays) if (r.cmdTopic == d.relays[0].cmdTopic) { r = d.relays[0]; return; }
            f.relays.push_back(d.relays[0]); return;
        }
        found.push_back(d);
    }
}

// ── selecting, and reading back ─────────────────────────────────────────────────────────────

struct Publish { std::string topic, payload; };

/** The messages that connect antenna `relayIdx` (0-based into `relays`) and nothing else: every other relay OFF first,
 *  then the chosen one ON (break before make). Empty when the index is out of range. */
inline std::vector<Publish> selectCommands(const std::vector<Relay>& relays, int relayIdx) {
    std::vector<Publish> out;
    if (relayIdx < 0 || size_t(relayIdx) >= relays.size()) return out;
    for (size_t i = 0; i < relays.size(); ++i)
        if (int(i) != relayIdx) out.push_back({ relays[i].cmdTopic, relays[i].off });
    out.push_back({ relays[size_t(relayIdx)].cmdTopic, relays[size_t(relayIdx)].on });
    return out;
}

/** Tracks each relay's last reported state and says which ONE is on: its index, -1 when none is known to be on, -2 when
 *  more than one is (a switch misbehaving, or another controller — say so, never guess). */
class StateTracker {
public:
    void setRelays(const std::vector<Relay>& r) { relays_ = r; on_.assign(r.size(), -1); }
    /** Feed a message; true if it was one of ours. Tasmota's stat/<t>/RESULT {"POWER2":"ON"} is understood too. */
    bool feed(const std::string& topic, const std::string& payload) {
        bool ours = false;
        for (size_t i = 0; i < relays_.size(); ++i) {
            if (topic == relays_[i].stateTopic) {
                on_[i] = payload == relays_[i].on ? 1 : payload == relays_[i].off ? 0 : on_[i];
                ours = true;
            }
        }
        // ★ Tasmota also reports on stat/<t>/RESULT as JSON — the same relay, a different envelope.
        const std::string res = "/RESULT";
        if (!ours && topic.size() > res.size() && topic.compare(topic.size() - res.size(), res.size(), res) == 0) {
            Json j;
            if (parseJson(payload, j) && j.kind == Json::Obj) {
                const std::string prefix = topic.substr(0, topic.size() - res.size()) + "/POWER";
                for (size_t i = 0; i < relays_.size(); ++i) {
                    const std::string& st = relays_[i].stateTopic;
                    if (st.compare(0, prefix.size(), prefix) != 0) continue;
                    const std::string key = "POWER" + st.substr(prefix.size());
                    const Json* v = j.get(key);
                    if (!v && st.substr(prefix.size()) == "1") v = j.get("POWER");   // a one-relay Tasmota says "POWER"
                    if (v && v->kind == Json::Str) { on_[i] = v->s == relays_[i].on ? 1 : v->s == relays_[i].off ? 0 : on_[i]; ours = true; }
                }
            }
        }
        return ours;
    }
    int connected() const {
        int idx = -1;
        for (size_t i = 0; i < on_.size(); ++i) if (on_[i] == 1) { if (idx >= 0) return -2; idx = int(i); }
        return idx;
    }
private:
    std::vector<Relay> relays_;
    std::vector<int> on_;
};

}  // namespace antswitch
}  // namespace vibe
