// test-navtex-fec.cpp — the NAVTEX (CCIR 476 / SITOR-B FEC) coder, word by word (audit 2026-10-04, rows 11-13).
//
// ★★ WHY. SITOR-B sends every character twice — the DX copy, then the RX copy five slots later — and the decoder's
//    whole job is to keep the two streams apart and use one copy to repair the other. Three ways it failed:
//     · one misread character that happened to be a PHASING code (0x66 / 0x0F) swapped DX and RX for the rest of the
//       message (row 12);
//     · bad words were counted in BOTH slots, repaired or not, and three of them threw away the shift and the phase
//       (row 11 — measured on audio by scripts/test-navtex.sh);
//     · FIGS 0x4B (BEL) reached the UI as a raw control byte (row 13).
//  These drive Ccir476 with exact code streams, built the way a transmitter builds them (fldigi's create_fec), so each
//  failure is one corrupted word and the expected text is known to the character. Against the coder before 2026-10-04
//  eight of these fail (the scoring, three of the four phasing misreads, the '_' and both BEL checks).
#include "decoders/fsk_decoder.h"
#include <cstdio>
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
/** Slot of the DX / RX copy of code `i` in a stream from fec(.., phasing). */
static size_t dxSlot(int phasing, size_t i) { return 2 * phasing + 2 * i; }
static size_t rxSlot(int phasing, size_t i) { return 2 * phasing + 2 * i + 5; }

struct Run { std::string text; int score = 0, worst = 1; };
static Run decode(const std::vector<uint8_t>& s, size_t from = 0) {
    Ccir476 c; c.reset(); Run r;
    for (size_t i = from; i < s.size(); i++) {
        int sc = 0; const char32_t ch = c.processChar(s[i], sc);
        r.score += sc; if (sc < r.worst) r.worst = sc;
        if (ch) r.text += (char)ch;
    }
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
    const auto clean = fec(codes, P);
    // the code that prints the 'S' of "SW" — the message's first S, a letter in the middle of a line
    size_t iS = 0;
    for (size_t i = 0; i < codes.size(); i++) if (codes[i] == find(LT, 'S')) { iS = i; break; }

    std::printf("── 1. clean stream ──\n");
    { const Run r = decode(clean); ok(r.text == msg, "decodes exactly: " + show(r.text.substr(0, 40)) + "…"); }

    std::printf("── 2. one copy lost: the other repairs it, and only an RX loss scores ──\n");
    { auto s = clean; s[rxSlot(P, iS)] = 0x00;            // RX copy unreadable
      const Run r = decode(s); ok(r.text == msg, "RX copy unreadable → repaired from the DX copy");
      ok(r.worst == 0, "...scored 0 (repaired), not -2"); }
    { auto s = clean; s[dxSlot(P, iS)] = 0x00;            // DX copy unreadable
      const Run r = decode(s); ok(r.text == msg, "DX copy unreadable → the RX copy prints");
      ok(r.worst >= 0, "...and the DX slot does not score at all"); }

    std::printf("── 3. row 12: one misread word that is a PHASING code does not swap DX and RX ──\n");
    { auto s = clean; s[rxSlot(P, iS)] = REP;
      const Run r = decode(s); ok(r.text == msg, "RX copy misread as 0x66 → still exact: " + show(r.text.substr(11, 12))); }
    { auto s = clean; s[rxSlot(P, iS)] = ALPHA;
      const Run r = decode(s); ok(r.text == msg, "RX copy misread as 0x0F → still exact"); }
    { auto s = clean; s[dxSlot(P, iS)] = ALPHA;
      const Run r = decode(s); ok(r.text == msg, "DX copy misread as 0x0F → still exact"); }
    { auto s = clean; s[dxSlot(P, iS)] = REP;
      const Run r = decode(s); ok(r.text == msg, "DX copy misread as 0x66 → still exact"); }

    std::printf("── 4. ...but real phasing in the wrong phase IS followed ──\n");
    { const Run r = decode(clean, 1);   // start one slot late: the decoder's first guess is the wrong phase
      ok(r.text == msg, "starting on an RX slot, the phasing run sets the phase: " + show(r.text.substr(0, 20))); }
    { // a message, then a second one whose phasing arrives in the OPPOSITE slot parity (as after a slip)
      auto s = clean; s.push_back(CHAR32); const auto two = fec(encode("ZCZC GB17\r\nNNNN\r\n"), P);
      s.insert(s.end(), two.begin(), two.end());
      const Run r = decode(s); ok(r.text == msg + "ZCZC GB17\r\nNNNN\r\n", "a later phasing run in the other parity re-phases");
    }

    std::printf("── 5. both copies lost → '_' (ITU-R M.476), never a silent gap ──\n");
    { auto s = clean; s[rxSlot(P, iS)] = 0x00; s[dxSlot(P, iS)] = 0x00;
      std::string want = msg; want[want.find("SW")] = '_';
      const Run r = decode(s); ok(r.text == want, "prints " + show(r.text.substr(10, 12)));
      ok(r.worst == -2, "...and scores -2"); }

    std::printf("── 6. row 13: BEL ──\n");
    { const std::string m = "ZCZC GA43\r\n\x07NNNN\r\n";
      const Run r = decode(fec(encode(m), P));
      ok(r.text.find('\x07') == std::string::npos, "no BEL (0x07) in the output: " + show(r.text));
      ok(r.text == "ZCZC GA43\r\n'NNNN\r\n", "printed as an apostrophe, as fldigi does"); }

    if (fails) { std::printf("  %d FAILED\n", fails); return 1; }
    std::printf("  all passed\n");
    return 0;
}
