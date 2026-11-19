// decoders/ale_decoder.cpp — 2G ALE receiver, its protocol reader and test transmitter. See ale_decoder.h. V12 2026-11-19.
#include "ale_decoder.h"
#include "digimodes/q65_simd.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>

namespace vibe {

// ── the extended Golay (24,12) code ──────────────────────────────────────────────────────────────────────────────────
// The check bits of each unit data word (bit i of the 12): the code's generator, as the standard's Figure and every
// implementation define it. A codeword is data << 12 | check.
static const uint16_t kGolayRow[12] = {0x5C7, 0xB8D, 0x2DE, 0x5BC, 0xB78, 0x337, 0x66D, 0xCD9, 0xC76, 0xD2B, 0xF92, 0xAE3};
static uint16_t golayCheck(uint32_t d) { uint16_t c = 0; for (int i = 0; i < 12; ++i) if ((d >> i) & 1) c ^= kGolayRow[i]; return c; }
uint32_t aleGolayEncode(uint32_t d) { d &= 0xFFF; return (d << 12) | golayCheck(d); }

namespace {
struct GolayTable {   // syndrome → the lightest error pattern (24 bits) and its weight; weight 4+ = 0xFF
    uint32_t err[4096]; uint8_t w[4096];
    GolayTable() {
        for (int s = 0; s < 4096; ++s) { err[s] = 0; w[s] = 0xFF; }
        auto add = [&](uint32_t e, int wt) {
            const uint16_t s = (uint16_t)((e & 0xFFF) ^ golayCheck(e >> 12));
            if (wt < w[s]) { w[s] = (uint8_t)wt; err[s] = e; }
        };
        add(0, 0);
        for (int a = 0; a < 24; ++a) {
            add(1u << a, 1);
            for (int b = a + 1; b < 24; ++b) {
                add((1u << a) | (1u << b), 2);
                for (int c = b + 1; c < 24; ++c) add((1u << a) | (1u << b) | (1u << c), 3);
            }
        }
    }
};
const GolayTable& gtab() { static const GolayTable t; return t; }
} // namespace

int aleGolayDecode(uint32_t code, uint32_t& data) {
    code &= 0xFFFFFF;
    const uint16_t s = (uint16_t)((code & 0xFFF) ^ golayCheck(code >> 12));
    const GolayTable& t = gtab();
    if (t.w[s] == 0xFF) { data = code >> 12; return -1; }
    data = ((code ^ t.err[s]) >> 12) & 0xFFF;
    return t.w[s];
}

uint32_t aleWord(int pre, const char* c) {
    return ((uint32_t)(pre & 7) << 21) | ((uint32_t)(c[0] & 127) << 14) | ((uint32_t)(c[1] & 127) << 7) | (uint32_t)(c[2] & 127);
}

const char* AleDecoder::preambleName(int p) {
    static const char* n[8] = {"DATA", "THRU", "TO", "TWAS", "FROM", "TIS", "CMD", "REP"};
    return n[p & 7];
}

namespace {
enum { kData = 0, kThru = 1, kTo = 2, kTwas = 3, kFrom = 4, kTis = 5, kCmd = 6, kRep = 7 };
inline bool basic38(int c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '@' || c == '?'; }
inline bool set64(int c) { return c >= 0x20 && c <= 0x5F; }
inline bool addressPre(int p) { return p == kTo || p == kTis || p == kTwas || p == kThru || p == kFrom; }

/** the 49 bits of one word as sent: Golay A and B (B's check bits inverted) interleaved A1 B1 …, then the stuff bit */
void wordBits(uint32_t w, uint8_t* out49) {
    const uint32_t a = aleGolayEncode(w >> 12), b = aleGolayEncode(w & 0xFFF) ^ 0xFFF;
    for (int i = 0; i < 24; ++i) { out49[2 * i] = (a >> (23 - i)) & 1; out49[2 * i + 1] = (b >> (23 - i)) & 1; }
    out49[48] = 0;
}

constexpr int kTones = 8, kOffs = 3, kPh = 8, kF = kTones * kOffs;   // 24 filters
constexpr int kRegBits = 147;

struct Reg {   // the last 147 bits, newest in bit 0
    uint64_t l0 = 0, l1 = 0, l2 = 0;
    void push(int bit) {
        l2 = ((l2 << 1) | (l1 >> 63)) & ((1ull << (kRegBits - 128)) - 1);
        l1 = (l1 << 1) | (l0 >> 63);
        l0 = (l0 << 1) | (uint64_t)(bit & 1);
    }
    uint64_t field(int start, int len) const {   // bits [start, start + len), len ≤ 64, as a number (higher bits older)
        uint64_t v = 0;
        for (int k = len - 1; k >= 0; --k) {
            const int b = start + k;
            const uint64_t bit = b < 64 ? (l0 >> b) & 1 : b < 128 ? (l1 >> (b - 64)) & 1 : (l2 >> (b - 128)) & 1;
            v = (v << 1) | bit;
        }
        return v;
    }
};

struct Track {   // one (offset, timing)
    Reg reg; int64_t syms = 0;
    int64_t pendingAt = -1; uint32_t pendingWord = 0; int pendingErr = 0, pendingBad = 0;
};

struct Section { int pre; std::string s; int lastExt = -1; };

std::string trimPad(std::string s) {
    while (!s.empty() && (s.back() == '@' || s.back() == ' ')) s.pop_back();
    size_t i = 0; while (i < s.size() && s[i] == '@') ++i;
    return s.substr(i);
}
} // namespace

struct AleDecoder::Impl {
    AleDecoder* self = nullptr;
    double inRate; int q = 1; double step = 1, frac = 0; float acc = 0; int qn = 0; float prev = 0; bool interp = false;
    // the tone filters: a complex partial sum a block (8 samples), the window = the last 8 blocks
    float cosT[128][kF], sinT[128][kF];
    float pr[kF] = {}, pi[kF] = {};          // this block's partial sums
    float br[kPh][kF], bi[kPh][kF];          // the last 8 blocks
    int64_t n = 0; int inBlock = 0; int64_t blocks = 0;
    Track tr[kOffs][kPh];
    // sync
    int so = -1, sp = -1; int64_t nextAt = 0; int misses = 0;
    std::vector<AleDecoder::Word> words;
    int state = 0; std::string lastStatus;
    // context: who called whom, when (seconds of audio), for "response" and "ack"
    std::map<std::string, double> recent;
    double now() const { return (double)n / 8000.0; }

