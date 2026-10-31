// decoders/paging_decoder.cpp — see paging_decoder.h. POCSAG from ITU-R M.584-2 Annex 1.
#include "decoders/paging_decoder.h"

#include <array>
#include <cmath>

namespace vibe {

namespace {
constexpr uint32_t kGen = 0x769u;          // x¹⁰+x⁹+x⁸+x⁶+x⁵+x³+1 (M.584-2 §1.4)
inline int pop32(uint32_t v) { return __builtin_popcount(v); }

/** Remainder of a 31-bit block (bits 30..0) divided by the generator. */
uint32_t bchRem(uint32_t v31) {
    for (int b = 30; b >= 10; --b)
        if (v31 & (1u << b)) v31 ^= kGen << (b - 10);
    return v31 & 0x3FFu;
}
/** Syndrome → the 31-bit error pattern of every 1- and 2-bit error (t = 2: they are all distinct). */
const std::array<uint32_t, 1024>& synTable() {
    static const std::array<uint32_t, 1024> t = [] {
        std::array<uint32_t, 1024> a{};
        for (int i = 0; i < 31; ++i) {
            a[bchRem(1u << i)] = 1u << i;
            for (int j = i + 1; j < 31; ++j) a[bchRem((1u << i) | (1u << j))] = (1u << i) | (1u << j);
        }
        return a;
    }();
    return t;
}
const char kNumeric[17] = "0123456789*U -][";   // Table 3: 1010 spare (shown *), 1011 U, 1100 space, 1101 hyphen, 1110 ], 1111 [
} // namespace

uint32_t PagingDecoder::encodeWord(uint32_t info21) {
    const uint32_t v = (info21 & 0x1FFFFFu) << 10;
    const uint32_t cw31 = v | bchRem(v);
    return (cw31 << 1) | (uint32_t)(pop32(cw31) & 1);
}

int PagingDecoder::correctWord(uint32_t& cw) {
    uint32_t v31 = cw >> 1;
    const uint32_t s = bchRem(v31);
    int k = 0;
    if (s) {
        const uint32_t e = synTable()[s];
        if (!e) return -1;
        v31 ^= e; k = pop32(e);
    }
    const bool parityBad = ((pop32(v31) + (int)(cw & 1u)) & 1) != 0;
    if (k + (parityBad ? 1 : 0) > 2) return -1;
    cw = (v31 << 1) | (uint32_t)(pop32(v31) & 1);
    return k + (parityBad ? 1 : 0);
}

// ── one bit rate's receiver ──────────────────────────────────────────────────────────────────────────────────────────
struct PagingDecoder::Rx {
    PagingDecoder* o;
    int   baud;
    float inc, invInc;         // bits per sample, and its reciprocal (no divide per sample)
    float aFast, aSlow, aLp;   // one-pole coefficients
    float crossAt;             // where a zero crossing lands in the bit after the low-pass's delay (phase units)
    float dc = 0, lp = 0, lpPrev = 0, ph = 0, acc = 0;
    float dInc = 0;            // ★ the loop's second order: the transmitter's clock error, learnt (bits per sample)
    bool  lpPos = false;
    uint32_t sr = 0;
    enum class St { Hunt, Batch, ExpectSync } st = St::Hunt;
    bool  inv = false;
    bool  counted = false;     // the last batch passed the noise guard
    int   nBits = 0, nWords = 0;
    uint32_t cur = 0;
    std::array<uint32_t, 16> w{};
    // the page being assembled
    bool  inMsg = false;
    Page  pg;
    std::vector<uint32_t> chunks;

    Rx(PagingDecoder* owner, int b, double fs) : o(owner), baud(b) {
        inc = (float)(b / fs); invInc = 1.0f / inc;
        aFast = 1.0f - std::exp(-(float)(b / fs) / 6.0f);       // ≈ 6 bits (the preamble is balanced)
        aSlow = 1.0f - std::exp(-(float)(b / fs) / 150.0f);     // ≈ 150 bits once locked
        aLp   = 1.0f - std::exp(-2.0f * (float)M_PI * 0.8f * (float)(b / fs));
        crossAt = ((1.0f - aLp) / aLp) * inc;                   // the one-pole's group delay, in bits
        crossAt -= std::floor(crossAt);
    }

