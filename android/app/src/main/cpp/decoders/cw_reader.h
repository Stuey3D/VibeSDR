// decoders/cw_reader.h — the CW skimmer's READER: soft decisions over the element run, a short beam, and a small ham-CW
// language model that breaks ties. ★ Included by cw_skimmer.cpp ONLY (it carries the model's tables, cw_lm_data.h).
//
// ★★★ WHY (Stuart, 2026-10-29, the 80 m screenshots): a human fist's timing is ambiguous in BOTH directions —
//   • 3555.30, G4TPJ at 24 WPM: word gaps barely longer than letter gaps, so a hard split ran the words together
//     ("OPISRICHARDRICHARDESQTHISNRTONBRIDGE…");
//   • 3554, slow hand-sent: stretched gaps inside letters read as letter ends ("E E BT LA TEE M T O").
//   A hard threshold must pick one reading per gap at once. The reader keeps, for every gap, the probability that it ends
//   an element, a letter or a word (from the station's own timing: the lane's centres and spread), and for every mark the
//   probability it is a dash; it carries the few most likely READINGS of the recent run (a beam of 8) and commits text
//   only where they all agree (or the run is long, or a pause ends it). Where the timing is clear the beam is one reading
//   and the text is exactly the hard decoder's; where it is not, the language model chooses.
// ★★ THE MODEL ONLY BREAKS TIES ("the cheap option" Stuart chose, 21:45): it scores the readings the timing allows. It
//    never puts in a letter that was not sent, never changes a letter whose marks were clear, and a reading the timing
//    rules out (a gap 30 % past a station's own word centre read as no space) is never chosen for being more English.
//    Other languages: the model was trained on Italian, Spanish, French and German exchanges too, and its weight is
//    small beside clear timing — an Italian QSO reads as sent.
// ★ Cost: a few hundred operations a CHARACTER per lane (nothing per hop), and only on CONFIRMED signals — the hundreds
//   of tentative carriers on a busy band cost nothing here. Fixed arrays; nothing allocates but the committed string.
#pragma once
#include "cw_lm_data.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vibe {
namespace cw {
namespace {

// ── the model's alphabet (must match scripts/gen-cw-lm.mjs) ──
constexpr const char* kPunct = "/?=+.,'-():\"@";
const char* const kProsigns[5] = { "<SN>", "<AS>", "<SK>", "<KA>", "<HH>" };
/** One decoded character's text (as morseText gives it) → its model symbol; −1 = none. */
int symbolOf(const char* t) {
    if (!t || !*t) return -1;
    if (t[1] == 0) {
        const char c = t[0];
        if (c == ' ') return 0;
        if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
        if (c >= '0' && c <= '9') return 27 + (c - '0');
        if (const char* p = std::strchr(kPunct, c)) return 37 + (int)(p - kPunct);
        return -1;
    }
    for (int i = 0; i < 5; ++i) if (!std::strcmp(t, kProsigns[i])) return 50 + i;
    return -1;
}
constexpr uint32_t kCtxMod = 64u * 64u * 64u * 64u;   // four symbols of context
inline uint32_t pushCtx(uint32_t ctx, int sym) { return (ctx * 64u + (uint32_t)sym) % kCtxMod; }

template <size_t N>
int findKey(const uint32_t (&keys)[N], uint32_t k) {
    const uint32_t* e = keys + N;
    const uint32_t* p = std::lower_bound(keys, e, k);
    return (p != e && *p == k) ? (int)(p - keys) : -1;
}
/** ★ −ln P(sym | the four symbols before it), in nats — the back-off model (ARPA form): the longest n-gram the model
 *  kept, plus the back-off weights of the longer contexts it passed through. */
float lmCost(uint32_t ctx, int sym) {
    using namespace lmdata;
    float back = 0;
    // order 5 … 2: context of 4 … 1 symbols
    const uint32_t mod[5] = { 1u, 64u, 64u * 64u, 64u * 64u * 64u, kCtxMod };
    for (int n = 5; n >= 2; --n) {
        const uint32_t h = ctx % mod[n - 1];
        const uint32_t key = h * 64u + (uint32_t)sym;
        int i = -1;
        switch (n) {
            case 5: i = findKey(kKey5, key); if (i >= 0) return back + kP5[i] / 16.0f; break;
            case 4: i = findKey(kKey4, key); if (i >= 0) return back + kP4[i] / 16.0f; break;
            case 3: i = findKey(kKey3, key); if (i >= 0) return back + kP3[i] / 16.0f; break;
            case 2: i = findKey(kKey2, key); if (i >= 0) return back + kP2[i] / 16.0f; break;
        }
        // ★ Not kept at this order: pay the context's back-off weight (if the context itself was kept) and go down one.
        int j = -1;
        switch (n - 1) {
            case 4: j = findKey(kKey4, h); if (j >= 0) back -= kB4[j] / 16.0f; break;
            case 3: j = findKey(kKey3, h); if (j >= 0) back -= kB3[j] / 16.0f; break;
            case 2: j = findKey(kKey2, h); if (j >= 0) back -= kB2[j] / 16.0f; break;
            case 1: j = findKey(kKey1, h); if (j >= 0) back -= kB1[j] / 16.0f; break;
        }
    }
    const int i = findKey(lmdata::kKey1, (uint32_t)sym);
    return back + (i >= 0 ? lmdata::kP1[i] / 16.0f : 8.0f);
}
/** A word the vocabulary knows (codes, QSO words, names, towns, common words of other languages). */
bool lmWord(const char* w, int n) {
    using namespace lmdata;
    if (n <= 0 || n > 15) return false;
    char buf[16]; std::memcpy(buf, w, (size_t)n); buf[n] = 0;
    const char* const* e = kWord + kWords;
    const char* const* p = std::lower_bound(kWord, e, buf, [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
    return p != e && !std::strcmp(*p, buf);
}

/** ★★★ THE READER — a beam of readings over one lane's elements. The lane gives each mark P(dash) and each gap P(element
 *  gap), P(letter gap), P(word gap) from the station's own timing; the reader keeps the best few readings and commits
 *  the text they agree on. */
struct Reader {
    static constexpr int K = 8;           // readings kept
    static constexpr int kPend = 48;      // uncommitted characters a reading may hold
    static constexpr float kBeam = 14.0f; // nats behind the best before a reading is dropped
    static constexpr float kMinP = 0.015f;   // an option the timing gives less than this is not a reading at all
    float lambda = 0.55f;                 // ★ the model's weight beside the timing (tuned on cw_bench human)
    float wordBonus = 0.6f;               // ★ a finished word the vocabulary knows (or a callsign, a report)
    struct Hyp {
        float score = 0;
        uint32_t ctx = 0;                 // the model's context: the last four symbols (0 = spaces: a fresh start)
        uint16_t code = 1; uint8_t nel = 0; bool spoilt = false;
        uint8_t n = 0;                    // pending characters in t
        uint8_t wordStart = 0;            // where the current word starts in t (or 0 when it began before t)
        bool wordInT = false;             // the current word began inside t (so it can be checked whole)
        char t[kPend];
    };
    Hyp h[K]; int nh = 1;
    std::string out;                      // ★ committed text, taken by the lane (take())
    uint32_t chars = 0;                   // characters committed (for the tests)

    Reader() {
        reset();
#ifdef CW_DEBUG
        if (const char* e = std::getenv("CW_LAMBDA")) lambda = (float)std::atof(e);   // ★ tuning only (cw_bench)
        if (const char* e = std::getenv("CW_WORDBONUS")) wordBonus = (float)std::atof(e);
#endif
    }
    void reset() { nh = 1; h[0] = Hyp(); h[0].wordInT = true; out.clear(); }
    bool idle() const { return nh == 1 && h[0].nel == 0 && h[0].n == 0; }
    bool midChar() const { for (int i = 0; i < nh; ++i) if (h[i].nel) return true; return false; }

    // ── events ──
    void spoil() { for (int i = 0; i < nh; ++i) h[i].spoilt = true; }
    void mark(float pDash) {
        Hyp nx[2 * K]; int m = 0;
        const float pd = std::max(1e-4f, std::min(1.0f - 1e-4f, pDash));
        for (int i = 0; i < nh; ++i) {
            for (int dash = 0; dash < 2; ++dash) {
                const float p = dash ? pd : 1.0f - pd;
                if (p < kMinP) continue;
                Hyp x = h[i];
                if (x.nel < 9) { x.code = (uint16_t)(x.code * 2 + dash); ++x.nel; } else x.spoilt = true;
                x.score += std::log(p);
                nx[m++] = x;
            }
        }
        keep(nx, m);
    }
    void gap(float pE, float pC, float pW) {
        const float s = pE + pC + pW;
        if (s <= 0) return;
        pE /= s; pC /= s; pW /= s;
        Hyp nx[3 * K]; int m = 0;
        for (int i = 0; i < nh; ++i) {
            if (!h[i].nel) { nx[m++] = h[i]; continue; }   // ★ nothing open (a gap after a flush): no choice to make
            if (pE >= kMinP) { Hyp x = h[i]; x.score += std::log(pE); nx[m++] = x; }
            if (pC >= kMinP) { Hyp x = h[i]; x.score += std::log(pC); endChar(x); nx[m++] = x; }
            if (pW >= kMinP) { Hyp x = h[i]; x.score += std::log(pW); endChar(x); endWord(x); nx[m++] = x; }
        }
        if (!m) return;
        keep(nx, m);
    }
    /** ★ A pause (or the signal leaving): the best reading's letter and word end, and everything is committed. */
    void flush() {
        int b = 0; for (int i = 1; i < nh; ++i) if (h[i].score > h[b].score) b = i;
        Hyp x = h[b];
        if (x.nel) endChar(x);
        if (x.n && x.t[x.n - 1] != ' ') endWord(x);
        else if (!x.n && x.ctx % 64u != 0) endWord(x);
        out.append(x.t, x.n); chars += x.n;
        x.n = 0; x.wordStart = 0; x.wordInT = true; x.score = 0;
        h[0] = x; nh = 1;
    }
    std::string take() { std::string o; o.swap(out); return o; }
    /** As flush, but what was held is thrown away (another lane's reader has shown the same keying). */
    void discard() { const size_t keep = out.size(); flush(); out.resize(keep); }

private:
    void append(Hyp& x, const char* s) {
        for (const char* p = s; *p; ++p) if (x.n < kPend) x.t[x.n++] = *p;
    }
    void endChar(Hyp& x) {
        const char* t = (!x.spoilt && x.nel) ? morseText(x.code) : nullptr;
        x.code = 1; x.nel = 0; x.spoilt = false;
        const int sym = symbolOf(t);
        if (sym < 0) { x.score += std::log(0.05f); return; }   // ★ not a character: nothing shown (as the hard decoder)
        x.score -= lambda * lmCost(x.ctx, sym);
        x.ctx = pushCtx(x.ctx, sym);
        append(x, t);
    }
    void endWord(Hyp& x) {
        if (x.ctx % 64u == 0) return;   // ★ already at a word boundary
        x.score -= lambda * lmCost(x.ctx, 0);
        // ★ The word just finished, if it is whole in t: a word the vocabulary knows, a callsign, a report or a figure.
        if (x.wordInT) {
            const int a = x.wordStart, n = x.n - a;
            if (n > 0 && wordKnown(x.t + a, n)) x.score += wordBonus;
        }
        x.ctx = pushCtx(x.ctx, 0);
        append(x, " ");
        x.wordStart = x.n; x.wordInT = true;
    }
    static bool wordKnown(const char* w, int n) {
        if (lmWord(w, n)) return true;
        bool digits = true; for (int i = 0; i < n; ++i) digits &= w[i] >= '0' && w[i] <= '9';
        if (digits && n <= 4) return true;
        if (n >= 3 && n <= 10) { std::string s(w, (size_t)n); return isCallsign(s); }
        return false;
    }
    static bool same(const Hyp& a, const Hyp& b) {
        return a.ctx == b.ctx && a.code == b.code && a.nel == b.nel && a.spoilt == b.spoilt && a.n == b.n && !std::memcmp(a.t, b.t, a.n);
    }
    /** The best K of the candidates (identical readings merged, the far-behind dropped), then the agreed text committed. */
    void keep(Hyp* nx, int m) {
        std::sort(nx, nx + m, [](const Hyp& a, const Hyp& b) { return a.score > b.score; });
        nh = 0;
        for (int i = 0; i < m && nh < K; ++i) {
            if (nx[i].score < nx[0].score - kBeam) break;
            bool dup = false; for (int j = 0; j < nh; ++j) if (same(h[j], nx[i])) { dup = true; break; }
            if (!dup) h[nh++] = nx[i];
        }
        // ★ Scores relative to the best, so they never grow without bound
        const float top = h[0].score; for (int i = 0; i < nh; ++i) h[i].score -= top;
        commit();
    }
    void commit() {
        // ★ The text every reading agrees on is final.
        int c = h[0].n;
        for (int i = 1; i < nh; ++i) { int k = 0; while (k < c && k < h[i].n && h[i].t[k] == h[0].t[k]) ++k; c = k; }
        // ★ A run too long to hold: the best reading's older half is committed and the readings that disagree go.
        if (c == 0 && h[0].n >= kPend - 8) {
            c = h[0].n - 16;
            int k = 0;
            for (int i = 0; i < nh; ++i) if (h[i].n >= c && !std::memcmp(h[i].t, h[0].t, (size_t)c)) h[k++] = h[i];
            nh = k;
        }
        if (c <= 0) return;
        out.append(h[0].t, (size_t)c); chars += (uint32_t)c;
        for (int i = 0; i < nh; ++i) {
            Hyp& x = h[i];
            std::memmove(x.t, x.t + c, (size_t)(x.n - c)); x.n = (uint8_t)(x.n - c);
            if (x.wordStart >= c) x.wordStart = (uint8_t)(x.wordStart - c); else { x.wordStart = 0; x.wordInT = false; }
        }
    }
};

} // namespace
} // namespace cw
} // namespace vibe