    Impl(double rate) : inRate(rate) {
        q = std::max(1, (int)std::floor(rate / 8000 + 1e-9));
        while (q > 1 && rate / q < 8000 * 0.999) --q;
        const double mid = rate / q; interp = std::fabs(mid - 8000) > 1e-6; step = mid / 8000;
        for (int m = 0; m < 128; ++m)
            for (int o = 0; o < kOffs; ++o)
                for (int t = 0; t < kTones; ++t) {
                    const int f = o * kTones + t, k = 12 + 4 * t + (o - 1);   // (750 + 250 t − 62.5 + 62.5 o) / 8000 · 128
                    cosT[m][f] = (float)std::cos(2 * M_PI * k * m / 128.0);
                    sinT[m][f] = (float)-std::sin(2 * M_PI * k * m / 128.0);
                }
        std::memset(br, 0, sizeof br); std::memset(bi, 0, sizeof bi);
    }
    void sample(float x) {
        // ★ every filter four a lane (q65v): 24 complex multiply-adds a sample
        const int m = (int)(n & 127);
        const q65v::V X = q65v::set1(x);
        for (int f = 0; f < kF; f += 4) {
            q65v::st(pr + f, q65v::add(q65v::ld(pr + f), q65v::mul(X, q65v::ld(cosT[m] + f))));
            q65v::st(pi + f, q65v::add(q65v::ld(pi + f), q65v::mul(X, q65v::ld(sinT[m] + f))));
        }
        ++n;
        if (++inBlock < 8) return;
        inBlock = 0;
        const int slot = (int)(blocks & 7);
        std::memcpy(br[slot], pr, sizeof pr); std::memcpy(bi[slot], pi, sizeof pi);
        std::memset(pr, 0, sizeof pr); std::memset(pi, 0, sizeof pi);
        ++blocks;
        if (blocks < 8) return;
        // the window of the last 64 samples: every filter's power
        float wr[kF], wi[kF];
        for (int f = 0; f < kF; f += 4) {
            q65v::V sr = q65v::ld(br[0] + f), si = q65v::ld(bi[0] + f);
            for (int b = 1; b < 8; ++b) { sr = q65v::add(sr, q65v::ld(br[b] + f)); si = q65v::add(si, q65v::ld(bi[b] + f)); }
            q65v::st(wr + f, sr); q65v::st(wi + f, si);
        }
        const int ph = (int)(blocks & 7);
        for (int o = 0; o < kOffs; ++o) {
            float best = -1; int bt = 0;
            for (int t = 0; t < kTones; ++t) {
                const int f = o * kTones + t; const float p = wr[f] * wr[f] + wi[f] * wi[f];
                if (p > best) { best = p; bt = t; }
            }
            symbol(o, ph, bt ^ (bt >> 1));
        }
    }
    /** majority word of track (o,p): bad = copies not unanimous; returns errors (−1 none) and the 24-bit word */
    int readWord(const Track& t, uint32_t& w, int& bad) const {
        const uint64_t c3 = t.reg.field(1, 48), c2 = t.reg.field(50, 48), c1 = t.reg.field(99, 48), M = (1ull << 48) - 1;
        const uint64_t maj = ((c1 & c2) | (c1 & c3) | (c2 & c3)) & M;
        bad = 48 - __builtin_popcountll(~((c1 ^ c2) | (c1 ^ c3)) & M);
        uint32_t a = 0, b = 0;
        for (int i = 0; i < 24; ++i) { a = (a << 1) | (uint32_t)((maj >> (47 - 2 * i)) & 1); b = (b << 1) | (uint32_t)((maj >> (46 - 2 * i)) & 1); }
        b ^= 0xFFF;
        uint32_t da, db; const int ea = aleGolayDecode(a, da), eb = aleGolayDecode(b, db);
        if (ea < 0 || eb < 0) return -1;
        w = (da << 12) | db;
        return std::max(ea, eb);
    }
    static bool plausible(uint32_t w, bool acquiring) {
        const int p = (w >> 21) & 7, a = (w >> 14) & 127, b = (w >> 7) & 127, c = w & 127;
        if (addressPre(p)) return basic38(a) && basic38(b) && basic38(c);
        if (acquiring) return false;   // ★ a transmission is found by its address words only
        if (p == kData || p == kRep) return (basic38(a) && basic38(b) && basic38(c)) || (set64(a) && set64(b) && set64(c));
        return true;                    // CMD: any 7-bit
    }
    void symbol(int o, int p, int tri) {
        Track& t = tr[o][p];
        t.reg.push((tri >> 2) & 1); t.reg.push((tri >> 1) & 1); t.reg.push(tri & 1);
        ++t.syms;
        if (t.syms < 49) return;
        if (so >= 0) {
            if (o != so || p != sp || t.syms < nextAt) return;
            nextAt = t.syms + 49;
            uint32_t w; int bad; const int e = readWord(t, w, bad);
            if (e >= 0 && bad <= 30 && plausible(w, false)) { misses = 0; take(w, e, bad, o); }
            else if (++misses >= 3) endTransmission();   // (two in a row at −3 dB split one sounding into two lines)
            return;
        }
        // acquiring: a candidate word, confirmed by the next one 49 tones on
        if (t.pendingAt >= 0 && t.syms == t.pendingAt + 49) {
            uint32_t w; int bad; const int e = readWord(t, w, bad);
            if (e >= 0 && e <= 2 && bad <= 24 && plausible(w, false)) {
                so = o; sp = p; nextAt = t.syms + 49; misses = 0;
                for (auto& row : tr) for (auto& x : row) x.pendingAt = -1;
                take(t.pendingWord, t.pendingErr, t.pendingBad, o);
                take(w, e, bad, o);
                return;
            }
            t.pendingAt = -1;
        }
        if (t.pendingAt >= 0) return;
        uint32_t w; int bad;
        // ★ three gates, cheapest first: copies that agree (noise: 12 of 48 unanimous), both halves within 1 error of
        //   a codeword, an address word in the 38 characters
        const uint64_t c3 = t.reg.field(1, 48), c2 = t.reg.field(50, 48), c1 = t.reg.field(99, 48), M = (1ull << 48) - 1;
        if (48 - __builtin_popcountll(~((c1 ^ c2) | (c1 ^ c3)) & M) > 20) return;
        const int e = readWord(t, w, bad);
        if (e < 0 || e > 1 || !plausible(w, true)) return;
        t.pendingAt = t.syms; t.pendingWord = w; t.pendingErr = e; t.pendingBad = bad;
    }
    void take(uint32_t w, int e, int bad, int o) {
        AleDecoder::Word x; x.pre = (w >> 21) & 7; x.c[0] = (char)((w >> 14) & 127); x.c[1] = (char)((w >> 7) & 127); x.c[2] = (char)(w & 127); x.c[3] = 0;
        x.errors = e; x.votesBad = bad; x.offsetHz = (float)(62.5 * (o - 1));
        if (words.size() < 400) words.push_back(x);   // (bounded: a transmission of 400 words is 2.6 minutes)
        if (self->onWord) self->onWord(x);
        if (state != 1) { state = 1; if (self->onState) self->onState(1); }
        status();
    }
    void status() {
        char js[200]; std::string last;
        if (!words.empty()) { const auto& x = words.back(); last = std::string(preambleName(x.pre)) + " " + std::string(x.c, 3); }
        for (char& ch : last) if (ch == '"' || ch == '\\' || (unsigned char)ch < 32) ch = '?';
        std::snprintf(js, sizeof js, "{\"words\":%d,\"offset\":%.0f,\"last\":\"%s\"}", (int)words.size(),
                      words.empty() ? 0.0 : (double)words.back().offsetHz, last.c_str());
        if (js != lastStatus) { lastStatus = js; if (self->onStatus) self->onStatus(js); }
    }
    std::string stamp() const {
        if (self->clock) return self->clock();
        const std::time_t t = std::time(nullptr); std::tm g{};
#if defined(_WIN32)
        gmtime_s(&g, &t);
#else
        gmtime_r(&t, &g);
#endif
        char b[16]; std::snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec); return b;
    }
    void endTransmission() {
        so = sp = -1; misses = 0;
        for (auto& row : tr) for (auto& x : row) x.pendingAt = -1;
        if (!words.empty()) assemble();
        words.clear();
        if (state != 0) { state = 0; if (self->onState) self->onState(0); }
        status();
    }
    /** the words of one transmission → who called whom (and any AMD message) */
    void assemble() {
        std::vector<Section> sec;
        for (const auto& w : words) {
            const std::string three(w.c, 3);
            if (addressPre(w.pre) || w.pre == kCmd) {
                // a scanning call repeats its first word: the same word again is not a new address
                if (!sec.empty() && sec.back().pre == w.pre && sec.back().s == three) continue;
                sec.push_back({w.pre, three});
            } else if (!sec.empty() && sec.back().s.size() < 90) {
                // ★ extensions alternate DATA, REP, DATA …: the same one twice means the word that led the second was
                //   missed — not more address ("G4XYZ@YZ" when a sounding's TIS was lost between its repetitions)
                Section& b = sec.back();
                if (b.lastExt == w.pre || (b.lastExt < 0 && w.pre != kData)) continue;
                b.s += three; b.lastExt = w.pre;
            }
        }
        std::string to, from, thru, amd; bool twas = false; std::vector<std::string> cmds;
        // ★ an address heard twice keeps its longest reading: a repetition whose extension word was missed reads as a
        //   shorter address ("G4X" for "G4XYZ"), which must not replace the whole one (measured at −3 dB: 2 in 20)
        auto keep = [](std::string& cur, const std::string& a) {
            if (cur.empty() || (a.size() > cur.size() && a.compare(0, cur.size(), cur) == 0)) cur = a;
            else if (cur.compare(0, std::min(cur.size(), a.size()), a, 0, std::min(cur.size(), a.size())) != 0) cur = a;
        };
        for (const auto& s : sec) {
            const std::string a = trimPad(s.s);
            if (s.pre == kTo) keep(to, a);
            else if (s.pre == kTis || s.pre == kTwas) { keep(from, a); twas = s.pre == kTwas; }
            else if (s.pre == kThru) thru = a;
            else if (s.pre == kFrom && from.empty()) from = a;
            else if (s.pre == kCmd) {
                if (!s.s.empty() && set64((unsigned char)s.s[0])) { std::string t = s.s; while (!t.empty() && t.back() == ' ') t.pop_back(); amd += (amd.empty() ? "" : " ") + t; }
                else if (!s.s.empty()) { char b[16]; std::snprintf(b, sizeof b, "CMD %c", s.s[0]); cmds.push_back(b); }
            }
        }
        if (to.empty() && from.empty()) return;
        std::string kind, who;
        const double t = now();
        if (!to.empty() && !from.empty()) {
            kind = "call";
            const auto back = recent.find(to + ">" + from);    // they called us
            const auto again = recent.find("R" + from + ">" + to);
            if (again != recent.end() && t - again->second < 60) { kind = "ack"; }
            else if (back != recent.end() && t - back->second < 60) { kind = "response"; recent["R" + to + ">" + from] = t; }
            recent[from + ">" + to] = t;
            who = from + " \xe2\x86\x92 " + to + (kind == "ack" ? "  (linked)" : "");
        } else if (!from.empty()) {
            kind = "sound"; who = from + (twas ? "  (TWAS: not taking calls)" : "");
        } else {
            kind = "call"; who = "? \xe2\x86\x92 " + to;
        }
        if (recent.size() > 64) recent.clear();
        std::string head = "[ALE] " + stamp() + "  " + kind + "  " + who;
        if (!thru.empty()) head += "  via " + thru;
        for (const auto& c : cmds) head += "  \xc2\xb7 " + c;
        std::string pub = head, priv;
        if (!amd.empty()) {
            char b[64]; std::snprintf(b, sizeof b, "  \xc2\xb7 AMD message (%d characters)", (int)amd.size());
            pub += b;
            priv = head + "  \xc2\xb7 AMD: " + amd + "\n";
        }
        pub += "\n";
        if (self->onLine) self->onLine(pub, priv);
    }
    void feed(const float* x, int cnt) {
        for (int i = 0; i < cnt; ++i) {
            acc += x[i];
            if (++qn < q) continue;
            const float v = acc / q; acc = 0; qn = 0;
            if (!interp) { sample(v); continue; }
            while (frac < 1.0) { sample(prev + (float)frac * (v - prev)); frac += step; }
            frac -= 1.0; prev = v;
        }
    }
};