    void feed(const float* x, int n) {
        for (int i = 0; i < n; ++i) {
            const float a = (st == St::Hunt) ? aFast : aSlow;
            dc += a * (x[i] - dc);
            const float v = x[i] - dc;
            lpPrev = lp;
            lp += aLp * (v - lp);
            const bool pos = lp >= 0.0f;
            if (pos != lpPos) {   // a crossing: pull the bit clock so it falls at crossAt
                lpPos = pos;
                // ★ where between the two samples (one divide per CROSSING, not per sample): the phase then
                const float fr = lpPrev / (lpPrev - lp);
                // ★ ph is the bit phase at the END of the previous sample's interval (time t − ½)
                float e = ph - (1.5f - fr) * inc - crossAt;
                if (e > 0.5f) e -= 1.0f; else if (e < -0.5f) e += 1.0f;
                const bool hunt = st == St::Hunt;
                ph -= (hunt ? 0.12f : 0.04f) * e;
                // ★ a first-order loop leaves a standing phase error of drift ÷ gain (0.25 bit at 0.5 % clock error):
                //   integrate the error into the rate as well, clamped to ±2 %
                dInc -= (hunt ? 0.004f : 0.0015f) * e * inc;
                if (dInc > 0.02f * inc) dInc = 0.02f * inc; else if (dInc < -0.02f * inc) dInc = -0.02f * inc;
                if (ph < 0.0f) ph += 1.0f;
            }
            // ★ each sample stands for the interval t ± ½; ph is the phase at its END. The share past the bit edge opens the
            //   next bit, so a bit is integrated over exactly its own time at any samples-per-bit (5 at 2400 on 12 kHz).
            ph += inc + dInc;
            if (ph >= 1.0f) {
                ph -= 1.0f;
                const float after = ph * invInc;          // 0..1 of this sample's interval past the edge
                acc += v * (1.0f - after);
                bit(acc > 0.0f ? 1u : 0u);
                acc = v * after;
            } else acc += v;
        }
    }

    void bit(uint32_t b) {
        sr = (sr << 1) | b;
        switch (st) {
        case St::Hunt: {
            const int e = pop32(sr ^ kSync), ei = pop32(sr ^ ~kSync);
            if (e <= 2 || ei <= 2) { inv = ei < e; st = St::Batch; nBits = 0; nWords = 0; cur = 0; }
            break;
        }
        case St::Batch:
            cur = (cur << 1) | (b ^ (inv ? 1u : 0u));
            if (++nBits == 32) {
                w[(size_t)nWords++] = cur; nBits = 0; cur = 0;
                if (nWords == 16) { batch(); if (st == St::Batch) { st = St::ExpectSync; nBits = 0; } }
            }
            break;
        case St::ExpectSync:
            if (++nBits == 32) {
                if (pop32(sr ^ (inv ? ~kSync : kSync)) <= 4) { st = St::Batch; nBits = 0; nWords = 0; cur = 0; }
                else lose();
            }
            break;
        }
    }

    void lose() {
        if (inMsg) { pg.truncated = true; emit(); }
        st = St::Hunt; counted = false;
    }

    void batch() {
        std::array<uint32_t, 16> c = w;
        std::array<int, 16> k{};
        int good = 0;
        for (int i = 0; i < 16; ++i) { k[(size_t)i] = correctWord(c[(size_t)i]); good += (k[(size_t)i] >= 0 && k[(size_t)i] <= 1); }
        // ★★★ the noise guard (see the header): a batch counts only if ≥ 12 of 16 words are clean or one bit out
        if (good < 12) { lose(); return; }
        counted = true;
        for (int i = 0; i < 16; ++i) {
            const uint32_t cw = c[(size_t)i];
            if (k[(size_t)i] < 0) { if (inMsg) { pg.truncated = true; emit(); } continue; }
            if ((cw & 0x80000000u) == 0) {               // address (or idle): ends any message
                if (inMsg) emit();
                if (cw == kIdle) continue;
                if (k[(size_t)i] > 1) continue;          // ★ an address must be clean or one bit out (a slipped word is not)
                inMsg = true; chunks.clear();
                pg = Page{};
                pg.proto = "POCSAG"; pg.baud = baud; pg.inverted = inv;
                pg.address = (((cw >> 13) & 0x3FFFFu) << 3) | (uint32_t)(i / 2);
                pg.function = (int)((cw >> 11) & 3u);
                pg.corrected = k[(size_t)i];
            } else if (inMsg) {
                chunks.push_back((cw >> 11) & 0xFFFFFu);
                pg.corrected += k[(size_t)i];
            }
        }
    }

