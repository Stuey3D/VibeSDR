// test-navtex-snr.cpp — beta96 (Stuart 11-26): NAVTEX's SNR readout, measured on synthetic SITOR-B FSK at KNOWN SNRs.
//
// ★ DISPLAY ONLY — NavtexRx::measureBit reads the front end and writes nothing the decoder reads. These check that the
//   number it reports is right: within ±1 dB of the SNR the signal was made with, in the BIT BANDWIDTH (100 Hz at
//   100 baud, as the SITOR-A status reports). The noise is white,
//   specified in 3 kHz), so bit-bandwidth SNR = SNR(3 kHz) + 10·log10(3000/100) = +14.8 dB.
// Also: the 1-second slices (a mid-message fade reads low in the middle, and only there); the message's own SNR
// carried in lastMessage (what the host sends in op 0x07); the signal / noise dBFS; the decoded text unchanged by it.
#include "decoders/fsk_decoder.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

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
static std::vector<uint8_t> fec(const std::vector<uint8_t>& c, int phasing) {
    std::vector<uint8_t> s;
    for (int i = 0; i < phasing; i++) { s.push_back(REP); s.push_back(ALPHA); }
    for (size_t i = 0; i < c.size(); i++) { s.push_back(c[i]); s.push_back(i >= 2 ? c[i - 2] : ALPHA); }
    s.push_back(CHAR32); s.push_back(c[c.size() - 2]); s.push_back(CHAR32); s.push_back(c.back());
    for (int i = 0; i < 20; i++) { s.push_back(REP); s.push_back(ALPHA); }
    return s;
}

/** Phase-continuous FSK, 48 kHz, mark = 585 Hz (the higher), space 415; amplitude 0.02 FS;
 *  white noise SNR dB in 3 kHz; `fade(t)` scales the signal (1 = none). */
template <class F>
static std::vector<int16_t> audio(const std::vector<uint8_t>& slots, double snr3k, unsigned seed, F fade) {
    const double FS = 48000, BAUD = 100, A = 0.02;   // -34 dBFS: the noise never clips (a real receiver's AGC holds the total)
    std::mt19937 g(seed); std::normal_distribution<double> n(0, 1);
    /* ★ TRUE POWER SNR in 3 kHz: signal power A²/2, noise power in 3 kHz = σ²·3000/24000. (synth_navtex.py's own
     *  "snr" sets σ from the AMPLITUDE, √0.5·A·…, which puts its noise 3 dB under what its label says in power terms —
     *  its "+15 dB" is a 12 dB power SNR. Left alone there: the bench's CER thresholds were measured on it.) */
    const double sd = 2 * A * std::pow(10, -snr3k / 20);
    std::vector<int16_t> out; double ph = 0;
    const double spb = FS / BAUD; double acc = 0;
    for (size_t k = 0; k < slots.size() * 7; k++) {
        const bool mark = slots[k / 7] >> (k % 7) & 1;
        acc += spb; const int ns = (int)acc; acc -= ns;
        for (int i = 0; i < ns; i++) {
            ph += 2 * M_PI * (mark ? 585.0 : 415.0) / FS;
            if (ph > 2 * M_PI) ph -= 2 * M_PI;
            const double t = out.size() / FS;
            double y = A * fade(t) * std::sin(ph) + sd * n(g);
            y = std::max(-1.0, std::min(1.0, y));
            out.push_back((int16_t)std::lround(y * 32767));
        }
    }
    return out;
}
struct Res { std::string text; NavtexRx::Counts last; double sig = NAN, noise = NAN; std::vector<double> sliceSnr, sliceSig; bool msgOk = false; };
static Res run(const std::vector<int16_t>& a) {
    NavtexRx rx(48000, 500, 170, 100, false);
    Res r;
    rx.onChar = [&](char32_t c) { r.text += (char)c; };
    rx.onSlice = [&](double s, double n) { r.sliceSnr.push_back(s - n); r.sliceSig.push_back(s); };
    for (size_t i = 0; i < a.size(); i += 960) rx.process(&a[i], (int)std::min<size_t>(960, a.size() - i));
    r.last = rx.lastMessage;
    r.msgOk = rx.snrOf(r.last, r.sig, r.noise);
    return r;
}

