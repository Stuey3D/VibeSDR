// test-wspr.cpp — the WSPR-2 decoder (decoders/wspr_decoder.h), measured.
//
// ★★★ WHAT IS PINNED.
//   • the protocol: type 1 / 2 / 3 messages packed and unpacked (calls with the digit second or third, prefixes,
//     one- and two-character suffixes, 6-character locators behind a hash), illegal fields refused, the encoder's
//     162 symbols always carrying the published sync vector, the Fano decoder exact on clean soft bits;
//   • the decoder on synthetic slots generated from the spec (tools/wspr-bench/wspr_synth.h — SNR in 2500 Hz): every
//     signal from −20 to −28 dB at several frequencies, starts (DT −2 … +4) and drifts (±3 Hz); SNR, frequency, DT
//     and drift reported within their tolerances; 12, 24 and 48 kHz audio;
//   • the HASHED CALL: a type 3 prints only after its call has been heard in full (type 1 or 2), never a guess;
//   • PURE NOISE gives no spot — the decoder runs unattended into the gallery, so a false spot is a lie written down;
//   • SUBTRACTION PAYS: a weak station two tones beside a strong one is found by pass 2, not by pass 1 alone;
//   • the LIVE decoder: int16 blocks with capture times → one spot per transmission, the right slot and RF frequency;
//   • REAL SIGNALS (when VIBE_TEST_AUDIO holds wspr/): slots of Stuart's UberSDR on 40 m and 20 m — every decode one a
//     reference receiver also heard, a type 3 resolved through a hash learnt two slots earlier, and the spots' SNR on
//     the WSPRNET scale: within ±1 dB on average, ±3 dB each, of what STUEY3D reported for the same stations.
//   • two SNRs: `snrTrue` (the definition) is exact on synthetic slots; `snr` (the WSPRnet scale) equals it there too —
//     on clean white noise the two conventions agree; they part only on a real, busy band.
// ★ Silent: nothing is played. Deterministic: fixed seeds.
#include "decoders/wspr_decoder.h"
#include "../tools/wspr-bench/wspr_synth.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }

static wspr::Message M(int type, const char* call, const char* grid, int dbm) {
    wspr::Message m; m.type = type; m.call = call; m.grid = grid; m.dbm = dbm; return m;
}

static bool roundTrip(const wspr::Message& m, const std::string& want) {
    uint8_t bits[7];
    if (!wspr::pack(m, bits)) return false;
    wspr::Message u;
    if (!wspr::unpack(bits, u, &wspr::callHashes())) return false;
    return u.text() == want;
}

static std::vector<WsprSpot> decodeSlot(std::vector<float>& a, int rate, int passes = 3) {
    WsprSlotDecoder d; WsprSlotOptions o; o.maxPasses = passes;
    std::vector<WsprSpot> out; WsprSlotStats st;
    d.decodeAudio(a.data(), (int)a.size(), rate, 14095600, 0, o, out, st);
    return out;
}

