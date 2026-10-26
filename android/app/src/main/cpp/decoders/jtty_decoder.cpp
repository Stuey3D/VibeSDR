// decoders/jtty_decoder.cpp — JTTY: the source grammar, the FEC, our own modulator (for the tests) and the slot-less,
// whole-passband receiver. See jtty_decoder.h for the design and docs/v12/JTTY.md for the protocol and its sources.
//
// ★★ Written from the protocol description that ships with WSJT-X 3.2 (lib/jtty/jtty_source_encoding.txt,
//    jtty_design.md) and the code-profile constants (K = 10, generators 1167/1545 octal, CRC-12 0x80F). Every table here
//    is typed from its MEANING and checked against the published 34-bit golden vectors in vibeserver/test-jtty.cpp.
#include "jtty_decoder.h"
#include "../vibedsp/simd_internal.h"     // ★ VIBE_NEON / VIBE_SSE (and VIBE_FORCE_SCALAR) — the project's one SIMD switch
#include "../vibedsp/third_party/pffft/pffft.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <cstdio>
#include <cstdlib>

namespace vibe {
namespace jtty {

const uint8_t kSync[kSyncSyms] = { 0, 2, 2, 3, 0, 0, 3, 2, 1, 3, 1, 2, 0 };
const char kAlphabet[65] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?!\"#$%,&*()_'=[]{}<>|:;";

namespace {

// ── Small helpers ─────────────────────────────────────────────────────────────────────────────────────────────────
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isLetter(char c) { return c >= 'A' && c <= 'Z'; }
int alphaIndex(char c) {
    for (int i = 0; i < 64; ++i) if (kAlphabet[i] == c) return i;
    return -1;
}
int base36(char c) { return isDigit(c) ? c - '0' : isLetter(c) ? 10 + (c - 'A') : -1; }
char base36Char(int v) { return v < 10 ? (char)('0' + v) : (char)('A' + v - 10); }
std::string pad(int v, int width) {
    std::string s = std::to_string(v);
    while ((int)s.size() < width) s.insert(s.begin(), '0');
    return s;
}

/** The control phrases, by id 0–17. */
const char* const kControl[18] = {
    "AGN?", "CALL?", "AGN CALL", "NR?", "AGN NR", "EXCH?", "STATE?", "SECTION?", "ZONE?",
    "GRID?", "RPRT?", "QSL TU", "TU", "QRZ?", "QSO B4", "WAIT", "NIL?", "OK?" };

/** ★ The ARRL/RAC Field Day sections in the order the FT8-family 77-bit grammar numbers them (1–86). The index is the
 *  protocol fact; the order is checked by the CLASS_SECTION golden vector (1D EMA → 11) in the test. */
const char* const kSections[86] = {
    "AB", "AK", "AL", "AR", "AZ", "BC", "CO", "CT", "DE", "EB",
    "EMA", "ENY", "EPA", "EWA", "GA", "GH", "IA", "ID", "IL", "IN",
    "KS", "KY", "LA", "LAX", "NS", "MB", "MDC", "ME", "MI", "MN",
    "MO", "MS", "MT", "NC", "ND", "NE", "NFL", "NH", "NL", "NLI",
    "NM", "NNJ", "NNY", "TER", "NTX", "NV", "OH", "OK", "ONE", "ONN",
    "ONS", "OR", "ORG", "PAC", "PR", "QC", "RI", "SB", "SC", "SCV",
    "SD", "SDG", "SF", "SFL", "SJV", "SK", "SNJ", "STX", "SV", "TN",
    "UT", "VA", "VI", "VT", "WCF", "WI", "WMA", "WNY", "WPA", "WTX",
    "WV", "WWA", "WY", "DX", "PE", "NB" };

// ★ The 28-bit call field: 2 063 592 special tokens, then 2²² hashes, then the standard calls.
constexpr uint32_t kNTokens = 2063592u, kMax22 = 4194304u;

bool tokenValue(const std::string& t, uint32_t& v) {
    if (t.size() != 2 && t.size() != 3) return false;
    v = 0;
    for (char c : t) { const int d = base36(c); if (d < 0) return false; v = v * 36 + (uint32_t)d; }
    if (t.size() == 3 && v < 36u * 36u) return false;   // ★ a leading zero has a canonical two-character form
    return true;
}
std::string tokenText(uint32_t v, int n) {
    std::string s((size_t)n, '0');
    for (int i = n - 1; i >= 0; --i) { s[(size_t)i] = base36Char((int)(v % 36)); v /= 36; }
    return s;
}
bool gridIndex(const std::string& g, uint32_t& v) {
    if (g.size() != 4) return false;
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || !isDigit(g[2]) || !isDigit(g[3])) return false;
    v = (uint32_t)((((g[0] - 'A') * 18 + (g[1] - 'A')) * 10 + (g[2] - '0')) * 10 + (g[3] - '0'));
    return true;
}
std::string gridText(uint32_t v) {
    std::string g(4, ' ');
    g[3] = (char)('0' + v % 10); v /= 10;
    g[2] = (char)('0' + v % 10); v /= 10;
    g[1] = (char)('A' + v % 18); v /= 18;
    g[0] = (char)('A' + v);
    return g;
}
bool validNumber(int kind, int value) {
    if (kind < 0 || kind > 7 || value < 0 || value > 131071) return false;
    if (kind == kCqZone) return value >= 1 && value <= 40;
    if (kind == kItuZone) return value >= 1 && value <= 90;
    if (kind == kLicenceYear) return value <= 9999;
    return true;
}
std::string renderNumber(int kind, int value) {
    switch (kind) {
        case kSerial: return pad(value, 3);
        case kCqZone: case kItuZone: case kCheck: return pad(value, 2);
        case kLicenceYear: return pad(value, 4);
        default: return std::to_string(value);
    }
}
std::string withRole(int role, const std::string& f) { return role ? "599 " + f : f; }

} // namespace

// ── The 28-bit standard call ──────────────────────────────────────────────────────────────────────────────────────
bool standardCall(const std::string& c) {
    const int n = (int)c.size();
    if (n < 3 || n > 6) return false;
    for (char ch : c) if (!isDigit(ch) && !isLetter(ch)) return false;
    if (c[0] == 'Q') return false;
    if (!isLetter(c[0]) && !isLetter(c[1])) return false;
    int area = 0;                              // ★ the call area: the LAST digit, which must be the 2nd or 3rd character
    for (int i = n - 1; i >= 1; --i) if (isDigit(c[(size_t)i])) { area = i + 1; break; }
    if (area != 2 && area != 3) return false;
    bool letter = false;
    for (int i = 0; i < area - 1; ++i) if (isLetter(c[(size_t)i])) letter = true;
    if (!letter) return false;
    const int suffix = n - area;
    if (suffix < 1 || suffix > 3) return false;
    for (int i = area; i < n; ++i) if (!isLetter(c[(size_t)i])) return false;
    return true;
}
bool packCall28(const std::string& call, uint32_t& n28) {
    if (!standardCall(call)) return false;
    int area = 0;
    for (int i = (int)call.size() - 1; i >= 1; --i) if (isDigit(call[(size_t)i])) { area = i + 1; break; }
    std::string s = area == 2 ? " " + call : call;
    while (s.size() < 6) s += ' ';
    if (s.size() != 6) return false;
    auto in = [](const char* set, char ch) { const char* p = std::strchr(set, ch); return p && ch ? (int)(p - set) : -1; };
    const int i1 = in(" 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", s[0]), i2 = in("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", s[1]);
    const int i3 = in("0123456789", s[2]);
    const int i4 = in(" ABCDEFGHIJKLMNOPQRSTUVWXYZ", s[3]), i5 = in(" ABCDEFGHIJKLMNOPQRSTUVWXYZ", s[4]),
              i6 = in(" ABCDEFGHIJKLMNOPQRSTUVWXYZ", s[5]);
    if (i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0 || i5 < 0 || i6 < 0) return false;
    n28 = (uint32_t)(((((i1 * 36 + i2) * 10 + i3) * 27 + i4) * 27 + i5) * 27 + i6) + kNTokens + kMax22;
    return true;
}
bool unpackCall28(uint32_t n28, std::string& call) {
    if (n28 >= (1u << 28) || n28 < kNTokens + kMax22) return false;   // ★ tokens and hashes are not JTTY calls
    uint32_t n = n28 - kNTokens - kMax22;
    char s[7]; s[6] = 0;
    s[5] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ"[n % 27]; n /= 27;
    s[4] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ"[n % 27]; n /= 27;
    s[3] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ"[n % 27]; n /= 27;
    s[2] = "0123456789"[n % 10]; n /= 10;
    s[1] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[n % 36]; n /= 36;
    if (n >= 37) return false;
    s[0] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[n];
    std::string c(s);
    while (!c.empty() && c.front() == ' ') c.erase(c.begin());
    while (!c.empty() && c.back() == ' ') c.pop_back();
    if (c.find(' ') != std::string::npos) return false;
    uint32_t back = 0;
    if (!standardCall(c) || !packCall28(c, back) || back != n28) return false;   // ★ exact round trip only
    call = c;
    return true;
}
const char* arrlSection(int i) { return i >= 1 && i <= 86 ? kSections[i - 1] : ""; }
int arrlSectionIndex(const std::string& s) {
    for (int i = 0; i < 86; ++i) if (s == kSections[i]) return i + 1;
    return 0;
}

// ── Atoms ↔ 34 bits ───────────────────────────────────────────────────────────────────────────────────────────────
bool packAtom(const Atom& a, bool eom, uint64_t& bits34) {
    uint64_t n32 = 0, body = 0;
    int family = -1;
    auto role = [&](int r) { return r == 0 || r == 1; };
    switch (a.kind) {
        case Kind::Call: {
            if (a.sub < 0 || a.sub > 5) return false;
            uint32_t n28;
            if (!packCall28(a.text, n28)) return false;
            const int i2 = a.sub <= 3 ? 0 : 1, n2 = a.sub <= 3 ? a.sub : a.sub - 4;
            n32 = ((uint64_t)n28 << 4) | ((uint64_t)n2 << 2) | (uint64_t)i2;
            break;
        }
        case Kind::ExchNum:
            if (!role(a.role) || !validNumber(a.sub, a.value)) return false;
            body = ((uint64_t)a.role << 26) | ((uint64_t)a.sub << 22) | ((uint64_t)a.value << 5);
            family = 0; break;
        case Kind::ExchLoc: {
            uint32_t t;
            if (!role(a.role) || a.sub < 0 || a.sub > 4 || !tokenValue(a.text, t)) return false;
            body = ((uint64_t)a.role << 26) | ((uint64_t)a.sub << 22) | ((uint64_t)(a.text.size() - 2) << 21) | ((uint64_t)t << 5);
            family = 1; break;
        }
        case Kind::ExchPair: {
            uint64_t pair = 0;
            if (a.sub == kZoneLoc3) {
                uint32_t t;
                if (a.value < 1 || a.value > 40 || !tokenValue(a.text, t)) return false;
                pair = ((uint64_t)a.value << 17) | ((uint64_t)(a.text.size() - 2) << 16) | t;
            } else if (a.sub == kClassSection) {
                if (a.value < 1 || a.value > 32 || a.value2 < 1 || a.value2 > 86 || a.text.size() != 1) return false;
                const int cls = a.text[0] - 'A';
                if (cls < 0 || cls > 5) return false;
                pair = ((uint64_t)a.value << 17) | ((uint64_t)cls << 14) | ((uint64_t)a.value2 << 7);
            } else return false;
            body = ((uint64_t)a.sub << 24) | (pair << 1);
            family = 2; break;
        }
        case Kind::NumTime:
            if (!role(a.role) || a.value < 0 || a.value > 16383 || a.value2 < 0 || a.value2 > 1439) return false;
            body = ((uint64_t)a.role << 26) | ((uint64_t)a.value << 12) | ((uint64_t)a.value2 << 1);
            family = 3; break;
        case Kind::Control:
            if (a.sub < 0 || a.sub > 17) return false;
            body = (0ull << 23) | ((uint64_t)a.sub << 16);
            family = 4; break;
        case Kind::Grid4: {
            uint32_t g;
            if (!role(a.role) || !gridIndex(a.text, g)) return false;
            body = (1ull << 23) | ((uint64_t)a.role << 22) | ((uint64_t)g << 7);
            family = 4; break;
        }
        case Kind::Text5: {
            if (a.text.size() != 5) return false;
            uint64_t t = 0;
            for (char c : a.text) { const int k = alphaIndex(c); if (k < 0) return false; t = t * 64 + (uint64_t)k; }
            n32 = (t << 2) | 3u;
            break;
        }
    }
    if (family >= 0) n32 = (((body << 3) | (uint64_t)family) << 2) | 2u;
    if (n32 == 0 || n32 >> 32) return false;
    bits34 = (n32 << 2) | (eom ? 1u : 0u);
    return true;
}

bool unpackAtom(uint64_t bits34, Atom& a, bool& eom) {
    a = Atom();
    if (bits34 >> 34) return false;
    if (bits34 & 2u) return false;                       // ★ bit 33: reserved, zero
    eom = (bits34 & 1u) != 0;
    const uint64_t n32 = bits34 >> 2;
    if (n32 == 0) return false;                          // ★ the all-zero grammar word is the invalid sentinel
    const int i2 = (int)(n32 & 3u);
    const uint64_t top30 = n32 >> 2;
    if (i2 == 0 || i2 == 1) {
        const uint32_t n28 = (uint32_t)(n32 >> 4);
        const int n2 = (int)((n32 >> 2) & 3u);
        if (i2 == 1 && n2 > 1) return false;             // ★ 1.2, 1.3 reserved
        a.kind = Kind::Call; a.sub = i2 == 0 ? n2 : n2 + 4;
        return unpackCall28(n28, a.text);
    }
    if (i2 == 3) {
        a.kind = Kind::Text5; a.text.assign(5, ' ');
        for (int i = 0; i < 5; ++i) a.text[(size_t)i] = kAlphabet[(top30 >> (6 * (4 - i))) & 63u];
        return true;
    }
    const int family = (int)(top30 & 7u);
    const uint64_t body = top30 >> 3;
    auto bits = [&](uint64_t v, int lo, int n) { return (uint64_t)((v >> lo) & ((1ull << n) - 1)); };
    switch (family) {
        case 0:
            a.kind = Kind::ExchNum; a.role = (int)bits(body, 26, 1); a.sub = (int)bits(body, 22, 4); a.value = (int)bits(body, 5, 17);
            return bits(body, 0, 5) == 0 && validNumber(a.sub, a.value);
        case 1: {
            a.kind = Kind::ExchLoc; a.role = (int)bits(body, 26, 1); a.sub = (int)bits(body, 22, 4);
            const int n = (int)bits(body, 21, 1) + 2; const uint32_t t = (uint32_t)bits(body, 5, 16);
            if (bits(body, 0, 5) != 0 || a.sub > 4) return false;
            if (t >= (n == 2 ? 1296u : 46656u) || (n == 3 && t < 1296u)) return false;
            a.text = tokenText(t, n);
            return true;
        }
        case 2: {
            a.kind = Kind::ExchPair; a.sub = (int)bits(body, 24, 3);
            const uint64_t pair = bits(body, 1, 23);
            if (bits(body, 0, 1) != 0) return false;
            if (a.sub == kZoneLoc3) {
                a.value = (int)bits(pair, 17, 6);
                const int n = (int)bits(pair, 16, 1) + 2; const uint32_t t = (uint32_t)bits(pair, 0, 16);
                if (a.value < 1 || a.value > 40) return false;
                if (t >= (n == 2 ? 1296u : 46656u) || (n == 3 && t < 1296u)) return false;
                a.text = tokenText(t, n);
                return true;
            }
            if (a.sub == kClassSection) {
                a.value = (int)bits(pair, 17, 6); const int cls = (int)bits(pair, 14, 3); a.value2 = (int)bits(pair, 7, 7);
                if (a.value < 1 || a.value > 32 || cls > 5 || a.value2 < 1 || a.value2 > 86 || bits(pair, 0, 7) != 0) return false;
                a.text = std::string(1, (char)('A' + cls));
                return true;
            }
            return false;
        }
        case 3:
            a.kind = Kind::NumTime; a.role = (int)bits(body, 26, 1); a.value = (int)bits(body, 12, 14); a.value2 = (int)bits(body, 1, 11);
            return bits(body, 0, 1) == 0 && a.value2 <= 1439;
        case 4: {
            const int sub = (int)bits(body, 23, 4); const uint64_t data = bits(body, 0, 23);
            if (sub == 0) {
                a.kind = Kind::Control; a.sub = (int)bits(data, 16, 7);
                return bits(data, 0, 16) == 0 && a.sub <= 17;
            }
            if (sub == 1) {
                a.kind = Kind::Grid4; a.role = (int)bits(data, 22, 1);
                const uint32_t g = (uint32_t)bits(data, 7, 15);
                if (bits(data, 0, 7) != 0 || g >= 32400u) return false;
                a.text = gridText(g);
                return true;
            }
            return false;
        }
        default: return false;                           // ★ 101 profiled, 110 versioned (reserved), 111 invalid guard
    }
}

std::string render(const Atom& a) {
    switch (a.kind) {
        case Kind::Call:
            switch (a.sub) {
                case kCq: return "CQ " + a.text + " CQ";
                case kCall: return a.text;
                case kTuCq: return "TU " + a.text + " CQ";
                case kCallTu: return a.text + " TU";
                case kCallAgn: return a.text + " AGN?";
                case kTuNow: return "TU NOW " + a.text;
            }
            return "";
        case Kind::ExchNum: return withRole(a.role, renderNumber(a.sub, a.value));
        case Kind::ExchLoc: return withRole(a.role, a.text);
        case Kind::ExchPair:
            if (a.sub == kZoneLoc3) return "599 " + pad(a.value, 2) + " " + a.text;
            return std::to_string(a.value) + a.text + " " + arrlSection(a.value2);
        case Kind::NumTime: return withRole(a.role, pad(a.value, 3) + " " + pad(a.value2 / 60, 2) + pad(a.value2 % 60, 2));
        case Kind::Control: return a.sub >= 0 && a.sub <= 17 ? kControl[a.sub] : "";
        case Kind::Grid4: return withRole(a.role, a.text);
        case Kind::Text5: return a.text;
    }
    return "";
}

std::string renderMessage(const std::vector<Atom>& atoms) {
    std::string s;
    for (auto& a : atoms) s += a.kind == Kind::Text5 ? a.text : render(a) + " ";
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string normalise(const std::string& in) {
    std::string o;
    bool lastSpace = true;
    for (char c : in) {
        if (((unsigned char)c & 0xC0) == 0x80) continue;      // ★ a UTF-8 character is ONE unsupported character: one '#'
        if (c == 0 || c == '~' || c == '\t' || c == '\n' || c == '\r') c = ' ';
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c != ' ' && alphaIndex(c) < 0) c = '#';
        if (c == ' ') { if (lastSpace) continue; lastSpace = true; }
        else lastSpace = false;
        o += c;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    if (o.size() > 80) o.resize(80);
    return o;
}

/** ★ Fewest frames, from the spec's description of the dynamic program: TEXT5 anywhere (five characters, only the last
 *  padded), a structured atom only at a token boundary and only when its canonical rendering IS the text there (plus the
 *  one separating space it implies). Ties: structured before TEXT5, then the longer span, then the lower kind/sub/role. */
bool packMessage(const std::string& text, std::vector<uint64_t>& frames) {
    frames.clear();
    const std::string m = normalise(text);
    const int n = (int)m.size();
    if (n == 0) return false;
    const int INF = 999;
    std::vector<int> dp((size_t)n + 1, INF), next((size_t)n + 1, 0);
    std::vector<Atom> choice((size_t)n + 1);
    dp[(size_t)n] = 0;
    auto key = [](const Atom& a) { return 100 * (int)a.kind + 2 * a.sub + a.role; };
    for (int i = n - 1; i >= 0; --i) {
        auto consider = [&](const Atom& a, int nx) {
            const int cost = 1 + dp[(size_t)nx];
            if (cost > kMaxFrames || cost > dp[(size_t)i]) return;
            if (cost == dp[(size_t)i]) {
                const bool t = a.kind == Kind::Text5, bt = choice[(size_t)i].kind == Kind::Text5;
                if (t && !bt) return;
                if (t == bt) {
                    if (nx < next[(size_t)i]) return;
                    if (nx == next[(size_t)i] && key(a) >= key(choice[(size_t)i])) return;
                }
            }
            dp[(size_t)i] = cost; next[(size_t)i] = nx; choice[(size_t)i] = a;
        };
        auto offer = [&](const Atom& a) {
            uint64_t w; Atom back; bool e;
            if (!packAtom(a, false, w) || !unpackAtom(w, back, e)) return;
            const std::string r = render(back);
            if (r.empty() || i + (int)r.size() > n || m.compare((size_t)i, r.size(), r) != 0) return;
            const int last = i + (int)r.size();
            if (last < n && m[(size_t)last] != ' ') return;
            consider(back, last < n ? last + 1 : n);
        };
        {   // TEXT5
            Atom t; t.kind = Kind::Text5; t.text = m.substr((size_t)i, 5);
            while (t.text.size() < 5) t.text += ' ';
            consider(t, std::min(n, i + 5));
        }
        if (i > 0 && m[(size_t)i - 1] != ' ') continue;
        std::vector<std::string> w;
        for (size_t p = (size_t)i; p < (size_t)n && w.size() < 3;) {
            size_t e = m.find(' ', p); if (e == std::string::npos) e = (size_t)n;
            w.push_back(m.substr(p, e - p)); p = e + 1;
        }
        for (auto& word : w)
            if (standardCall(word))
                for (int act = 0; act <= 5; ++act) { Atom a; a.kind = Kind::Call; a.sub = act; a.text = word; offer(a); }
        for (int c = 0; c <= 17; ++c) { Atom a; a.kind = Kind::Control; a.sub = c; offer(a); }
        for (int role = 0; role <= 1; ++role) {
            size_t f = 0;
            if (role == 1) { if (w.size() < 2 || w[0] != "599") continue; f = 1; }
            const std::string& fw = w[f];
            bool digits = !fw.empty() && fw.size() <= 6;
            for (char c : fw) if (!isDigit(c)) digits = false;
            if (digits) { const long v = std::stol(fw); if (v <= 131071) { Atom a; a.kind = Kind::ExchNum; a.role = role; a.sub = kGenericNum; a.value = (int)v; offer(a); } }
            if (fw.size() == 4) { Atom a; a.kind = Kind::Grid4; a.role = role; a.text = fw; offer(a); }
            if (role == 1 && (fw.size() == 2 || fw.size() == 3)) {
                bool letter = false; for (char c : fw) if (isLetter(c)) letter = true;
                if (letter) { Atom a; a.kind = Kind::ExchLoc; a.role = 1; a.sub = kGenericQth; a.text = fw; offer(a); }
            }
        }
        if (w.size() >= 2 && (w[0].size() == 2 || w[0].size() == 3)) {
            const std::string cnt = w[0].substr(0, w[0].size() - 1);
            bool digits = !cnt.empty(); for (char c : cnt) if (!isDigit(c)) digits = false;
            const int sec = arrlSectionIndex(w[1]);
            if (digits && sec) {
                Atom a; a.kind = Kind::ExchPair; a.sub = kClassSection; a.value = std::stoi(cnt); a.value2 = sec;
                a.text = w[0].substr(w[0].size() - 1); offer(a);
            }
        }
    }
    if (dp[0] > kMaxFrames) return false;
    for (int i = 0; i < n; i = next[(size_t)i]) {
        uint64_t w;
        if (!packAtom(choice[(size_t)i], false, w)) { frames.clear(); return false; }
        frames.push_back(w);
    }
    if (!frames.empty()) frames.back() |= 1u;   // ★ EOM on the last atom only
    return !frames.empty();
}

// ── FEC ───────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace {
constexpr uint32_t kG0 = 01167, kG1 = 01545;   // ★ octal, as the code tables write them; bit 0 = the newest input
constexpr int kStates = 512;
constexpr uint8_t kGray[4] = { 0, 1, 3, 2 };    // (b0,b1) = 00,01,10,11 → tones 0,1,3,2
inline int parity(uint32_t v) { v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (int)(v & 1u); }
inline uint8_t toneOfReg(uint32_t reg) { return kGray[2 * parity(reg & kG0) + parity(reg & kG1)]; }

/** 46 info bits (34 payload MSB first, then the CRC MSB first) — bit i of the result is info bit i (0-based). */
void infoBits(uint64_t bits34, uint8_t out[kInfoBits]) {
    for (int i = 0; i < kPayloadBits; ++i) out[i] = (uint8_t)((bits34 >> (kPayloadBits - 1 - i)) & 1u);
    const uint16_t c = crc12(bits34);
    for (int i = 0; i < kCrcBits; ++i) out[kPayloadBits + i] = (uint8_t)((c >> (kCrcBits - 1 - i)) & 1u);
}
} // namespace

uint16_t crc12(uint64_t bits34) {
    uint32_t r = 0;
    for (int i = 0; i < kPayloadBits; ++i) {
        r ^= (uint32_t)((bits34 >> (kPayloadBits - 1 - i)) & 1u) << 11;
        r = (r & 0x800u) ? ((r << 1) ^ 0x80Fu) : (r << 1);
        r &= 0xFFFu;
    }
    return (uint16_t)r;
}

void encodeFrame(uint64_t bits34, uint8_t tones[kFrameSyms]) {
    uint8_t b[kInfoBits];
    infoBits(bits34, b);
    uint32_t st = 0;                                         // ★ tail-biting: start in the state the frame ends in
    for (int i = kInfoBits - 9; i < kInfoBits; ++i) st = ((st << 1) | b[i]) & (kStates - 1);
    for (int i = 0; i < kSyncSyms; ++i) tones[i] = kSync[i];
    for (int t = 0; t < kInfoBits; ++t) {
        const uint32_t reg = ((st << 1) | b[t]) & 0x3FFu;
        tones[kSyncSyms + t] = toneOfReg(reg);
        st = reg & (kStates - 1);
    }
}

// ── Our modulator (the tests' transmitter) ────────────────────────────────────────────────────────────────────────
void synthesise(const uint8_t* tones, int nTones, double f0, double rate, float amp, std::vector<float>& out, size_t at,
                double phase0) {
    if (nTones <= 0) return;
    const int nsps = (int)std::lround(rate / kBaud);
    const double bt = 2.0, c = M_PI * std::sqrt(2.0 / std::log(2.0));
    // ★ The Gaussian-smoothed frequency pulse over three symbols: the standard GFSK pulse,
    //   ½[erf(c·BT·(t+½)) − erf(c·BT·(t−½))], t in symbols from the pulse centre.
    std::vector<double> pulse((size_t)3 * nsps);
    for (int i = 0; i < 3 * nsps; ++i) {
        const double t = (i - 1.5 * nsps) / nsps;
        pulse[(size_t)i] = 0.5 * (std::erf(c * bt * (t + 0.5)) - std::erf(c * bt * (t - 0.5)));
    }
    const size_t len = (size_t)(nTones + 2) * nsps;
    std::vector<double> dphi(len, 0.0);
    const double peak = 2.0 * M_PI / nsps;                    // h = 1
    for (int j = 0; j < nTones; ++j)
        for (int i = 0; i < 3 * nsps; ++i) dphi[(size_t)j * nsps + i] += peak * pulse[(size_t)i] * tones[j];
    // ★ the first and last tones extended one symbol outwards, so the smoothing has nothing to ring against
    for (int i = 0; i < 2 * nsps; ++i) dphi[(size_t)i] += peak * tones[0] * pulse[(size_t)nsps + i];
    for (int i = 0; i < 2 * nsps; ++i) dphi[(size_t)nTones * nsps + i] += peak * tones[nTones - 1] * pulse[(size_t)i];
    const size_t nOut = (size_t)nTones * nsps;
    if (out.size() < at + nOut) out.resize(at + nOut, 0.0f);
    const int ramp = (int)std::lround(nsps / 8.0);
    double ph = phase0;
    const double w0 = 2.0 * M_PI * f0 / rate;
    for (size_t k = 0; k < nOut; ++k) {
        double env = 1.0;
        if ((int)k < ramp) env = 0.5 * (1.0 - std::cos(M_PI * (double)k / ramp));
        else if (k >= nOut - (size_t)ramp) env = 0.5 * (1.0 + std::cos(M_PI * (double)(k - (nOut - ramp)) / ramp));
        out[at + k] += (float)(amp * env * std::sin(ph));
        ph += dphi[(size_t)nsps + k] + w0;
        if (ph > 2 * M_PI) ph -= 2 * M_PI;
    }
}

const char* qualityWord(float q) { return q >= 0.7f ? "high" : q >= 0.45f ? "medium" : q >= 0.25f ? "low" : "poor"; }

// ── The kernels ───────────────────────────────────────────────────────────────────────────────────────────────────
// ★★ Every loop the receiver runs over the WHOLE passband, or per sample, is here: a NEON body (ARMv7-safe intrinsics
//    only, so the Pi 2 runs it), an SSE2 body (nothing newer, so a 32-bit Atom runs it) and the scalar reference.
namespace {
/** p[i] = |z[i]|² over interleaved complex bins. */
void power(const float* z, float* p, int n) {
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t v = vld2q_f32(z + 2 * i);
        vst1q_f32(p + i, vmlaq_f32(vmulq_f32(v.val[0], v.val[0]), v.val[1], v.val[1]));
    }
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) {
        __m128 re, im; vibedsp::sseLoad2(z + 2 * i, re, im);
        _mm_storeu_ps(p + i, _mm_add_ps(_mm_mul_ps(re, re), _mm_mul_ps(im, im)));
    }
#endif
    for (; i < n; ++i) p[i] = z[2 * i] * z[2 * i] + z[2 * i + 1] * z[2 * i + 1];
}
/** acc[i] += row[i] for i < n — the sync and total sums, one row of the spectrogram at a time. */
void addRow(float* acc, const float* row, int n) {
    int i = 0;
#if VIBE_NEON
    for (; i + 4 <= n; i += 4) vst1q_f32(acc + i, vaddq_f32(vld1q_f32(acc + i), vld1q_f32(row + i)));
#elif VIBE_SSE
    for (; i + 4 <= n; i += 4) _mm_storeu_ps(acc + i, _mm_add_ps(_mm_loadu_ps(acc + i), _mm_loadu_ps(row + i)));
#endif
    for (; i < n; ++i) acc[i] += row[i];
}
/** score[i] = 3·A[i] / max(T[i] − A[i], tiny): the sync tones' power against the other three tones' mean. */
void ratio(const float* A, const float* T, float* score, int n) {
    int i = 0;
#if VIBE_NEON
    const float32x4_t three = vdupq_n_f32(3.0f), tiny = vdupq_n_f32(1e-30f);
    for (; i + 4 <= n; i += 4) {
        const float32x4_t a = vld1q_f32(A + i);
        const float32x4_t d = vmaxq_f32(vsubq_f32(vld1q_f32(T + i), a), tiny);
        float32x4_t r = vrecpeq_f32(d);
        r = vmulq_f32(vrecpsq_f32(d, r), r);                      // ★ two Newton steps: ~1e-6 relative, plenty for a score
        r = vmulq_f32(vrecpsq_f32(d, r), r);
        vst1q_f32(score + i, vmulq_f32(vmulq_f32(a, three), r));
    }
#elif VIBE_SSE
    const __m128 three = _mm_set1_ps(3.0f), tiny = _mm_set1_ps(1e-30f);
    for (; i + 4 <= n; i += 4) {
        const __m128 a = _mm_loadu_ps(A + i);
        const __m128 d = _mm_max_ps(_mm_sub_ps(_mm_loadu_ps(T + i), a), tiny);
        _mm_storeu_ps(score + i, _mm_div_ps(_mm_mul_ps(a, three), d));
    }
#endif
    for (; i < n; ++i) score[i] = 3.0f * A[i] / std::max(T[i] - A[i], 1e-30f);
}
/** Σ a[i]·b[i] — the decimating FIR's taps. */
float dot(const float* a, const float* b, int n) {
    int i = 0;
    float s = 0;
#if VIBE_NEON
    float32x4_t acc0 = vdupq_n_f32(0), acc1 = vdupq_n_f32(0);
    for (; i + 8 <= n; i += 8) {
        acc0 = vmlaq_f32(acc0, vld1q_f32(a + i), vld1q_f32(b + i));
        acc1 = vmlaq_f32(acc1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
    }
    const float32x4_t acc = vaddq_f32(acc0, acc1);
    const float32x2_t h = vadd_f32(vget_low_f32(acc), vget_high_f32(acc));
    s = vget_lane_f32(vpadd_f32(h, h), 0);
#elif VIBE_SSE
    __m128 acc0 = _mm_setzero_ps(), acc1 = _mm_setzero_ps();
    for (; i + 8 <= n; i += 8) {
        acc0 = _mm_add_ps(acc0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
        acc1 = _mm_add_ps(acc1, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
    }
    s = vibedsp::sseAddv(_mm_add_ps(acc0, acc1));
#endif
    for (; i < n; ++i) s += a[i] * b[i];
    return s;
}

/** ln I₀(x), x ≥ 0 — Abramowitz & Stegun 9.8.1 / 9.8.2. The noncoherent FSK likelihood of one tone. */
float lnI0(float x) {
    if (x < 3.75f) {
        const float t = (x / 3.75f) * (x / 3.75f);
        return std::log(1.0f + t * (3.5156229f + t * (3.0899424f + t * (1.2067492f + t * (0.2659732f + t * (0.0360768f + t * 0.0045813f))))));
    }
    const float u = 3.75f / x;
    const float p = 0.39894228f + u * (0.01328592f + u * (0.00225319f + u * (-0.00157565f + u * (0.00916281f
                  + u * (-0.02057706f + u * (0.02635537f + u * (-0.01647633f + u * 0.00392377f)))))));
    return x - 0.5f * std::log(x) + std::log(p);
}
} // namespace

const char* Decoder::simdName() {
#if VIBE_NEON
    return "neon";
#elif VIBE_SSE
    return "sse2";
#else
    return "scalar";
#endif
}

// ── The receiver ──────────────────────────────────────────────────────────────────────────────────────────────────
struct Decoder::Impl {
    // geometry (12 kHz)
    static constexpr int kNsps = 384, kHop = 48, kRowsPerSym = kNsps / kHop, kN = 768;
    static constexpr int kRows = 512;                      // ★ ≥ one frame (472 rows) and some
    static constexpr int kScoreRows = 16;
    static constexpr int kRing = 1 << 16;                  // 5.46 s of audio
    static constexpr int kDec = 16, kSps = kNsps / kDec;   // 750 Hz, 24 samples a symbol
    static constexpr int kTaps = 97;                       // the decimating low-pass (group delay 48)
    static constexpr int kD = 6;                           // fine time search ± (decimated samples)
    static constexpr int kFrameSamples = kFrameSyms * kNsps;   // 22 656
    static constexpr double kBin = 12000.0 / kN;           // 15.625 Hz: half a tone

    Config cfg;
    float thr = 0;
    int bLo = 0, bHi = 0, nb = 0, nbStore = 0, nSearch = 0;
    PFFFT_Setup* setup = nullptr;
    float *fin = nullptr, *fout = nullptr, *work = nullptr;
    std::vector<float> spec;          // kRows × nbStore
    std::vector<float> score;         // kScoreRows × nSearch
    std::vector<float> accA, accT, pw;
    std::vector<float> ring;
    int64_t nAbs = 0;                 // samples fed
    int64_t nextRow = 0;              // the next spectrogram row to compute
    int64_t scoredTo = 0;             // rows whose score exists: [.., scoredTo)
    int64_t pickedTo = 0;             // rows peak-picked: [.., pickedTo)
    std::vector<float> taps;
    // fine stage scratch
    std::vector<float> mixRe, mixIm, yRe, yIm;
    // tone references for the fine search: [dfIndex][tone][n] (re, im)
    static constexpr int kNdf = 65; static constexpr double kDfStep = 0.25;   // ±8 Hz; the passes take every 6th, 2nd, 1st
    std::vector<float> refRe, refIm;

    struct Cand { int64_t ws; double f0; int64_t ready; bool cont; int range; double dfRange; bool recentred = false; };
    std::vector<Cand> pending;
    struct Done { int64_t ws; double f0; };
    std::deque<Done> done;

    // statistics
    uint64_t nCand = 0, nCont = 0, nFrames = 0, nCrcFail = 0;
    double scoreSum = 0; uint64_t scoreCells = 0, scoreAbove = 0;

    // the message/signal layer
    struct Sig {
        int id = 0; double f = 0; float snr = 0, q = 0;
        int64_t firstMs = 0, lastMs = 0; unsigned frames = 0;
        std::string call, cqCall;
        std::deque<std::string> msgs;     // finished messages
        bool open = false; std::string cur; int64_t nextSample = 0;
        std::map<std::string, int64_t> spotted;
    };
    std::vector<Sig> sigs;
    int nextId = 1;
    int64_t startMs = 0;

    std::function<void(const Frame&)>* onFrame = nullptr;
    std::function<void(int, double, const std::string&, bool)>* onMessage = nullptr;
    std::function<void(const Spot&)>* onSpot = nullptr;

    explicit Impl(const Config& c) : cfg(c) {
        thr = c.syncThreshold > 0 ? c.syncThreshold : kDefaultThreshold;
        bLo = std::max(1, (int)std::floor(c.loHz / kBin));
        bHi = std::min(kN / 2 - 7, (int)std::ceil(c.hiHz / kBin));
        nSearch = std::max(4, bHi - bLo + 1);
        nb = nSearch + 6;                               // ★ tone 3 of the top search bin
        nbStore = (nb + 3) & ~3;
        setup = pffft_new_setup(kN, PFFFT_REAL);
        fin = (float*)pffft_aligned_malloc(kN * sizeof(float));
        fout = (float*)pffft_aligned_malloc(kN * sizeof(float));
        work = (float*)pffft_aligned_malloc(kN * sizeof(float));
        spec.assign((size_t)kRows * nbStore, 0.0f);
        score.assign((size_t)kScoreRows * nSearch, 0.0f);
        accA.assign((size_t)nSearch + 4, 0.0f); accT.assign((size_t)nSearch + 4, 0.0f);
        pw.assign((size_t)kN / 2 + 8, 0.0f);
        ring.assign(kRing, 0.0f);
        // ★ The decimating low-pass: windowed sinc, cut-off 140 Hz (the signal is ±64 Hz around its centre plus up to
        //   ±8 Hz of error), Blackman — what folds back from 750 Hz ± 140 Hz is ≥ 70 dB down.
        taps.resize(kTaps);
        double sum = 0;
        for (int i = 0; i < kTaps; ++i) {
            const double m = i - (kTaps - 1) / 2.0, fc = 140.0 / 12000.0;
            const double s = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
            const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (kTaps - 1)) + 0.08 * std::cos(4 * M_PI * i / (kTaps - 1));
            taps[(size_t)i] = (float)(s * w); sum += s * w;
        }
        for (auto& t : taps) t = (float)(t / sum);
        refRe.resize((size_t)kNdf * 4 * kSps); refIm.resize(refRe.size());
        for (int d = 0; d < kNdf; ++d)
            for (int k = 0; k < 4; ++k)
                for (int n = 0; n < kSps; ++n) {
                    const double f = (k - 1.5) * kBaud + (d - kNdf / 2) * kDfStep;
                    const double ph = -2 * M_PI * f * n / (12000.0 / kDec);
                    refRe[(size_t)(d * 4 + k) * kSps + n] = (float)std::cos(ph);
                    refIm[(size_t)(d * 4 + k) * kSps + n] = (float)std::sin(ph);
                }
        startMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    ~Impl() {
        if (setup) pffft_destroy_setup(setup);
        if (fin) pffft_aligned_free(fin);
        if (fout) pffft_aligned_free(fout);
        if (work) pffft_aligned_free(work);
    }
    // ★ Measured on pure noise (vibeserver/test-jtty.cpp "threshold"): see docs/v12/JTTY.md §5 for the numbers.
    static constexpr float kDefaultThreshold = 2.6f;

    int64_t msAt(int64_t sample) const { return startMs + (int64_t)std::llround((double)sample * 1000.0 / cfg.rate); }
    float* row(int64_t t) { return spec.data() + (size_t)(t & (kRows - 1)) * nbStore; }
    float* scoreRow(int64_t t) { return score.data() + (size_t)(t & (kScoreRows - 1)) * nSearch; }
    float at(int64_t s) const { return s < 0 ? 0.0f : ring[(size_t)(s & (kRing - 1))]; }   // ★ before the stream: silence

    void feed(const float* x, int n) {
        for (int i = 0; i < n; ++i) {
            ring[(size_t)(nAbs & (kRing - 1))] = x[i];
            ++nAbs;
            // a row t covers samples [48t, 48t + 384)
            if (nAbs >= nextRow * kHop + kNsps) {
                computeRow(nextRow); ++nextRow; scoreAndPick();
                // ★ candidates are decoded as soon as their frame is in — never after a big feed has pushed it out of the ring
                if (!pending.empty() && readyAny()) runPending();
            }
        }
        expireMessages();
    }

    void computeRow(int64_t t) {
        const int64_t s0 = t * kHop;
        for (int i = 0; i < kNsps; ++i) fin[i] = at(s0 + i);
        std::memset(fin + kNsps, 0, (kN - kNsps) * sizeof(float));
        pffft_transform_ordered(setup, fin, fout, work, PFFFT_FORWARD);
        // ★ ordered real output: [DC, Nyquist, re1, im1, re2, im2 …] — bins from 1 are interleaved complex
        power(fout + 2 * bLo, row(t), nb);
    }

    void scoreAndPick() {
        // the score of row t needs rows t … t+96
        const int64_t t = nextRow - 1 - (kSyncSyms - 1) * kRowsPerSym;
        if (t < 0) return;
        std::fill(accA.begin(), accA.end(), 0.0f);
        std::fill(accT.begin(), accT.end(), 0.0f);
        for (int j = 0; j < kSyncSyms; ++j) {
            const float* r = row(t + j * kRowsPerSym);
            addRow(accA.data(), r + 2 * kSync[j], nSearch);
            for (int k = 0; k < 4; ++k) addRow(accT.data(), r + 2 * k, nSearch);
        }
        float* sc = scoreRow(t);
        ratio(accA.data(), accT.data(), sc, nSearch);
        for (int b = 0; b < nSearch; ++b) { scoreSum += sc[b]; if (sc[b] > thr) ++scoreAbove; }
        scoreCells += (uint64_t)nSearch;
        scoredTo = t + 1;
        // ★ peak-pick row p = t − 4: a local maximum over ±4 rows (half a symbol) and ±2 bins (one tone), above threshold
        const int64_t p = t - 4;
        if (p < 0) return;
        const float* sp = scoreRow(p);
        for (int b = 0; b < nSearch; ++b) {
            const float v = sp[b];
            if (v <= thr) continue;
            bool peak = true;
            for (int dt = -4; dt <= 4 && peak; ++dt) {
                if (p + dt < 0) continue;                       // ★ no rows before the stream began
                const float* o = scoreRow(p + dt);
                for (int db = -2; db <= 2; ++db) {
                    if (!dt && !db) continue;
                    const int bb = b + db;
                    if (bb < 0 || bb >= nSearch) continue;
                    const float w = o[bb];
                    // ★ ties go to the earlier cell, so a flat-topped peak is taken once
                    if (w > v || (w == v && (dt < 0 || (dt == 0 && db < 0)))) { peak = false; break; }
                }
            }
            if (!peak) continue;
            Cand c; c.ws = p * kHop; c.f0 = (bLo + b) * kBin; c.cont = false; c.range = kD; c.dfRange = 7.5;
            c.ready = c.ws + (int64_t)(kFrameSyms + 1) * kNsps + kTaps;
            pending.push_back(c);
        }
        pickedTo = p + 1;
    }

    bool readyAny() const { for (auto& c : pending) if (c.ready <= nAbs) return true; return false; }
    bool isDone(int64_t ws, double f0) const {
        for (auto& d : done) if (std::llabs(d.ws - ws) < kNsps && std::fabs(d.f0 - f0) < 20.0) return true;
        return false;
    }

    void runPending() {
        if (pending.empty()) return;
        // ★ oldest first; a continuation before a sync candidate for the same instant (it is the cheaper search)
        std::stable_sort(pending.begin(), pending.end(), [](const Cand& a, const Cand& b) {
            return a.ready != b.ready ? a.ready < b.ready : (a.cont && !b.cont); });
        size_t k = 0;
        for (; k < pending.size(); ++k) {
            const Cand c = pending[k];
            if (c.ready > nAbs) break;
            if (c.ws - kNsps - kD * kDec - kTaps < nAbs - kRing + 1) continue;   // ★ fell out of the ring (a stall): skip
            if (isDone(c.ws, c.f0)) continue;
            if (c.cont) ++nCont; else ++nCand;
            decodeAt(c);
        }
        pending.erase(pending.begin(), pending.begin() + (long)k);
        while (!done.empty() && done.front().ws < nAbs - 30 * 12000) done.pop_front();
    }

    /** ★ The fine stage: mix the candidate's 127 Hz down to 0, low-pass and decimate to 750 Hz, search the sync finely,
     *  then the soft metrics → the tail-biting list Viterbi → the CRC and grammar. */
    bool decodeAt(const Cand& c) {
        const int D = c.range;
        const double fc = c.f0 + 1.5 * kBaud;                   // the middle of the four tones
        const int64_t o = c.ws - kNsps - (int64_t)D * kDec;     // decimated sample 0 is here
        const int M = kSps + 2 * D + kFrameSyms * kSps + 2;
        const int64_t m0 = o - kTaps / 2;
        const int L = (M - 1) * kDec + kTaps;
        mixRe.resize((size_t)L); mixIm.resize((size_t)L);
        {   // ★ the mixer: a phasor stepped in double, renormalised — 23 k samples, once per candidate
            const double w = -2 * M_PI * fc / cfg.rate;
            double cr = 1, ci = 0;
            const double sr = std::cos(w), si = std::sin(w);
            for (int i = 0; i < L; ++i) {
                const float x = at(m0 + i);
                mixRe[(size_t)i] = (float)(x * cr); mixIm[(size_t)i] = (float)(x * ci);
                const double nr = cr * sr - ci * si; ci = cr * si + ci * sr; cr = nr;
                if ((i & 1023) == 1023) { const double g = 1.0 / std::sqrt(cr * cr + ci * ci); cr *= g; ci *= g; }
            }
        }
        yRe.resize((size_t)M); yIm.resize((size_t)M);
        for (int m = 0; m < M; ++m) {
            yRe[(size_t)m] = dot(taps.data(), mixRe.data() + (size_t)m * kDec, kTaps);
            yIm[(size_t)m] = dot(taps.data(), mixIm.data() + (size_t)m * kDec, kTaps);
        }
        // symbol j of a hypothesis d starts at decimated sample kSps + D + d + 24 j
        auto corr = [&](int start, int dfi, int k, float& re, float& im) {
            const float* rr = refRe.data() + (size_t)(dfi * 4 + k) * kSps;
            const float* ri = refIm.data() + (size_t)(dfi * 4 + k) * kSps;
            const float* yr = yRe.data() + start; const float* yi = yIm.data() + start;
            // (yr + j yi)(rr + j ri)
            re = dot(yr, rr, kSps) - dot(yi, ri, kSps);
            im = dot(yr, ri, kSps) + dot(yi, rr, kSps);
        };
        // ★ Where it is, in three passes, each sharper than the last:
        //   COARSE — the 13 sync symbols, noncoherently, every 1.5 Hz and every decimated sample (1/24 symbol);
        //   FINE   — all 59 symbols (sync tones where known, the strongest tone where not), ±1 Hz in 0.5 Hz, ±2 samples;
        //   COHERENT (later, once there are tones to follow) — the phase itself: h = 1 makes a JTTY transmitter
        //            phase-continuous, so the right tones line up in phase across the frame and their coherent sum
        //            pins frequency to a fraction of a hertz, which the 31 Hz-wide tone filters never could.
        const int dfMax = std::min(kNdf / 2, (int)std::lround(c.dfRange / kDfStep));
        const int mid = kNdf / 2;
        float best = -1; int bd = 0, bf = mid;
        for (int dfi = mid - dfMax; dfi <= mid + dfMax; dfi += 6)
            for (int d = -D; d <= D; ++d) {
                float s = 0;
                for (int j = 0; j < kSyncSyms; ++j) {
                    float re, im; corr(kSps + D + d + j * kSps, dfi, kSync[j], re, im);
                    s += re * re + im * im;
                }
                if (s > best) { best = s; bd = d; bf = dfi; }
            }
        {
            const int cd = bd, cf = bf;
            best = -1;
            for (int dfi = std::max(0, cf - 4); dfi <= std::min(kNdf - 1, cf + 4); dfi += 2)
                for (int d = std::max(-D, cd - 2); d <= std::min(D, cd + 2); ++d) {
                    float s = 0;
                    for (int j = 0; j < kFrameSyms; ++j) {
                        const int st = kSps + D + d + j * kSps;
                        if (j < kSyncSyms) { float re, im; corr(st, dfi, kSync[j], re, im); s += re * re + im * im; continue; }
                        float mx = 0;
                        for (int k = 0; k < 4; ++k) { float re, im; corr(st, dfi, k, re, im); mx = std::max(mx, re * re + im * im); }
                        s += mx;
                    }
                    if (s > best) { best = s; bd = d; bf = dfi; }
                }
        }
        // ★ The best was at the edge of the search: the real frame lies beyond it (a sidelobe of the sync, or a peak of the
        //   coarse grid that belongs to a neighbouring cell, can win the cell). Search again centred there — once.
        if (!c.recentred && (bd == -D || bd == D || bf <= mid - dfMax + 1 || bf >= mid + dfMax - 1)) {
            Cand c2 = c; c2.recentred = true;
            c2.ws = c.ws + (int64_t)bd * kDec; c2.f0 = c.f0 + (bf - mid) * kDfStep;
            if (!isDone(c2.ws, c2.f0)) return decodeAt(c2);
            return false;
        }
        // every symbol's four tones at (d, df)
        float P[kFrameSyms][4], Zr[kFrameSyms][4], Zi[kFrameSyms][4];
        auto measure = [&](int d, int dfi) {
            // ★ h = 1: each symbol advances the phase by 2π(k − 1.5) ≡ π about the centre whatever its tone, plus 2π·δf·T
            //   for an offset δf — the reference restarts every symbol, so that much is put back here. Then (−1)^j z has
            //   the SAME phase all frame long on the transmitted tone (drifting only with what error is left).
            const double step = -2 * M_PI * (dfi - mid) * kDfStep / kBaud + M_PI;
            for (int j = 0; j < kFrameSyms; ++j) {
                const float rc = (float)std::cos(step * j), rs = (float)std::sin(step * j);
                for (int k = 0; k < 4; ++k) {
                    float re, im; corr(kSps + D + d + j * kSps, dfi, k, re, im);
                    const float r2 = re * rc - im * rs, i2 = re * rs + im * rc;
                    Zr[j][k] = r2; Zi[j][k] = i2; P[j][k] = r2 * r2 + i2 * i2;
                }
            }
        };
        measure(bd, bf);
        double sig = 0, noise = 0;
        for (int j = 0; j < kSyncSyms; ++j)
            for (int k = 0; k < 4; ++k) (k == kSync[j] ? sig : noise) += P[j][k];
        sig /= kSyncSyms; noise /= 3.0 * kSyncSyms;
        if (!(noise > 1e-20)) return false;
        const double syncRatio = sig / noise;
        if (syncRatio < (c.cont ? 1.6 : 1.8)) return false;
        const double a2 = std::max(sig - noise, 0.05 * noise);
        const float snr = (float)(10.0 * std::log10(a2 / noise * (kBaud / 2500.0)));
        const float g = (float)(2.0 * std::sqrt(a2) / noise);

        /* ★★ COHERENCE of a tone sequence (computed without measuring the other three tones — a quarter of the work): blocks of eight symbols summed WITH their phases, against the same symbols'
         *    powers summed without. Noise — and any codeword that is not really there — gives ≈ 1 whatever the decoder
         *    chose, its phases being random. A JTTY frame gives ≈ (1 + 8·Es/N0)/(1 + Es/N0): five or more at −16 dB. The
         *    CRC cannot tell a lucky path through noise from a frame; this can, because h = 1 makes the sender coherent. */
        auto coherenceAt = [&](int d, int dfi, const uint8_t* tones) {
            const double step = -2 * M_PI * (dfi - mid) * kDfStep / kBaud + M_PI;
            const float sr1 = (float)std::cos(step), si1 = (float)std::sin(step);
            float rc = 1, rs = 0;
            double num = 0, den = 0, br = 0, bi = 0;
            for (int j = 0; j < kFrameSyms; ++j) {
                float re, im; corr(kSps + D + d + j * kSps, dfi, tones[j], re, im);
                const float r2 = re * rc - im * rs, i2 = re * rs + im * rc;
                br += r2; bi += i2; den += r2 * r2 + i2 * i2;
                if ((j & 7) == 7 || j == kFrameSyms - 1) { num += br * br + bi * bi; br = bi = 0; }
                const float nr = rc * sr1 - rs * si1; rs = rc * si1 + rs * sr1; rc = nr;
            }
            return den > 0 ? (float)(num / den) : 0.0f;
        };
        /* Follow the phase: the whole ±8 Hz in 0.25 Hz steps (an offset in time can masquerade as one in frequency to
         * the tone filters, never to the phase), then time, then both finely. Leaves Zr/Zi/P measured at the best. */
        auto followPhase = [&](const uint8_t* tones) {
            float bestC = -1;
            auto tryAt = [&](int d, int dfi) {
                if (dfi < 0 || dfi >= kNdf || d < -D || d > D) return;
                const float ch = coherenceAt(d, dfi, tones);
                if (ch > bestC) { bestC = ch; cd = d; cf = dfi; }
            };
            const int d0 = cd;
            for (int dfi = 0; dfi < kNdf; ++dfi) tryAt(d0, dfi);
            const int f1 = cf;
            for (int d = -D; d <= D; ++d) if (d != d0) tryAt(d, f1);
            const int d1 = cd;
            for (int dfi = f1 - 4; dfi <= f1 + 4; ++dfi) if (dfi != f1) tryAt(d1, dfi);
            const int f2 = cf;
            for (int d = d1 - 1; d <= d1 + 1; d += 2) tryAt(d, f2);
            measure(cd, cf);
            return bestC;
        };

        float met[kDataSyms][4];
        for (int j = 0; j < kDataSyms; ++j)
            for (int k = 0; k < 4; ++k) met[j][k] = lnI0(g * std::sqrt(P[kSyncSyms + j][k]));
        std::vector<uint64_t> paths;
        listViterbi(met, cfg.listSize, paths);
        if (paths.empty()) { ++nCrcFail; return false; }

        // ── test one path: CRC, reserved bit, the whole grammar, then the coherence of its tones ──
        auto tryPath = [&](uint64_t info, int rank, bool coherentPass) {
            const uint64_t bits34 = info >> kCrcBits;
            if (bits34 & 2u) return false;                          // reserved bit
            if (crc12(bits34) != (uint16_t)(info & 0xFFFu)) return false;
            Atom a; bool eom;
            if (!unpackAtom(bits34, a, eom)) return false;          // ★ the WHOLE grammar, before anything is shown
            uint8_t tones[kFrameSyms];
            encodeFrame(bits34, tones);
            const float coh = followPhase(tones);
            int agree = 0;
            for (int j = 0; j < kDataSyms; ++j) {
                const float* p = P[kSyncSyms + j];
                int mx = 0; for (int k = 1; k < 4; ++k) if (p[k] > p[mx]) mx = k;
                if (mx == tones[kSyncSyms + j]) ++agree;
            }
            /* ★★ THE FRAME MUST BE CONSISTENT WITH ITSELF (docs/v12/JTTY.md §5). Its sync says how strong it is (Es/N0);
             *    a real frame that strong puts its tones where the decoder says at a rate that follows from it (4-FSK
             *    noncoherent detection), and lines them up in phase. Noise that slipped a path through the CRC fails the
             *    first on the weak side, the coherence on all sides; a STRONG coherent interferer (an FT8 signal lights
             *    a sync very brightly) fails the agreement. A weak frame must also be coherent; a strong one need not
             *    (fading may scramble its phase), but then its agreement must be what its strength promises. */
            const double esn0 = a2 / noise;
            // ★ predicted from HALF the sync's Es/N0: a peak that was picked from noise reads high, and GFSK, residual timing
            //   and frequency cost a real frame some symbols — the test only has to catch what is grossly inconsistent
            const double pc = probCorrect4(0.5 * esn0);
            // ★ capped at 38 of 46: a strong real frame keeps a few symbols for QRM, clicks and a fade
            const double minAgree = std::min(38.0, kDataSyms * pc - 3.0 * std::sqrt(kDataSyms * pc * (1 - pc)) - 2.0);
            if (esn0 >= 6.0 && agree < minAgree) return false;   // ★ weaker than that, the coherence gates below decide
            if (syncRatio < kStrongSync) {
                if (coh < (coherentPass ? kMinCoherenceCoherent : kMinCoherence)) return false;
                if (coherentPass && agree < kMinAgreeCoherent) return false;
            }
            Frame f;
            f.sample = c.ws + (int64_t)cd * kDec;                   // ★ the sync's first sample, to 1/24 symbol
            f.audioHz = c.f0 + (cf - mid) * kDfStep;
            f.snrDb = snr; f.rank = rank; f.agree = agree; f.eom = eom; f.bits = bits34; f.atom = a;
            f.continuation = c.cont;
            // ★ confidence: how far the coherence stands above what noise reaches, and how cleanly the tones decode
            const float qc = std::min(1.0f, std::max(0.0f, (coh - kMinCoherence) / 8.0f));
            const float qa = std::min(1.0f, std::max(0.0f, ((float)agree / kDataSyms - 0.5f) / 0.45f));
            f.quality = 0.5f * qc + 0.5f * qa;
            f.utcMs = msAt(f.sample);
            accept(f);
            return true;
        };
        int rank = 0;
        for (uint64_t info : paths) if (tryPath(info, ++rank, false)) return true;

        /* ★★ THE COHERENT PASS — when no noncoherent path passed. The best path's tones (most of them right even when the
         *    CRC fails) say where the energy is; following their phase gives the carrier phase symbol by symbol, and with
         *    it the COHERENT likelihood of each tone, 2·A·Re(z·e^−jψ)/N — worth well over a dB where it matters. */
        {
            uint8_t tones[kFrameSyms];
            const uint64_t info = paths.front();
            // re-encode the decided info bits without the CRC check: the tones the decoder believes
            {
                uint8_t b[kInfoBits];
                for (int i = 0; i < kInfoBits; ++i) b[i] = (uint8_t)((info >> (kInfoBits - 1 - i)) & 1u);
                uint32_t st = 0;
                for (int i = kInfoBits - 9; i < kInfoBits; ++i) st = ((st << 1) | b[i]) & (kStates - 1);
                for (int i = 0; i < kSyncSyms; ++i) tones[i] = kSync[i];
                for (int t = 0; t < kInfoBits; ++t) {
                    const uint32_t reg = ((st << 1) | b[t]) & 0x3FFu;
                    tones[kSyncSyms + t] = toneOfReg(reg); st = reg & (kStates - 1);
                }
            }
            followPhase(tones);
            // phase reference per symbol: the decided tones' (−1)^j z over ±kPhaseWin symbols, the symbol itself left out
            float cr[kFrameSyms], ci[kFrameSyms];
            for (int j = 0; j < kFrameSyms; ++j) { cr[j] = Zr[j][tones[j]]; ci[j] = Zi[j][tones[j]]; }
            const float gc = (float)(2.0 * std::sqrt(a2) / noise);
            for (int j = 0; j < kDataSyms; ++j) {
                const int jj = kSyncSyms + j;
                double sr = 0, si = 0;
                for (int i = std::max(0, jj - kPhaseWin); i <= std::min(kFrameSyms - 1, jj + kPhaseWin); ++i)
                    if (i != jj) { sr += cr[i]; si += ci[i]; }
                const double mag = std::sqrt(sr * sr + si * si);
                const float ur = mag > 0 ? (float)(sr / mag) : 1.0f, ui = mag > 0 ? (float)(si / mag) : 0.0f;
                for (int k = 0; k < 4; ++k) met[j][k] = gc * (Zr[jj][k] * ur + Zi[jj][k] * ui);
            }
            listViterbi(met, cfg.listSize, paths);
            rank = 0;
            for (uint64_t p : paths) if (tryPath(p, ++rank, true)) return true;
        }
        ++nCrcFail;
        return false;
    }
    // ★ docs/v12/JTTY.md §5 (measured over ten hours of white noise, ~850 000 candidates): a CRC-passing path through
    //   noise reaches coherence 2.8 from the noncoherent pass and 4.1 from the coherent one (that pass chose tones BY
    //   their phase, so some coherence is self-made) — and its strongest-tone agreement stays ≤ 22 of 46. Real frames
    //   at −15 to −18 dB: coherence 4.6–6.1, agreement ≥ 26. A gate per pass, both measures on the coherent one.
    static constexpr float kMinCoherence = 3.2f;
    static constexpr float kMinCoherenceCoherent = 4.5f;
    static constexpr int kMinAgreeCoherent = 24;
    /** P(the strongest of 4 noncoherent tones is the sent one) at Es/N0 = e — the classic closed form. */
    static double probCorrect4(double e) {
        return 1.0 - 1.5 * std::exp(-e / 2.0) + 1.0 * std::exp(-2.0 * e / 3.0) - 0.25 * std::exp(-3.0 * e / 4.0);
    }
    static constexpr float kStrongSync = 5.0f;     // a fine sync ratio noise has not been seen to reach
    static constexpr int kPhaseWin = 8;

    /** ★★ The tail-biting list Viterbi: two circular passes (the second starts from the first's end metrics, so every
     *  state starts with a fair estimate of where the circle closes), then EVERY end state traced back; the paths that
     *  close on themselves (start state = end state) are the tail-biting codewords, best metric first, duplicates out. */
    void listViterbi(const float met[kDataSyms][4], int listSize, std::vector<uint64_t>& out) {
        // ★ the tone on the branch INTO state ns from the predecessor that dropped bit d (built once, thread-safe)
        struct Tab { uint8_t t[kStates][2]; Tab() {
            for (int ns = 0; ns < kStates; ++ns)
                for (int d = 0; d < 2; ++d) t[ns][d] = toneOfReg((uint32_t)ns | ((uint32_t)d << 9)); } };
        static const Tab tab;
        const auto& toneTab = tab.t;
        float pm[kStates], npm[kStates];
        uint8_t dec[kDataSyms][kStates];
        std::fill(pm, pm + kStates, 0.0f);
        for (int pass = 0; pass < 2; ++pass) {
            for (int t = 0; t < kDataSyms; ++t) {
                const float* m = met[t];
                float mx = -1e30f;
                for (int ns = 0; ns < kStates; ++ns) {
                    const int p0 = ns >> 1, p1 = p0 | 256;
                    const float m0 = pm[p0] + m[toneTab[ns][0]], m1 = pm[p1] + m[toneTab[ns][1]];
                    const bool one = m1 > m0;
                    npm[ns] = one ? m1 : m0; dec[t][ns] = (uint8_t)one;
                    if (npm[ns] > mx) mx = npm[ns];
                }
                for (int s = 0; s < kStates; ++s) pm[s] = npm[s] - mx;
            }
        }
        struct P { float m; uint64_t bits; };
        std::vector<P> cands; cands.reserve(64);
        for (int s = 0; s < kStates; ++s) {
            int cur = s; uint64_t bits = 0;
            for (int t = kDataSyms - 1; t >= 0; --t) {
                bits |= (uint64_t)(cur & 1) << (kDataSyms - 1 - t);
                cur = (cur >> 1) | (dec[t][cur] << 8);
            }
            if (cur != s) continue;
            cands.push_back({ pm[s], bits });
        }
        std::sort(cands.begin(), cands.end(), [](const P& a, const P& b) { return a.m > b.m; });
        out.clear();
        for (auto& c : cands) {
            if ((int)out.size() >= listSize) break;
            if (std::find(out.begin(), out.end(), c.bits) != out.end()) continue;
            out.push_back(c.bits);
        }
    }

    // ── frames → messages, signals and spots ──
    void accept(Frame& f) {
        ++nFrames;
        done.push_back({ f.sample, f.audioHz });
        Sig* s = nullptr; double bestD = 1e9;
        for (auto& g : sigs) { const double d = std::fabs(g.f - f.audioHz); if (d < 12.0 && d < bestD) { bestD = d; s = &g; } }
        if (!s) {
            Sig g; g.id = nextId++; g.f = f.audioHz; g.firstMs = f.utcMs;
            sigs.push_back(g); s = &sigs.back();
        }
        f.signalId = s->id;
        s->f = s->frames ? 0.7 * s->f + 0.3 * f.audioHz : f.audioHz;
        s->lastMs = f.utcMs; ++s->frames;
        s->snr = s->frames > 1 ? 0.6f * s->snr + 0.4f * f.snrDb : f.snrDb;
        s->q = s->frames > 1 ? 0.6f * s->q + 0.4f * f.quality : f.quality;
        if (onFrame && *onFrame) (*onFrame)(f);
        // the message
        const int64_t tol = 1500;                               // 0.125 s
        if (s->open) {
            const int64_t gap = f.sample - s->nextSample;
            if (std::llabs(gap) <= tol) { /* the next frame */ }
            else if (gap > 0 && gap < 3 * kFrameSamples + tol && std::llabs(gap % kFrameSamples) <= tol) {
                s->cur += "… ";                                 // ★ frames missed in between
            } else closeMessage(*s, false);
        }
        if (!s->open) { s->open = true; s->cur.clear(); }
        s->cur += f.atom.kind == Kind::Text5 ? f.atom.text : render(f.atom) + " ";
        s->nextSample = f.sample + kFrameSamples;
        // the spot
        if (f.atom.kind == Kind::Call) {
            const bool cq = f.atom.sub == kCq || f.atom.sub == kTuCq;
            if (cq) s->cqCall = f.atom.text;
            s->call = !s->cqCall.empty() ? s->cqCall : f.atom.text;
            const std::string how = cq ? "CQ" : "QSO";
            const std::string key = f.atom.text + "|" + how;
            auto it = s->spotted.find(key);
            if (it == s->spotted.end() || f.utcMs - it->second > 600000) {
                s->spotted[key] = f.utcMs;
                Spot sp; sp.call = f.atom.text; sp.audioHz = f.audioHz; sp.snrDb = f.snrDb; sp.how = how;
                sp.utcMs = f.utcMs; sp.signalId = s->id;
                if (onSpot && *onSpot) (*onSpot)(sp);
            }
        }
        if (f.eom) closeMessage(*s, true);
        else if (onMessage && *onMessage) (*onMessage)(s->id, s->f, trimmed(s->cur), false);
        // ★ step 5: the next frame of this transmission is 59 symbols on, at this frequency — try it directly
        if (cfg.continuation) {
            Cand c; c.ws = f.sample + kFrameSamples; c.f0 = f.audioHz; c.cont = true; c.range = 3; c.dfRange = 3.0;
            c.ready = c.ws + (int64_t)(kFrameSyms + 1) * kNsps + kTaps;
            pending.push_back(c);
        }
    }
    static std::string trimmed(std::string s) { while (!s.empty() && s.back() == ' ') s.pop_back(); return s; }
    void closeMessage(Sig& s, bool complete) {
        if (!s.open) return;
        s.open = false;
        std::string t = trimmed(s.cur);
        if (!complete) t += " …";
        if (onMessage && *onMessage) (*onMessage)(s.id, s.f, t, true);
        s.msgs.push_back(t);
        while (s.msgs.size() > 6) s.msgs.pop_front();
        s.cur.clear();
    }
    void expireMessages() {
        for (auto& s : sigs)
            if (s.open && nAbs > s.nextSample + 2 * kFrameSamples) closeMessage(s, false);
        const int64_t now = msAt(nAbs);
        sigs.erase(std::remove_if(sigs.begin(), sigs.end(), [&](const Sig& s) {
            return !s.open && now - s.lastMs > (int64_t)cfg.keepGoneSec * 1000; }), sigs.end());
    }
};

Decoder::Decoder(const Config& c) : d_(new Impl(c)) {
    d_->onFrame = &onFrame; d_->onMessage = &onMessage; d_->onSpot = &onSpot;
}
Decoder::~Decoder() = default;
void Decoder::feed(const float* x, int n) { if (n > 0) d_->feed(x, n); }
void Decoder::setStartUtcMs(int64_t ms) { d_->startMs = ms; }
int64_t Decoder::nowMs() const { return d_->msAt(d_->nAbs); }
uint64_t Decoder::samples() const { return (uint64_t)d_->nAbs; }
uint64_t Decoder::candidates() const { return d_->nCand; }
uint64_t Decoder::continuations() const { return d_->nCont; }
uint64_t Decoder::framesDecoded() const { return d_->nFrames; }
uint64_t Decoder::crcRejected() const { return d_->nCrcFail; }
float Decoder::threshold() const { return d_->thr; }
int Decoder::activeSignals() const {
    int n = 0; const int64_t now = nowMs();
    for (auto& s : d_->sigs) if (s.open || now - s.lastMs < 30000) ++n;
    return n;
}
Decoder::Stats Decoder::scoreStats() const {
    Stats s; s.cells = d_->scoreCells; s.above = d_->scoreAbove;
    s.meanScore = d_->scoreCells ? d_->scoreSum / (double)d_->scoreCells : 0.0;
    return s;
}
std::vector<SignalView> Decoder::signals(bool includeGone) const {
    std::vector<SignalView> v;
    const int64_t now = nowMs();
    for (auto& s : d_->sigs) {
        SignalView r;
        r.id = s.id; r.audioHz = s.f; r.snrDb = s.snr; r.quality = s.q; r.firstMs = s.firstMs; r.lastMs = s.lastMs;
        r.call = s.call; r.frames = s.frames;
        r.active = s.open || now - s.lastMs < 30000;            // ★ heard in the last 30 s
        if (!r.active && !includeGone) continue;
        std::string t;
        for (auto& m : s.msgs) { if (!t.empty()) t += "  ·  "; t += m; }
        if (s.open) { if (!t.empty()) t += "  ·  "; t += Impl::trimmed(s.cur); }
        if (t.size() > 240) t.erase(0, t.size() - 240);
        r.text = t;
        v.push_back(r);
    }
    std::sort(v.begin(), v.end(), [](const SignalView& a, const SignalView& b) { return a.audioHz < b.audioHz; });
    return v;
}

} // namespace jtty
} // namespace vibe