    static std::string decodeNumeric(const std::vector<uint32_t>& ch) {
        std::string s; uint32_t v = 0; int nb = 0;
        for (uint32_t c : ch)
            for (int b = 19; b >= 0; --b) {
                v |= ((c >> b) & 1u) << nb;
                if (++nb == 4) { s += kNumeric[v]; v = 0; nb = 0; }
            }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
    static std::string decodeAlpha(const std::vector<uint32_t>& ch, int* printable, int* total) {
        std::string s; uint32_t v = 0; int nb = 0; *printable = 0; *total = 0;
        for (uint32_t c : ch)
            for (int b = 19; b >= 0; --b) {
                v |= ((c >> b) & 1u) << nb;
                if (++nb == 7) {
                    if (v >= 0x20 && v < 0x7F) { s += (char)v; ++*printable; ++*total; }
                    else if (v == '\n' || v == '\r') { s += ' '; ++*printable; ++*total; }
                    else if (v != 0 && v != 0x03 && v != 0x04 && v != 0x17) ++*total;   // NUL / ETX / EOT / ETB: fill
                    v = 0; nb = 0;
                }
            }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }

    void emit() {
        inMsg = false;
        if (pg.truncated && chunks.empty()) return;      // ★ an address whose message never came is not a page
        if (chunks.empty()) pg.kind = "tone";
        else {
            int pr = 0, tot = 0;
            const std::string a = decodeAlpha(chunks, &pr, &tot);
            // ★ M.584 defines function 00 = numeric and 11 = alphanumeric; 01/10 carry whichever reads as text
            const bool alpha = pg.function == 3 || (pg.function != 0 && tot > 0 && pr * 10 >= tot * 9);
            pg.kind = alpha ? "alpha" : "numeric";
            pg.text = alpha ? a : decodeNumeric(chunks);
        }
        chunks.clear();
        if (o->onPage) o->onPage(pg);
    }
};

const std::vector<PagingDecoder::ModeInfo>& PagingDecoder::modes() {
    static const std::vector<ModeInfo> m = {
        { Mode::Auto,       "auto",       "AUTO" },
        { Mode::Pocsag512,  "pocsag512",  "POCSAG 512" },
        { Mode::Pocsag1200, "pocsag1200", "POCSAG 1200" },
        { Mode::Pocsag2400, "pocsag2400", "POCSAG 2400" },
    };
    return m;
}
PagingDecoder::Mode PagingDecoder::modeFromId(const std::string& id) {
    for (const auto& i : modes()) if (id == i.id) return i.mode;
    return Mode::Auto;
}
const char* PagingDecoder::idOf(Mode m) {
    for (const auto& i : modes()) if (i.mode == m) return i.id;
    return "auto";
}

PagingDecoder::PagingDecoder(double rateHz, Mode m) : fs_(rateHz), mode_(m) { build_(); }
PagingDecoder::~PagingDecoder() = default;

void PagingDecoder::build_() {
    rx_.clear();
    const int all[3] = {512, 1200, 2400};
    for (int b : all) {
        const bool want = mode_ == Mode::Auto || (mode_ == Mode::Pocsag512 && b == 512)
                       || (mode_ == Mode::Pocsag1200 && b == 1200) || (mode_ == Mode::Pocsag2400 && b == 2400);
        if (want && fs_ >= 4.0 * b) rx_.emplace_back(new Rx(this, b, fs_));
    }
}
void PagingDecoder::setMode(Mode m) { if (m != mode_) { mode_ = m; build_(); } }
void PagingDecoder::reset() { build_(); }
void PagingDecoder::feed(const float* x, int n) { for (auto& r : rx_) r->feed(x, n); }

std::string PagingDecoder::detected() const {
    for (auto& r : rx_)
        if (r->counted && r->st != Rx::St::Hunt) return "POCSAG " + std::to_string(r->baud);
    return "";
}

} // namespace vibe
