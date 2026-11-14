// test-u8-conv.cpp — the 8-bit IQ conversion and its ADC statistics (vibe_u8_conv.h), against a plain reference.
// ★★★ Why: the SSE2 (x86) path counted the statistics and never handed them back, so VibeAGC on every x86 server with an
//     RTL-SDR read "no measurement" for the ADC peak and never cut for overload (2026-09-16 → 2026-11-14; Mad Radio
//     DXer's VirtualBox server at 48 dB on his strong local multiplex). Build it for each architecture the server
//     ships on — on an Apple Silicon Mac: native (NEON) and `-arch x86_64` under Rosetta (SSE2).
#include "vibe_u8_conv.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int fails = 0;
static void ok(bool c, const char* what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what); if (!c) ++fails; }
static uint32_t seed = 99; static uint32_t rnd() { seed = seed * 1664525u + 1013904223u; return seed >> 8; }

int main() {
#if defined(__ARM_NEON)
    std::printf("test-u8-conv (NEON)\n");
#elif defined(__SSE2__)
    std::printf("test-u8-conv (SSE2)\n");
#else
    std::printf("test-u8-conv (scalar)\n");
#endif
    // lengths that exercise the vector body, the 128-block rail flush, and the scalar tail
    for (int n : { 7, 16, 33, 2048, 2048 * 16 + 5, 262144 }) {
        for (int lvl : { 20, 120, 128 }) {           // quiet, near full scale, railing
            std::vector<uint8_t> in((size_t)n);
            for (auto& b : in) { int v = 127 + (int)(rnd() % (2 * lvl + 1)) - lvl; b = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
            AdcStats ref; for (uint8_t v : in) { if (v >= 254 || v <= 1) ++ref.rails; if (v > ref.maxV) ref.maxV = v; if (v < ref.minV) ref.minV = v; }
            ref.total = (uint32_t)n;
            AdcStats got; std::vector<float> out((size_t)n);
            vibe::convU8ToF32Dg(in.data(), out.data(), n, 1.0f, &got);
            char msg[160];
            std::snprintf(msg, sizeof msg, "n=%d level ±%d: total %u/%u rails %u/%u max %u/%u min %u/%u", n, lvl,
                          got.total, ref.total, got.rails, ref.rails, got.maxV, ref.maxV, got.minV, ref.minV);
            ok(got.total == ref.total && got.rails == ref.rails && got.maxV == ref.maxV && got.minV == ref.minV, msg);
            bool conv = true;
            for (int i = 0; i < n; ++i) if (std::fabs(out[(size_t)i] - ((float)in[(size_t)i] - 127.4f) / 128.0f) > 1e-6f) { conv = false; break; }
            if (!conv) ok(false, "and the floats are the reference conversion");
        }
    }
    // ★ the statistics accumulate across calls (the capture adds buffers into one 1 s window)
    std::vector<uint8_t> a(4096, 127), b(4096, 127); a[100] = 255; b[200] = 0;
    std::vector<float> o(4096); AdcStats w;
    vibe::convU8ToF32Dg(a.data(), o.data(), 4096, 1.0f, &w);
    vibe::convU8ToF32Dg(b.data(), o.data(), 4096, 1.0f, &w);
    ok(w.total == 8192 && w.rails == 2 && w.maxV == 255 && w.minV == 0, "two buffers add into one window (8192 samples, 2 on the rails)");
    std::printf(fails ? "  %d FAILED\n" : "  all passed\n", fails);
    return fails ? 1 : 0;
}
