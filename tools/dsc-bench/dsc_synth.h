// dsc_synth.h — synthetic DSC calls for the bench (dsc_bench.cpp) and the tests (vibeserver/test-dsc.cpp): one call of
// every format, transmitted as ITU-R M.493 says (dot pattern, phasing, DX/RX time diversity, ECC), modulated, in noise
// at an SNR quoted in 3 kHz, optionally through a slow Rayleigh fade.
#pragma once
#include "decoders/dsc_decoder.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace dscsynth {
using namespace vibe::dsc;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double uni() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
    double gauss() { double u = uni(), v = uni(); if (u < 1e-300) u = 1e-300; return std::sqrt(-2 * std::log(u)) * std::cos(2 * M_PI * v); }
};

/** A test call: its symbols (F … EOS), expansions, and the words its line must contain. */
struct TestCall {
    std::string name;
    std::vector<int> syms;
    std::vector<std::vector<int>> exp;
    std::vector<std::string> expect;   // substrings of describe()
};

inline std::vector<TestCall> calls() {
    using namespace vibe::dsc::build;
    std::vector<TestCall> v;
    auto add = [&](const std::string& n, std::vector<int> s, std::vector<std::string> e, std::vector<std::vector<int>> x = {}) {
        v.push_back({ n, std::move(s), std::move(x), std::move(e) });
    };
    { std::vector<int> s{ 112 }; mmsi(s, "235012345"); s.push_back(105); position(s, 50.2, -4.1667); time(s, 14, 32); s.push_back(109); s.push_back(127);
      add("distress alert", s, { "DISTRESS", "MMSI 235012345 (United Kingdom)", "Sinking", "50\xc2\xb0" "12'N 004\xc2\xb0" "10'W", "14:32 UTC", "J3E" }); }
    { std::vector<int> s{ 112 }; mmsi(s, "538001234"); s.push_back(107); unknownPosition(s); time(s, -1, 0); s.push_back(100); s.push_back(127);
      add("distress, no position", s, { "DISTRESS", "538001234 (Marshall Islands)", "Undesignated distress", "position unknown", "time unknown" }); }
    { std::vector<int> s{ 116, 112 }; mmsi(s, "002320004"); s.push_back(110); mmsi(s, "235012345"); s.push_back(105); position(s, 50.2, -4.1667);
      time(s, 14, 32); s.push_back(109); s.push_back(127);
      add("distress acknowledgement", s, { "DISTRESS ACKNOWLEDGEMENT", "all ships", "coast station 002320004 (United Kingdom)", "ship in distress 235012345" }); }
    { std::vector<int> s{ 116, 112 }; mmsi(s, "002191000"); s.push_back(112); mmsi(s, "636091234"); s.push_back(101); position(s, -12.5, 44.25);
      time(s, 3, 5); s.push_back(109); s.push_back(127);
      add("distress relay, all ships", s, { "DISTRESS RELAY", "coast station 002191000 (Denmark)", "636091234 (Liberia)", "Flooding", "12\xc2\xb0" "30'S 044\xc2\xb0" "15'E" }); }
    { std::vector<int> s{ 120 }; mmsi(s, "244123000"); s.push_back(112); mmsi(s, "002442000"); s.push_back(112); mmsi(s, "305555000"); s.push_back(108);
      position(s, 53.1, 4.5); time(s, 22, 0); s.push_back(100); s.push_back(117);
      add("distress relay, individual", s, { "DISTRESS RELAY", "to 244123000 (Netherlands)", "Abandoning ship", "acknowledgement requested" }); }
    { std::vector<int> s{ 116, 110 }; mmsi(s, "002320004"); s.push_back(109); s.push_back(126); freqKhz(s, 2182.0); noFreq(s); s.push_back(127);
      add("urgency, all ships", s, { "URGENCY", "all ships", "J3E telephony", "2182.0 kHz" }); }
    { std::vector<int> s{ 116, 108 }; mmsi(s, "002570000"); s.push_back(109); s.push_back(126); freqKhz(s, 2187.5); noFreq(s); s.push_back(127);
      add("safety, all ships", s, { "SAFETY", "coast station 002570000 (Norway)", "2187.5 kHz" }); }
    { std::vector<int> s{ 102 }; s.push_back(15); s.push_back(0); s.push_back(5); s.push_back(10); s.push_back(20);   // 1 50 005 10 20 → 15 00 05 10 20
      s.push_back(110); mmsi(s, "002320004"); s.push_back(109); s.push_back(126); freqKhz(s, 2182.0); noFreq(s); s.push_back(127);
      add("geographic area, urgency", s, { "URGENCY", "ships in the area 50\xc2\xb0N 005\xc2\xb0W", "10\xc2\xb0 south", "20\xc2\xb0 east" }); }
    { std::vector<int> s{ 114 }; mmsi(s, "023200001"); s.push_back(100); mmsi(s, "002320004"); s.push_back(109); s.push_back(126);
      freqKhz(s, 4125.0); noFreq(s); s.push_back(127);
      add("group, routine", s, { "ROUTINE", "group 023200001 (United Kingdom)", "4125.0 kHz" }); }
    { std::vector<int> s{ 120 }; mmsi(s, "211234560"); s.push_back(100); mmsi(s, "002111240"); s.push_back(100); s.push_back(126);
      vhfChannel(s, 16); noFreq(s); s.push_back(117);
      add("individual, VHF channel", s, { "ROUTINE", "to 211234560 (Germany)", "coast station 002111240 (Germany)", "Ch 16", "acknowledgement requested" }); }
    { std::vector<int> s{ 120 }; mmsi(s, "002320004"); s.push_back(108); mmsi(s, "232001234"); s.push_back(118); s.push_back(126);
      noFreq(s); noFreq(s); s.push_back(117);
      add("test call", s, { "SAFETY TEST", "to coast station 002320004", "from 232001234", "acknowledgement requested" }); }
    { std::vector<int> s{ 120 }; mmsi(s, "232001234"); s.push_back(108); mmsi(s, "002320004"); s.push_back(118); s.push_back(126);
      noFreq(s); noFreq(s); s.push_back(122);
      add("test acknowledgement", s, { "SAFETY TEST", "to 232001234", "acknowledgement" }); }
    { std::vector<int> s{ 120 }; mmsi(s, "002320004"); s.push_back(100); mmsi(s, "366123450"); s.push_back(121); s.push_back(126);
      s.push_back(55); position(s, 36.85, -76.3); time(s, 9, 41); s.push_back(122);
      add("position reply", s, { "ROUTINE POSITION", "366123450 (United States)", "36\xc2\xb0" "51'N 076\xc2\xb0" "18'W at 09:41 UTC" }); }
    { std::vector<int> s{ 120 }; mmsi(s, "538001234"); s.push_back(100); mmsi(s, "002442000"); s.push_back(104); s.push_back(102);
      noFreq(s); noFreq(s); s.push_back(122);
      add("unable to comply", s, { "unable to comply: busy" }); }
    { std::vector<int> s{ 123 }; mmsi(s, "002320004"); s.push_back(100); mmsi(s, "235012345"); s.push_back(101); s.push_back(126);
      noFreq(s); noFreq(s); s.push_back(105); s.push_back(4); s.push_back(41); s.push_back(23); s.push_back(45); s.push_back(67); s.push_back(89);
      s.push_back(117);
      add("semi-automatic", s, { "number 44123456789", "F3E/G3E duplex telephony" }); }
    { std::vector<int> s{ 112 }; mmsi(s, "235012345"); s.push_back(100); position(s, 51.5, 1.25); time(s, 8, 15); s.push_back(100); s.push_back(127);
      std::vector<int> e{ 100, 12, 34, 56, 78, 102, 1, 23, 103, 18, 25, 127 };
      add("distress + expansion", s, { "Fire, explosion", "51\xc2\xb0" "30.1234'N 001\xc2\xb0" "15.5678'E", "12.3 kn", "course 182.5" }, { e }); }
    return v;
}

