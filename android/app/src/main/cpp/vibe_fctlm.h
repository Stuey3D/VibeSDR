// vibe_fctlm.h — V12 beta90: FUNcube telemetry (AO-73 / FUNcube-1), docs/v12/SATELLITE-BEST.md 1.2 + 1.3. Our own code.
//
// ★★★ SOURCES (published, read for facts only):
//   · AMSAT-UK, "FUNcube-1 Downlink Data", issue 1.0 final as flown (2015-03-05), "unlimited public release":
//     1200 bps BPSK with the AO-40 FEC; a 256-byte frame = 2-bit satellite id + 6-bit frame type (MSB first) + 440 bits
//     of real-time telemetry + 1600 bits (200 bytes) of payload; one frame every 5 s; the 24-frame schedule (WO1–12,
//     HR1–3, FM1–9); the real-time field list and widths (EPS 192, BOB 100, RF 48, PA 32, ANTS 20, SW 48). ★ The
//     document publishes NO calibration for any channel — every analogue channel is sent RAW, labelled raw.
//   · P. Karn KA9Q, "AO-40 FEC telemetry" (ka9q.net/papers/ao40tlm.html): two RS(160,128) words (even data bytes → word
//     0, odd → word 1), shortened from CCSDS (255,223) by 95 leading zeros, field x⁸+x⁷+x²+x+1, roots α^(11j)
//     j = 112…143, CONVENTIONAL basis; the PN h(x)=x⁸+x⁷+x⁵+x³+1 from all ones over the 320 RS bytes, its first bit on the
//     first byte's MSB; K=7 r½ 171/133 (register fed from the high end), G2 INVERTED, starting all zeros, six zero tail
//     bits unscrambled; written by rows into an 80 × 65 interleaver from row 2, row 1 = the 65-bit sync vector
//     (x⁷+x³+1 from all ones), read out by columns into the differential encoder; 5132 coded + 65 sync = 5197 of 5200
//     (the last three unused). Bytes MSB first.
// ★★ WHAT THE SOURCES DO NOT SAY, and how we do not depend on it: which of the two coder outputs goes first, the
//    differential code's polarity (and whether FUNcube's 1200 Bd keeps AO-40's differential code at all), the carrier's
//    phase. The framer tries the sync vector on the coherent stream and on its differential decode, both signs (four
//    cases, cheap), and the Viterbi on both output orders; RS decides. A frame is trusted only when BOTH RS words
//    decode AND the 95 padding bytes are still zero after correction — 0 false frames on noise (test-fctlm §3).
// ★ The Viterbi, the Reed–Solomon (255,223) and the PN are vibe_lrpt_fec.h's — the same code, field, roots and
//   scrambler (LRPT is CCSDS too); only the shortening, the de-interleaver and the sync are new here.
// ★ Content: FUNcube is an AMATEUR satellite; its telemetry and fitter messages are amateur transmissions meant to be
//   received by anyone (they are the point of the mission), so they are shown — text whitelisted to printable ASCII.
#pragma once

#include "vibe_bpsk_rx.h"
#include "vibe_lrpt_fec.h"

#include <array>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace vibefc {

inline constexpr int kRows = 80, kCols = 65, kSyms = kRows * kCols;   // 5200 a frame
inline constexpr int kDataBytes = 256, kRsBytes = 320, kCoded = (kRsBytes * 8 + 6) * 2;   // 5132
inline constexpr int kPad = 95;                                          // 255 − 160

/** The 65-bit sync vector (x⁷+x³+1 from all ones) — generated, and checked against Karn's printed sequence in the test. */
inline const std::array<uint8_t, 65>& syncVector() {
    static const std::array<uint8_t, 65> v = [] {
        std::array<uint8_t, 65> s{};
        unsigned sr = 0x7F;
        for (int i = 0; i < 65; ++i) {
            s[(size_t)i] = uint8_t((sr >> 6) & 1);
            const unsigned fb = ((sr >> 6) ^ (sr >> 3)) & 1;   // x⁷ + x³ + 1 (Fibonacci, the output from the top)
            sr = ((sr << 1) | fb) & 0x7F;
        }
        return s;
    }();
    return v;
}

