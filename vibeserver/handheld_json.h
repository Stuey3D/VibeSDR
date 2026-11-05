// handheld_json.h — a small, strict JSON reader for the handheld profile (R36S, 2026-11-05).
//
// ★ WHY ANOTHER ONE. The rest of the daemon reads its own files with key-by-key extractors written for one known shape
//   (vibeserver_config.cpp). The handheld half talks to two parties whose lines are not ours to shape — the hardware
//   service (docs/v12/R36S-HANDHELD-IMAGE.md §0.2) and the page's per-radio memory — so it needs a real parser: nested
//   objects, escapes, and the RAW TEXT of a value, so a client's memory blob is stored exactly as it came and never
//   re-serialised (a re-serialiser is a second place for a number's format to drift).
// ★★ BOUNDED: depth 32, and the caller caps the input (every reader in handheld.cpp does). Anything malformed is "not
//    JSON" — never a partial answer.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hhjson {

struct Value {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;                                       // Str: the decoded string
    std::vector<Value> a;                                // Arr
    std::vector<std::pair<std::string, Value>> o;        // Obj, in source order
    std::string raw;                                     // ★ the value's exact source text

    const Value* get(const std::string& k) const {
        if (kind != Obj) return nullptr;
        for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string str(const std::string& k, const std::string& dflt = "") const {
        const Value* v = get(k); return v && v->kind == Str ? v->s : dflt;
    }
    double num(const std::string& k, double dflt = 0) const {
        const Value* v = get(k); return v && v->kind == Num ? v->n : dflt;
    }
    bool boolean(const std::string& k, bool dflt = false) const {
        const Value* v = get(k); return v && v->kind == Bool ? v->b : dflt;
    }
    bool has(const std::string& k) const { return get(k) != nullptr; }
};

namespace detail {
struct P {
    const std::string& t; size_t i = 0; int depth = 0;
    explicit P(const std::string& s) : t(s) {}
    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++; }
    static void utf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(uint32_t& v) {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = t[i++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (i >= t.size() || t[i] != '"') return false;
        i++;
        while (i < t.size()) {
            const char c = t[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (i >= t.size()) return false;
            const char e = t[i++];
            switch (e) {
                case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
                case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp; if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        i += 2; uint32_t lo; if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp); break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Value& v) {
        if (++depth > 32) return false;
        ws();
        if (i >= t.size()) return false;
        const size_t start = i;
        const char c = t[i];
        bool ok = false;
        if (c == '{') {
            v.kind = Value::Obj; i++; ws();
            if (i < t.size() && t[i] == '}') { i++; ok = true; }
            else for (;;) {
                ws(); std::string k; if (!string(k)) break;
                ws(); if (i >= t.size() || t[i] != ':') break; i++;
                Value child; if (!value(child)) break;
                v.o.emplace_back(std::move(k), std::move(child));
                ws(); if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == '}') { i++; ok = true; }
                break;
            }
        } else if (c == '[') {
            v.kind = Value::Arr; i++; ws();
            if (i < t.size() && t[i] == ']') { i++; ok = true; }
            else for (;;) {
                Value child; if (!value(child)) break;
                v.a.push_back(std::move(child));
                ws(); if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; ok = true; }
                break;
            }
        } else if (c == '"') { v.kind = Value::Str; ok = string(v.s); }
        else if (t.compare(i, 4, "true") == 0)  { v.kind = Value::Bool; v.b = true;  i += 4; ok = true; }
        else if (t.compare(i, 5, "false") == 0) { v.kind = Value::Bool; v.b = false; i += 5; ok = true; }
        else if (t.compare(i, 4, "null") == 0)  { v.kind = Value::Null; i += 4; ok = true; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            const char* b = t.c_str() + i; char* e = nullptr;
            v.kind = Value::Num; v.n = std::strtod(b, &e);
            ok = e && e > b; if (ok) i += (size_t)(e - b);
        }
        depth--;
        if (ok) v.raw = t.substr(start, i - start);
        return ok;
    }
};
}  // namespace detail

/** Parse one whole JSON text. False (and `out` unspecified) for anything malformed or with trailing junk. */
inline bool parse(const std::string& text, Value& out) {
    detail::P p(text);
    if (!p.value(out)) return false;
    p.ws();
    return p.i == text.size();
}

/** A JSON string literal, quotes included. */
inline std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o + "\"";
}

}  // namespace hhjson
