// VibeSDR V12 — JS8 frames. See js8_message.h.
#include "js8_message.h"
#include <cstdio>
#include <cstring>

namespace vibe {
namespace js8 {
namespace {

// ★ The call/grid alphabet of JS8's packers: digits, letters, space, '/', '@' (indices 0-35, 36, 37, 38).
const char kAlnum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@";
int alnumIndex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    if (c == ' ') return 36;
    if (c == '/') return 37;
    if (c == '@') return 38;
    return -1;
}

uint64_t getBits(const uint8_t* b, int at, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1) | (b[at + i] & 1u);
    return v;
}
void putBits(uint8_t* b, int at, int n, uint64_t v) {
    for (int i = n - 1; i >= 0; --i) { b[at + i] = (uint8_t)(v & 1u); v >>= 1; }
}

// ★ A 28-bit call is 37·36·10·27·27·27 = 262,177,560 ordinary calls; the values just above are names.
constexpr uint32_t kBaseCall = 37u * 36u * 10u * 27u * 27u * 27u;
/* The names above kBaseCall, in order from kBaseCall + 1 (protocol facts: JS8's groups). */
const char* const kGroups[] = {
    "<....>", "@ALLCALL", "@JS8NET",
    "@DX/NA", "@DX/SA", "@DX/EU", "@DX/AS", "@DX/AF", "@DX/OC", "@DX/AN",
    "@REGION/1", "@REGION/2", "@REGION/3",
    "@GROUP/0", "@GROUP/1", "@GROUP/2", "@GROUP/3", "@GROUP/4", "@GROUP/5", "@GROUP/6", "@GROUP/7", "@GROUP/8",
    "@GROUP/9",
    "@COMMAND", "@CONTROL", "@NET", "@NTS",
    "@RESERVE/0", "@RESERVE/1", "@RESERVE/2", "@RESERVE/3", "@RESERVE/4",
    "@APRSIS", "@RAGCHEW", "@JS8", "@EMCOMM", "@ARES", "@MARS", "@AMRRON", "@RACES", "@RAYNET", "@RADAR",
    "@SKYWARN", "@CQ", "@HB", "@QSO", "@QSOPARTY", "@CONTEST", "@FIELDDAY", "@SOTA", "@IOTA", "@POTA", "@QRP",
    "@QRO",
};
constexpr int kNumGroups = (int)(sizeof kGroups / sizeof kGroups[0]);

/* The 32 directed commands by index, as they read after the addressee (protocol facts). */
const char* const kCommands[32] = {
    " SNR?", " DIT DIT", " NACK", " HEARING?", " GRID?", ">", " STATUS?", " STATUS",
    " HEARING", " MSG", " MSG TO:", " QUERY", " QUERY MSGS", " QUERY CALL", " ACK", " GRID",
    " INFO?", " INFO", " FB", " HW CPY?", " SK", " RR", " QSL?", " QSL",
    " CMD", " SNR", " NO", " YES", " 73", " HEARTBEAT SNR", " AGN?", " ",
};
bool isSnrCommand(int i) { return i == 25 || i == 29; }

/* CQ wordings by their 3-bit index (protocol facts). */
const char* const kCq[8] = { "CQ CQ CQ", "CQ DX", "CQ QRP", "CQ CONTEST", "CQ FIELD", "CQ FD", "CQ CQ", "CQ" };

/* ★ The uncompressed text code: a complete prefix code over 44 characters, shortest for the commonest (space,
 *  E, T, A, O …). Protocol facts, typed in; test-js8 checks it is prefix-free and complete (Kraft sum exactly 1). */
struct HuffCode { char ch; const char* bits; };
const HuffCode kHuff[] = {
    {' ', "01"}, {'E', "100"}, {'T', "1101"}, {'A', "0011"}, {'O', "11111"}, {'I', "11100"}, {'N', "10111"},
    {'S', "10100"}, {'H', "00011"}, {'R', "00000"}, {'D', "111011"}, {'L', "110011"}, {'C', "110001"},
    {'U', "101101"}, {'M', "101011"}, {'W', "001011"}, {'F', "001001"}, {'G', "000101"}, {'Y', "000011"},
    {'P', "1111011"}, {'B', "1111001"}, {'.', "1110100"}, {'V', "1100101"}, {'K', "1100100"}, {'-', "1100001"},
    {'+', "1100000"}, {'?', "1011001"}, {'!', "1011000"}, {'"', "1010101"}, {'X', "1010100"}, {'0', "0010101"},
    {'J', "0010100"}, {'1', "0010001"}, {'Q', "0010000"}, {'2', "0001001"}, {'Z', "0001000"}, {'3', "0000101"},
    {'5', "0000100"}, {'4', "11110101"}, {'9', "11110100"}, {'8', "11110001"}, {'6', "11110000"},
    {'7', "11101011"}, {'/', "11101010"},
};
constexpr int kNumHuff = (int)(sizeof kHuff / sizeof kHuff[0]);

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == ' ') ++a;
    while (b > a && s[b - 1] == ' ') --b;
    return s.substr(a, b - a);
}