// ── The encoder (OUR TRANSMITTER for the tests, written from the same two documents) ─────────────────────────────────
/** 256 data bytes → 5200 channel bits (0/1) in transmission order, BEFORE the differential encoder. */
inline void encodeFrame(const uint8_t* data, uint8_t* out5200) {
    const auto& rs = vibelrpt::Rs223::get();
    uint8_t words[2][160];
    for (int w = 0; w < 2; ++w) {
        uint8_t d[223] = {}, p[32];
        for (int i = 0; i < 128; ++i) d[kPad + i] = data[2 * i + w];
        rs.encode(d, p);
        for (int i = 0; i < 128; ++i) words[w][i] = d[kPad + i];
        for (int i = 0; i < 32; ++i) words[w][128 + i] = p[i];
    }
    const uint8_t* pn = vibelrpt::pnSequence();
    std::vector<uint8_t> bits; bits.reserve(kRsBytes * 8 + 6);
    for (int i = 0; i < kRsBytes; ++i) {
        const uint8_t b = uint8_t(words[i & 1][i >> 1] ^ pn[i % 255]);
        for (int k = 7; k >= 0; --k) bits.push_back(uint8_t((b >> k) & 1));
    }
    for (int k = 0; k < 6; ++k) bits.push_back(0);   // the tail, unscrambled
    std::vector<uint8_t> coded; coded.reserve(kCoded);
    vibelrpt::convEncode(bits.data(), bits.size(), 0, coded);
    for (size_t i = 1; i < coded.size(); i += 2) coded[i] ^= 1;   // G2 inverted
    uint8_t A[kRows][kCols] = {};
    const auto& sv = syncVector();
    for (int c = 0; c < kCols; ++c) A[0][c] = sv[(size_t)c];
    for (int k = 0; k < kCoded; ++k) A[1 + k / kCols][k % kCols] = coded[(size_t)k];
    for (int c = 0; c < kCols; ++c) for (int r = 0; r < kRows; ++r) out5200[c * kRows + r] = A[r][c];
}

// ── The decoder ──────────────────────────────────────────────────────────────────────────────────────────────────
/** 5200 soft channel values in transmission order (POSITIVE = bit 1) → 256 data bytes. Returns RS bytes corrected
 *  (≥ 0), or −1. Tries both coder output orders. */
inline int decodeFrame(const int8_t* soft, uint8_t* data256, int* orderOut = nullptr) {
    std::vector<int8_t> c((size_t)kCoded);
    for (int k = 0; k < kCoded; ++k) { const int r = 1 + k / kCols, col = k % kCols; c[(size_t)k] = soft[col * kRows + r]; }
    static thread_local vibelrpt::Viterbi vit;
    const int steps = kCoded / 2;   // 2566
    std::vector<int8_t> pair((size_t)kCoded);
    std::vector<uint8_t> bits((size_t)steps);
    const auto& rs = vibelrpt::Rs223::get();
    const uint8_t* pn = vibelrpt::pnSequence();
    for (int order = 0; order < 2; ++order) {
        for (int t = 0; t < steps; ++t) {
            int8_t a = c[(size_t)(2 * t)], b = c[(size_t)(2 * t + 1)];
            if (order) std::swap(a, b);
            pair[(size_t)(2 * t)] = a;
            pair[(size_t)(2 * t + 1)] = int8_t(b == -128 ? 127 : -b);   // G2 inverted on the air
        }
        vit.decode(pair.data(), steps, 0, steps, bits.data());
        uint8_t words[2][255];
        for (int w = 0; w < 2; ++w) std::memset(words[w], 0, kPad);
        for (int i = 0; i < kRsBytes; ++i) {
            uint8_t b = 0;
            for (int k = 0; k < 8; ++k) b = uint8_t((b << 1) | (bits[(size_t)(i * 8 + k)] & 1));
            words[i & 1][kPad + (i >> 1)] = uint8_t(b ^ pn[i % 255]);
        }
        int fixed = 0; bool ok = true;
        for (int w = 0; w < 2 && ok; ++w) {
            const int e = rs.decode(words[w]);
            if (e < 0) { ok = false; break; }
            for (int i = 0; i < kPad; ++i) if (words[w][i]) { ok = false; break; }   // ★ a "correction" into the padding
            fixed += e;
        }
        if (!ok) continue;
        for (int i = 0; i < 128; ++i) { data256[2 * i] = words[0][kPad + i]; data256[2 * i + 1] = words[1][kPad + i]; }
        if (orderOut) *orderOut = order;
        return fixed;
    }
    return -1;
}

