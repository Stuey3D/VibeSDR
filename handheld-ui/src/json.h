// json.h — a small, strict-enough JSON reader for the engine's messages (objects, arrays, strings, numbers, bools,
// null). ★ Never throws: a malformed message parses to a null Value and is ignored by its reader. Strings decode \uXXXX
// to UTF-8 (station names carry accents). Writing is done by hand in the callers — the messages we send are tiny.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hh {

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::map<std::string, Json> o;

    bool isNull() const { return kind == Null; }
    const Json& operator[](const std::string& k) const {
        static const Json none;
        if (kind != Obj) return none;
        auto it = o.find(k);
        return it == o.end() ? none : it->second;
    }
    const Json& operator[](size_t i) const { static const Json none; return kind == Arr && i < a.size() ? a[i] : none; }
    size_t size() const { return kind == Arr ? a.size() : kind == Obj ? o.size() : 0; }
    bool has(const std::string& k) const { return kind == Obj && o.count(k); }
    double num(double def = 0) const { return kind == Num ? n : kind == Bool ? (b ? 1 : 0) : def; }
    bool boolean(bool def = false) const { return kind == Bool ? b : kind == Num ? n != 0 : def; }
    const std::string& str() const { static const std::string e; return kind == Str ? s : e; }

    static Json parse(const std::string& text) {
        size_t i = 0;
        Json v;
        if (!value(text, i, v, 0)) return Json();
        return v;
    }

private:
    static void ws(const std::string& t, size_t& i) { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++; }
    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
        else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 63)); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
    }
    static bool hex4(const std::string& t, size_t i, uint32_t& v) {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (size_t k = 0; k < 4; k++) {
            const char c = t[i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    static bool string(const std::string& t, size_t& i, std::string& out) {
        if (t[i] != '"') return false;
        i++;
        while (i < t.size()) {
            const char c = t[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= t.size()) return false;
            const char e = t[i++];
            switch (e) {
                case '"': out += '"'; break;  case '\\': out += '\\'; break; case '/': out += '/'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;  case 'n': out += '\n'; break;
                case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(t, i, cp)) return false;
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        uint32_t lo;
                        if (hex4(t, i + 2, lo) && lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                    }
                    utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    static bool value(const std::string& t, size_t& i, Json& v, int depth) {
        if (depth > 32) return false;
        ws(t, i);
        if (i >= t.size()) return false;
        const char c = t[i];
        if (c == '{') {
            v.kind = Obj; i++; ws(t, i);
            if (i < t.size() && t[i] == '}') { i++; return true; }
            while (i < t.size()) {
                ws(t, i);
                std::string k;
                if (i >= t.size() || !string(t, i, k)) return false;
                ws(t, i);
                if (i >= t.size() || t[i] != ':') return false;
                i++;
                Json x;
                if (!value(t, i, x, depth + 1)) return false;
                v.o[k] = std::move(x);
                ws(t, i);
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == '}') { i++; return true; }
                return false;
            }
            return false;
        }
        if (c == '[') {
            v.kind = Arr; i++; ws(t, i);
            if (i < t.size() && t[i] == ']') { i++; return true; }
            while (i < t.size()) {
                Json x;
                if (!value(t, i, x, depth + 1)) return false;
                v.a.push_back(std::move(x));
                ws(t, i);
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; return true; }
                return false;
            }
            return false;
        }
        if (c == '"') { v.kind = Str; return string(t, i, v.s); }
        if (t.compare(i, 4, "true") == 0) { v.kind = Bool; v.b = true; i += 4; return true; }
        if (t.compare(i, 5, "false") == 0) { v.kind = Bool; v.b = false; i += 5; return true; }
        if (t.compare(i, 4, "null") == 0) { v.kind = Null; i += 4; return true; }
        char* end = nullptr;
        const double d = std::strtod(t.c_str() + i, &end);
        if (end == t.c_str() + i) return false;
        v.kind = Num; v.n = d; i = (size_t)(end - t.c_str());
        return true;
    }
};

/** ★ For the messages we send: a JSON string literal of `s`. */
inline std::string jsonQuote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

}  // namespace hh