std::string formatSnr(int snr) {
    if (snr < -60 || snr > 60) return "";
    char buf[8];
    std::snprintf(buf, sizeof buf, snr >= 0 ? "+%02d" : "-%02d", snr >= 0 ? snr : -snr);
    return buf;
}

/* ★ Maidenhead from degrees, the WSJT way round: longitude counted WEST-positive. JS8's 15-bit grid stores
 *  ((west-lon + 180) / 2) · 180 + (lat + 90) of the square's corner, whole degrees. */
std::string deg2grid4(double westLon, double lat) {
    if (westLon < -180) westLon += 360;
    if (westLon > 180) westLon -= 360;
    const int nlong = (int)(60.0 * (180.0 - westLon) / 5.0);
    const int nlat = (int)(60.0 * (lat + 90.0) / 2.5);
    std::string g(4, ' ');
    g[0] = (char)('A' + nlong / 240);
    g[2] = (char)('0' + (nlong % 240) / 24);
    g[1] = (char)('A' + nlat / 240);
    g[3] = (char)('0' + (nlat % 240) / 24);
    return g;
}

bool isGrid4(const std::string& g) {
    return g.size() == 4 && g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R'
        && g[2] >= '0' && g[2] <= '9' && g[3] >= '0' && g[3] <= '9';
}
} // namespace

const char* frameKindName(FrameKind k) {
    switch (k) {
        case FrameKind::Heartbeat: return "heartbeat";
        case FrameKind::Cq: return "cq";
        case FrameKind::Compound: return "compound";
        case FrameKind::CompoundDirected: return "compound-directed";
        case FrameKind::Directed: return "directed";
        case FrameKind::Data: return "data";
        case FrameKind::DataCompressed: return "data-compressed";
    }
    return "?";
}

const char* commandWord(int i) { return (i >= 0 && i < 32) ? kCommands[i] : ""; }

