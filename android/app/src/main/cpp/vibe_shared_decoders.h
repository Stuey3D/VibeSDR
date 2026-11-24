// vibe_shared_decoders.h — ★★★ A DECODER ON A SHARED DIAL IS THE ROOM'S (Stuart, 2026-11-24).
//
//   "if the aprs decoder is open for user 1 then user 2 connecting it should be open for, same with all decoders. then if
//    user 2 closes it then it closes for them only this is unless it is a tunes the sdr itself decoder like dab"
//
// On a SHARED DIAL (and a one-listener radio) there is ONE decoder host, fed from the one pipeline (DecoderRouter::shared).
// What used to happen there:
//   • a listener arriving was told nothing — the decoder somebody else had open was invisible to them;
//   • a listener CLOSING the box sent audio_extension_detach, and the shim stopped the shared host outright — so the second
//     listener closing their RTTY box stopped the first listener's RTTY (the same for the spots).
// This is the book that fixes both: WHO HOLDS the shared host's running decoder (and its spots run), by decoder socket.
//   • a listener's attach that STARTED a decoder (DecoderHost::Start::Ok — a new one, or a replacement) makes them its only
//     holder; one that JOINED it (the same decoder, the same parameters) adds them;
//   • a detach / a closed socket RELEASES that one listener; the decoder stops only when the LAST holder lets go;
//   • the room is TOLD (a `dec_shared` message on the spectrum socket — the shared-dial contract: the server sends the full
//     state, a joiner adopts it) what runs, with the attach parameters that JOIN it rather than restart it under another
//     key (the beta92 SATELLITE "params" lesson).
// ★ A SATELLITE pick's decoder is held by the pick itself (kSatHolder) and is NOT listed: the pick already tells the room
//   (satSendPicked "shared":true, with its params) — listing it here as well would make every page follow it twice.
// ★ Whole-radio modes (DAB, ADS-B, ISM, AIS, NDB, radiosondes, the aviation datalink, SATELLITE, the scanner, the DX log…)
//   are not on this host at all; they keep the shared behaviour they always had. PAGING is never listed (it runs only on the
//   device's own radio, on its own screen).
// ★ Bounded: two entries, each with at most kMaxHolders sockets (a radio has far fewer listeners than that); the parameters
//   are rebuilt from the attach message's flat scalar fields (≤ kMaxFields, each value ≤ kMaxValue) — never a client's
//   JSON passed through to other listeners. A leaf mutex: safe to call under the shim's clientMtx.
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vibe {

class SharedDecoderRoom {
public:
    enum Slot { Text = 0, Spots = 1 };
    /** The holder id of a SATELLITE pick's decoder (a socket's id is its address — never 1). */
    static constexpr uintptr_t kSatHolder = 1;
    static constexpr size_t kMaxHolders = 64;
    static constexpr size_t kMaxFields = 24;
    static constexpr size_t kMaxValue = 64;

    /** What a release means for the decoder. */
    enum class Release {
        Untracked,   // the book does not know the running decoder (started some other way) — the caller does what it always did
        Kept,        // somebody else still holds it (or this socket never did): it runs on
        Last,        // this was the last holder: the caller stops it
    };

    /** A start on the shared host. `runningKey` is the host's startKey after it (spots: "spots|<scope>"). `joined` = the
     *  host said Joined (the same decoder was already running). A start that is not a join replaces the run, and with it
     *  every holder. @return true when the room's view changed (broadcast it). */
    bool started(Slot s, uintptr_t holder, const std::string& session, const std::string& ext,
                 const std::string& paramsJson, const std::string& runningKey, bool joined, bool sat = false) {
        std::lock_guard<std::mutex> lk(m_);
        Entry& e = e_[s];
        bool changed = false;
        /* ★★★ A HOLDER CHANGING THE SETTINGS (Stuart, 2026-11-24: "it would save cpu on the host if one set of settings
         *  applies to all users"): the same decoder's box (RTTY's shift, WEFAX's LPM, one signal ↔ skimmer …) re-attached by
         *  somebody who holds it is the ROOM's run with new settings — one restart, and everybody who held it holds the new
         *  run (their clients are told the new parameters and show them). A different decoder still replaces the room's. */
        if (!joined && !sat && !e.sat && !e.key.empty() && e.key != runningKey && e.holders.count(holder) && !ext.empty()
            && family(e.ext) == family(ext)) {
            e.key = runningKey; e.ext = ext; e.params = paramsJson;
            if (!session.empty()) e.by = session;
            e.holders[holder] = session;
            return true;
        }
        if (!joined || e.key != runningKey) {
            e = Entry();
            e.key = runningKey; e.ext = ext; e.params = paramsJson; e.sat = sat;
            changed = true;
        }
        if (sat) e.sat = true;
        auto it = e.holders.find(holder);
        if (it == e.holders.end()) {
            if (e.holders.size() >= kMaxHolders) return changed;
            e.holders[holder] = session; changed = true;
        } else if (it->second != session) { it->second = session; changed = true; }
        if (session.size() && e.by.empty()) e.by = session;
        if (!joined && session.size()) e.by = session;
        return changed;
    }
    /** `holder` lets go of the slot's decoder. `runningKey` = the host's key NOW ("" = nothing running). */
    Release release(Slot s, uintptr_t holder, const std::string& runningKey, bool* changed = nullptr) {
        std::lock_guard<std::mutex> lk(m_);
        Entry& e = e_[s];
        if (changed) *changed = false;
        if (e.key.empty() || e.key != runningKey) {
            // ★ the book is stale (the decoder was stopped or replaced elsewhere) — forget it
            if (!e.key.empty()) { e = Entry(); if (changed) *changed = true; }
            return Release::Untracked;
        }
        auto it = e.holders.find(holder);
        if (it == e.holders.end()) return Release::Kept;   // ★ not this socket's to stop (a held session's, somebody else's)
        const std::string gone = it->second;
        e.holders.erase(it);
        if (changed) *changed = true;
        if (e.holders.empty()) { e = Entry(); return Release::Last; }
        // ★ the admin table's credit moves to somebody still holding it
        if (e.by == gone) { e.by.clear(); for (auto& h : e.holders) if (!h.second.empty()) { e.by = h.second; break; } }
        return Release::Kept;
    }
    /** The slot's decoder was stopped (by anyone, for any reason). @return true when that changed the room's view. */
    bool clear(Slot s) {
        std::lock_guard<std::mutex> lk(m_);
        if (e_[s].key.empty()) return false;
        e_[s] = Entry();
        return true;
    }
    /** Does `holder` hold the slot's decoder? */
    bool holds(Slot s, uintptr_t holder) const {
        std::lock_guard<std::mutex> lk(m_);
        return e_[s].holders.count(holder) != 0;
    }
    size_t holderCount(Slot s) const { std::lock_guard<std::mutex> lk(m_); return e_[s].holders.size(); }
    /** Who the admin table credits ("" = nobody in particular). */
    std::string by(Slot s) const { std::lock_guard<std::mutex> lk(m_); return e_[s].by; }
    std::string key(Slot s) const { std::lock_guard<std::mutex> lk(m_); return e_[s].key; }

    /** ★ THE ROOM'S VIEW, for one listener: {"type":"dec_shared","list":[{"ext","params","key","slot","held","mine"}]}.
     *  `textKey` / `spotsKey` = what the host runs NOW (an entry whose key no longer matches is not listed — the book is
     *  only believed while the host agrees). `session` = the listener it is for: "held" = one of their sockets holds it,
     *  "mine" = they started it. A SATELLITE pick's decoder is never listed (the pick tells the room). Empty list = none. */
    std::string listJson(const std::string& session, const std::string& textKey, const std::string& spotsKey) const {
        std::lock_guard<std::mutex> lk(m_);
        std::string out = "{\"type\":\"dec_shared\",\"list\":[";
        bool first = true;
        for (int s = 0; s < 2; ++s) {
            const Entry& e = e_[s];
            const std::string& now = s == Text ? textKey : spotsKey;
            if (e.key.empty() || e.key != now || e.sat || e.ext.empty() || e.ext == "paging") continue;
            bool held = false;
            if (!session.empty()) for (auto& h : e.holders) if (h.second == session) { held = true; break; }
            if (!first) out += ',';
            first = false;
            out += "{\"slot\":\"" + std::string(s == Text ? "decoder" : "spots") + "\",\"ext\":\"" + e.ext + "\",\"params\":"
                 + (e.params.empty() ? std::string("{}") : e.params) + ",\"key\":\"" + jsonEsc(e.key) + "\",\"held\":"
                 + (held ? "true" : "false") + ",\"mine\":" + (!session.empty() && e.by == session ? "true" : "false")
                 + ",\"n\":" + std::to_string(e.holders.size()) + "}";
        }
        out += "]}";
        return out;
    }

    /** ★★ The attach message's PARAMETERS as a clean flat JSON object — what a joiner sends back to JOIN the run (the same
     *  DecoderHost::paramsOf key). The app nests them ({"params":{…}}), the web page spreads them at the top level; both
     *  are read. Only scalar fields (a number, true / false, a short plain string) with plain names are kept; the envelope
     *  (type, extension_name, vfo, params) is not. Numbers are copied as written, so the key reads them back identically.
     *  "" = not shareable (malformed) — the decoder runs, the room is not told. */
    static std::string paramsJson(const std::string& msg) {
        size_t p = 0;
        // the nested object when there is one
        const size_t np = findKey(msg, "params");
        std::string body = msg;
        if (np != std::string::npos) {
            size_t q = np; skipWs(msg, q);
            if (q < msg.size() && msg[q] == '{') {
                size_t end = q;
                if (!matchObject(msg, end)) return "";
                body = msg.substr(q, end - q);
            }
        }
        p = 0; skipWs(body, p);
        if (p >= body.size() || body[p] != '{') return "";
        ++p;
        std::string out = "{";
        size_t n = 0;
        for (;;) {
            skipWs(body, p);
            if (p >= body.size()) return "";
            if (body[p] == '}') break;
            if (body[p] == ',') { ++p; continue; }
            std::string name;
            if (!readString(body, p, name)) return "";
            skipWs(body, p);
            if (p >= body.size() || body[p] != ':') return "";
            ++p; skipWs(body, p);
            if (p >= body.size()) return "";
            std::string val;   // the value's JSON text, or "" = skipped
            const char c = body[p];
            if (c == '"') {
                std::string sv;
                if (!readString(body, p, sv)) return "";
                if (sv.size() <= kMaxValue && plainText(sv)) val = "\"" + sv + "\"";
            } else if (c == '{' || c == '[') {
                size_t end = p;
                if (!matchObject(body, end)) return "";
                p = end;           // ★ nested values are not part of the key (paramsOf reads scalars): skipped
            } else {
                const size_t s0 = p;
                while (p < body.size() && body[p] != ',' && body[p] != '}' && !std::isspace((unsigned char)body[p])) ++p;
                const std::string tok = body.substr(s0, p - s0);
                if (tok == "true" || tok == "false" || (tok.size() <= 32 && isNumber(tok))) val = tok;
                else if (tok != "null") return "";
            }
            if (val.empty() || name == "type" || name == "extension_name" || name == "vfo" || name == "params" || !plainName(name)) continue;
            if (++n > kMaxFields) return "";
            if (out.size() > 1) out += ',';
            out += "\"" + name + "\":" + val;
        }
        return out + "}";
    }

    /** The decoder's BOX: one family for RTTY's one signal and skimmer, CW's, PSK's, and every time station (the clients'
     *  sharedDecoderKey, src/services/sharedDecoders.ts). */
    static std::string family(const std::string& ext) {
        if (ext == "fsk" || ext == "rttyskim") return "rtty";
        if (ext == "cwskim") return "cw";
        if (ext == "pskskim") return "psk";
        if (ext == "time" || ext == "msf" || ext == "dcf77" || ext == "rwm" || ext == "wwv" || ext == "wwvb" || ext == "jjy" || ext == "tdf") return "time";
        return ext;
    }

private:
    struct Entry {
        std::string key, ext, params, by;
        bool sat = false;
        std::map<uintptr_t, std::string> holders;   // decoder socket → its listener's session
    };
    mutable std::mutex m_;
    Entry e_[2];

    static std::string jsonEsc(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if ((unsigned char)c < 0x20) o += ' ';
            else o += c;
        }
        return o;
    }
    static void skipWs(const std::string& s, size_t& p) { while (p < s.size() && std::isspace((unsigned char)s[p])) ++p; }
    /** A JSON string at p (p on the opening quote) → its text; p after the closing quote. Escapes are kept only as the plain
     *  check below then refuses them. */
    static bool readString(const std::string& s, size_t& p, std::string& out) {
        if (p >= s.size() || s[p] != '"') return false;
        ++p; out.clear();
        while (p < s.size()) {
            const char c = s[p++];
            if (c == '"') return true;
            if (c == '\\') { if (p >= s.size()) return false; out += '\\'; out += s[p++]; continue; }
            out += c;
            if (out.size() > 4096) return false;
        }
        return false;
    }
    /** p on '{' or '[' → p after its match (strings respected). */
    static bool matchObject(const std::string& s, size_t& p) {
        int depth = 0;
        while (p < s.size()) {
            const char c = s[p];
            if (c == '"') { std::string t; if (!readString(s, p, t)) return false; continue; }
            if (c == '{' || c == '[') { if (++depth > 16) return false; }
            else if (c == '}' || c == ']') { if (--depth == 0) { ++p; return true; } }
            ++p;
        }
        return false;
    }
    /** The value position after "key": at the TOP level of the message (depth 1) — or npos. */
    static size_t findKey(const std::string& s, const char* key) {
        size_t p = 0; skipWs(s, p);
        if (p >= s.size() || s[p] != '{') return std::string::npos;
        ++p;
        int depth = 1;
        bool expectKey = true;
        while (p < s.size() && depth > 0) {
            const char c = s[p];
            if (c == '"') {
                std::string t;
                if (!readString(s, p, t)) return std::string::npos;
                if (depth == 1 && expectKey) {
                    size_t q = p; skipWs(s, q);
                    if (q < s.size() && s[q] == ':') { if (t == key) return q + 1; p = q + 1; expectKey = false; continue; }
                }
                continue;
            }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') --depth;
            else if (c == ',' && depth == 1) expectKey = true;
            ++p;
        }
        return std::string::npos;
    }
    static bool plainName(const std::string& n) {
        if (n.empty() || n.size() > 32) return false;
        for (char c : n) if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
        return true;
    }
    static bool plainText(const std::string& v) {
        for (char c : v) if (!(std::isalnum((unsigned char)c) || c == ' ' || c == '_' || c == '-' || c == '.' || c == '/' || c == '+' || c == ':')) return false;
        return true;
    }
    static bool isNumber(const std::string& t) {
        if (t.empty()) return false;
        size_t i = 0;
        if (t[i] == '-') ++i;
        bool digits = false, dot = false, exp = false;
        for (; i < t.size(); ++i) {
            const char c = t[i];
            if (std::isdigit((unsigned char)c)) digits = true;
            else if (c == '.' && !dot && !exp) dot = true;
            else if ((c == 'e' || c == 'E') && digits && !exp) {
                exp = true; digits = false;
                if (i + 1 < t.size() && (t[i + 1] == '+' || t[i + 1] == '-')) ++i;
            } else return false;
        }
        return digits;
    }
};

}  // namespace vibe
