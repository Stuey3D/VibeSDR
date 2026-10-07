#pragma once
// ★★★ THE DECODER FEED'S RESAMPLER IS ONE CONTINUOUS STREAM, NOT ONE PER PACKET (2026-10-07).
//
// A network backend's audio (Kiwi) reaches the decoder sidecar packet by packet and is upsampled to
// the decoders' 48 kHz. The old loop started at position 0 for every packet and stopped before the
// last sample (s < n − 1), so EACH PACKET LOST ONE INPUT SAMPLE and its fractional phase. A Kiwi
// packet is ~1024 samples → 0.098 % of the audio gone → a WEFAX line (0.5 s, 1809 px) short by
// 1.77 px every line. Stuart's live JMH off a Japanese Kiwi leaned ~2 px a line — ten times what the
// aligner's slant search covers. Every decoder heard it (SSTV sheared, FT8/RTTY ran 0.1 % fast).
//
// Here the read position and the previous packet's last sample carry over, so a packet boundary is
// invisible: x[−1] is the last sample of the packet before. And the rate is the EXACT one the
// backend reports (a Kiwi's sample_rate ≈ 12001.1), never a rounded one — the same lesson as the
// server's exact-rate resampler (RC25). See test-decfeed-resampler.cpp.
#include <cmath>
#include <cstdint>
#include <vector>

namespace vibe {

struct DecFeedResampler {
    double outRate = 48000.0;
    double pos = 0.0;         // read position relative to the NEXT packet's first sample
    float  prev = 0.0f;       // the previous packet's last sample (x[−1])
    bool   havePrev = false;
    double rate = 0.0;        // the input rate this state belongs to

    void reset() { pos = 0.0; prev = 0.0f; havePrev = false; }

    /** Appends the resampled mono floats for one packet of int16 at `inRate` Hz to `out`. */
    void push(const int16_t* pcm, int n, double inRate, std::vector<float>& out) {
        if (n < 1 || !(inRate > 0)) return;
        if (inRate != rate) { rate = inRate; reset(); }
        const double step = inRate / outRate;
        double s = pos;
        if (!havePrev && s < 0.0) s = 0.0;
        for (; s <= (double)(n - 1); s += step) {
            const int i = (int)std::floor(s);
            const double f = s - i;
            const float a = (i < 0) ? prev : pcm[i] / 32768.0f;
            const float b = (i + 1 < n) ? pcm[i + 1] / 32768.0f : a;   // f == 0 there (s == n − 1)
            out.push_back((float)((1.0 - f) * a + f * b));
        }
        pos = s - (double)n;  // ∈ [−1, −1 + step): between this packet's last sample and the next's first
        prev = pcm[n - 1] / 32768.0f;
        havePrev = true;
    }
};

}  // namespace vibe