static void protocol() {
    std::printf("\n— protocol —\n");
    wspr::callHashes().clear();
    ok(roundTrip(M(1, "K1ABC", "FN42", 37), "K1ABC FN42 37"), "type 1: K1ABC FN42 37 (digit second: leading space)");
    ok(roundTrip(M(1, "G4ABC", "IO92", 30), "G4ABC IO92 30"), "type 1: G4ABC IO92 30");
    ok(roundTrip(M(1, "2E0XYZ", "IO91", 23), "2E0XYZ IO91 23"), "type 1: 2E0XYZ (digit first and third)");
    ok(roundTrip(M(1, "A61AB", "LL75", 0), "A61AB LL75 0"), "type 1: A61AB, power 0");
    ok(roundTrip(M(1, "VK3QN", "QF22", 60), "VK3QN QF22 60"), "type 1: power 60, far grid");
    ok(roundTrip(M(2, "PJ4/K1ABC", "", 37), "PJ4/K1ABC 37"), "type 2: three-character prefix");
    ok(roundTrip(M(2, "F/G4ABC", "", 10), "F/G4ABC 10"), "type 2: one-character prefix");
    ok(roundTrip(M(2, "K1ABC/P", "", 33), "K1ABC/P 33"), "type 2: one-letter suffix");
    ok(roundTrip(M(2, "VK3ARW/2", "", 33), "VK3ARW/2 33"), "type 2: one-digit suffix");
    ok(roundTrip(M(2, "K1ABC/12", "", 27), "K1ABC/12 27"), "type 2: two-digit suffix");
    uint8_t bits[7];
    ok(!wspr::pack(M(1, "K1ABC", "FN42", 36), bits), "illegal power (36) refused");
    ok(!wspr::pack(M(1, "K1ABC", "ZZ42", 37), bits), "illegal locator refused");
    ok(!wspr::pack(M(1, "KABCDE", "FN42", 37), bits), "a call without its digit refused");
    ok(!wspr::pack(M(2, "K1ABC/00", "", 37), bits), "suffix /00 refused (not sendable)");

    // ★ the hash: a type 3 is a guess until the call has been heard in full
    wspr::Message t3 = M(3, "K9XYZ", "EN52AB", 23);
    ok(wspr::pack(t3, bits), "type 3 packs");
    wspr::Message u;
    ok(wspr::unpack(bits, u, &wspr::callHashes()) && u.type == 3 && u.call.empty() && u.grid == "EN52AB" && u.dbm == 23,
       "type 3 before the call is known: grid + power, call EMPTY (no guess)");
    wspr::callHashes().save("K9XYZ");
    ok(wspr::unpack(bits, u, &wspr::callHashes()) && u.call == "K9XYZ" && u.text() == "<K9XYZ> EN52AB 23", "…and resolved once heard: <K9XYZ> EN52AB 23");
    ok(wspr::callHash("K9XYZ") < 32768 && wspr::callHash("K9XYZ") != wspr::callHash("K9XYY"), "hash is 15 bits and call-dependent");

    // the encoder always carries the sync vector, and the Fano decoder is exact on clean soft bits
    uint8_t tones[162];
    bool syncOk = true, fanoOk = true;
    const char* calls[] = {"K1ABC", "G4ABC", "DL1XYZ", "JA1GGG", "VK2FF"};
    for (int i = 0; i < 5; ++i) {
        wspr::Message m = M(1, calls[i], "JO22", 23);
        wspr::encode(m, tones);
        for (int k = 0; k < 162; ++k) if ((tones[k] & 1) != wspr::kSyncVector[k]) syncOk = false;
        float llr[162];
        for (int p = 0; p < 162; ++p) llr[p] = (tones[wspr::interleavePos(p)] >> 1) ? 4.f : -4.f;
        uint8_t got[7], want[7]; long cyc = 0;
        wspr::pack(m, want);
        if (!wspr::fanoDecode(llr, 100000, 2.f, got, &cyc) || std::memcmp(got, want, 7) != 0 || cyc > 200) fanoOk = false;
    }
    ok(syncOk, "every symbol's low bit is the published sync bit");
    ok(fanoOk, "Fano: clean soft bits decode in under 200 cycles, bit-exact");
}

