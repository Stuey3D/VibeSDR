// test-navtex-fec.cpp — the NAVTEX (CCIR 476 / SITOR-B FEC) receiver's character layer, bit by bit (audit 2026-10-04,
// rows 11-13; rewritten 2026-10-05 for NavtexRx).
//
// ★★ WHY. SITOR-B sends every character twice — the DX copy, then the RX copy five slots later — and the decoder's
//    whole job is to find which slots are which and use one copy to repair the other. These drive NavtexRx::pushBit
//    with exact bit streams built the way a transmitter builds them (fldigi's create_fec), each bit a SOFT value
//    (+1 mark, -1 space, smaller = less sure), so every failure is one corrupted word and the expected text is known
//    to the character. Covered:
//     · the lock: from phasing, one slot late, MID-MESSAGE with no phasing at all, inverted tones, and never on noise
//       (the old rule — four valid words in a row — locks on random bits: 27 % of 7-bit words are valid);
//     · a phasing code misread in either copy does not swap DX and RX (row 12); a real phasing run in the other
//       parity (after a slip) is followed;
//     · FEC: either copy lost; both lost → '_' (and only in text); the soft tiers — the two copies summed, the least
//       certain bit flipped (BOTH ways: fldigi's flip_smallest_bit never made the 5-mark → 4-mark repair), and two
//       valid copies that disagree settled by their soft bits;
//     · FIGS BEL prints an apostrophe (row 13); the per-message clean / repaired / lost counts.
#include "decoders/fsk_decoder.h"
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

// code -> char, the decoder's own table (fsk_decoder.cpp Ccir476, = fldigi's code_to_ltrs / code_to_figs), rows of 16
static const char* LT = "________________" "_______J___F_CK_" "_______W___Y_PQ_" "_____G___MX_V___"
                        "_______A___S_IU_" "___D_RE__N__ ___" "___Z_L___H__\n___" "_OB_T___\r_______";
static const char* FG = "________________" "_______'___!_:(_" "_______2___6_01_" "_____&___./_;___"
                        "_______-___\x07_87_" "___$_43__,__ ___" "___\"_)___#__\n___" "_9?_5___\r_______";
static const uint8_t LTRS = 0x5A, FIGS = 0x36, ALPHA = 0x0F, REP = 0x66, CHAR32 = 0x6A;

static int find(const char* t, char c) { for (int i = 0; i < 128; i++) if (t[i] == c && c != '_') return i; return -1; }
static std::vector<uint8_t> encode(const std::string& text) {
    std::vector<uint8_t> v{LTRS}; bool fig = false;
    for (char c : text) {
        const int l = find(LT, c), f = find(FG, c);
        if (c == ' ' || c == '\r' || c == '\n') v.push_back((uint8_t)l);
        else if (l >= 0 && !fig) v.push_back((uint8_t)l);
        else if (f >= 0 && fig) v.push_back((uint8_t)f);
        else if (f >= 0) { v.push_back(FIGS); v.push_back((uint8_t)f); fig = true; }
        else { v.push_back(LTRS); v.push_back((uint8_t)l); fig = false; }
    }
    return v;
}
/** fldigi's create_fec: phasing pairs (DX 0x66, RX 0x0F), then DX of code i in slot 2i, its RX in slot 2i + 5. */
static std::vector<uint8_t> fec(const std::vector<uint8_t>& c, int phasing) {
    std::vector<uint8_t> s;
    for (int i = 0; i < phasing; i++) { s.push_back(REP); s.push_back(ALPHA); }
    for (size_t i = 0; i < c.size(); i++) { s.push_back(c[i]); s.push_back(i >= 2 ? c[i - 2] : ALPHA); }
    s.push_back(CHAR32); s.push_back(c[c.size() - 2]); s.push_back(CHAR32); s.push_back(c.back());
    return s;
}
static size_t dxSlot(int phasing, size_t i) { return 2 * phasing + 2 * i; }
static size_t rxSlot(int phasing, size_t i) { return 2 * phasing + 2 * i + 5; }

/** Slots → soft bits, least significant bit first (+1 mark). Then phasing after, so the last characters flush. */
static std::vector<double> bits(const std::vector<uint8_t>& slots, int tail = 20) {
    std::vector<double> b;
    auto put = [&](uint8_t c) { for (int i = 0; i < 7; i++) b.push_back((c >> i & 1) ? 1.0 : -1.0); };
    for (uint8_t c : slots) put(c);
    for (int i = 0; i < tail; i++) { put(REP); put(ALPHA); }
    return b;
}
/** Overwrite the soft bits of one slot (7 values). */
static void setSlot(std::vector<double>& b, size_t slot, const double (&v)[7]) { for (int i = 0; i < 7; i++) b[slot * 7 + i] = v[i]; }
static void softWord(uint8_t code, double mag, double (&out)[7]) { for (int i = 0; i < 7; i++) out[i] = (code >> i & 1) ? mag : -mag; }

