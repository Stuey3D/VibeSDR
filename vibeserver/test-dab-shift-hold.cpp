// test-dab-shift-hold.cpp — an ESTABLISHED integer carrier shift survives single torn frames, and still follows a
// real move (Stuart, 2026-11-14: a B210 at 56 MS/s, MER 15+, "lock keeps dropping" — its USB link lost ~2 chunks a
// second and the offset flipped 777 Hz ↔ 11–14 kHz on screen: torn frames' searches picking a carrier 12–14 away).
//
// A synthesised Mode I stream, 1 carrier + 200 Hz off (a crystal a few ppm out). Single frames are moved a further
// 13 carriers — what a torn frame's search reported — and must NOT move the receiver; a lasting move must, within
// three frames.
#include "vibe_dab_receiver.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace vibedab;
static int fails = 0;
static void ok(bool c, const char* what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what); if (!c) ++fails; }
static uint32_t seed = 4242; static uint32_t rnd() { seed = seed * 1664525u + 1013904223u; return seed; }

static std::vector<Cplx> makeFrame() {
    const Mode& m = modeI();
    const int N = m.usefulSamples, G = m.guardSamples, K = m.carriers;
    std::vector<std::vector<C32>> syms;
    std::vector<C32> ref(size_t(K), C32{}); prsSymbol(ref.data()); syms.push_back(ref);
    std::vector<C32> prev = ref;
    static const C32 rot[4] = { {0.7071f,0.7071f}, {-0.7071f,0.7071f}, {-0.7071f,-0.7071f}, {0.7071f,-0.7071f} };
    for (int s = 1; s < m.symbolsPerFrame; ++s) {
        std::vector<C32> cur((size_t)K);
        for (int k = 0; k < K; ++k) cur[size_t(k)] = prev[size_t(k)] * rot[rnd() & 3];
        syms.push_back(cur); prev = cur;
    }
    std::vector<Cplx> out; out.reserve(size_t(m.frameSamples));
    for (int i = 0; i < m.nullSamples; ++i) out.push_back({0.0f, 0.0f});   // the null symbol
    std::vector<double> cs((size_t)N), sn((size_t)N);                       // twiddles: the same inverse DFT as
    for (int t = 0; t < N; ++t) { cs[(size_t)t] = std::cos(2.0 * M_PI * t / N); sn[(size_t)t] = std::sin(2.0 * M_PI * t / N); }
    std::vector<Cplx> useful((size_t)N);                                     // test-dab-receiver, from a table
    for (const auto& carriers : syms) {
        for (int t = 0; t < N; ++t) {
            double re = 0, im = 0; int j = 0;
            for (int k = -K / 2; k <= K / 2; ++k) {
                if (k == 0) continue;
                const C32 c = carriers[size_t(j++)];
                const size_t ix = size_t(((long long)k * t % N + N) % N);
                re += double(c.real()) * cs[ix] - double(c.imag()) * sn[ix];
                im += double(c.real()) * sn[ix] + double(c.imag()) * cs[ix];
            }
            useful[size_t(t)] = { float(re / N), float(im / N) };
        }
        for (int i = 0; i < G; ++i) out.push_back(useful[size_t(N - G + i)]);   // cyclic prefix
        for (int i = 0; i < N; ++i) out.push_back(useful[size_t(i)]);
    }
    return out;
}

/** Runs `frames` frames; frame i is offset by baseHz + extra(i) Hz. Returns the shift read after each frame. */
/** Builds `frames` frames of air; frame i offset by baseHz + extra(i) Hz (continuous phase — a frequency, not a splice),
 *  then drives the receiver exactly as DabService's worker does: a two-frame window, push, advanceAfterPush,
 *  syncConsumed. Returns the integer shift after each decoded frame. */
template <class F>
static std::vector<int> run(DabReceiver& rx, const std::vector<Cplx>& frame, int frames, double baseHz, F extra) {
    std::vector<Cplx> air; air.reserve(frame.size() * size_t(frames + 2));
    double phase = 0.0;
    for (int i = 0; i < frames + 2; ++i) {
        const double hz = baseHz + extra(i < frames ? i : frames - 1);
        const double step = 2.0 * M_PI * hz / double(kCanonicalRateHz);
        for (const auto& c0 : frame) {
            const float cr = float(std::cos(phase)), ci = float(std::sin(phase));
            air.push_back({ c0.re * cr - c0.im * ci, c0.re * ci + c0.im * cr });
            phase += step; if (phase > M_PI) phase -= 2.0 * M_PI;
        }
    }
    std::vector<int> seen;
    const size_t need = size_t(modeI().frameSamples) * 2;
    size_t head = 0;
    while (air.size() - head >= need) {
        if (rx.push(air.data() + head, need)) seen.push_back(rx.stats().intOffsetCarriers);
        const size_t adv = rx.advanceAfterPush(size_t(modeI().frameSamples));
        rx.syncConsumed(adv);
        head += adv;
    }
    return seen;
}

int main() {
    std::printf("test-dab-shift-hold\n");
    std::printf("  synthesising a Mode I frame…\n");
    const std::vector<Cplx> frame = makeFrame();
    const std::vector<int> torn = { 8, 12, 13, 17, 21 };       // torn frames, scattered like the B210's holes
    auto isTorn = [&](int i) { for (int t : torn) if (t == i) return true; return false; };

    DabReceiver rx;
    const auto seen = run(rx, frame, 26, 1200.0, [&](int i) { return isTorn(i) ? 13000.0 : 0.0; });
    std::printf("  shift per frame:");
    for (int s : seen) std::printf(" %d", s);
    std::printf("\n");
    int settled = -1; for (size_t i = 0; i < seen.size(); ++i) if (seen[i] != 0) { settled = int(i); break; }
    ok(seen.size() > 6 && seen[5] == 1 && seen[6] == 1 && seen[7] == 1, "locks at +1 carrier (1.2 kHz off) and holds it");
    bool held = true; for (int i = 8; i < 26; ++i) if (seen[size_t(i)] != 1) held = false;
    ok(held, "★ single frames 13 carriers off (torn frames) never move the established shift");
    char msg[96]; std::snprintf(msg, sizeof msg, "the overruled frames are counted (shiftHeld %d, torn %zu)", rx.stats().shiftHeld, torn.size());
    ok(rx.stats().shiftHeld >= int(torn.size()) - 1, msg);
    (void)settled;

    // ★ a real move — the radio retuned by its PPM correction, say — is followed within three frames
    DabReceiver rx2;
    const auto seen2 = run(rx2, frame, 20, 1200.0, [&](int i) { return i >= 10 ? 3000.0 : 0.0; });
    std::printf("  shift per frame:");
    for (int s : seen2) std::printf(" %d", s);
    std::printf("\n");
    ok(seen2[9] == 1, "before the move: +1");
    ok(seen2[12] == 4 && seen2[19] == 4, "★ a lasting move of 3 carriers is followed by the third frame, and kept");

    // ★ first lock is untouched: a receiver with nothing established takes the first answer it gets
    DabReceiver rx3;
    const auto seen3 = run(rx3, frame, 4, 13200.0, [](int) { return 0.0; });
    ok(!seen3.empty() && seen3.back() == 13, "a fresh lock 13 carriers off is taken at once (weak-signal acquisition unchanged)");

    std::printf(fails ? "  %d FAILED\n" : "  all passed\n", fails);
    return fails ? 1 : 0;
}