/** `bits` keyed as audio at 48 kHz, in white noise at `snrDb` (signal power over noise power in 3 kHz), with `lead` and
 *  `tail` seconds of noise around it. `fadeHz` > 0: a flat Rayleigh fade of that Doppler spread. */
inline std::vector<float> render(const std::vector<uint8_t>& bits, bool vhf, double snrDb, Rng& rng, double lead = 0.7,
                                 double tail = 0.6, double fadeHz = 0, double centre = 1700, double ppm = 0, double rate = 48000) {
    const double ph0 = rng.uni() * 2 * M_PI;
    std::vector<float> s = build::modulate(bits, vhf, rate, centre, false, ppm, ph0);
    std::vector<float> c;
    if (fadeHz > 0) c = build::modulate(bits, vhf, rate, centre, false, ppm, ph0 + M_PI / 2);
    const size_t nl = (size_t)(lead * rate), nt = (size_t)(tail * rate);
    std::vector<float> out(nl + s.size() + nt);
    // noise: σ² so that 0.5 / (σ² · 3000 / (rate/2)) = SNR
    const double snr = std::pow(10.0, snrDb / 10.0);
    const double sigma = std::sqrt(0.5 / snr * (rate / 2) / 3000.0);
    double gr = 1, gi = 0, ar = 0, ai = 0;
    const double a = fadeHz > 0 ? 1.0 - std::exp(-2 * M_PI * fadeHz / rate) : 0;
    // ★ a fade starts from its long-run state, not from 1
    if (fadeHz > 0) { ar = rng.gauss() * std::sqrt(0.5); ai = rng.gauss() * std::sqrt(0.5); }
    const double norm = fadeHz > 0 ? std::sqrt(2.0 / a) : 1;   // one-pole lowpass of white noise: keep E|g|² = 1
    for (size_t i = 0; i < out.size(); ++i) {
        double v = 0;
        if (i >= nl && i < nl + s.size()) {
            const size_t k = i - nl;
            if (fadeHz > 0) {
                ar += a * (rng.gauss() * std::sqrt(0.5) * norm - ar);
                ai += a * (rng.gauss() * std::sqrt(0.5) * norm - ai);
                gr = ar; gi = ai;
                v = c[k] * gr - s[k] * gi;
            } else v = s[k];
        }
        out[i] = (float)(v + sigma * rng.gauss());
    }
    return out;
}
} // namespace dscsynth
