// test-rtty-shift.cpp — RTTY's letters/figures shift across a lost signal (2026-10-13).
//
// ★★ WHY. A fade that costs the lock and finds it again within 5 s KEEPS the sender's shift (5dee869c, NickB: DWD's
//    synop groups dropped back to letters). But the fade can swallow the LTRS itself, and then the next word printed in
//    figures — Stuart on DDK2 4583 kHz: "FREQUENCIES 4583((0"" for "4583 KHZ", "DDK" DHU". A kept FIGURES shift is
//    now provisional: the first printing character after the re-sync decides (Ita2::processChar). Covered:
//     · the coder, code by code: a lost LTRS → the word back in letters; a synop group → still figures; '/', '.', '-'
//       keep figures; space decides nothing; an explicit FIGS / LTRS always wins; NO re-sync = no change (a '$' in
//       figures stays a '$'); a reset (≥ 5 s, never locked) is letters as before; a kept LETTERS shift is not judged;
//     · the whole decoder on audio (50 Bd, 450 Hz, reverse — DWD): a second of silence where the LTRS was → "DDH7";
//       a second of silence inside a synop line → the groups after it still print as digits.
#include "decoders/fsk_decoder.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

// ITA2 (the decoder's table, fsk_decoder.cpp): letters and figures by code
static const char* LT = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\0MXV\0";
static const char* FG = "\0" "3\n- \a87\r$4',!:(5\")2#6019?&\0./;\0";
static const int LTRS = 31, FIGS = 27;

/** 5N1 code word for the coder: start (0) at bit 0, data bits 1..5, stop (1) at bit 6. */
static uint16_t word(int code) { return (uint16_t)((code << 1) | (1 << 6)); }
static int codeOf(const char* t, char c) { for (int i = 1; i < 32; i++) if (t[i] == c) return i; return -1; }

/** Codes for `text` as a sender sends them: a shift only when a character needs one; space/CR/LF never shift. */
static std::vector<int> encode(const std::string& text, bool& fig) {
    std::vector<int> v;
    for (char c : text) {
        const int l = codeOf(LT, c), f = codeOf(FG, c);
        if (c == ' ' || c == '\r' || c == '\n') v.push_back(l);
        else if (l >= 0) { if (fig) { v.push_back(LTRS); fig = false; } v.push_back(l); }
        else if (f >= 0) { if (!fig) { v.push_back(FIGS); fig = true; } v.push_back(f); }
    }
    return v;
}
/** Feed raw codes; returns the text (the coder prints one character late, so a trailing space flushes it). */
static std::string feed(Ita2& c, const std::vector<int>& codes) {
    std::string out;
    for (int k : codes) { const char32_t ch = c.processChar(word(k)); if (ch) out += (char)ch; }
    return out;
}
static std::string flush(Ita2& c) { return feed(c, { 4 }); }

static void coder() {
    std::printf("── the coder: a kept figures shift after a re-sync ──\n");
    {   // the LTRS before "DDH7" lost in the fade
        Ita2 c("5N1"); bool fig = false;
        std::string out = feed(c, encode("DE DDK2 ", fig));
        ok(c.inFigures(), "after \"DDK2 \" the coder is in figures");
        c.resync();                                     // the fade: the LTRS that came next was never seen
        ok(c.provisional(), "a re-sync in figures makes the shift provisional");
        out += feed(c, { 9, 9, 20, FIGS, 7, 4 });       // D D H FIGS 7 space — the LTRS before D is missing
        out += flush(c);
        ok(out.find("DDH7") != std::string::npos, "lost LTRS: the next word is back in letters (\"" + out + "\")");
        ok(out.find('$') == std::string::npos && out.find('#') == std::string::npos, "...and no '$' or '#' printed");
    }
    {   // KHZ after "4583 ", LTRS lost — Stuart's '"'
        Ita2 c("5N1"); bool fig = false;
        std::string out = feed(c, encode("FREQUENCIES 4583 ", fig));
        c.resync();
        out += feed(c, { 15, 20, 17, 4 });              // K H Z space
        out += flush(c);
        ok(out.find("4583") != std::string::npos && out.find("KHZ") != std::string::npos && out.find('(') == std::string::npos,
           "lost LTRS before KHZ: KHZ, not '(#\"' (\"" + out + "\")");
    }
    {   // a synop line: the fade falls between groups — the groups after it stay digits
        Ita2 c("5N1"); bool fig = false;
        std::vector<int> a = encode("SYNOP 10384 41/96 ", fig), b = encode("82508 10118 8807/ 10100.8 -5 ", fig);
        std::string out = feed(c, a);
        c.resync();
        out += feed(c, b) + flush(c);
        ok(out.find("82508 10118 8807/ 10100.8 -5") != std::string::npos, "synop after a short loss: still figures (\"" + out + "\")");
        ok(!c.provisional(), "...and the first digit settled it");
    }
    for (const char* first : { "/96 ", ".8 ", "-5 " }) {
        Ita2 c("5N1"); bool fig = false;
        feed(c, encode("41 ", fig)); c.resync();
        const std::string out = feed(c, encode(first, fig)) + flush(c);
        ok(out.rfind(std::string(first).substr(0, 3), 0) == 0, std::string("'") + first[0] + "' first after a re-sync keeps figures (\"" + out + "\")");
    }
    {   // a space decides nothing: the character after it does
        Ita2 c("5N1"); bool fig = false;
        feed(c, encode("4583 ", fig)); c.resync();
        std::string out = feed(c, { 4, 4, 15, 20, 17, 4 }) + flush(c);
        ok(out.find("KHZ") != std::string::npos, "spaces after a re-sync leave it provisional; the letter after decides (\"" + out + "\")");
    }
    {   // an explicit FIGS after the re-sync wins over the judgement
        Ita2 c("5N1"); bool fig = false;
        feed(c, encode("4583 ", fig)); c.resync();
        std::string out = feed(c, { FIGS, 9, 20, 4 }) + flush(c);
        ok(out.find("$#") != std::string::npos && !c.provisional(), "an explicit FIGS settles it: '$#' prints as sent (\"" + out + "\")");
    }
    {   // NO re-sync: nothing changes — a '$' in figures is a '$'
        Ita2 c("5N1"); bool fig = false;
        std::string out = feed(c, encode("4583 ", fig));
        ok(!c.provisional(), "no re-sync: not provisional");
        out += feed(c, { 9, 20, 4 }) + flush(c);
        ok(out.find("$#") != std::string::npos, "no re-sync: figures punctuation prints as figures (\"" + out + "\")");
    }
    {   // a long loss / never locked: reset, letters, as before
        Ita2 c("5N1"); bool fig = false;
        feed(c, encode("4583 ", fig)); c.reset();
        ok(!c.inFigures() && !c.provisional(), "reset (≥ 5 s or never locked): letters, not provisional");
        const std::string out = feed(c, { 23, 22, 4 }) + flush(c);   // Q P → letters as before
        ok(out.find("QP") != std::string::npos, "reset: 'QP' in letters as before (\"" + out + "\")");
    }
    {   // a kept LETTERS shift is never judged
        Ita2 c("5N1"); bool fig = false;
        feed(c, encode("CQ DE ", fig)); c.resync();
        ok(!c.provisional(), "a re-sync in letters is not provisional");
        const std::string out = feed(c, { 23, 22, 4 }) + flush(c);
        ok(out.find("QP") != std::string::npos, "...and 'QP' stays letters (\"" + out + "\")");
    }
}