static void synthetic() {
    std::printf("\n— synthetic slots (SNR in 2500 Hz) —\n");
    struct Case { double snr, centre, dt, drift; };
    const Case cases[] = {
        {-20, 1450, 0.0, 0}, {-20, 1395, -2.0, 3.0}, {-20, 1605, 4.0, -3.0},
        {-24, 1500, 0.5, 1.5}, {-26, 1420, -1.0, -2.0}, {-27, 1580, 2.0, 0.5}, {-28, 1510, 0.3, 0}, {-28, 1470, 1.0, -1.0},
    };
    int n = 0;
    for (const Case& c : cases) {
        std::vector<float> a((size_t)(12000 * 120), 0.f);
        wsprsynth::addNoise(a, 11 + (uint64_t)n);
        wsprsynth::Sig s; s.msg = wsprsynth::msg1("G4ABC", "IO92", 37); s.snr = c.snr; s.centreHz = c.centre; s.dt = c.dt; s.drift = c.drift;
        wsprsynth::addSignal(a, 12000, s, 1.0, 21 + (uint64_t)n);
        wspr::callHashes().clear();
        const auto out = decodeSlot(a, 12000);
        const WsprSpot* hit = nullptr;
        for (auto& x : out) if (x.message == "G4ABC IO92 37") hit = &x;
        char what[200];
        if (hit) std::snprintf(what, sizeof what, "%+.0f dB at %.0f Hz, DT %+.1f, drift %+.1f → snr %.1f (WSPRnet %d), %.2f Hz, DT %+.2f, drift %+.2f",
                               c.snr, c.centre, c.dt, c.drift, hit->snrTrue, hit->snr, hit->audioHz, hit->dt, hit->drift);
        else std::snprintf(what, sizeof what, "%+.0f dB at %.0f Hz, DT %+.1f, drift %+.1f → NOT DECODED", c.snr, c.centre, c.dt, c.drift);
        ok(hit && out.size() == 1 && std::fabs(hit->snrTrue - c.snr) <= 1.5 && std::fabs(hit->snr - c.snr) <= 1.5
               && std::fabs(hit->audioHz - c.centre) <= 0.3
               && std::fabs(hit->dt - c.dt) <= 0.1 && std::fabs(hit->drift - c.drift) <= 0.6, what);
        ++n;
    }
    // other audio rates: the decimator's first stage scales with the rate
    for (int rate : {24000, 48000}) {
        std::vector<float> a((size_t)(rate * 120), 0.f);
        wsprsynth::addNoise(a, 77);
        wsprsynth::Sig s; s.msg = wsprsynth::msg1("DL1XYZ", "JO62", 30); s.snr = -24; s.centreHz = 1530; s.dt = 0.4;
        wsprsynth::addSignal(a, rate, s, 1.0, 78);
        const auto out = decodeSlot(a, rate);
        ok(out.size() == 1 && out[0].message == "DL1XYZ JO62 30" && std::fabs(out[0].snrTrue + 24) <= 1.5,
           std::string("decodes at ") + std::to_string(rate) + " Hz audio");
    }
}

static void hashedAndNoise() {
    std::printf("\n— hashed calls across slots, and pure noise —\n");
    wspr::callHashes().clear();
    auto slotWith = [](const wspr::Message& m, uint64_t seed) {
        std::vector<float> a((size_t)(12000 * 120), 0.f);
        wsprsynth::addNoise(a, seed);
        wsprsynth::Sig s; s.msg = m; s.snr = -22; s.centreHz = 1488;
        wsprsynth::addSignal(a, 12000, s, 1.0, seed + 1);
        return decodeSlot(a, 12000);
    };
    const wspr::Message t3 = M(3, "PJ4/K1ABC", "FK52UD", 37);
    auto o1 = slotWith(t3, 501);
    ok(o1.empty(), "a type 3 whose call was never heard: no spot");
    auto o2 = slotWith(M(2, "PJ4/K1ABC", "", 37), 503);
    ok(o2.size() == 1 && o2[0].message == "PJ4/K1ABC 37" && o2[0].type == 2, "the type 2 (compound call) decodes: PJ4/K1ABC 37");
    auto o3 = slotWith(t3, 505);
    ok(o3.size() == 1 && o3[0].message == "<PJ4/K1ABC> FK52UD 37" && o3[0].call == "PJ4/K1ABC" && o3[0].grid == "FK52UD",
       "…then the type 3 resolves: <PJ4/K1ABC> FK52UD 37");

    int falses = 0;
    for (int t = 0; t < 30; ++t) {
        std::vector<float> a((size_t)(12000 * 120), 0.f);
        wsprsynth::addNoise(a, 9000 + (uint64_t)t);
        falses += (int)decodeSlot(a, 12000).size();
    }
    ok(falses == 0, "30 slots of pure noise: " + std::to_string(falses) + " false decodes");
}

static void subtraction() {
    std::printf("\n— subtraction —\n");
    // a strong station and a weak one TWO TONES (2.93 Hz) above it, overlapping in time
    std::vector<float> a((size_t)(12000 * 120), 0.f);
    wsprsynth::addNoise(a, 4242);
    wsprsynth::Sig s1; s1.msg = wsprsynth::msg1("F5AAA", "JN18", 37); s1.snr = -8; s1.centreHz = 1500; s1.dt = 0.2;
    wsprsynth::Sig s2; s2.msg = wsprsynth::msg1("OH2CC", "KP20", 23); s2.snr = -24; s2.centreHz = 1500 + 2 * wspr::kBaud; s2.dt = 0.6;
    wsprsynth::addSignal(a, 12000, s1, 1.0, 1);
    wsprsynth::addSignal(a, 12000, s2, 1.0, 2);
    std::vector<float> b = a;
    const auto one = decodeSlot(a, 12000, 1), three = decodeSlot(b, 12000, 3);
    std::set<std::string> m1, m3;
    for (auto& x : one) m1.insert(x.message);
    for (auto& x : three) m3.insert(x.message + (x.pass > 1 ? " (pass 2+)" : ""));
    ok(m1.count("F5AAA JN18 37") && !m1.count("OH2CC KP20 23"), "pass 1 alone: the strong one only");
    ok(m3.count("F5AAA JN18 37") && m3.count("OH2CC KP20 23 (pass 2+)") && three.size() == 2, "with subtraction: the weak one too, found by pass 2");
}