// ── calls ──
uint32_t packCall28(const std::string& in, bool* portable) {
    std::string c;
    for (char ch : in) c += (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch);
    c = trim(c);
    for (int g = 0; g < kNumGroups; ++g) if (c == kGroups[g]) return kBaseCall + 1 + (uint32_t)g;
    if (portable) *portable = false;
    if (c.size() > 2 && c.compare(c.size() - 2, 2, "/P") == 0) { c.resize(c.size() - 2); if (portable) *portable = true; }
    if (c.rfind("3DA0", 0) == 0) c = "3D0" + c.substr(4);                       // Eswatini: a 4-character prefix
    if (c.rfind("3X", 0) == 0 && c.size() > 2 && c[2] >= 'A' && c[2] <= 'Z') c = "Q" + c.substr(2);   // Guinea
    if (c.size() < 2 || c.size() > 6) return 0;
    // ★ Six places: [digit-letter-space][digit-letter][DIGIT][letter-space]×3. A short call goes in with a space in
    //   front (when its digit is second) or after; the last arrangement that fits is the one.
    auto fits = [](const std::string& w) {
        if (w.size() != 6) return false;
        const int i0 = alnumIndex(w[0]), i1 = alnumIndex(w[1]);
        if (i0 < 0 || i0 > 36 || i1 < 0 || i1 > 35 || w[2] < '0' || w[2] > '9') return false;
        for (int k = 3; k < 6; ++k) if (!((w[k] >= 'A' && w[k] <= 'Z') || w[k] == ' ')) return false;
        return true;
    };
    std::string pick;
    std::string cands[3] = { c, " " + c, c };
    cands[1].resize(6, ' '); cands[2].resize(6, ' ');
    for (const std::string& w : cands) if (fits(w)) pick = w;
    if (pick.empty()) return 0;
    uint32_t v = (uint32_t)alnumIndex(pick[0]);
    v = 36 * v + (uint32_t)alnumIndex(pick[1]);
    v = 10 * v + (uint32_t)(pick[2] - '0');
    for (int k = 3; k < 6; ++k) v = 27 * v + (uint32_t)(alnumIndex(pick[k]) - 10);
    return v;
}

std::string unpackCall28(uint32_t v, bool portable) {
    if (v > kBaseCall && v <= kBaseCall + (uint32_t)kNumGroups) return kGroups[v - kBaseCall - 1];
    if (v >= kBaseCall) return "";
    char w[7]; w[6] = 0;
    for (int k = 5; k >= 3; --k) { w[k] = kAlnum[v % 27 + 10]; v /= 27; }
    w[2] = kAlnum[v % 10]; v /= 10;
    w[1] = kAlnum[v % 36]; v /= 36;
    w[0] = kAlnum[v];                    // < 37 by the range check above
    std::string c = trim(w);
    if (c.rfind("3D0", 0) == 0) c = "3DA0" + c.substr(3);
    if (c.size() > 1 && c[0] == 'Q' && c[1] >= 'A' && c[1] <= 'Z') c = "3X" + c.substr(1);
    if (portable && !c.empty()) c += "/P";
    return c;
}

/* ★ The 50-bit call: eleven places, radix 39 · 38 · 38 · 2 · 38 · 38 · 38 · 2 · 38 · 38 · 38 — the 2s are "a '/'
 *  here or not" at places 3 and 7, where a space is put when the call has no '/' there. */
uint64_t packCall50(const std::string& in) {
    std::string w;
    for (char ch : in) { char u = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch; if (alnumIndex(u) >= 0) w += u; }
    if (w.size() > 3 && w[3] != '/') w.insert(3, " ");
    if (w.size() > 7 && w[7] != '/') w.insert(7, " ");
    if (w.size() > 11) return 0;
    w.resize(11, ' ');
    static const int radix[11] = { 39, 38, 38, 2, 38, 38, 38, 2, 38, 38, 38 };
    uint64_t v = 0;
    for (int k = 0; k < 11; ++k) {
        int d;
        if (radix[k] == 2) d = w[k] == '/' ? 1 : 0;
        else { d = alnumIndex(w[k]); if (d < 0 || d >= radix[k]) return 0; }
        v = v * (uint64_t)radix[k] + (uint64_t)d;
    }
    return v;
}

std::string unpackCall50(uint64_t v) {
    static const int radix[11] = { 39, 38, 38, 2, 38, 38, 38, 2, 38, 38, 38 };
    char w[12]; w[11] = 0;
    for (int k = 10; k >= 0; --k) {
        const int d = (int)(v % (uint64_t)radix[k]); v /= (uint64_t)radix[k];
        w[k] = radix[k] == 2 ? (d ? '/' : ' ') : kAlnum[d];
    }
    std::string out;
    for (int k = 0; k < 11; ++k) if (w[k] != ' ') out += w[k];
    return out;
}