// ── The frame's meaning (the Downlink Data document) ─────────────────────────────────────────────────────────────
struct Value { std::string key, name, unit; double v = 0; bool raw = false; };
struct Decoded {
    int satId = 0, frameType = 0;
    int rsFixed = 0;        ///< RS bytes corrected in this frame (the run fills it in)
    std::string frameName, mode;
    long long seq = -1;
    std::vector<Value> values;
    std::string fitter;     ///< a fitter message frame's text (printable ASCII only), else ""
};
/** MSB-first bit reader. */
struct Bits {
    const uint8_t* p; size_t pos = 0;
    explicit Bits(const uint8_t* d, size_t start = 0) : p(d), pos(start) {}
    uint32_t get(int n) { uint32_t v = 0; for (int i = 0; i < n; ++i, ++pos) v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1u); return v; }
};
inline std::string frameName(int t) {
    if (t >= 1 && t <= 12) return "WO" + std::to_string(t);
    static const char* rest[] = {"HR1", "FM1", "FM2", "FM3", "HR2", "FM4", "FM5", "FM6", "HR3", "FM7", "FM8", "FM9"};
    if (t >= 13 && t <= 24) return rest[t - 13];
    return "type " + std::to_string(t);
}
inline const char* satIdName(int id) {
    switch (id) { case 0: return "FUNcube-1 engineering model"; case 1: return "FUNcube-2 (UKube-1)"; case 2: return "FUNcube-1"; default: return "extended"; }
}
/** ★★ Real-time telemetry, field by field in the document's order. Analogue channels: RAW (no published calibration).
 *  Counts, flags and the sequence number are exact (not raw). */
inline Decoded parse(const uint8_t* d) {
    Decoded o;
    o.satId = d[0] >> 6; o.frameType = d[0] & 0x3F; o.frameName = frameName(o.frameType);
    Bits b(d, 8);
    auto raw = [&](const char* k, const char* n, int w) { Value v; v.key = k; v.name = n; v.v = b.get(w); v.raw = true; o.values.push_back(v); };
    auto cnt = [&](const char* k, const char* n, int w) { Value v; v.key = k; v.name = n; v.v = b.get(w); o.values.push_back(v); };
    // EPS 192
    raw("eps.pv1", "Photo voltage 1", 16); raw("eps.pv2", "Photo voltage 2", 16); raw("eps.pv3", "Photo voltage 3", 16);
    raw("eps.ipv", "Total photo current", 16); raw("eps.vbat", "Battery voltage", 16); raw("eps.isys", "Total system current", 16);
    cnt("eps.reboots", "Reboot count", 16); cnt("eps.swerr", "EPS software errors", 16);
    raw("eps.tboost1", "Boost converter temp 1", 8); raw("eps.tboost2", "Boost converter temp 2", 8); raw("eps.tboost3", "Boost converter temp 3", 8);
    raw("eps.tbat", "Battery temp", 8); cnt("eps.lu5", "Latch-up count 5 V", 8); cnt("eps.lu33", "Latch-up count 3.3 V", 8);
    cnt("eps.reset", "Reset cause", 8); cnt("eps.ppt", "Power point tracking mode", 8);
    // BOB 100
    raw("bob.sunx", "Sun sensor X+", 10); raw("bob.suny", "Sun sensor Y+", 10); raw("bob.sunz", "Sun sensor Z+", 10);
    raw("bob.tpxp", "Solar panel temp X+", 10); raw("bob.tpxm", "Solar panel temp X-", 10); raw("bob.tpyp", "Solar panel temp Y+", 10);
    raw("bob.tpym", "Solar panel temp Y-", 10); raw("bob.v33", "3.3 V bus voltage", 10); raw("bob.i33", "3.3 V bus current", 10);
    raw("bob.v50", "5.0 V bus voltage", 10);
    // RF 48
    raw("rf.doppler", "Receiver Doppler", 8); raw("rf.rssi", "Receiver RSSI", 8); raw("rf.temp", "RF temperature", 8);
    raw("rf.irx", "Receive current", 8); raw("rf.itx33", "Transmit current 3.3 V bus", 8); raw("rf.itx50", "Transmit current 5.0 V bus", 8);
    // PA 32 (as flown: reverse then forward)
    raw("pa.rev", "PA reverse power", 8); raw("pa.fwd", "PA forward power", 8); raw("pa.temp", "PA board temperature", 8); raw("pa.i", "PA board current", 8);
    // ANTS 20
    raw("ant.t0", "Antenna temp 0", 8); raw("ant.t1", "Antenna temp 1", 8);
    cnt("ant.dep0", "Antenna 0 deployed", 1); cnt("ant.dep1", "Antenna 1 deployed", 1); cnt("ant.dep2", "Antenna 2 deployed", 1); cnt("ant.dep3", "Antenna 3 deployed", 1);
    // SW 48
    const uint32_t seq = b.get(24);
    { Value v; v.key = "sw.seq"; v.name = "Sequence number"; v.v = seq; o.values.push_back(v); o.seq = seq; }
    cnt("sw.dtmfn", "DTMF command count", 6); cnt("sw.dtmflast", "DTMF last command", 5); cnt("sw.dtmfok", "DTMF command success", 1);
    cnt("sw.vasib", "Data valid ASIB", 1); cnt("sw.veps", "Data valid EPS", 1); cnt("sw.vpa", "Data valid PA", 1); cnt("sw.vrf", "Data valid RF", 1);
    cnt("sw.vmse", "Data valid MSE", 1); cnt("sw.vantb", "Data valid ANTS bus B", 1); cnt("sw.vanta", "Data valid ANTS bus A", 1);
    const uint32_t eclipse = b.get(1), safe = b.get(1);
    { Value v; v.key = "sw.eclipse"; v.name = "In eclipse mode"; v.v = eclipse; o.values.push_back(v); }
    { Value v; v.key = "sw.safe"; v.name = "In safe mode"; v.v = safe; o.values.push_back(v); }
    cnt("sw.habf", "Hardware ABF on", 1); cnt("sw.sabf", "Software ABF on", 1); cnt("sw.depwait", "Deployment wait at next boot", 1);
    o.mode = safe ? "Safe" : eclipse ? "Eclipse" : "Sunlight";
    // ★ a fitter message frame: its 200 payload bytes are the message, copied verbatim on board
    const bool fm = o.frameType >= 14 && o.frameType <= 24 && o.frameType != 17 && o.frameType != 21;
    if (fm) {
        for (int i = 56; i < 256; ++i) {
            const unsigned char ch = d[i];
            if (ch == 0) break;
            o.fitter += (ch >= 0x20 && ch < 0x7F) ? char(ch) : (ch == '\n' || ch == '\r') ? ' ' : '.';
        }
        while (!o.fitter.empty() && o.fitter.back() == ' ') o.fitter.pop_back();
    }
    return o;
}