static void live() {
    std::printf("\n— the live decoder (int16 blocks, capture times) —\n");
    wspr::callHashes().clear();
    const int rate = 12000;
    const double slot = 1793000040.0;                  // an even minute
    const double t0 = slot - 10.0;                     // audio starts 10 s before it
    const int total = (int)(rate * 140.0);
    std::vector<float> a((size_t)total, 0.f);
    wsprsynth::addNoise(a, 31337, 600.0);              // int16 scale
    // the synth's buffer starts kLeadSec before the minute: place the signal into a copy at that origin
    std::vector<float> sig((size_t)(rate * 120), 0.f);
    wsprsynth::Sig s; s.msg = wsprsynth::msg1("GW3WW", "IO81", 23); s.snr = -21; s.centreHz = 1444; s.dt = 0.1;
    wsprsynth::addSignal(sig, rate, s, 600.0, 5);
    const size_t off = (size_t)((10.0 - wspr::kLeadSec) * rate);
    for (size_t i = 0; i < sig.size() && off + i < a.size(); ++i) a[off + i] += sig[i];
    std::vector<int16_t> pcm(a.size());
    for (size_t i = 0; i < a.size(); ++i) pcm[i] = (int16_t)std::max(-32767.f, std::min(32767.f, a[i]));

    std::vector<WsprSpot> got;
    std::mutex gm;
    {
        WsprDecoder d(rate);
        d.dialHz.store(7038600);
        d.onSpot = [&](const WsprSpot& sp) { std::lock_guard<std::mutex> lk(gm); got.push_back(sp); };
        const int blk = 1200;
        for (int i = 0; i + blk <= total; i += blk) d.process(pcm.data() + i, blk, t0 + (double)(i + blk) / rate + 0.05);
        for (int w = 0; w < 200 && d.slotsDecoded.load() < 1; ++w) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ok(d.slotsDecoded.load() == 1 && d.slotsSkipped.load() == 0, "one slot cut and decoded, none skipped");
        char ms[64]; std::snprintf(ms, sizeof ms, "decode took %.0f ms, %d passes", d.lastDecodeMs.load(), d.lastPasses.load());
        std::printf("  %s\n", ms);
    }
    std::lock_guard<std::mutex> lk(gm);
    ok(got.size() == 1 && got[0].message == "GW3WW IO81 23" && got[0].slotUtc == slot && std::fabs(got[0].freqHz - (7038600 + 1444)) < 0.3
           && std::fabs(got[0].dt - 0.1) < 0.15,
       got.empty() ? std::string("no spot") : "spot " + got[0].message + " at " + std::to_string(got[0].freqHz) + " Hz, DT " + std::to_string(got[0].dt));
}

static bool readWav(const std::string& path, std::vector<float>& out, int& rate) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char hdr[44];
    if (std::fread(hdr, 1, 44, f) != 44 || std::memcmp(hdr, "RIFF", 4)) { std::fclose(f); return false; }
    std::memcpy(&rate, hdr + 24, 4);
    std::vector<int16_t> s;
    int16_t buf[4096]; size_t n;
    while ((n = std::fread(buf, 2, 4096, f)) > 0) s.insert(s.end(), buf, buf + n);
    std::fclose(f);
    out.resize(s.size());
    for (size_t i = 0; i < s.size(); ++i) out[i] = s[i] / 32768.f;
    return true;
}

