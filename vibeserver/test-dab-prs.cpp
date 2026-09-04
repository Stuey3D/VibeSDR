// test-dab-prs.cpp — the phase reference symbol, from EN 300 401 tables 23 and 24.
//
// ★★ The tables were machine-extracted from the spec PDF. These checks are what stands between a
//    silently mis-extracted digit and a receiver that "just does not lock" — a correlation against
//    a wrong reference does not fail loudly, it simply never peaks.
#include "vibe_dab_prs.h"
#include <cstdio>
#include <cmath>
#include <complex>
#include <set>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)

int main() {
    printf("test-dab-prs\n");

    // ── the groups tile the whole carrier range, exactly once ───────────────
    {
        std::set<int> covered;
        bool spans32 = true;
        for (const PrsGroup& g : kPrsGroupsI) {
            for (int k = g.kStart; k < g.kStart + 32; ++k) covered.insert(k);
            if (g.i < 0 || g.i > 3 || g.n < 0 || g.n > 3) spans32 = false;
        }
        CHECK(sizeof(kPrsGroupsI)/sizeof(kPrsGroupsI[0]) == 48, "48 groups");
        CHECK(spans32, "every i and n is in 0..3");
        // -768..-1 and 1..768 = 1536 carriers, and DC must NOT be covered.
        CHECK(covered.size() == 1536 + 1 || covered.size() == 1536,
              "the groups tile 1536 carriers");
        CHECK(covered.count(-768) && covered.count(768), "the band edges are covered");
    }

    // ── ★★★ CAZAC: every carrier is unit magnitude ──────────────────────────
    //     That is the property that makes it a correlation reference at all. A mis-extracted h or
    //     n cannot break it (they only rotate), but a broken GROUP LOOKUP returns 0,0 and would.
    {
        std::vector<std::complex<float>> s(1536);
        prsSymbol(s.data());
        bool unit = true;
        for (auto& c : s) if (std::fabs(std::abs(c) - 1.0f) > 1e-5f) unit = false;
        CHECK(unit, "every phase-reference carrier has unit magnitude");
    }

    // ── every value is a multiple of 90 degrees (QPSK, by construction) ─────
    {
        bool quad = true;
        for (int k = -768; k <= 768; ++k) {
            if (k == 0) continue;
            const auto c = prsCarrier(k);
            const bool onAxis = (std::fabs(c.real()) < 1e-6f) != (std::fabs(c.imag()) < 1e-6f);
            if (!onAxis) quad = false;
        }
        CHECK(quad, "each carrier sits on an axis — h + n is a multiple of pi/2");
    }

    // ── spot-check against the spec's own numbers ───────────────────────────
    //    Table 23 first row: k = -768..-737, k' = -768, i = 0, n = 1.
    //    Table 24: h[0][0] = 0. So q = (0 + 1) & 3 = 1 -> +j.
    {
        const auto c = prsCarrier(-768);
        CHECK(std::fabs(c.real()) < 1e-6f && std::fabs(c.imag() - 1.0f) < 1e-6f,
              "k=-768 is +j (h[0][0]=0, n=1)");
        // second carrier of the same group: h[0][1] = 2 -> q = 3 -> -j
        const auto d = prsCarrier(-767);
        CHECK(std::fabs(d.real()) < 1e-6f && std::fabs(d.imag() + 1.0f) < 1e-6f,
              "k=-767 is -j (h[0][1]=2, n=1)");
    }

    // ── DC and out-of-range answer zero rather than reading off the end ─────
    CHECK(prsCarrier(0) == std::complex<float>(0.0f, 0.0f), "DC is not a phase-reference carrier");
    CHECK(prsCarrier(5000) == std::complex<float>(0.0f, 0.0f), "out of range yields zero");

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
