// test-js8 — the JS8 decoder (decoders/js8_*.cpp): its code, its frames, every submode on a synthetic band, and the
// live decoder's slots (2026-10-26).
//
// ★★★ WHAT MUST HOLD.
//   1. The LDPC(174,87) code: the generator built from the parity checks makes codewords every check accepts; the
//      CRC-12 catches every single-bit error.
//   2. Frames: heartbeats, CQs, directed frames (calls, /P, groups, numbers), compound calls, grids and Huffman text
//      come back as they went in; the Huffman code is prefix-free and complete.
//   3. Every submode (Normal, Fast, Turbo, Slow) decodes its own frames — DT, frequency and SNR on WSJT-X's scales —
//      down to its threshold, and NOTHING is decoded from pure noise (thousands of candidates a slot).
//   4. The live decoder cuts every submode's slot from one stream by the capture clock, decodes the four at once.
// Synthetic: JS8's waveform (continuous-phase FSK, tone spacing = symbol rate), known SNR/DT/frequency, white noise.
#include "decoders/js8_decoder.h"
#include "decoders/js8_ldpc.h"
#include "decoders/js8_message.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

static const int kFs = 12000;
static const double kSigma = 0.02;

// one JS8 transmission added to `out`: continuous-phase FSK, `t0` s into the buffer, SNR in 2500 Hz
static void js8Add(std::vector<double>& out, const uint8_t* tones, Js8Submode s, double f0, double t0, double snrDb, double phase0) {
    const int nsps = js8Info(s).nsps12k;
    const double baud = (double)kFs / nsps;
    const double amp = std::sqrt(2.0 * kSigma * kSigma * (2500.0 / (kFs / 2.0)) * std::pow(10.0, snrDb / 10.0));
    double ph = phase0;
    const long i0 = std::lround(t0 * kFs);
    for (int k = 0; k < 79; ++k) {
        const double dph = 2 * M_PI * (f0 + tones[k] * baud) / kFs;
        for (int j = 0; j < nsps; ++j) {
            const long i = i0 + (long)k * nsps + j;
            if (i >= 0 && i < (long)out.size()) out[(size_t)i] += amp * std::cos(ph);
            ph += dph;
        }
    }
}
static void noise(std::vector<double>& v, std::mt19937& rng) {
    std::normal_distribution<double> g(0, kSigma);
    for (double& x : v) x += g(rng);
}

struct Tx { uint8_t bits72[72]; int flags; std::string text; };
static Tx heartbeat(const std::string& call, const std::string& grid, int cq = -1) {
    Tx t{}; js8::packHeartbeat(call, grid, cq, t.bits72); t.flags = js8::kFlagFirst | js8::kFlagLast;
    js8::Frame f; js8::unpack(t.bits72, t.flags, f); t.text = f.text; return t;
}
static Tx directed(const std::string& from, const std::string& to, int cmd, int num = 99) {
    Tx t{}; js8::packDirected(from, to, cmd, num, t.bits72); t.flags = js8::kFlagFirst | js8::kFlagLast;
    js8::Frame f; js8::unpack(t.bits72, t.flags, f); t.text = f.text; return t;
}