// ── audio: DWD (50 Bd, 450 Hz, reverse) through the whole decoder, with a silent second where the fade was ─────────
struct Sig {
    std::vector<int16_t> s; double ph = 0;
    const double fs = 48000, spb = 48000 / 50.0, mark = 1000 - 225, space = 1000 + 225;   // reverse: mark below
    void tone(double f, double n) { for (int i = 0; i < (int)n; i++) { ph += 2 * M_PI * f / fs; s.push_back((int16_t)(12000 * std::sin(ph))); } }
    void code(int c) { tone(space, spb); for (int b = 0; b < 5; b++) tone((c >> b) & 1 ? mark : space, spb); tone(mark, 1.5 * spb); }
    void codes(const std::vector<int>& v) { for (int c : v) code(c); }
    void silence(double secs) { for (int i = 0; i < (int)(secs * fs); i++) s.push_back(0); }
};
static std::string decode(const Sig& g, unsigned long* resyncs = nullptr) {
    FskDecoder d(48000, 1000, 450, 50, "5N1.5", "ITA2", true);
    std::string out;
    d.onChar = [&](char32_t c) { if (c != U'\r') out += c < 128 ? (char)c : '?'; };
    for (size_t i = 0; i < g.s.size(); i += 960) d.process(&g.s[i], (int)std::min<size_t>(960, g.s.size() - i));
    if (resyncs) *resyncs = d.resyncs();
    return out;
}

static void audio() {
    std::printf("── the decoder on audio: a silent second where the fade was ──\n");
    {
        Sig g; bool fig = false; g.tone(g.mark, 4800);
        g.codes({ LTRS, LTRS });
        for (int i = 0; i < 3; i++) g.codes(encode("CQ CQ CQ DE DDK2 ", fig));
        g.silence(1.0);                                         // the fade — and the LTRS with it
        g.tone(g.mark, 2400);
        fig = false;                                            // the sender sent LTRS; we never heard it
        for (int i = 0; i < 3; i++) g.codes(encode("DDH7 DDK9 ", fig));
        g.codes(encode("FREQUENCIES 4583 KHZ   ", fig));
        g.tone(g.mark, 9600);
        const std::string out = decode(g);
        const size_t cut = out.rfind("DDK2");
        const std::string after = cut == std::string::npos ? out : out.substr(cut);
        ok(after.find("DDH7 DDK9") != std::string::npos && after.find('$') == std::string::npos,
           "lost LTRS in a 1 s fade: the next word prints in letters (\"" + after.substr(0, 40) + "\")");
    }
    {
        Sig g; bool fig = false; g.tone(g.mark, 4800);
        g.codes({ LTRS, LTRS });
        g.codes(encode("SYNOP 10384 41/96 82508 10118 20072 ", fig));
        g.silence(1.0);
        g.tone(g.mark, 2400);
        for (int i = 0; i < 3; i++) g.codes(encode("30115 40206 57012 8807/ 333 10173 20065 ", fig));
        g.tone(g.mark, 9600);
        unsigned long rs = 0;
        const std::string out = decode(g, &rs);
        ok(rs >= 1, "the silence cost the lock (re-syncs counted: " + std::to_string(rs) + ")");
        ok(out.find("57012 8807/ 333 10173 20065") != std::string::npos,
           "synop after a 1 s fade: the groups still print as digits (\"" + out.substr(out.size() > 60 ? out.size() - 60 : 0) + "\")");
    }
}

int main() {
    coder();
    audio();
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
