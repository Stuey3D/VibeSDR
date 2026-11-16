// decoders/tones_tx.h — V12 tone decoders: OUR OWN TRANSMITTER for the tests, the benchmark row and the end-to-end clip
// (docs/v12/TONES.md). DTMF from ITU-T Q.23's pairs; 5-tone from tones_fivetone.h's tables, with the repeat rule applied
// on the way out ("12334" is sent "123E4"). Phase-continuous sines with 2 ms raised-cosine edges (no clicks).
#pragma once
#include "decoders/tones_dtmf.h"
#include "decoders/tones_fivetone.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace vibe {
namespace tones {
namespace tx {

/** Appends `sec` of silence. */
inline void silence(std::vector<float>& a, double sec, double fs = 48000) { a.insert(a.end(), (size_t)(sec * fs), 0.f); }

/** One burst of one or two tones, `sec` long, 2 ms edges. `amp` each tone's peak. */
inline void burst(std::vector<float>& a, double f1, double f2, double sec, double amp1, double amp2, double fs = 48000) {
    const int n = (int)std::lround(sec * fs), r = (int)std::lround(0.002 * fs);
    for (int i = 0; i < n; ++i) {
        double g = 1;
        if (i < r) g = 0.5 - 0.5 * std::cos(M_PI * i / r);
        else if (i >= n - r) g = 0.5 - 0.5 * std::cos(M_PI * (n - 1 - i) / r);
        const double t = i / fs;
        double v = amp1 * std::sin(2 * M_PI * f1 * t);
        if (f2 > 0) v += amp2 * std::sin(2 * M_PI * f2 * t);
        a.push_back((float)(g * v));
    }
}

struct DtmfOpt {
    double toneSec = 0.07, gapSec = 0.07;
    double amp = 0.25;        ///< the low tone's peak
    double twistDb = 2.0;     ///< high − low (Q.23's sender lifts the high group ~2 dB)
    double offLow = 0, offHigh = 0;   ///< relative frequency error (0.015 = +1.5 %)
};
/** A dialled string of "0123456789*#ABCD". Unknown characters are skipped. */
inline void dtmf(std::vector<float>& a, const std::string& keys, const DtmfOpt& o = DtmfOpt(), double fs = 48000) {
    for (char k : keys) {
        const char* p = std::strchr(Dtmf::kKeys, k);
        if (!p || !k) continue;
        const int i = (int)(p - Dtmf::kKeys);
        const double fl = Dtmf::kLow[i / 4] * (1 + o.offLow), fh = Dtmf::kHigh[i % 4] * (1 + o.offHigh);
        burst(a, fl, fh, o.toneSec, o.amp, o.amp * std::pow(10.0, o.twistDb / 20), fs);
        silence(a, o.gapSec, fs);
    }
}

/** A 5-tone call in table `table` (tones_fivetone.h), `digits` of "0–9A–F" as meant — the repeat rule is applied here. A
 *  continuous-phase sequence, each tone `periodMs` (0 = the table's nominal) — no gaps, as a sequential encoder sends. */
inline std::string withRepeats(const std::string& digits) {
    std::string out; char prev = 0;
    for (char d : digits) { out += (d == prev) ? 'E' : d; prev = (d == prev) ? 0 : d; }
    return out;
}
inline void fiveTone(std::vector<float>& a, int table, const std::string& digits, double periodMs = 0, double amp = 0.3,
                     double off = 0, double fs = 48000) {
    const ToneTable& t = toneTables()[table];
    const double per = (periodMs > 0 ? periodMs : t.periodMs) / 1000.0;
    const std::string sent = withRepeats(digits);
    const int n = (int)std::lround(per * fs), r = (int)std::lround(0.002 * fs);
    const int total = n * (int)sent.size();
    double ph = 0;
    for (size_t k = 0; k < sent.size(); ++k) {
        const char* hex = "0123456789ABCDEF";
        const int d = (int)(std::strchr(hex, sent[k]) - hex);
        const double f = t.hz[d] * (1 + off);
        for (int i = 0; i < n; ++i) {
            const int j = (int)k * n + i;
            double g = 1;
            if (j < r) g = 0.5 - 0.5 * std::cos(M_PI * j / r);
            else if (j >= total - r) g = 0.5 - 0.5 * std::cos(M_PI * (total - 1 - j) / r);
            ph += 2 * M_PI * f / fs;
            a.push_back((float)(g * amp * std::sin(ph)));
        }
    }
}

} // namespace tx
} // namespace tones
} // namespace vibe