struct Run { std::string text; NavtexRx::Counts total, last; bool inverted = false; int lockedAt = -1; };
static Run decode(const std::vector<double>& b, size_t from = 0, const NavtexOptions& o = NavtexOptions()) {
    NavtexRx rx(48000, 500, 170, 100, false, o);
    Run r;
    rx.onChar = [&](char32_t c) { r.text += (char)c; };
    for (size_t i = from; i < b.size(); i++) {
        rx.pushBit(b[i]);
        if (r.lockedAt < 0 && rx.stateNow() == NavtexRx::ReadData) r.lockedAt = (int)(i - from);
    }
    r.total = rx.total; r.last = rx.lastMessage; r.inverted = rx.invertedNow();
    return r;
}
static std::string show(std::string s) {
    std::string o; for (char c : s) { if (c == '\r') o += "\\r"; else if (c == '\n') o += "\\n"; else if (c == 7) o += "<BEL>"; else o += c; }
    return o;
}

int main() {
    const std::string msg = "ZCZC GA42\r\nWIND SW 6 TO 8, GUSTS 45KT (ROUGH)\r\nPOSN 51-23.4N 001-45.2E\r\nNNNN\r\n";
    const int P = 20;
    const auto codes = encode(msg);
    const auto slots = fec(codes, P);
    const auto clean = bits(slots);
    // the code that prints the 'S' of "SW" — the message's first S, a letter in the middle of a line
    size_t iS = 0;
    for (size_t i = 0; i < codes.size(); i++) if (codes[i] == find(LT, 'S')) { iS = i; break; }
    double w[7], w2[7];

    std::printf("── 1. the lock ──\n");
    { const Run r = decode(clean); ok(r.text == msg, "clean stream decodes exactly: " + show(r.text.substr(0, 40)) + "…");
      ok(r.last.clean > 60 && r.last.repaired == 0 && r.last.failed == 0, "...the message counted all clean (" + std::to_string(r.last.clean) + ")"); }
    { const Run r = decode(clean, 7);   // one slot late: the first slot seen is an RX slot
      ok(r.text == msg, "starting one slot late (an RX slot first): exact"); }
    { const Run r = decode(clean, 3);   // and mid-word
      ok(r.text == msg, "starting three bits into a word: exact"); }
    { // ★ MID-MESSAGE, no phasing at all: join at the DX copy of character 20
      const size_t from = dxSlot(P, 20) * 7; const Run r = decode(clean, from);
      const bool suffix = r.text.size() <= msg.size() && msg.compare(msg.size() - r.text.size(), r.text.size(), r.text) == 0;
      ok(suffix && r.text.size() + 30 >= msg.size() - 18,
         "joining mid-message (no phasing) prints the rest exactly, phase and all: …" + show(r.text.substr(0, 24)));
      ok(r.lockedAt >= 0 && r.lockedAt <= 110, "...locked " + std::to_string(r.lockedAt) + " bits in (one second is 100)"); }
    { auto inv = clean; for (double& v : inv) v = -v;
      const Run r = decode(inv); ok(r.text == msg && r.inverted, "inverted tones: found the polarity itself, exact"); }
    { // ★ NOISE NEVER LOCKS: 10 minutes of random soft bits
      std::mt19937 g(5); std::normal_distribution<double> n(0, 1);
      std::vector<double> noise(60000); for (double& v : noise) v = n(g);
      const Run r = decode(noise); ok(r.lockedAt < 0 && r.text.empty(), "10 minutes of noise: no lock, nothing printed ("
                                      + std::to_string(r.text.size()) + " chars)"); }

    std::printf("── 2. one copy lost: the other repairs it ──\n");
    { auto b = clean; softWord(0x00, 1, w); setSlot(b, rxSlot(P, iS), w);
      const Run r = decode(b); ok(r.text == msg, "RX copy unreadable → repaired from the DX copy");
      ok(r.last.repaired == 1 && r.last.failed == 0, "...counted as repaired"); }
    { auto b = clean; softWord(0x00, 1, w); setSlot(b, dxSlot(P, iS), w);
      const Run r = decode(b); ok(r.text == msg, "DX copy unreadable → the RX copy prints");
      ok(r.last.repaired == 0, "...and the DX slot is not counted at all"); }

    std::printf("── 3. row 12: one misread word that is a PHASING code does not swap DX and RX ──\n");
    for (uint8_t bad : { REP, ALPHA })
        for (bool rxCopy : { true, false }) {
            auto b = clean; softWord(bad, 1, w); setSlot(b, rxCopy ? rxSlot(P, iS) : dxSlot(P, iS), w);
            const Run r = decode(b);
            ok(r.text == msg, std::string(rxCopy ? "RX" : "DX") + " copy misread as " + (bad == REP ? "0x66" : "0x0F") + " → still exact");
        }
    { // a message, then a second one whose phasing arrives in the OPPOSITE slot parity (as after a slip)
      auto s = slots; s.push_back(CHAR32); const auto two = fec(encode("ZCZC GB17\r\nNNNN\r\n"), P);
      s.insert(s.end(), two.begin(), two.end());
      const Run r = decode(bits(s)); ok(r.text == msg + "ZCZC GB17\r\nNNNN\r\n", "a later phasing run in the other parity re-phases");
    }

    std::printf("── 4. both copies lost → '_' (ITU-R M.476), in text only ──\n");
    { auto b = clean; softWord(0x00, 1, w); setSlot(b, rxSlot(P, iS), w); setSlot(b, dxSlot(P, iS), w);
      std::string want = msg; want[want.find("SW")] = '_';
      const Run r = decode(b); ok(r.text == want, "prints " + show(r.text.substr(10, 12)));
      ok(r.last.failed == 1, "...and counts one lost"); }
    { auto b = clean; softWord(0x00, 1, w); setSlot(b, rxSlot(P, 0) - 4, w); setSlot(b, rxSlot(P, 0) - 9, w);   // an RX slot and its DX
      const Run r = decode(b); ok(r.text == msg, "...but a lost word in PHASING prints nothing"); }

    std::printf("── 5. soft FEC ──\n");
    const uint8_t S = codes[iS];
    for (bool ml : { false, true }) {   // fldigi's tiers (sum, then the flips), and maximum likelihood over both copies
      NavtexOptions o; o.fecMl = ml; const std::string tag = ml ? "  [ML]" : "  [fldigi's tiers]";
      { // both copies one bit wrong, different bits, weakly: the sum is right
        auto b = clean; softWord(S, 1, w); softWord(S, 1, w2);
        w[0] = -w[0] * 0.3; w2[3] = -w2[3] * 0.3;                 // RX: bit 0 flipped weakly; DX: bit 3 flipped weakly
        setSlot(b, rxSlot(P, iS), w); setSlot(b, dxSlot(P, iS), w2);
        const Run r = decode(b, 0, o); ok(r.text == msg, "both copies one bit wrong (different bits): the two summed are right" + tag); }
      { // ★ RX with a SPACE read weakly as MARK (5 marks), DX lost: flip the weakest mark → fldigi never made this repair
        auto b = clean; softWord(S, 1, w);
        int sp = -1; for (int i = 0; i < 7; i++) if (!(S >> i & 1)) { sp = i; break; }
        w[sp] = 0.2;                                               // a weak mark where a space was sent
        setSlot(b, rxSlot(P, iS), w); softWord(0x00, 1, w2); setSlot(b, dxSlot(P, iS), w2);
        const Run r = decode(b, 0, o); ok(r.text == msg, "5 marks (a weak extra mark), DX lost: the weakest mark flipped back" + tag); }
      { // RX with a MARK read weakly as SPACE (3 marks), DX lost
        auto b = clean; softWord(S, 1, w);
        int mk = -1; for (int i = 0; i < 7; i++) if (S >> i & 1) { mk = i; break; }
        w[mk] = -0.2;
        setSlot(b, rxSlot(P, iS), w); softWord(0x00, 1, w2); setSlot(b, dxSlot(P, iS), w2);
        const Run r = decode(b, 0, o); ok(r.text == msg, "3 marks (a weak missing mark), DX lost: the weakest space flipped back" + tag); }
      { // two mark bits weak and wrong in BOTH copies: every tier sees 2 marks and gives up; the best word is still S
        auto b = clean; softWord(S, 1, w); softWord(S, 1, w2);
        int m1 = -1, m2 = -1; for (int i = 0; i < 7; i++) if (S >> i & 1) { if (m1 < 0) m1 = i; else if (m2 < 0) m2 = i; }
        w[m1] = w[m2] = -0.2; w2[m1] = w2[m2] = -0.3;
        setSlot(b, rxSlot(P, iS), w); setSlot(b, dxSlot(P, iS), w2);
        std::string want = msg; if (!ml) want[want.find("SW")] = '_';
        const Run r = decode(b, 0, o);
        ok(r.text == want, std::string(ml ? "two weak wrong bits in both copies: recovered" : "two weak wrong bits in both copies: '_'") + tag); }
    }
    { // ★ two VALID copies that disagree: a burst turned the RX copy into another letter (two weak bits); DX is sure
      auto b = clean; softWord(S, 1, w);
      int mk = -1, sp = -1; for (int i = 0; i < 7; i++) { if ((S >> i & 1) && mk < 0) mk = i; if (!(S >> i & 1) && sp < 0) sp = i; }
      w[mk] = -0.3; w[sp] = 0.3;                                 // still 4 marks: a valid, WRONG word
      setSlot(b, rxSlot(P, iS), w);
      const Run r = decode(b); ok(r.text == msg, "RX copy a different VALID letter (weak bits), DX sure: the DX copy wins"); }

    std::printf("── 6. row 13: BEL ──\n");
    { const std::string m = "ZCZC GA43\r\n\x07NNNN\r\n";
      const Run r = decode(bits(fec(encode(m), P)));
      ok(r.text.find('\x07') == std::string::npos, "no BEL (0x07) in the output: " + show(r.text));
      ok(r.text == "ZCZC GA43\r\n'NNNN\r\n", "printed as an apostrophe, as fldigi does"); }

    if (fails) { std::printf("  %d FAILED\n", fails); return 1; }
    std::printf("  all passed\n");
    return 0;
}
