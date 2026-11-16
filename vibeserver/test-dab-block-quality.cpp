// test-dab-block-quality.cpp — ★★ V12 the per-block DAB grade (vibe_dab_block_quality.h): the rule, the live grader's
// settle / span / block change, and the memory (written only on a change or a 30-min refresh, JSON, file round trip,
// a corrupt file). Stuart, 2026-11-16: "label working, marginal and non working ones".
#include "vibe_dab_block_quality.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace vibedab;
static int passes = 0, fails = 0;
static void ok(bool c, const char* what) { if (c) ++passes; else { ++fails; std::printf("   FAIL %s\n", what); } }

static const char* kNames[] = { "5A", "5B", "6C", "6D", "7D" };
static const char* nameOf(int i) { return kNames[i]; }
static int indexOf(const char* n) { for (int i = 0; i < 5; ++i) if (!std::strcmp(n, kNames[i])) return i; return -1; }

int main() {
    // ── the rule ──
    ok(gradeBlock(false, 1.0f, 20.0f, 1000, 0) == BlockGrade::None, "no lock: no grade (an empty block is nothing, not 'not working')");
    ok(gradeBlock(true, 0.0f, 7.0f, 0, 0) == BlockGrade::Bad, "Almere 6D: locked, every FIB failing, MER 7 dB = NOT WORKING");
    ok(gradeBlock(true, 0.49f, 12.0f, 0, 0) == BlockGrade::Bad, "under half the FIBs = NOT WORKING");
    ok(gradeBlock(true, 0.5f, 12.0f, 0, 0) == BlockGrade::Marginal, "half the FIBs = MARGINAL");
    ok(gradeBlock(true, 0.89f, 16.0f, 1000, 0) == BlockGrade::Marginal, "FIBs under 90 % = MARGINAL even with clean audio");
    ok(gradeBlock(true, 1.0f, 16.4f, 219, 0) == BlockGrade::Good, "Almere 6C: every FIB, 0 of 219 units lost = WORKING");
    ok(gradeBlock(true, 1.0f, 16.0f, 1000, 10) == BlockGrade::Good, "1 % units lost is still WORKING");
    ok(gradeBlock(true, 1.0f, 16.0f, 1000, 11) == BlockGrade::Marginal, "more than 1 % lost = MARGINAL");
    ok(gradeBlock(true, 0.95f, 9.0f, 10, 5) == BlockGrade::Good, "too few units to judge: the MER stands in (9 dB = WORKING)");
    ok(gradeBlock(true, 0.95f, 7.5f, 0, 0) == BlockGrade::Marginal, "no audio, MER under 8 dB = MARGINAL");

    // ── the live grader ──
    {
        BlockGrader g; BlockGrader::Verdict v; double t = 0; int got = 0; BlockGrade last = BlockGrade::None; int lastCh = -9;
        uint32_t in = 0, bad = 0;
        auto run = [&](int ch, bool locked, float fib, float mer, uint32_t perIn, uint32_t perBad, double secs) {
            for (double e = 0; e < secs; e += 0.5, t += 0.5) {
                in += perIn; bad += perBad;
                if (g.feed(ch, locked, fib, mer, in, bad, t, v)) { ++got; last = v.q.g; lastCh = v.channel; }
            }
        };
        run(2, true, 1.0f, 16.0f, 20, 0, 5.5);
        ok(got == 0, "nothing in the first 6 s on a block (the AGC's climb, the first FIC cycles)");
        run(2, true, 1.0f, 16.0f, 20, 0, 12);
        ok(got == 1 && last == BlockGrade::Good && lastCh == 2, "settled + one 10-s span: 6C WORKING");
        run(2, true, 1.0f, 16.0f, 20, 1, 11);
        ok(got == 2 && last == BlockGrade::Marginal, "a span losing 5 % of its units: MARGINAL (judged on the span's own audio)");
        in = 0; bad = 0;   // ★ a new block: the decoder's counters start again
        run(3, true, 0.0f, 7.0f, 0, 0, 5.5);
        ok(got == 2, "a block change restarts the settle");
        run(3, true, 0.0f, 7.0f, 0, 0, 12);
        ok(got == 3 && last == BlockGrade::Bad && lastCh == 3, "6D locked with no FIB: NOT WORKING");
        run(4, false, 0.0f, 0.0f, 0, 0, 30);
        ok(got == 3, "an empty block (never locked): no grade at all");
        // a lock that comes for a moment in a span of noise is not a failing multiplex
        BlockGrader h; int hgot = 0; double ht = 0;
        for (int k = 0; k < 40; ++k, ht += 0.5) if (h.feed(1, k == 20, 0.0f, 3.0f, 0, 0, ht, v)) ++hgot;
        ok(hgot == 0, "one locked snapshot in 17 s of noise: no grade (locked for at least half the span is needed)");
    }

    // ── the memory ──
    {
        BlockQualityMemory m;
        ok(m.note(3, BlockQ{ BlockGrade::Bad, 7.0f, 0.0f, 0 }, 1000), "a first grade is a change");
        ok(!m.note(3, BlockQ{ BlockGrade::Bad, 6.8f, 0.0f, 0 }, 1500), "the same grade within 30 min: not written again");
        ok(m.note(3, BlockQ{ BlockGrade::Bad, 6.8f, 0.0f, 0 }, 1000 + 1800), "the same grade 30 min on: its time refreshed");
        ok(m.note(3, BlockQ{ BlockGrade::Marginal, 9.0f, 0.7f, 0 }, 3000), "a different grade: written at once");
        ok(!m.note(2, BlockQ{ BlockGrade::None, 0, 0, 0 }, 3000), "no grade is never written");
        m.note(2, BlockQ{ BlockGrade::Good, 16.4f, 1.0f, 0 }, 4000);
        const std::string j = m.json(nameOf, 5);
        ok(j == ",\"blockQ\":{\"6C\":{\"g\":\"good\",\"mer\":16.4,\"fib\":1.00,\"t\":4000},\"6D\":{\"g\":\"marginal\",\"mer\":9.0,\"fib\":0.70,\"t\":3000}}",
           "the status fragment");
        if (j.find("6C") == std::string::npos) std::printf("      json: %s\n", j.c_str());
        ok(BlockQualityMemory().json(nameOf, 5).empty(), "nothing remembered: nothing sent (an older client sees no change)");
        // file round trip + a corrupt file
        const char* path = "/tmp/vibe-test-dab-quality.txt";
        if (FILE* f = std::fopen(path, "w")) { m.save(f, nameOf, 5); std::fputs("ZZ bad 1 0 5\n6D weird 1 0 5\n5A good 999 0.5 7\n5B good 10 1.0 -4\n7D bad 7.0 0.000 1763300000\n", f); std::fclose(f); }
        BlockQualityMemory r;
        if (FILE* f = std::fopen(path, "r")) { r.load(f, indexOf); std::fclose(f); }
        std::remove(path);
        ok(r.all().size() == 3, "the file read back: 6C, 6D, 7D — the four corrupt lines skipped");
        ok(r.all().count(4) && r.all().at(4).g == BlockGrade::Bad && r.all().at(4).t == 1763300000, "7D NOT WORKING with its time");
        ok(r.all().at(2).g == BlockGrade::Good && r.all().at(3).g == BlockGrade::Marginal, "6C and 6D kept their grades");
    }
    std::printf("test-dab-block-quality: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