// ── grids ──
uint16_t packGrid15(const std::string& g) {
    if (!isGrid4(g)) return 32767;
    // the square's corner + the middle of its first subsquare, WSJT's grid2deg, west-positive
    const double westLon = 180.0 - 20.0 * (g[0] - 'A') - 2.0 * (g[2] - '0') - 5.0 * 12.5 / 60.0;
    const double lat = -90.0 + 10.0 * (g[1] - 'A') + (g[3] - '0') + 2.5 * 12.5 / 60.0;
    const int ilong = (int)westLon, ilat = (int)(lat + 90.0);
    return (uint16_t)(((ilong + 180) / 2) * 180 + ilat);
}

std::string unpackGrid15(uint16_t v) {
    if (v > 180 * 180) return "";
    const double lat = (double)(v % 180) - 90.0;
    const double westLon = (double)(v / 180) * 2.0 - 180.0 + 2.0;
    return deg2grid4(westLon, lat);
}

bool isSpotCall(const std::string& c) {
    if (c.size() < 3 || c.size() > 11 || c[0] == '@' || c[0] == '<') return false;
    bool digit = false, letter = false;
    for (char ch : c) {
        if (ch >= '0' && ch <= '9') digit = true;
        else if (ch >= 'A' && ch <= 'Z') letter = true;
        else if (ch != '/') return false;
    }
    return digit && letter;
}

// ── frames ──
bool unpack(const uint8_t* b, int flags, Frame& f) {
    f = Frame{};
    f.flags = flags & 7;
    // the last zero bit: data frames end [0][1 … 1]
    int lastZero = -1;
    for (int i = 71; i >= 0; --i) if (!(b[i] & 1)) { lastZero = i; break; }
    if (flags & kFlagData) {
        // ★ a "fast data" frame: all 72 bits are dictionary-compressed text — not decodable here (header)
        if (lastZero <= 0) return false;
        f.kind = FrameKind::DataCompressed; f.textKnown = false;
        return true;
    }
    if (b[0] & 1) {                                          // 1xx: data
        const bool compressed = b[1] & 1;
        if (lastZero <= 2) return false;
        if (compressed) { f.kind = FrameKind::DataCompressed; f.textKnown = false; return true; }
        f.kind = FrameKind::Data;
        // Huffman over bits 2 .. lastZero-1
        std::string text; int i = 2;
        while (i < lastZero) {
            bool hit = false;
            for (int h = 0; h < kNumHuff && !hit; ++h) {
                const char* code = kHuff[h].bits; const int n = (int)std::strlen(code);
                if (i + n > lastZero) continue;
                bool same = true;
                for (int k = 0; k < n && same; ++k) same = (b[i + k] & 1) == (code[k] == '1');
                if (same) { text += kHuff[h].ch; i += n; hit = true; }
            }
            if (!hit) break;
        }
        if (text.empty()) return false;
        f.text = text;
        return true;
    }
    const int type = (int)getBits(b, 0, 3);
    if (type == 3) {                                         // directed
        const uint32_t from = (uint32_t)getBits(b, 3, 28), to = (uint32_t)getBits(b, 31, 28);
        const int cmd = (int)getBits(b, 59, 5);
        const bool pFrom = b[64] & 1, pTo = b[65] & 1;
        const int extra = (int)getBits(b, 66, 6);
        f.kind = FrameKind::Directed;
        f.from = unpackCall28(from, pFrom);
        f.to = unpackCall28(to, pTo);
        if (f.from.empty() || f.to.empty()) return false;
        f.cmd = kCommands[cmd];
        if (extra) f.num = isSnrCommand(cmd) ? formatSnr(extra - 31) : std::to_string(extra - 31);
        f.text = f.from + ": " + f.to + f.cmd + (f.num.empty() ? "" : " " + f.num);
        return true;
    }
    // heartbeat / compound: [3][50 call][11 extra-high] … [5 extra-low][3 kind]
    const uint64_t call50 = getBits(b, 3, 50);
    const uint16_t extra = (uint16_t)((getBits(b, 53, 11) << 5) | getBits(b, 64, 5));
    const int bits3 = (int)getBits(b, 69, 3);
    f.from = unpackCall50(call50);
    if (f.from.empty()) return false;
    if (type == 0) {
        const bool cq = extra & 0x8000;
        f.grid = unpackGrid15((uint16_t)(extra & 0x7FFF));
        f.kind = cq ? FrameKind::Cq : FrameKind::Heartbeat;
        f.text = f.from + ": " + (cq ? std::string("@ALLCALL ") + kCq[bits3] : std::string("@HB HEARTBEAT"))
               + (f.grid.empty() ? "" : " " + f.grid);
        return true;
    }
    if (type == 1 || type == 2) {
        f.kind = type == 1 ? FrameKind::Compound : FrameKind::CompoundDirected;
        constexpr int kBaseGrid = 180 * 180, kUserGrid = kBaseGrid + 10, kMaxGrid = 32767;
        if (extra <= kBaseGrid) f.grid = unpackGrid15(extra);
        else if (extra >= kUserGrid && extra < kMaxGrid) {
            const int v = extra - kUserGrid;
            if (v & 0x80) { f.cmd = kCommands[(v & 0x40) ? 29 : 25]; f.num = formatSnr((v & 0x3F) - 31); }
            else if ((v & 0x7F) < 32) f.cmd = kCommands[v & 0x7F];
        }
        if (type == 1) f.text = f.from + ":" + (f.grid.empty() ? "" : " " + f.grid);
        else f.text = f.from + f.cmd + (f.num.empty() ? "" : " " + f.num);
        return true;
    }
    return false;
}