inline std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c >= 0x20 && c < 0x7F) o += char(c);
    }
    return o;
}
inline std::string num(double v) {
    char b[32];
    if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.0f", v); else std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}
/** `{"type":"sat_tlm","data":{…}}` — the contract shape (docs/v12/SATELLITE-BEST.md "Contract additions (beta90)"). */
inline std::string tlmJson(const Decoded& d, const uint8_t* bytes, size_t n, const std::string& sat, double unixNow) {
    std::string j = "{\"type\":\"sat_tlm\",\"data\":{\"sat\":\"" + esc(sat) + "\",\"format\":\"funcube-rt\",\"t\":" + num(std::floor(unixNow))
                  + ",\"frame\":\"" + esc(d.frameName) + "\",\"satId\":\"" + esc(satIdName(d.satId)) + "\"";
    if (d.seq >= 0) j += ",\"seq\":" + std::to_string(d.seq);
    if (!d.mode.empty()) j += ",\"mode\":\"" + d.mode + "\"";
    if (!d.fitter.empty()) j += ",\"fitter\":\"" + esc(d.fitter) + "\"";
    j += ",\"values\":[";
    for (size_t i = 0; i < d.values.size() && i < 64; ++i) {
        const Value& v = d.values[i];
        if (i) j += ",";
        j += "{\"k\":\"" + v.key + "\",\"name\":\"" + esc(v.name) + "\",\"v\":" + num(v.v);
        if (!v.unit.empty()) j += ",\"unit\":\"" + esc(v.unit) + "\"";
        if (v.raw) j += ",\"raw\":true";
        j += "}";
    }
    j += "],\"hex\":\"";
    static const char* hx = "0123456789abcdef";
    for (size_t i = 0; i < n && i < 256; ++i) { j += hx[bytes[i] >> 4]; j += hx[bytes[i] & 15]; }
    return j + "\"}}";
}

// ── The framer: soft symbols in, frames out ─────────────────────────────────────────────────────────────────────
/** ★★ Watches the symbol stream for the sync vector (one bit every 80 symbols) in four forms — coherent / differential
 *  × either sign — and decodes the 5200 symbols behind a match. A match needs ≥ kSyncMin of 65 bits; on noise a chance
 *  match is ~1e-9 a symbol, and RS + the zero padding then refuse it. */
class Framer {
public:
    static constexpr int kSyncMin = 54;
    std::function<void(const uint8_t* data256, int rsFixed, int form)> onFrame;
    std::function<void()> onFail;   ///< a sync matched, the frame did not decode

    void push(float s) {
        const float prev = last_;
        last_ = s;
        put(coh_, s);
        put(dif_, -(s * prev));   // ★ a phase CHANGE = 1 (the other convention is the inverted form, tried too)
        ++n_;
        if (n_ < (uint64_t)kSyms || hold_ > 0) { if (hold_ > 0) --hold_; return; }
        tryAt(coh_, 0);
        if (hold_ == 0) tryAt(dif_, 2);
    }
    uint64_t syncs() const { return syncs_; }
    uint64_t good() const { return good_; }
    uint64_t failed() const { return failed_; }
    /** Symbols since the last good frame (a lock indicator for the demodulator's hunt). */
    uint64_t sinceGood() const { return n_ - lastGood_; }

private:
    static constexpr size_t kRing = 8192;   // > 5200, a power of two
    struct R { std::vector<float> v = std::vector<float>(kRing, 0.f); };
    void put(R& r, float s) { r.v[(size_t)(n_ & (kRing - 1))] = s; }
    float at(const R& r, uint64_t i) const { return r.v[(size_t)(i & (kRing - 1))]; }
    void tryAt(const R& r, int form) {
        const uint64_t start = n_ - (uint64_t)kSyms;   // the newest symbol is n_ − 1: a whole frame ends there
        const auto& sv = syncVector();
        int match = 0;
        for (int j = 0; j < 65; ++j) match += ((at(r, start + (uint64_t)(80 * j)) > 0) == (sv[(size_t)j] != 0));
        int sign = 0;
        if (match >= kSyncMin) sign = 1; else if (65 - match >= kSyncMin) sign = -1;
        if (!sign) return;
        ++syncs_;
        // soft int8: scaled by the frame's own mean magnitude (≈ 40 per unit), sign-corrected
        double m = 0;
        for (int i = 0; i < kSyms; ++i) m += std::fabs(at(r, start + (uint64_t)i));
        m = m / kSyms;
        const double g = m > 1e-9 ? 40.0 / m : 0.0;
        int8_t soft[kSyms];
        for (int i = 0; i < kSyms; ++i) {
            const double v = sign * at(r, start + (uint64_t)i) * g;
            soft[i] = int8_t(std::max(-127.0, std::min(127.0, std::round(v))));
        }
        uint8_t data[kDataBytes];
        const int e = decodeFrame(soft, data);
        if (e < 0) { ++failed_; hold_ = 0; if (onFail) onFail(); return; }
        ++good_; lastGood_ = n_;
        hold_ = kSyms - 200;   // ★ the next frame starts ≥ 5200 symbols on (frames are 5 s apart)
        if (onFrame) onFrame(data, e, form + (sign < 0 ? 1 : 0));
    }
    R coh_, dif_;
    float last_ = 0;
    uint64_t n_ = 0, syncs_ = 0, good_ = 0, failed_ = 0, lastGood_ = 0;
    int hold_ = 0;
};

// ── ★★ V12 beta91: where and when a frame was heard (the gallery's per-pass log) ────────────────────────────────
struct TlmCtx {
    std::string sat;          ///< "AO-73"
    double unix = 0;          ///< the frame's time (the server's corrected clock), seconds
    double aos = 0, los = 0;  ///< the pass (the satellite engine's), 0 = not known (a hand-tuned listener)
    double elDeg = NAN;       ///< the bird's elevation now, NaN = not known
};

// ── The run inside a decoder host ───────────────────────────────────────────────────────────────────────────────
/** ★ 48 kHz listener audio in (USB, the signal near `centre` Hz); `sat_tlm` per good frame, `fctlm_status` once a
 *  second, a readable gallery line per frame. All on the host's decode thread (never the audio thread). */
class Run {
public:
    struct Out {
        std::function<void(const std::string&)> json;   ///< a JSON text frame to the decoder socket's peers
        std::function<void(const std::string&)> line;   ///< a readable line (the gallery's day log)
        std::function<std::string()> satName;           ///< the pick's satellite ("" = AO-73)
        std::function<double()> unixNow;                ///< the server's corrected clock
        /** ★★ V12 beta91 TELEMETRY CHARTS: every good frame, parsed, for the gallery's per-pass CSV and day log
         *  (vibe_fctlm_gallery.h). Optional. */
        std::function<void(const Decoded&, const TlmCtx&)> record;
        /** ★ beta91: the pass the frame belongs to (the satellite engine's AOS / LOS and elevation now) — 0 = not known. */
        std::function<void(TlmCtx&)> pass;
    };
    Run(const vibebpsk::Rx::Cfg& cfg, bool invert, Out o) : rx_(cfg), inv_(invert), o_(std::move(o)) {
        rx_.onSymbol = [this](float s) { fr_.push(inv_ ? -s : s); };
        fr_.onFrame = [this](const uint8_t* d, int fixed, int form) { frame(d, fixed, form); };
    }
    void feedAudio(const float* x, int n) {
        rx_.feed(x, n);
        // ★ ten seconds without a frame: hunt again. ★★ AND NO FRAME YET = hunting: sinceGood() counts from 0 before the
        //   first frame, so without good() > 0 a fresh run thought itself locked for its first 10 s and never found a
        //   signal more than the Costas loop's ~60 Hz off (the benchmark row decoded 0; 20 ms blocks lost frames).
        rx_.setLocked(fr_.good() > 0 && fr_.sinceGood() < 12000);
        if (rx_.samples() - lastStatus_ >= 48000) { lastStatus_ = rx_.samples(); status(); }
    }
    std::string statusJson() const {
        char b[320];
        std::snprintf(b, sizeof b, "{\"type\":\"fctlm_status\",\"data\":{\"sync\":%s,\"frames\":%llu,\"good\":%llu,\"failed\":%llu,"
                      "\"afcHz\":%.1f,\"snrDb\":%.1f,\"centreHz\":%.0f,\"auto\":%s}}",
                      fr_.sinceGood() < 12000 && fr_.good() > 0 ? "true" : "false", (unsigned long long)fr_.syncs(),
                      (unsigned long long)fr_.good(), (unsigned long long)fr_.failed(), rx_.afcHz(), rx_.snrDb(), rx_.centreHz(),
                      rx_.afcOn() ? "true" : "false");
        return b;
    }
    const Framer& framer() const { return fr_; }
    const vibebpsk::Rx& rx() const { return rx_; }
    vibebpsk::Rx& rxMut() { return rx_; }
private:
    void status() { if (o_.json) o_.json(statusJson()); }
    void frame(const uint8_t* d, int fixed, int form) {
        const Decoded p = parse(d);
        std::string sat = o_.satName ? o_.satName() : std::string();
        if (sat.empty()) sat = "AO-73";
        const double now = o_.unixNow ? o_.unixNow() : 0.0;
        if (o_.json) o_.json(tlmJson(p, d, kDataBytes, sat, now));
        if (o_.record) {
            TlmCtx c; c.sat = sat; c.unix = now;
            if (o_.pass) o_.pass(c);
            Decoded q = p; q.rsFixed = fixed;
            o_.record(q, c);
        }
        if (o_.line) {
            std::string l = sat + " telemetry " + p.frameName + " \xc2\xb7 seq " + std::to_string(p.seq) + " \xc2\xb7 " + p.mode
                          + " \xc2\xb7 battery " + num(p.values.size() > 4 ? p.values[4].v : 0) + " (raw) \xc2\xb7 RS fixed " + std::to_string(fixed);
            if (!p.fitter.empty()) l += " \xc2\xb7 \"" + p.fitter.substr(0, 200) + "\"";
            o_.line(l);
        }
        (void)form;
        status();
    }
    vibebpsk::Rx rx_;
    Framer fr_;
    bool inv_;
    Out o_;
    uint64_t lastStatus_ = 0;
};

} // namespace vibefc
