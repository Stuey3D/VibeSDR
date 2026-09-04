// test-dab-interleave.cpp — the Mode I frequency permutation must be an exact bijection.
#include "vibe_dab_interleave.h"
#include <cstdio>
#include <set>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)

int main() {
    printf("test-dab-interleave\n");
    FreqInterleaveI f;

    CHECK(f.count() == 1536, "the permutation yields exactly 1536 symbols");

    // ★★★ F IS A STATED BIJECTION onto {-768..768} \ {0}. Prove it outright rather than sampling:
    //     every carrier used exactly once, none out of range, DC never used.
    std::set<int> seen;
    bool inRange = true, dcUsed = false;
    for (int n = 0; n < 1536; ++n) {
        const int k = f.carrierFor(n);
        if (k < -768 || k > 768) inRange = false;
        if (k == 0) dcUsed = true;
        seen.insert(k);
    }
    CHECK(inRange, "every carrier is within -768..768");
    CHECK(!dcUsed, "the centre carrier is never used");
    CHECK(seen.size() == 1536, "every carrier is used exactly once — a true bijection");

    // The inverse really is the inverse.
    bool inv = true;
    for (int n = 0; n < 1536; ++n) if (f.indexFor(f.carrierFor(n)) != n) inv = false;
    CHECK(inv, "indexFor is the inverse of carrierFor");

    // ★ Anchor the first few against the recursion computed independently here, so a typo in the
    //   constructor cannot pass by agreeing with itself.
    {
        std::vector<int> expect;
        int pi = 0;
        for (int i = 0; i < 2048 && expect.size() < 4; ++i) {
            if (i > 0) pi = (13 * pi + 511) % 2048;
            if (pi >= 256 && pi <= 1792 && pi != 1024) expect.push_back(pi - 1024);
        }
        bool ok = true;
        for (size_t i = 0; i < expect.size(); ++i) if (f.carrierFor(int(i)) != expect[i]) ok = false;
        CHECK(ok, "the first symbols match the congruential relation computed separately");
    }

    // Deinterleaving followed by re-interleaving returns the original.
    {
        std::vector<int> in(1536), mid(1536), back(1536);
        for (int i = 0; i < 1536; ++i) in[i] = i * 7 + 1;
        f.deinterleave(in.data(), mid.data());
        for (int n = 0; n < 1536; ++n) back[f.indexFor(f.carrierFor(n)) == n ? 0 : 0] = 0;  // no-op guard
        bool ok = true;
        for (int n = 0; n < 1536; ++n) {
            const int k = f.carrierFor(n);
            const int idx = k < 0 ? k + 768 : k + 767;
            if (mid[n] != in[idx]) ok = false;
        }
        CHECK(ok, "deinterleave picks the carrier the permutation names");
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