int main() {
    std::mt19937 rng(8087);

    std::printf("1. LDPC(174,87) + CRC-12\n");
    {
        bool degOk = true; int edges = 0;
        for (int m = 0; m < js8::kM; ++m) { const int d = js8::checkDegree(m); degOk &= d >= 5 && d <= 7; edges += d; }
        ok(degOk && edges == 174 * 3, "87 checks of 5-7 bits, 522 edges (every bit in three checks)");
        int bad = 0;
        for (int trial = 0; trial < 500; ++trial) {
            uint8_t info[87], cw[174];
            for (int i = 0; i < 87; ++i) info[i] = (uint8_t)(rng() & 1);
            js8::encode(info, cw);
            bool sys = std::memcmp(cw + 87, info, 87) == 0;
            for (int m = 0; m < js8::kM && sys; ++m) {
                int s = 0; const uint8_t* b = js8::checkBits(m);
                for (int j = 0; j < js8::checkDegree(m); ++j) s ^= cw[b[j]];
                sys = s == 0;
            }
            bad += !sys;
        }
        ok(bad == 0, "500 random codewords: systematic (information last) and every check satisfied");
        uint8_t b[75] = {}; for (int i = 0; i < 75; ++i) b[i] = (uint8_t)(rng() & 1);
        const uint16_t c0 = js8::crc12(b); int missed = 0;
        for (int i = 0; i < 75; ++i) { b[i] ^= 1; missed += js8::crc12(b) == c0; b[i] ^= 1; }
        ok(missed == 0, "CRC-12 changes for every single-bit error in the 75 bits");
        // BP corrects a noisy codeword; OSD recovers one BP gives up on
        uint8_t info[87], cw[174], out[174]; float llr[174];
        for (int i = 0; i < 87; ++i) info[i] = (uint8_t)(rng() & 1);
        uint8_t t72[72]; std::memcpy(t72, info, 72); js8Info87(t72, 3, info);
        js8::encode(info, cw);
        std::normal_distribution<float> g(0, 1);
        for (int i = 0; i < 174; ++i) llr[i] = (cw[i] ? 2.0f : -2.0f) + 1.3f * g(rng);
        const int e = js8::bpDecode(llr, 30, out);
        ok(e >= 0 && std::memcmp(out, cw, 174) == 0, "belief propagation corrects a noisy codeword (" + std::to_string(e) + " hard errors)");
        for (int i = 0; i < 174; ++i) llr[i] = (cw[i] ? 1.0f : -1.0f) * (0.2f + std::fabs(g(rng)));
        for (int i = 0; i < 12; ++i) llr[(size_t)(rng() % 174)] *= -0.3f;   // a few wrong, weakly
        const int eo = js8::osd1(llr, 30, out);
        ok(eo >= 0 && std::memcmp(out, cw, 174) == 0, "OSD-1 finds the codeword from a few weak errors");
    }

    std::printf("2. frames\n");
    {
        auto rt = [](const Tx& t, const char* want) {
            js8::Frame f; const bool u = js8::unpack(t.bits72, t.flags, f);
            ok(u && f.text == want, std::string("\"") + want + "\" (got \"" + f.text + "\")");
            return f;
        };
        js8::Frame f = rt(heartbeat("G4ABC", "IO91"), "G4ABC: @HB HEARTBEAT IO91");
        ok(f.kind == js8::FrameKind::Heartbeat && f.from == "G4ABC" && f.grid == "IO91", "heartbeat: from + grid");
        f = rt(heartbeat("KN4CRD", "EM73", 0), "KN4CRD: @ALLCALL CQ CQ CQ EM73");
        ok(f.kind == js8::FrameKind::Cq, "CQ kind");
        rt(heartbeat("VE3/LB9YHX", "FN03", 1), "VE3/LB9YHX: @ALLCALL CQ DX FN03");
        rt(heartbeat("M0XYZ/P", "", -1), "M0XYZ/P: @HB HEARTBEAT");
        rt(directed("KN4CRD", "G4ABC", 25, -12), "KN4CRD: G4ABC SNR -12");
        rt(directed("KN4CRD", "@ALLCALL", 31), "KN4CRD: @ALLCALL ");
        rt(directed("W1AW", "@JS8NET", 3), "W1AW: @JS8NET HEARING?");
        rt(directed("2E0ABC/P", "VK2XYZ", 4), "2E0ABC/P: VK2XYZ GRID?");
        rt(directed("3DA0RS", "@APRSIS", 31), "3DA0RS: @APRSIS ");
        rt(directed("KN4CRD", "@QRO", 28), "KN4CRD: @QRO 73");
        // calls through both packers
        const char* calls[] = { "G4ABC", "K1A", "W1AW", "VK2XYZ", "2E0ABC", "9A1AA", "3X2ABC", "A61AB", "M0X" };
        bool c28 = true, c50 = true;
        for (const char* c : calls) {
            bool p = false; const uint32_t v = js8::packCall28(c, &p);
            c28 &= v && js8::unpackCall28(v, p) == c;
            c50 &= js8::unpackCall50(js8::packCall50(c)) == c;
        }
        ok(c28, "28-bit calls round-trip (incl. 3X Guinea, a 1×1)"); ok(c50, "50-bit calls round-trip");
        // grids: every square's corner round-trips
        int gbad = 0, gn = 0;
        for (char a = 'A'; a <= 'R'; ++a) for (char b = 'A'; b <= 'R'; ++b) for (char c = '0'; c <= '9'; ++c) for (char d = '0'; d <= '9'; ++d) {
            const std::string g{a, b, c, d}; ++gn;
            gbad += js8::unpackGrid15(js8::packGrid15(g)) != g;
        }
        ok(gbad == 0, "all " + std::to_string(gn) + " 4-character grids round-trip through 15 bits");
        Tx t{}; const int used = js8::packHuffText("HELLO WORLD 73", t.bits72); t.flags = 0;   // 74 bits: the "3" cannot fit
        js8::Frame h; js8::unpack(t.bits72, 0, h);
        ok(used == 13 && h.kind == js8::FrameKind::Data && h.text == "HELLO WORLD 7", "Huffman text frame (\"" + h.text + "\")");
        Tx d{}; js8::packHuffText("X", d.bits72); d.bits72[1] = 1;
        js8::unpack(d.bits72, 0, h);
        ok(h.kind == js8::FrameKind::DataCompressed && !h.textKnown, "a dictionary-compressed frame is counted, its text unknown");
        ok(js8::isSpotCall("G4ABC") && js8::isSpotCall("VE3/LB9YHX") && !js8::isSpotCall("@ALLCALL") && !js8::isSpotCall("<....>")
           && !js8::isSpotCall("ABC"), "spot calls: real calls only");
    }

    std::printf("3. every submode on a synthetic band\n");
    const double snrs[] = { -6, -12, -16, -18, -20, -22, -24, -26 };
    for (Js8Submode s : kJs8Submodes) {
        const Js8SubmodeInfo& in = js8Info(s);
        Js8SlotDecoder dec(kFs, s);
        Js8SlotOptions o; o.maxPasses = 2; o.osdMaxErrors = 26;
        const int win = dec.windowSamples();
        std::string line;
        /* ★ "Decodes everything above": a little above where each submode's threshold falls on this band — they differ
         *  by the symbol rate (each halving of the symbol costs 3 dB): Turbo -12, Fast -16, Normal -16, Slow -20. */
        const double sure = s == Js8Submode::Turbo ? -12 : s == Js8Submode::Slow ? -20 : -16;
        int strongOk = 0, strongN = 0, falseN = 0; double ms = 0; int slots = 0;
        double dtErr = 0, hzErr = 0, snrErr = 0; int meas = 0;
        const int perSnr = s == Js8Submode::Slow ? 4 : 8;      // signals per SNR (several per slot)
        for (double snr : snrs) {
            int got = 0, n = 0;
            for (int rep = 0; rep < perSnr; rep += 4) {
                std::vector<double> band((size_t)win, 0.0);
                std::vector<Tx> txs; std::vector<double> hz, dts;
                for (int k = 0; k < 4; ++k) {
                    char call[8]; std::snprintf(call, sizeof call, "G%dX%c%c", (int)(rng() % 10), 'A' + (int)(rng() % 26), 'A' + (int)(rng() % 26));
                    const char* grids[] = { "IO91", "JO01", "EM73", "FN31" };
                    Tx t = (k & 1) ? heartbeat(call, grids[k], (int)(rng() % 3) - 1) : directed(call, "@ALLCALL", 31);
                    uint8_t info[87], tones[79];
                    js8Info87(t.bits72, t.flags, info); js8Tones(info, s, tones);
                    const double f = 400 + 500 * k + (double)(rng() % 100);
                    const double dt = ((int)(rng() % 9) - 3) * 0.1;    // -0.3 .. +0.5 s
                    js8Add(band, tones, s, f, in.startDelay + dt, snr, (rng() % 628) * 0.01);
                    txs.push_back(t); hz.push_back(f); dts.push_back(dt);
                }
                noise(band, rng);
                std::vector<float> a(band.begin(), band.end());
                std::vector<Js8Result> res; Js8SlotStats st;
                const auto t0 = std::chrono::steady_clock::now();
                dec.decode(a.data(), win, o, res, st);
                ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); ++slots;
                for (const Js8Result& r : res) {
                    bool match = false;
                    for (size_t k = 0; k < txs.size(); ++k)
                        if (r.frame.text == txs[k].text) {
                            match = true; ++got;
                            if (snr >= -16) { dtErr = std::max(dtErr, std::fabs(r.dt - dts[k])); hzErr = std::max(hzErr, (double)std::fabs(r.hz - hz[k]));
                                              snrErr = std::max(snrErr, std::fabs(r.snr - Js8SlotDecoder::kSnrScaleDb - snr)); ++meas; }
                        }
                    if (!match) { ++falseN; std::printf("    false: \"%s\"\n", r.frame.text.c_str()); }
                }
                n += 4;
            }
            char b[32]; std::snprintf(b, sizeof b, " %+.0f:%d/%d", snr, got, n); line += b;
            if (snr >= sure) { strongOk += got; strongN += n; }
        }
        std::printf("    %-6s decoded by SNR —%s   (%.1f ms/slot, 2 passes + OSD, this machine)\n", in.name, line.c_str(), ms / slots);
        ok(strongOk == strongN, std::string(in.name) + ": every signal at " + std::to_string((int)sure) + " dB and above decoded");
        ok(falseN == 0, std::string(in.name) + ": no message that was not sent");
        ok(meas > 0 && dtErr < 0.6 * in.nsps12k / kFs + 0.01, std::string(in.name) + ": DT within half a symbol (worst " + std::to_string(dtErr) + " s)");
        ok(meas > 0 && hzErr < 0.6 * kFs / in.nsps12k, std::string(in.name) + ": frequency within half a tone (worst " + std::to_string(hzErr) + " Hz)");
        ok(meas > 0 && snrErr <= 4.0, std::string(in.name) + ": SNR (less the JS8 scale offset) within 4 dB of the truth (worst " + std::to_string(snrErr) + ")");
    }

    std::printf("4. pure noise: no decodes\n");
    for (Js8Submode s : kJs8Submodes) {
        Js8SlotDecoder dec(kFs, s);
        Js8SlotOptions o; o.maxPasses = 1; o.osdMaxErrors = 26; o.osdMinScore = 0; o.maxCandidates = 300; o.minScore = 4;
        int falseN = 0, cands = 0;
        const int slots = s == Js8Submode::Slow ? 10 : 25;
        for (int i = 0; i < slots; ++i) {
            std::vector<double> band((size_t)dec.windowSamples(), 0.0); noise(band, rng);
            std::vector<float> a(band.begin(), band.end());
            std::vector<Js8Result> res; Js8SlotStats st;
            dec.decode(a.data(), (int)a.size(), o, res, st);
            cands += st.candidates[0];
            for (const Js8Result& r : res) { ++falseN; std::printf("    false: \"%s\" (%d errors)\n", r.frame.text.c_str(), r.hardErrors); }
        }
        ok(falseN == 0, std::string(js8Info(s).name) + ": " + std::to_string(slots) + " noise slots, " + std::to_string(cands)
           + " candidates (BP + OSD on every one): 0 decodes");
    }

    std::printf("5. the live decoder: four submodes from one stream, cut by the capture clock\n");
    {
        Js8Decoder live(kFs);
        std::mutex mx; std::vector<Js8Spot> spots;
        live.onDecode = [&](const Js8Spot& s) { std::lock_guard<std::mutex> lk(mx); spots.push_back(s); };
        // 60 s of band from a UTC minute: one station per submode, every slot of it, -10 dB
        const double utc0 = 1792998000.0;                      // a whole minute
        std::vector<double> band((size_t)(62 * kFs), 0.0);
        const char* calls[4] = { "G0TBO", "G0FST", "G0NRM", "G0SLO" };
        for (int si = 0; si < 4; ++si) {
            const Js8Submode s = kJs8Submodes[si];
            const Js8SubmodeInfo& in = js8Info(s);
            Tx t = heartbeat(calls[si], "IO91");
            uint8_t info[87], tones[79]; js8Info87(t.bits72, t.flags, info); js8Tones(info, s, tones);
            for (double slot = 0; slot + in.period <= 60.0 + 1e-9; slot += in.period)
                js8Add(band, tones, s, 500.0 + 600.0 * si, slot + in.startDelay, -10, 0.3);
        }
        noise(band, rng);
        std::vector<int16_t> pcm(band.size());
        for (size_t i = 0; i < band.size(); ++i) pcm[i] = (int16_t)std::max(-32768.0, std::min(32767.0, band[i] * 32768.0));
        // in 20 ms blocks, stamped 40 ms late (the audio's path), as the host does
        const int blk = 240;
        for (size_t i = 0; i + blk <= pcm.size(); i += blk) {
            live.process(pcm.data() + i, blk, utc0 + (double)(i + blk) / kFs + 0.040);
            if ((i / blk) % 50 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (int w = 0; w < 400 && live.slotsDecoded.load() < 10 + 6 + 4 + 2; ++w) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        std::lock_guard<std::mutex> lk(mx);
        int per[4] = {}; bool times = true;
        for (const Js8Spot& s : spots) {
            for (int si = 0; si < 4; ++si) if (s.frame.from == calls[si]) {
                ++per[si];
                times &= s.submode == kJs8Submodes[si] && std::fabs(std::fmod(s.slotUtc - utc0, js8Info(s.submode).period)) < 1e-6;
            }
        }
        std::printf("    spots: Turbo %d/10, Fast %d/6, Normal %d/4, Slow %d/2; slots decoded %u, skipped %u\n",
                    per[0], per[1], per[2], per[3], live.slotsDecoded.load(), live.slotsSkipped.load());
        ok(per[0] >= 9 && per[1] >= 5 && per[2] >= 3 && per[3] >= 1, "every submode's station decoded in (nearly) every slot");
        ok(times, "each spot on its own submode's slot boundary");
        ok(live.slotsSkipped.load() == 0, "nothing skipped on this machine");
    }

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