AleDecoder::AleDecoder(double inRate) : d_(new Impl(inRate)) { d_->self = this; }
AleDecoder::~AleDecoder() = default;
void AleDecoder::feed(const float* x, int n) { d_->feed(x, n); }

// ── the test transmitter ─────────────────────────────────────────────────────────────────────────────────────────────
std::vector<float> aleSynth(const std::vector<uint32_t>& words, double fs, double offsetHz) {
    std::vector<int> tri;
    // 147 bits = 49 tribits a word (each 49-bit copy is not a whole number of tribits: the copies run on across them)
    std::vector<int> bits; for (uint32_t w : words) { uint8_t b[49]; wordBits(w, b); for (int r = 0; r < 3; ++r) bits.insert(bits.end(), b, b + 49); }
    tri.clear();
    for (size_t i = 0; i + 2 < bits.size(); i += 3) tri.push_back((bits[i] << 2) | (bits[i + 1] << 1) | bits[i + 2]);
    const double symS = 0.008; const size_t per = (size_t)std::lround(symS * fs);
    std::vector<float> out(tri.size() * per, 0.f); double ph = 0;
    for (size_t k = 0; k < tri.size(); ++k) {
        int g = tri[k], m = 0; for (int v = 0; v < 8; ++v) if ((v ^ (v >> 1)) == g) m = v;   // the tone whose Gray code it is
        const double f = 750 + 250 * m + offsetHz;
        for (size_t i = 0; i < per; ++i) { ph += 2 * M_PI * f / fs; out[k * per + i] = (float)std::sin(ph); }
        ph = std::fmod(ph, 2 * M_PI);
    }
    return out;
}

std::vector<uint32_t> aleCallWords(const std::string& to, const std::string& from, int rep, bool twas, const std::string& amd) {
    auto pad3 = [](std::string s) { while (s.size() % 3) s += '@'; return s; };
    auto addr = [&](int pre, const std::string& a, std::vector<uint32_t>& out) {
        const std::string s = pad3(a);
        for (size_t i = 0; i < s.size(); i += 3) out.push_back(aleWord(i == 0 ? pre : ((i / 3) % 2 ? kData : kRep), s.c_str() + i));
    };
    std::vector<uint32_t> w;
    if (!to.empty()) {
        const std::string s = pad3(to);
        for (int i = 0; i < rep; ++i) w.push_back(aleWord(kTo, s.c_str()));
        addr(kTo, to, w);
    } else for (int i = 0; i < rep; ++i) addr(twas ? kTwas : kTis, from, w);
    if (!amd.empty()) {
        std::string s = amd; while (s.size() % 3) s += ' ';
        for (size_t i = 0; i < s.size(); i += 3) w.push_back(aleWord(i == 0 ? kCmd : ((i / 3) % 2 ? kData : kRep), s.c_str() + i));
    }
    addr(twas ? kTwas : kTis, from, w);
    return w;
}

} // namespace vibe