bool packHeartbeat(const std::string& call, const std::string& grid, int cqIndex, uint8_t* b) {
    const uint64_t c = packCall50(call);
    if (!c) return false;
    uint16_t extra = isGrid4(grid) ? packGrid15(grid) : 32767;
    int bits3 = 0;
    if (cqIndex >= 0) { extra |= 0x8000; bits3 = cqIndex & 7; }
    std::memset(b, 0, 72);
    putBits(b, 0, 3, 0);
    putBits(b, 3, 50, c);
    putBits(b, 53, 11, extra >> 5);
    putBits(b, 64, 5, extra & 31);
    putBits(b, 69, 3, (uint64_t)bits3);
    return true;
}

bool packDirected(const std::string& from, const std::string& to, int cmd, int num, uint8_t* b) {
    bool pf = false, pt = false;
    const uint32_t f = packCall28(from, &pf), t = packCall28(to, &pt);
    if (!f || !t || cmd < 0 || cmd > 31) return false;
    std::memset(b, 0, 72);
    putBits(b, 0, 3, 3);
    putBits(b, 3, 28, f);
    putBits(b, 31, 28, t);
    putBits(b, 59, 5, (uint64_t)cmd);
    b[64] = pf; b[65] = pt;
    const int extra = (num >= -30 && num <= 31) ? num + 31 : 0;
    putBits(b, 66, 6, (uint64_t)extra);
    return true;
}

int packHuffText(const std::string& text, uint8_t* b) {
    std::memset(b, 0, 72);
    b[0] = 1; b[1] = 0;
    int at = 2, used = 0;
    for (char ch : text) {
        const char u = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
        const HuffCode* hc = nullptr;
        for (int h = 0; h < kNumHuff; ++h) if (kHuff[h].ch == u) hc = &kHuff[h];
        if (!hc) break;
        const int n = (int)std::strlen(hc->bits);
        if (at + n + 1 > 72) break;                          // room for the closing 0
        for (int k = 0; k < n; ++k) b[at + k] = hc->bits[k] == '1';
        at += n; ++used;
    }
    b[at++] = 0;
    while (at < 72) b[at++] = 1;
    return used;
}

} // namespace js8
} // namespace vibe