// the reference's spots for one dial: "message" (a type 3 printed "CALL GRID6 dBm", double spaces collapsed) → SNR
static std::map<std::pair<long, std::string>, int> readRef(const std::string& path, double dial) {
    std::map<std::pair<long, std::string>, int> m;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return m;
    char line[4096];
    auto field = [](const std::string& l, const char* key) {
        const std::string k = std::string("\"") + key + "\":";
        size_t p = l.find(k); if (p == std::string::npos) return std::string();
        p += k.size();
        if (l[p] == '"') return l.substr(p + 1, l.find('"', p + 1) - p - 1);
        size_t e = p; while (e < l.size() && l[e] != ',' && l[e] != '}') ++e;
        return l.substr(p, e - p);
    };
    while (std::fgets(line, sizeof line, f)) {
        const std::string l = line;
        if (std::fabs(std::atof(field(l, "frequency").c_str()) - dial) > 1) continue;
        std::string msg, raw = field(l, "message");
        for (char c : raw) if (!(c == ' ' && !msg.empty() && msg.back() == ' ')) msg += c;
        struct tm t = {};
        const std::string ts = field(l, "timestamp");
        if (std::sscanf(ts.c_str(), "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) continue;
        t.tm_year -= 1900; t.tm_mon -= 1;
        m[{(long)timegm(&t), msg}] = std::atoi(field(l, "snr").c_str());
    }
    std::fclose(f);
    return m;
}

struct Band {
    const char* label; const char* wav; const char* ref; double t0, dial; int slots, minDecodes;
    std::set<std::string> confirmed; const char* typeThree;   // a type 3 that must resolve from an earlier slot, or null
};

/* ★★ REAL SIGNALS, AND THE WSPRNET SNR SCALE (Stuart, 2026-10-27). Every decode must be a station another receiver
 *  heard; and the spots' SNR must read what Stuart's UberSDR (a WSPRnet reporter, STUEY3D) read for the same station
 *  in the same slot: mean within ±1 dB, and each one within ±3 dB (its own SNRs are whole decibels). */
static void realBand(const Band& b) {
    std::printf("\n— real signals: %s —\n", b.label);
    const char* dir = std::getenv("VIBE_TEST_AUDIO");
    std::vector<float> a; int rate = 0;
    if (!dir || !readWav(std::string(dir) + "/wspr/" + b.wav, a, rate)) {
        std::printf("  not run — needs VIBE_TEST_AUDIO holding wspr/%s\n", b.wav);
        return;
    }
    const auto ref = readRef(std::string(dir) + "/wspr/" + b.ref, b.dial);
    wspr::callHashes().clear();
    WsprSlotDecoder d;
    int total = 0, unconfirmed = 0, matched = 0, outside = 0;
    double sum = 0, sumTrue = 0, worst = 0;
    bool resolved = !b.typeThree;
    double ms = 0;
    const double first = std::ceil((b.t0 + wspr::kLeadSec) / 120.0) * 120.0;
    size_t refN = 0;
    for (const auto& r : ref) if (r.first.first >= (long)first && r.first.first < (long)(first + 120.0 * b.slots)) ++refN;
    for (int k = 0; k < b.slots; ++k) {
        const double slot = first + 120.0 * k;
        const long i0 = std::lround((slot - wspr::kLeadSec - b.t0) * rate);
        std::vector<WsprSpot> out; WsprSlotStats st;
        const auto c0 = std::chrono::steady_clock::now();
        d.decodeAudio(a.data() + i0, (int)std::min<long>((long)(120 * rate), (long)a.size() - i0), rate, b.dial, slot, WsprSlotOptions(), out, st);
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        total += (int)out.size();
        for (auto& x : out) {
            if (!b.confirmed.count(x.call)) { ++unconfirmed; std::printf("  unconfirmed: %s\n", x.message.c_str()); }
            if (b.typeThree && x.message == b.typeThree) resolved = true;
            std::string m = x.message; m.erase(std::remove(m.begin(), m.end(), '<'), m.end()); m.erase(std::remove(m.begin(), m.end(), '>'), m.end());
            const auto it = ref.find({(long)slot, m});
            if (it == ref.end()) continue;
            const double dd = x.snr - it->second;
            ++matched; sum += dd; sumTrue += x.snrTrue - it->second;
            worst = std::max(worst, std::fabs(dd));
            if (std::fabs(dd) > 3) { ++outside; std::printf("  SNR %s: ours %d, reference %d\n", x.message.c_str(), x.snr, it->second); }
        }
    }
    std::printf("  %d decodes in %d slots (reference: %zu), %.0f ms per slot; %d matched: SNR ours−reference %+.2f dB "
                "(definition %+.2f dB), worst %.0f dB\n", total, b.slots, refN, ms / b.slots, matched,
                matched ? sum / matched : 0, matched ? sumTrue / matched : 0, worst);
    ok(total >= b.minDecodes, std::string("at least ") + std::to_string(b.minDecodes) + " decodes");
    ok(unconfirmed == 0, "every decode confirmed by another receiver");
    if (b.typeThree) ok(resolved, std::string("a type 3 resolved through a hash learnt in an earlier slot: ") + b.typeThree);
    ok(matched >= 15 && std::fabs(sum / matched) <= 1.0, "WSPRnet-scale SNR: mean within ±1 dB of the reference's");
    ok(matched && outside == 0, "WSPRnet-scale SNR: every matched station within ±3 dB");
}

static void real() {
    // every call decoded in these slots by the reference decoder or confirmed on wspr.live by other receivers
    realBand({"Stuart's UberSDR, 40 m, 2026-10-26 07:30–07:36 UTC", "wspr_40m_7038600_20261026T0729.wav",
              "ubersdr_wspr_spots_20261026T0729.jsonl", 1792999757.582, 7038600, 3, 45,
              {"F4HHR","DB1IAT","PA2PGU","LA4PGA","EB1A","PE1AHJ","PE0FKO","F1ORV","PA4JAM","EA6LA","PH1RAM",
               "OK2BVG","PA3CNO","PA3EDR/G","OK1BF","EA6FG","PA2RVP","ON4WS","PH1A","HB9BAX","GM8HHC","PD1RE","SK7BQ","PA0ANH","MM6VTS","PA7PP",
               "WB1ABO","EA4TK","DK3BI","KC1ATT","PH9K","G4HSB","GM1MSS","DL3EL","PA1JCK","ON4LAO","M8KFH","VK3ARW/2","F6IDT","G0IDE","KK1D",
               "HB9AR","W1NEJ","PA5CA","SO2O","VE3CWM","EA6URP","OK2JO"}, "<EA4TK> IN80EJ 23"});
    // the same receiver ten hours later, five slots on each band (the busy evening 40 m: the definition reads 11 dB
    // under the reference there, 3.4 dB on 20 m — the case no constant offset could cover)
    realBand({"Stuart's UberSDR, 40 m, 2026-10-26 16:10–16:20 UTC", "wspr_40m_7038600_20261026T1607.wav",
              "ubersdr_wspr_spots_20261026T1607.jsonl", 1793030851.951, 7038600, 5, 80, {"2E0IJC","DB0SFA","DB1IAT","DB2OM","DB6OC","DG6RCH","DJ0MDO","DK3BI","DK3TG","DK9ES","DL0PBS","DL6OBU","EA1M","EA4TK","EA6LA","EB1A","F1ORV","F4HHR","G0VCA","G4EFE","G4HSB","G4LRP","G6CKK","G7CXR","G7MNP","G7SYO","G7UJY","G8LDJ","G8WDZ","GI4LGP","GM1MSS","GM4DTH","GM4HAM","GM4ISM","GM7FIE","GM8HHC","HB9AK","I0HCJ","I0LYO","LA4PGA","M0CUV","M0LHP","M0XMP","M1JRH","M5TXJ","OK2BVG","ON4LZ","ON4WS","OZ4JJ","PA0ANH","PA1JCK","PA2PGU","PA2RVP","PA2W","PA5CA","PA7PP","PD1RE","PE1AHJ","PH1RAM","PH9K","SK7BQ"}, nullptr});
    realBand({"Stuart's UberSDR, 20 m, 2026-10-26 16:10–16:20 UTC", "wspr_20m_14095600_20261026T1607.wav",
              "ubersdr_wspr_spots_20261026T1607.jsonl", 1793030851.951, 14095600, 5, 24, {"CT1DJG","EA3AGU","EA6ALL","EB1A","F1JSC","F4WCV","G0GCD","G4LEN","HA3MGA","HA4GHR","IU0JJD","IU1FIG","IU3UXD","N3TFX","OE1ELW","OE3KFB","OE6LHG","OH6CR","OH8RO","SV2BXH","VE3XIX","VK6XT"}, nullptr});
}

int main() {
    std::printf("test-wspr — the WSPR-2 decoder\n");
    protocol();
    synthetic();
    hashedAndNoise();
    subtraction();
    live();
    real();
    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