int main() {
    std::string msg = "ZCZC GA42\r\n";
    for (int i = 0; i < 6; i++) msg += "WIND SW 6 TO 8, GUSTS 45KT, SEA ROUGH. POSN 51-23.4N 001-45.2E VIS GOOD\r\n";
    msg += "NNNN\r\n";
    const auto slots = fec(encode(msg), 30);
    const double kBw = 10 * std::log10(3000.0 / 100.0);   // 3 kHz → the 100 Hz bit bandwidth
    std::printf("── 1. the message's SNR at known SNRs (±1 dB, bit bandwidth) ──\n");
    for (double s3 : { -8.0, -5.0, 0.0, 5.0, 10.0, 20.0, 60.0 }) {
        const Res r = run(audio(slots, s3, 7, [](double) { return 1.0; }));
        const double want = s3 + kBw, got = r.sig - r.noise;
        char b[200]; std::snprintf(b, sizeof b, "%+.0f dB in 3 kHz → want %.1f dB, read %.1f dB (sig %.1f dBFS, noise %.1f dBFS) · %zu chars",
                                   s3, want, got, r.sig, r.noise, r.text.size());
        ok(r.msgOk && std::fabs(got - want) <= 1.0, b);
    }
    { // the signal dBFS: 0.25 FS sine = -12.0 dBFS
      const Res r = run(audio(slots, 20, 3, [](double) { return 1.0; }));
      char b[120]; std::snprintf(b, sizeof b, "signal level: %.1f dBFS (a 0.02 FS sine is -34.0)", r.sig);
      ok(std::fabs(r.sig + 34.0) <= 0.5, b); }

    std::printf("── 2. 1-second slices: a fade in the middle reads low there, and only there ──\n");
    { const double total = slots.size() * 0.07;
      const double f0 = total * 0.45, f1 = f0 + 6;   // a 6-second, 25 dB fade in the middle of the message
      const Res r = run(audio(slots, 0, 11, [&](double t) { return t >= f0 && t < f1 ? std::pow(10, -25 / 20.0) : 1.0; }));
      const int n = (int)r.sliceSnr.size();
      ok(std::abs(n - (int)std::lround(total)) <= 1, "one slice a second: " + std::to_string(n) + " for " + std::to_string((int)total) + " s");
      int lowIn = 0, lowOut = 0, inN = 0;
      for (int i = 0; i < n; i++) {
          const double a = i, b = i + 1;   // slice i covers [i, i+1) s
          const bool inside = a >= f0 + 0.6 && b <= f1 - 0.1, near = b > f0 - 0.6 && a < f1 + 0.6;
          if (inside) { inN++; if (r.sliceSnr[i] < 5) lowIn++; }
          else if (!near && r.sliceSnr[i] < 10) lowOut++;
      }
      std::string sl; for (double v : r.sliceSnr) { char b[8]; std::snprintf(b, sizeof b, "%.0f ", v); sl += b; }
      std::printf("     slices (dB): %s\n", sl.c_str());
      ok(inN >= 4 && lowIn == inN, "every slice inside the fade reads red (< 5 dB): " + std::to_string(lowIn) + " of " + std::to_string(inN));
      ok(lowOut == 0, "no slice outside the fade reads under 10 dB (" + std::to_string(lowOut) + ")"); }

    std::printf("── 3. display only ──\n");
    { const Res r = run(audio(slots, 10, 5, [](double) { return 1.0; }));
      ok(r.text == msg, "clean 10 dB decode is exact with the meter running");
      NavtexOptions o; o.rcDemod = false;
      NavtexRx rx(48000, 500, 170, 100, false, o); double s, nz;
      const auto a = audio(slots, 10, 5, [](double) { return 1.0; });
      rx.process(a.data(), (int)a.size());
      ok(!rx.snrOf(rx.lastMessage, s, nz), "the biquad front end (no exact noise bandwidth) reports no SNR rather than a wrong one"); }

    if (fails) { std::printf("  %d FAILED\n", fails); return 1; }
    std::printf("  all passed\n");
    return 0;
}
